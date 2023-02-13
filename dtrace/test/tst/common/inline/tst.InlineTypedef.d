/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * ASSERTION:
 * Create inline names from aliases created using typedef.
 *
 * SECTION: Type and Constant Definitions/Inlines
 *
 * NOTES:
 *
 */

#pragma ident	"@(#)tst.InlineTypedef.d	1.1	06/08/28 SMI"

#pragma D option quiet


typedef char new_char;
inline new_char char_var = 'c';

#if !defined(__APPLE__)
typedef int * pointer;
inline pointer p = &`kmem_flags;
#else
typedef unsigned int * pointer;
inline pointer p = &`max_ncpus;
#endif /* __APPLE__ */

BEGIN
{
	printf("char_var: %c\npointer p: %d", char_var, *p);
	exit(0);
}
