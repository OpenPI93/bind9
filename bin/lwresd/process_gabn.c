/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#include <config.h>

#include <sys/types.h>

#include <isc/assertions.h>
#include <isc/mem.h>
#include <isc/result.h>
#include <isc/sockaddr.h>
#include <isc/socket.h>
#include <isc/task.h>
#include <isc/util.h>

#include <dns/fixedname.h>
#include <dns/adb.h>
#include <dns/events.h>

#include <lwres/lwres.h>
#include <lwres/result.h>

#include "client.h"

#define NEED_V4(c)	((((c)->find_wanted & LWRES_ADDRTYPE_V4) != 0) \
			 && ((c)->v4find == NULL))
#define NEED_V6(c)	((((c)->find_wanted & LWRES_ADDRTYPE_V6) != 0) \
			 && ((c)->v6find == NULL))

static void start_find(client_t *);

/*
 * Destroy any finds.  This can be used to "start over from scratch" and
 * should only be called when events are _not_ being generated by the finds.
 */
static void
cleanup_gabn(client_t *client)
{
	DP(50, "Cleaning up client %p\n");

	if (client->v4find != NULL)
		dns_adb_destroyfind(&client->v4find);
	if (client->v6find != NULL)
		dns_adb_destroyfind(&client->v6find);
}

static void
generate_reply(client_t *client)
{
	DP(50, "Generating gabn reply for client %p\n");
	cleanup_gabn(client);

	client_state_idle(client);
}

/*
 * Take the current real name, move it to an alias slot (if any are
 * open) then put this new name in as the real name for the target.
 *
 * Return success if it can be rendered, otherwise failure.  Note that
 * not having enough alias slots open is NOT a failure.
 */
static isc_result_t
add_alias(client_t *client)
{
	isc_buffer_t b;
	isc_result_t result;
	isc_uint16_t naliases;

	b = client->recv_buffer;

	/*
	 * Render the new name to the buffer.
	 */
	result = dns_name_totext(dns_fixedname_name(&client->target_name),
				 ISC_TRUE, &client->recv_buffer);
	if (result != ISC_R_SUCCESS)
		return (result);

	/*
	 * Are there any open slots?
	 */
	naliases = client->gabn.naliases;
	if (naliases < LWRES_MAX_ALIASES) {
		client->gabn.aliases[naliases] = client->gabn.realname;
		client->gabn.aliaslen[naliases] = client->gabn.realnamelen;
		client->gabn.naliases++;
	}

	/*
	 * Save this name away as the current real name.
	 */
	client->gabn.realname = b.base + b.used;
	client->gabn.realnamelen = client->recv_buffer.used - b.used;

	return (ISC_R_SUCCESS);
}

static void
process_gabn_finddone(isc_task_t *task, isc_event_t *ev)
{
	client_t *client = ev->arg;
	isc_eventtype_t result;

	DP(50, "Find done for task %p, client %p", task, client);

	result = ev->type;
	isc_event_free(&ev);

	/*
	 * No more info to be had?  If so, we have all the good stuff
	 * right now, so we can render things.
	 */
	if (result == DNS_EVENT_ADBNOMOREADDRESSES) {
		if (NEED_V4(client)) {
			client->v4find = client->find;
			client->find = NULL;
		} else if (NEED_V6(client)) {
			client->v6find = client->find;
			client->find = NULL;
		} else {
			dns_adb_destroyfind(&client->find);
		}
		generate_reply(client);
		return;
	}

	/*
	 * We probably don't need this find anymore.  We're either going to
	 * reissue it, or an error occurred.  Either way, we're done with
	 * it.
	 */
	if ((client->find != client->v4find)
	    && (client->find != client->v6find)) {
		dns_adb_destroyfind(&client->find);
	} else {
		client->find = NULL;
	}

	/*
	 * We have some new information we can gather.  Run off and fetch
	 * it.
	 */
	if (result == DNS_EVENT_ADBMOREADDRESSES) {
		start_find(client);
		return;
	}

	/*
	 * An error or other strangeness happened.  Drop this query.
	 */
	cleanup_gabn(client);
	error_pkt_send(client, LWRES_R_FAILURE);
}

