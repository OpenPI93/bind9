#!/bin/sh -e
#
# Copyright (C) 2011, 2012, 2014, 2016  Internet Systems Consortium, Inc. ("ISC")
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# $Id: sign.sh,v 1.3 2011/03/01 23:48:06 tbox Exp $

SYSTEMTESTTOP=../..
. $SYSTEMTESTTOP/conf.sh

zone=signed
infile=example.db
zonefile=signed.db

key1=`$KEYGEN -q -a rsasha256 -r $RANDFILE $zone`
key2=`$KEYGEN -q -a rsasha256 -r $RANDFILE -fk $zone`

cat $infile $key1.key $key2.key > $zonefile

$SIGNER -P -g -r $RANDFILE -o $zone $zonefile > /dev/null

zone=nsec3
infile=example.db
zonefile=nsec3.db

key1=`$KEYGEN -q -a rsasha256 -r $RANDFILE -3 $zone`
key2=`$KEYGEN -q -a rsasha256 -r $RANDFILE -3 -fk $zone`

cat $infile $key1.key $key2.key > $zonefile

$SIGNER -P -3 - -g -r $RANDFILE -o $zone $zonefile > /dev/null
