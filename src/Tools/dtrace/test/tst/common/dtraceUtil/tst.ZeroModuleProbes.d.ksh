#!/bin/sh -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#ident	"@(#)tst.ZeroModuleProbes.d.ksh	1.1	06/08/28 SMI"

##
#
# ASSERTION:
# The -Z option can be used to permit descriptions that match
# zero probes.
#
# SECTION: dtrace Utility/-Z Option;
# 	dtrace Utility/-m Option
#
##

dtrace=/usr/sbin/dtrace

if [ -f /usr/lib/dtrace/darwin.d ]; then
$dtrace -qZm wassup'{printf("Iamkool");}' \
-qn BEGIN'{printf("I am done"); exit(0);}'
else
$dtrace -qZm wassup'{printf("Iamkool");}' \
-qm unix'{printf("I am done"); exit(0);}'
fi

status=$?

if [ "$status" -ne 0 ]; then
	echo $tst: dtrace failed
fi

exit $status
ed
fi

exit $status