static void
start_find(client_t *client)
{
	unsigned int options;
	isc_result_t result;

	DP(50, "Starting find for client %p", client);

	/*
	 * Issue a find for the name contained in the request.  We won't
	 * set the bit that says "anything is good enough" -- we want it
	 * all.
	 */
	options = 0;
	options |= DNS_ADBFIND_WANTEVENT;

	/*
	 * Set the bits up here to mark that we want this address family
	 * and that we do not currently have a find pending.  We will
	 * set that bit again below if it turns out we will get an event.
	 */
	if (NEED_V4(client))
		options |= DNS_ADBFIND_INET;
	if (NEED_V6(client))
		options |= DNS_ADBFIND_INET6;

	INSIST(client->find == NULL);

 find_again:
	result = dns_adb_createfind(client->clientmgr->view->adb,
				    client->clientmgr->task,
				    process_gabn_finddone, client,
				    dns_fixedname_name(&client->target_name),
				    dns_rootname, options, 0,
				    dns_fixedname_name(&client->target_name),
				    &client->v4find);

	/*
	 * Did we get an error?
	 */
	if (result != ISC_R_SUCCESS && result != DNS_R_ALIAS) {
		error_pkt_send(client, LWRES_R_FAILURE);
		return;
	}

	/*
	 * Did we get an alias?  If so, save it and re-issue the query.
	 */
	if (result == DNS_R_ALIAS) {
		DP(50, "Found alias, restarting query.");
		dns_adb_destroyfind(&client->find);
		cleanup_gabn(client);
		result = add_alias(client);
		if (result != ISC_R_SUCCESS) {
			DP(50, "Out of buffer space adding alias");
			error_pkt_send(client, LWRES_R_FAILURE);
			return;
		}
		goto find_again;
	}

	/*
	 * Did we get our answer to V4 addresses?
	 */
	if (NEED_V4(client)
	    && ((client->find->query_pending & DNS_ADBFIND_INET) == 0))
		client->v4find = client->find;

	/*
	 * Did we get our answer to V6 addresses?
	 */
	if (NEED_V6(client)
	    && ((client->find->query_pending & DNS_ADBFIND_INET6) == 0))
		client->v6find = client->find;

	/*
	 * If we're going to get an event, set our internal pending flag
	 * and return.  When we get an event back we'll do the right
	 * thing, basically by calling this function again, perhaps with a
	 * new target name.
	 *
	 * If we have both v4 and v6, and we are still getting an event,
	 * we have a programming error, so die hard.
	 */
	if ((client->v4find->options & DNS_ADBFIND_WANTEVENT) != 0) {
		DP(50, "Event will be sent.");
		INSIST(client->v4find == NULL || client->v6find == NULL);
		return;
	}
	DP(50, "No event will be sent.");

	/*
	 * We seem to have everything we asked for, or at least we are
	 * able to respond with things we've learned.
	 */

	generate_reply(client);
}

/*
 * When we are called, we can be assured that:
 *
 *	client->sockaddr contains the address we need to reply to,
 *
 *	client->pkt contains the packet header data,
 *
 *	the packet "checks out" overall -- any MD5 hashes or crypto
 *	bits have been verified,
 *
 *	"b" points to the remaining data after the packet header
 *	was parsed off.
 *
 *	We are in a the RECVDONE state.
 *
 * From this state we will enter the SEND state if we happen to have
 * everything we need or we need to return an error packet, or to the
 * FINDWAIT state if we need to look things up.
 */
void
process_gabn(client_t *client, lwres_buffer_t *b)
{
	isc_result_t result;
	lwres_gabnrequest_t *req;
	isc_buffer_t namebuf;

	REQUIRE(CLIENT_ISRECVDONE(client));

	req = NULL;

	result = lwres_gabnrequest_parse(client->clientmgr->lwctx,
					 b, &client->pkt, &req);
	if (result != LWRES_R_SUCCESS)
		goto out;

	isc_buffer_init(&namebuf, req->name, req->namelen,
			ISC_BUFFERTYPE_TEXT);
	isc_buffer_add(&namebuf, req->namelen);

	dns_fixedname_init(&client->target_name);
	result = dns_name_fromtext(dns_fixedname_name(&client->target_name),
				   &namebuf, dns_rootname, ISC_FALSE, NULL);
	if (result != ISC_R_SUCCESS)
		goto out;

	client->find_wanted = req->addrtypes;

	/*
	 * Start the find.
	 */
	start_find(client);

	/*
	 * We no longer need to keep this around.
	 */
	lwres_gabnrequest_free(client->clientmgr->lwctx, &req);

	/*
	 * Initialize the real name and alias arrays in the reply we're
	 * going to build up.
	 */
	client_init_gabn(client);

	/*
	 * Do we have all the bits we need to generate a reply?
	 */
	if (client->find == NULL) { /* XXXMLG */
	}

	return;

	/*
	 * We're screwed.  Return an error packet to our caller.
	 */
 out:
	if (req != NULL)
		lwres_gabnrequest_free(client->clientmgr->lwctx, &req);

	error_pkt_send(client, LWRES_R_FAILURE);

	return;
}
