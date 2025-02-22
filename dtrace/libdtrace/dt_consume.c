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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)dt_consume.c	1.23	08/01/29 SMI"

#include <stdlib.h>
#include <strings.h> 
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <assert.h>
#include <ctype.h>
#include <alloca.h>
#include <dt_impl.h>

#define	DT_MASK_LO 0x00000000FFFFFFFFULL

/*
 * We declare this here because (1) we need it and (2) we want to avoid a
 * dependency on libm in libdtrace.
 */
static long double
dt_fabsl(long double x)
{
	if (x < 0)
		return (-x);

	return (x);
}

/*
 * 128-bit arithmetic functions needed to support the stddev() aggregating
 * action.
 */
static int
dt_gt_128(uint64_t *a, uint64_t *b)
{
	return (a[1] > b[1] || (a[1] == b[1] && a[0] > b[0]));
}

static int
dt_ge_128(uint64_t *a, uint64_t *b)
{
	return (a[1] > b[1] || (a[1] == b[1] && a[0] >= b[0]));
}

static int
dt_le_128(uint64_t *a, uint64_t *b)
{
	return (a[1] < b[1] || (a[1] == b[1] && a[0] <= b[0]));
}

/*
 * Shift the 128-bit value in a by b. If b is positive, shift left.
 * If b is negative, shift right.
 */
static void
dt_shift_128(uint64_t *a, int b)
{
	uint64_t mask;

	if (b == 0)
		return;

	if (b < 0) {
		b = -b;
		if (b >= 64) {
			a[0] = a[1] >> (b - 64);
			a[1] = 0;
		} else {
			a[0] >>= b;
			mask = 1LL << (64 - b);
			mask -= 1;
			a[0] |= ((a[1] & mask) << (64 - b));
			a[1] >>= b;
		}
	} else {
		if (b >= 64) {
			a[1] = a[0] << (b - 64);
			a[0] = 0;
		} else {
			a[1] <<= b;
			mask = a[0] >> (64 - b);
			a[1] |= mask;
			a[0] <<= b;
		}
	}
}

static int
dt_nbits_128(uint64_t *a)
{
	int nbits = 0;
	uint64_t tmp[2];
	uint64_t zero[2] = { 0, 0 };

	tmp[0] = a[0];
	tmp[1] = a[1];

	dt_shift_128(tmp, -1);
	while (dt_gt_128(tmp, zero)) {
		dt_shift_128(tmp, -1);
		nbits++;
	}

	return (nbits);
}

static void
dt_subtract_128(uint64_t *minuend, uint64_t *subtrahend, uint64_t *difference)
{
	uint64_t result[2];

	result[0] = minuend[0] - subtrahend[0];
	result[1] = minuend[1] - subtrahend[1] -
	    (minuend[0] < subtrahend[0] ? 1 : 0);

	difference[0] = result[0];
	difference[1] = result[1];
}

static void
dt_add_128(uint64_t *addend1, uint64_t *addend2, uint64_t *sum)
{
	uint64_t result[2];

	result[0] = addend1[0] + addend2[0];
	result[1] = addend1[1] + addend2[1] +
	    (result[0] < addend1[0] || result[0] < addend2[0] ? 1 : 0);

	sum[0] = result[0];
	sum[1] = result[1];
}

/*
 * The basic idea is to break the 2 64-bit values into 4 32-bit values,
 * use native multiplication on those, and then re-combine into the
 * resulting 128-bit value.
 *
 * (hi1 << 32 + lo1) * (hi2 << 32 + lo2) =
 *     hi1 * hi2 << 64 +
 *     hi1 * lo2 << 32 +
 *     hi2 * lo1 << 32 +
 *     lo1 * lo2
 */
static void
dt_multiply_128(uint64_t factor1, uint64_t factor2, uint64_t *product)
{
	uint64_t hi1, hi2, lo1, lo2;
	uint64_t tmp[2];

	hi1 = factor1 >> 32;
	hi2 = factor2 >> 32;

	lo1 = factor1 & DT_MASK_LO;
	lo2 = factor2 & DT_MASK_LO;

	product[0] = lo1 * lo2;
	product[1] = hi1 * hi2;

	tmp[0] = hi1 * lo2;
	tmp[1] = 0;
	dt_shift_128(tmp, 32);
	dt_add_128(product, tmp, product);

	tmp[0] = hi2 * lo1;
	tmp[1] = 0;
	dt_shift_128(tmp, 32);
	dt_add_128(product, tmp, product);
}

/*
 * This is long-hand division.
 *
 * We initialize subtrahend by shifting divisor left as far as possible. We
 * loop, comparing subtrahend to dividend:  if subtrahend is smaller, we
 * subtract and set the appropriate bit in the result.  We then shift
 * subtrahend right by one bit for the next comparison.
 */
static void
dt_divide_128(uint64_t *dividend, uint64_t divisor, uint64_t *quotient)
{
	uint64_t result[2] = { 0, 0 };
	uint64_t remainder[2];
	uint64_t subtrahend[2];
	uint64_t divisor_128[2];
	uint64_t mask[2] = { 1, 0 };
	int log = 0;

	assert(divisor != 0);

	divisor_128[0] = divisor;
	divisor_128[1] = 0;

	remainder[0] = dividend[0];
	remainder[1] = dividend[1];

	subtrahend[0] = divisor;
	subtrahend[1] = 0;

	while (divisor > 0) {
		log++;
		divisor >>= 1;
	}

	dt_shift_128(subtrahend, 128 - log);
	dt_shift_128(mask, 128 - log);

	while (dt_ge_128(remainder, divisor_128)) {
		if (dt_ge_128(remainder, subtrahend)) {
			dt_subtract_128(remainder, subtrahend, remainder);
			result[0] |= mask[0];
			result[1] |= mask[1];
		}

		dt_shift_128(subtrahend, -1);
		dt_shift_128(mask, -1);
	}

	quotient[0] = result[0];
	quotient[1] = result[1];
}

/*
 * This is the long-hand method of calculating a square root.
 * The algorithm is as follows:
 *
 * 1. Group the digits by 2 from the right.
 * 2. Over the leftmost group, find the largest single-digit number
 *    whose square is less than that group.
 * 3. Subtract the result of the previous step (2 or 4, depending) and
 *    bring down the next two-digit group.
 * 4. For the result R we have so far, find the largest single-digit number
 *    x such that 2 * R * 10 * x + x^2 is less than the result from step 3.
 *    (Note that this is doubling R and performing a decimal left-shift by 1
 *    and searching for the appropriate decimal to fill the one's place.)
 *    The value x is the next digit in the square root.
 * Repeat steps 3 and 4 until the desired precision is reached.  (We're
 * dealing with integers, so the above is sufficient.)
 *
 * In decimal, the square root of 582,734 would be calculated as so:
 *
 *     __7__6__3
 *    | 58 27 34
 *     -49       (7^2 == 49 => 7 is the first digit in the square root)
 *      --
 *       9 27    (Subtract and bring down the next group.)
 * 146   8 76    (2 * 7 * 10 * 6 + 6^2 == 876 => 6 is the next digit in
 *      -----     the square root)
 *         51 34 (Subtract and bring down the next group.)
 * 1523    45 69 (2 * 76 * 10 * 3 + 3^2 == 4569 => 3 is the next digit in
 *         -----  the square root)
 *          5 65 (remainder)
 *
 * The above algorithm applies similarly in binary, but note that the
 * only possible non-zero value for x in step 4 is 1, so step 4 becomes a
 * simple decision: is 2 * R * 2 * 1 + 1^2 (aka R << 2 + 1) less than the
 * preceding difference?
 *
 * In binary, the square root of 11011011 would be calculated as so:
 *
 *     __1__1__1__0
 *    | 11 01 10 11
 *      01          (0 << 2 + 1 == 1 < 11 => this bit is 1)
 *      --
 *      10 01 10 11
 * 101   1 01       (1 << 2 + 1 == 101 < 1001 => next bit is 1)
 *      -----
 *       1 00 10 11
 * 1101    11 01    (11 << 2 + 1 == 1101 < 10010 => next bit is 1)
 *       -------
 *          1 01 11
 * 11101    1 11 01 (111 << 2 + 1 == 11101 > 10111 => last bit is 0)
 *
 */
static uint64_t
dt_sqrt_128(uint64_t *square)
{
	uint64_t result[2] = { 0, 0 };
	uint64_t diff[2] = { 0, 0 };
	uint64_t one[2] = { 1, 0 };
	uint64_t next_pair[2];
	uint64_t next_try[2];
	uint64_t bit_pairs, pair_shift;
	int i;

	bit_pairs = dt_nbits_128(square) / 2;
	pair_shift = bit_pairs * 2;

	for (i = 0; i <= bit_pairs; i++) {
		/*
		 * Bring down the next pair of bits.
		 */
		next_pair[0] = square[0];
		next_pair[1] = square[1];
		dt_shift_128(next_pair, -pair_shift);
		next_pair[0] &= 0x3;
		next_pair[1] = 0;

		dt_shift_128(diff, 2);
		dt_add_128(diff, next_pair, diff);

		/*
		 * next_try = R << 2 + 1
		 */
		next_try[0] = result[0];
		next_try[1] = result[1];
		dt_shift_128(next_try, 2);
		dt_add_128(next_try, one, next_try);

		if (dt_le_128(next_try, diff)) {
			dt_subtract_128(diff, next_try, diff);
			dt_shift_128(result, 1);
			dt_add_128(result, one, result);
		} else {
			dt_shift_128(result, 1);
		}

		pair_shift -= 2;
	}

	assert(result[1] == 0);

	return (result[0]);
}

uint64_t
dt_stddev(uint64_t *data, uint64_t normal)
{
	uint64_t avg_of_squares[2];
	uint64_t square_of_avg[2];
	int64_t norm_avg;
	uint64_t diff[2];

	/*
	 * The standard approximation for standard deviation is
	 * sqrt(average(x**2) - average(x)**2), i.e. the square root
	 * of the average of the squares minus the square of the average.
	 */
	dt_divide_128(data + 2, normal, avg_of_squares);
	dt_divide_128(avg_of_squares, data[0], avg_of_squares);

	norm_avg = (int64_t)data[1] / (int64_t)normal / (int64_t)data[0];

	if (norm_avg < 0)
		norm_avg = -norm_avg;

	dt_multiply_128((uint64_t)norm_avg, (uint64_t)norm_avg, square_of_avg);

	dt_subtract_128(avg_of_squares, square_of_avg, diff);

	return (dt_sqrt_128(diff));
}

static int
dt_flowindent(dtrace_hdl_t *dtp, dtrace_probedata_t *data, dtrace_epid_t last,
    dtrace_bufdesc_t *buf, size_t offs)
{
	dtrace_probedesc_t *pd = data->dtpda_pdesc, *npd;
	dtrace_eprobedesc_t *epd = data->dtpda_edesc, *nepd;
	char *p = pd->dtpd_provider, *n = pd->dtpd_name, *sub;
	dtrace_flowkind_t flow = DTRACEFLOW_NONE;
	const char *str = NULL;
	static const char *e_str[2] = { " -> ", " => " };
	static const char *r_str[2] = { " <- ", " <= " };
	static const char *ent = "entry", *ret = "return";
	static int entlen = 0, retlen = 0;
	dtrace_epid_t next, id = epd->dtepd_epid;
	int rval;

	if (entlen == 0) {
		assert(retlen == 0);
		entlen = strlen(ent);
		retlen = strlen(ret);
	}

	/*
	 * If the name of the probe is "entry" or ends with "-entry", we
	 * treat it as an entry; if it is "return" or ends with "-return",
	 * we treat it as a return.  (This allows application-provided probes
	 * like "method-entry" or "function-entry" to participate in flow
	 * indentation -- without accidentally misinterpreting popular probe
	 * names like "carpentry", "gentry" or "Coventry".)
	 */
	if ((sub = strstr(n, ent)) != NULL && sub[entlen] == '\0' &&
	    (sub == n || sub[-1] == '-')) {
		flow = DTRACEFLOW_ENTRY;
		str = e_str[strcmp(p, "syscall") == 0];
	} else if ((sub = strstr(n, ret)) != NULL && sub[retlen] == '\0' &&
	    (sub == n || sub[-1] == '-')) {
		flow = DTRACEFLOW_RETURN;
		str = r_str[strcmp(p, "syscall") == 0];
	}

	/*
	 * If we're going to indent this, we need to check the ID of our last
	 * call.  If we're looking at the same probe ID but a different EPID,
	 * we _don't_ want to indent.  (Yes, there are some minor holes in
	 * this scheme -- it's a heuristic.)
	 */
	if (flow == DTRACEFLOW_ENTRY) {
		if ((last != DTRACE_EPIDNONE && id != last &&
		    pd->dtpd_id == dtp->dt_pdesc[last]->dtpd_id))
			flow = DTRACEFLOW_NONE;
	}

	/*
	 * If we're going to unindent this, it's more difficult to see if
	 * we don't actually want to unindent it -- we need to look at the
	 * _next_ EPID.
	 */
	if (flow == DTRACEFLOW_RETURN) {
		offs += epd->dtepd_size;

		do {
			if (offs >= buf->dtbd_size) {
				/*
				 * We're at the end -- maybe.  If the oldest
				 * record is non-zero, we need to wrap.
				 */
				if (buf->dtbd_oldest != 0) {
					offs = 0;
				} else {
					goto out;
				}
			}

			next = *(uint32_t *)((uintptr_t)buf->dtbd_data + offs);

			if (next == DTRACE_EPIDNONE)
				offs += sizeof (id);
		} while (next == DTRACE_EPIDNONE);

		if ((rval = dt_epid_lookup(dtp, next, &nepd, &npd)) != 0)
			return (rval);

		if (next != id && npd->dtpd_id == pd->dtpd_id)
			flow = DTRACEFLOW_NONE;
	}

out:
	if (flow == DTRACEFLOW_ENTRY || flow == DTRACEFLOW_RETURN) {
		data->dtpda_prefix = str;
	} else {
		data->dtpda_prefix = "| ";
	}

	if (flow == DTRACEFLOW_RETURN && data->dtpda_indent > 0)
		data->dtpda_indent -= 2;

	data->dtpda_flow = flow;

	return (0);
}

static int
dt_nullprobe()
{
	return (DTRACE_CONSUME_THIS);
}

static int
dt_nullrec()
{
	return (DTRACE_CONSUME_NEXT);
}

int
dt_print_quantline(dtrace_hdl_t *dtp, FILE *fp, int64_t val,
    uint64_t normal, long double total, char positives, char negatives)
{
	long double f;
	uint_t depth, len = 40;

	const char *ats = "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@";
	const char *spaces = "                                        ";

	assert(strlen(ats) == len && strlen(spaces) == len);
	assert(!(total == 0 && (positives || negatives)));
	assert(!(val < 0 && !negatives));
	assert(!(val > 0 && !positives));
	assert(!(val != 0 && total == 0));

	if (!negatives) {
		if (positives) {
			f = (dt_fabsl((long double)val) * len) / total;
			depth = (uint_t)(f + 0.5);
		} else {
			depth = 0;
		}

		return (dt_printf(dtp, fp, "|%s%s %-9lld\n", ats + len - depth,
		    spaces + depth, (long long)val / normal));
	}

	if (!positives) {
		f = (dt_fabsl((long double)val) * len) / total;
		depth = (uint_t)(f + 0.5);

		return (dt_printf(dtp, fp, "%s%s| %-9lld\n", spaces + depth,
		    ats + len - depth, (long long)val / normal));
	}

	/*
	 * If we're here, we have both positive and negative bucket values.
	 * To express this graphically, we're going to generate both positive
	 * and negative bars separated by a centerline.  These bars are half
	 * the size of normal quantize()/lquantize() bars, so we divide the
	 * length in half before calculating the bar length.
	 */
	len /= 2;
	ats = &ats[len];
	spaces = &spaces[len];

	f = (dt_fabsl((long double)val) * len) / total;
	depth = (uint_t)(f + 0.5);

	if (val <= 0) {
		return (dt_printf(dtp, fp, "%s%s|%*s %-9lld\n", spaces + depth,
		    ats + len - depth, len, "", (long long)val / normal));
	} else {
		return (dt_printf(dtp, fp, "%20s|%s%s %-9lld\n", "",
		    ats + len - depth, spaces + depth,
		    (long long)val / normal));
	}
}

int
dt_print_quantize(dtrace_hdl_t *dtp, FILE *fp, const void *addr,
    size_t size, uint64_t normal)
{
	const int64_t *data = addr;
	int i, first_bin = 0, last_bin = DTRACE_QUANTIZE_NBUCKETS - 1;
	long double total = 0;
	char positives = 0, negatives = 0;

	if (size != DTRACE_QUANTIZE_NBUCKETS * sizeof (uint64_t))
		return (dt_set_errno(dtp, EDT_DMISMATCH));

	while (first_bin < DTRACE_QUANTIZE_NBUCKETS - 1 && data[first_bin] == 0)
		first_bin++;

	if (first_bin == DTRACE_QUANTIZE_NBUCKETS - 1) {
		/*
		 * There isn't any data.  This is possible if (and only if)
		 * negative increment values have been used.  In this case,
		 * we'll print the buckets around 0.
		 */
		first_bin = DTRACE_QUANTIZE_ZEROBUCKET - 1;
		last_bin = DTRACE_QUANTIZE_ZEROBUCKET + 1;
	} else {
		if (first_bin > 0)
			first_bin--;

		while (last_bin > 0 && data[last_bin] == 0)
			last_bin--;

		if (last_bin < DTRACE_QUANTIZE_NBUCKETS - 1)
			last_bin++;
	}

	for (i = first_bin; i <= last_bin; i++) {
		positives |= (data[i] > 0);
		negatives |= (data[i] < 0);
		total += dt_fabsl((long double)data[i]);
	}

	if (dt_printf(dtp, fp, "\n%16s %41s %-9s\n", "value",
	    "------------- Distribution -------------", "count") < 0)
		return (-1);

	for (i = first_bin; i <= last_bin; i++) {
		if (dt_printf(dtp, fp, "%16lld ",
		    (long long)DTRACE_QUANTIZE_BUCKETVAL(i)) < 0)
			return (-1);

		if (dt_print_quantline(dtp, fp, data[i], normal, total,
		    positives, negatives) < 0)
			return (-1);
	}

	return (0);
}

int
dt_print_lquantize(dtrace_hdl_t *dtp, FILE *fp, const void *addr,
    size_t size, uint64_t normal)
{
	const int64_t *data = addr;
	int i, first_bin, last_bin, base;
	uint64_t arg;
	long double total = 0;
	uint16_t step, levels;
	char positives = 0, negatives = 0;

	if (size < sizeof (uint64_t))
		return (dt_set_errno(dtp, EDT_DMISMATCH));

	arg = *data++;
	size -= sizeof (uint64_t);

	base = DTRACE_LQUANTIZE_BASE(arg);
	step = DTRACE_LQUANTIZE_STEP(arg);
	levels = DTRACE_LQUANTIZE_LEVELS(arg);

	first_bin = 0;
	last_bin = levels + 1;

	if (size != sizeof (uint64_t) * (levels + 2))
		return (dt_set_errno(dtp, EDT_DMISMATCH));

	while (first_bin <= levels + 1 && data[first_bin] == 0)
		first_bin++;

	if (first_bin > levels + 1) {
		first_bin = 0;
		last_bin = 2;
	} else {
		if (first_bin > 0)
			first_bin--;

		while (last_bin > 0 && data[last_bin] == 0)
			last_bin--;

		if (last_bin < levels + 1)
			last_bin++;
	}

	for (i = first_bin; i <= last_bin; i++) {
		positives |= (data[i] > 0);
		negatives |= (data[i] < 0);
		total += dt_fabsl((long double)data[i]);
	}

	if (dt_printf(dtp, fp, "\n%16s %41s %-9s\n", "value",
	    "------------- Distribution -------------", "count") < 0)
		return (-1);

	for (i = first_bin; i <= last_bin; i++) {
		char c[32];
		int err;

		if (i == 0) {
			(void) snprintf(c, sizeof (c), "< %d",
			    base / (uint32_t)normal);
			err = dt_printf(dtp, fp, "%16s ", c);
		} else if (i == levels + 1) {
			(void) snprintf(c, sizeof (c), ">= %d",
			    base + (levels * step));
			err = dt_printf(dtp, fp, "%16s ", c);
		} else {
			err = dt_printf(dtp, fp, "%16d ",
			    base + (i - 1) * step);
		}

		if (err < 0 || dt_print_quantline(dtp, fp, data[i], normal,
		    total, positives, negatives) < 0)
			return (-1);
	}

	return (0);
}

/*ARGSUSED*/
static int
dt_print_average(dtrace_hdl_t *dtp, FILE *fp, caddr_t addr,
    size_t size, uint64_t normal)
{
	/* LINTED - alignment */
	int64_t *data = (int64_t *)addr;

	return (dt_printf(dtp, fp, " %16lld", data[0] ?
	    (long long)(data[1] / (int64_t)normal / data[0]) : 0));
}

/*ARGSUSED*/
static int
dt_print_stddev(dtrace_hdl_t *dtp, FILE *fp, caddr_t addr,
    size_t size, uint64_t normal)
{
	/* LINTED - alignment */
	uint64_t *data = (uint64_t *)addr;

	return (dt_printf(dtp, fp, " %16llu", data[0] ?
	    (unsigned long long) dt_stddev(data, normal) : 0));
}

/*ARGSUSED*/
int
dt_print_bytes(dtrace_hdl_t *dtp, FILE *fp, caddr_t addr,
    size_t nbytes, int width, int quiet)
{
	/*
	 * If the byte stream is a series of printable characters, followed by
	 * a terminating byte, we print it out as a string.  Otherwise, we
	 * assume that it's something else and just print the bytes.
	 */
	int i, j, margin = 5;
	char *c = (char *)addr;

	if (nbytes == 0)
		return (0);

	if (dtp->dt_options[DTRACEOPT_RAWBYTES] != DTRACEOPT_UNSET)
		goto raw;

	for (i = 0; i < nbytes; i++) {
		/*
		 * We define a "printable character" to be one for which
		 * isprint(3C) returns non-zero, isspace(3C) returns non-zero,
		 * or a character which is either backspace or the bell.
		 * Backspace and the bell are regrettably special because
		 * they fail the first two tests -- and yet they are entirely
		 * printable.  These are the only two control characters that
		 * have meaning for the terminal and for which isprint(3C) and
		 * isspace(3C) return 0.
		 */
		if (isprint(c[i]) || isspace(c[i]) ||
		    c[i] == '\b' || c[i] == '\a')
			continue;

		if (c[i] == '\0' && i > 0) {
			/*
			 * This looks like it might be a string.  Before we
			 * assume that it is indeed a string, check the
			 * remainder of the byte range; if it contains
			 * additional non-nul characters, we'll assume that
			 * it's a binary stream that just happens to look like
			 * a string, and we'll print out the individual bytes.
			 */
			for (j = i + 1; j < nbytes; j++) {
				if (c[j] != '\0')
					break;
			}

			if (j != nbytes)
				break;

			if (quiet)
				return (dt_printf(dtp, fp, "%s", c));
			else
				return (dt_printf(dtp, fp, "  %-*s", width, c));
		}

		break;
	}

	if (i == nbytes) {
		/*
		 * The byte range is all printable characters, but there is
		 * no trailing nul byte.  We'll assume that it's a string and
		 * print it as such.
		 */
		char *s = alloca(nbytes + 1);
		bcopy(c, s, nbytes);
		s[nbytes] = '\0';
		return (dt_printf(dtp, fp, "  %-*s", width, s));
	}

raw:
	if (dt_printf(dtp, fp, "\n%*s      ", margin, "") < 0)
		return (-1);

	for (i = 0; i < 16; i++)
		if (dt_printf(dtp, fp, "  %c", "0123456789abcdef"[i]) < 0)
			return (-1);

	if (dt_printf(dtp, fp, "  0123456789abcdef\n") < 0)
		return (-1);


	for (i = 0; i < nbytes; i += 16) {
		if (dt_printf(dtp, fp, "%*s%5x:", margin, "", i) < 0)
			return (-1);

		for (j = i; j < i + 16 && j < nbytes; j++) {
			if (dt_printf(dtp, fp, " %02x", (uchar_t)c[j]) < 0)
				return (-1);
		}

		while (j++ % 16) {
			if (dt_printf(dtp, fp, "   ") < 0)
				return (-1);
		}

		if (dt_printf(dtp, fp, "  ") < 0)
			return (-1);

		for (j = i; j < i + 16 && j < nbytes; j++) {
			if (dt_printf(dtp, fp, "%c",
			    c[j] < ' ' || c[j] > '~' ? '.' : c[j]) < 0)
				return (-1);
		}

		if (dt_printf(dtp, fp, "\n") < 0)
			return (-1);
	}

	return (0);
}

int
dt_print_stack(dtrace_hdl_t *dtp, FILE *fp, const char *format,
    caddr_t addr, int depth, int size)
{
	dtrace_syminfo_t dts;
	GElf_Sym sym;
	int i, indent;
	char c[PATH_MAX * 2];
	uint64_t pc;

	if (dt_printf(dtp, fp, "\n") < 0)
		return (-1);

	if (format == NULL)
		format = "%s";

	if (dtp->dt_options[DTRACEOPT_STACKINDENT] != DTRACEOPT_UNSET)
		indent = (int)dtp->dt_options[DTRACEOPT_STACKINDENT];
	else
		indent = _dtrace_stkindent;

	for (i = 0; i < depth; i++) {
		switch (size) {
		case sizeof (uint32_t):
			/* LINTED - alignment */
			pc = *((uint32_t *)addr);
			break;

		case sizeof (uint64_t):
			/* LINTED - alignment */
			pc = *((uint64_t *)addr);
			break;

		default:
			return (dt_set_errno(dtp, EDT_BADSTACKPC));
		}

		if (pc == NULL)
			break;

		addr += size;

		if (dt_printf(dtp, fp, "%*s", indent, "") < 0)
			return (-1);

#if defined(__APPLE__)
		if ((dtp->dt_options[DTRACEOPT_STACKSYMBOLS] != DTRACEOPT_UNSET) && dtrace_lookup_by_addr(dtp, pc, &sym, &dts) == 0) 
#else
		if (dtrace_lookup_by_addr(dtp, pc, &sym, &dts) == 0) 
#endif
		{
			if (pc > sym.st_value) {
				(void) snprintf(c, sizeof (c), "%s`%s+0x%llx",
				    dts.dts_object, dts.dts_name,
				    pc - sym.st_value);
			} else {
				(void) snprintf(c, sizeof (c), "%s`%s",
				    dts.dts_object, dts.dts_name);
			}
		} else {
			/*
			 * We'll repeat the lookup, but this time we'll specify
			 * a NULL GElf_Sym -- indicating that we're only
			 * interested in the containing module.
			 */
#if defined(__APPLE__)
			if ((dtp->dt_options[DTRACEOPT_STACKSYMBOLS] != DTRACEOPT_UNSET) && dtrace_lookup_by_addr(dtp, pc, NULL, &dts) == 0) 
#else
			if (dtrace_lookup_by_addr(dtp, pc, NULL, &dts) == 0) 
#endif
			{
				(void) snprintf(c, sizeof (c), "%s`0x%llx",
				    dts.dts_object, pc);
			} else {
				(void) snprintf(c, sizeof (c), "0x%llx", pc);
			}
		}

		if (dt_printf(dtp, fp, format, c) < 0)
			return (-1);

		if (dt_printf(dtp, fp, "\n") < 0)
			return (-1);
	}

	return (0);
}

int
dt_print_ustack(dtrace_hdl_t *dtp, FILE *fp, const char *format,
    caddr_t addr, uint64_t arg)
{
	/* LINTED - alignment */
	uint64_t *pc = (uint64_t *)addr;
	uint32_t depth = DTRACE_USTACK_NFRAMES(arg);
	uint32_t strsize = DTRACE_USTACK_STRSIZE(arg);
	const char *strbase = addr + (depth + 1) * sizeof (uint64_t);
	const char *str = strsize ? strbase : NULL;
	int err = 0;

	char name[PATH_MAX], objname[PATH_MAX], c[PATH_MAX * 2];
	struct ps_prochandle *P;
	GElf_Sym sym;
	int i, indent;
	pid_t pid;

	if (depth == 0)
		return (0);

	pid = (pid_t)*pc++;

	if (dt_printf(dtp, fp, "\n") < 0)
		return (-1);

	if (format == NULL)
		format = "%s";

	if (dtp->dt_options[DTRACEOPT_STACKINDENT] != DTRACEOPT_UNSET)
		indent = (int)dtp->dt_options[DTRACEOPT_STACKINDENT];
	else
		indent = _dtrace_stkindent;

	/*
	 * Ultimately, we need to add an entry point in the library vector for
	 * determining <symbol, offset> from <pid, address>.  For now, if
	 * this is a vector open, we just print the raw address or string.
	 */
#if defined(__APPLE__)
	if ((dtp->dt_options[DTRACEOPT_STACKSYMBOLS] != DTRACEOPT_UNSET) && dtp->dt_vector == NULL)
#else
	if (dtp->dt_vector == NULL)
#endif
		P = dt_proc_grab(dtp, pid, PGRAB_RDONLY | PGRAB_FORCE, 0);
	else
		P = NULL;

	if (P != NULL)
		dt_proc_lock(dtp, P); /* lock handle while we perform lookups */

	for (i = 0; i < depth && pc[i] != NULL; i++) {
#if defined(__APPLE__)
		prmap_t thread_local_map;
#endif
		const prmap_t *map;

		if ((err = dt_printf(dtp, fp, "%*s", indent, "")) < 0)
			break;

		if (P != NULL && Plookup_by_addr(P, pc[i],
		    name, sizeof (name), &sym) == 0) {
			(void) Pobjname(P, pc[i], objname, sizeof (objname));

			if (pc[i] > sym.st_value) {
				(void) snprintf(c, sizeof (c),
				    "%s`%s+0x%llx", dt_basename(objname), name,
				    (u_longlong_t)(pc[i] - sym.st_value));
			} else {
				(void) snprintf(c, sizeof (c),
				    "%s`%s", dt_basename(objname), name);
			}
		} else if (str != NULL && str[0] != '\0' && str[0] != '@' &&
#if defined(__APPLE__)
			   (P != NULL && ((map = Paddr_to_map(P, pc[i], &thread_local_map)) == NULL ||
#else
			   (P != NULL && ((map = Paddr_to_map(P, pc[i])) == NULL ||
#endif
					  (map->pr_mflags & MA_WRITE)))) {
			/*
			 * If the current string pointer in the string table
			 * does not point to an empty string _and_ the program
			 * counter falls in a writable region, we'll use the
			 * string from the string table instead of the raw
			 * address.  This last condition is necessary because
			 * some (broken) ustack helpers will return a string
			 * even for a program counter that they can't
			 * identify.  If we have a string for a program
			 * counter that falls in a segment that isn't
			 * writable, we assume that we have fallen into this
			 * case and we refuse to use the string.
			 */
			(void) snprintf(c, sizeof (c), "%s", str);
		} else {
			if (P != NULL && Pobjname(P, pc[i], objname,
			    sizeof (objname)) != NULL) {
				(void) snprintf(c, sizeof (c), "%s`0x%llx",
				    dt_basename(objname), (u_longlong_t)pc[i]);
			} else {
				(void) snprintf(c, sizeof (c), "0x%llx",
				    (u_longlong_t)pc[i]);
			}
		}

		if ((err = dt_printf(dtp, fp, format, c)) < 0)
			break;

		if ((err = dt_printf(dtp, fp, "\n")) < 0)
			break;

		if (str != NULL && str[0] == '@') {
			/*
			 * If the first character of the string is an "at" sign,
			 * then the string is inferred to be an annotation --
			 * and it is printed out beneath the frame and offset
			 * with brackets.
			 */
			if ((err = dt_printf(dtp, fp, "%*s", indent, "")) < 0)
				break;

			(void) snprintf(c, sizeof (c), "  [ %s ]", &str[1]);

			if ((err = dt_printf(dtp, fp, format, c)) < 0)
				break;

			if ((err = dt_printf(dtp, fp, "\n")) < 0)
				break;
		}

		if (str != NULL) {
			str += strlen(str) + 1;
			if (str - strbase >= strsize)
				str = NULL;
		}
	}

	if (P != NULL) {
		dt_proc_unlock(dtp, P);
		dt_proc_release(dtp, P);
	}

	return (err);
}

static int
dt_print_usym(dtrace_hdl_t *dtp, FILE *fp, caddr_t addr, dtrace_actkind_t act)
{
	/* LINTED - alignment */
	uint64_t pid = ((uint64_t *)addr)[0];
	/* LINTED - alignment */
	uint64_t pc = ((uint64_t *)addr)[1];
	const char *format = "  %-50s";
	char *s;
	int n, len = 256;

	if (act == DTRACEACT_USYM && dtp->dt_vector == NULL) {
		struct ps_prochandle *P;

		if ((P = dt_proc_grab(dtp, pid,
		    PGRAB_RDONLY | PGRAB_FORCE, 0)) != NULL) {
			GElf_Sym sym;

			dt_proc_lock(dtp, P);

			if (Plookup_by_addr(P, pc, NULL, 0, &sym) == 0)
				pc = sym.st_value;

			dt_proc_unlock(dtp, P);
			dt_proc_release(dtp, P);
		}
	}

	do {
		n = len;
		s = alloca(n);
	} while ((len = dtrace_uaddr2str(dtp, pid, pc, s, n)) >= n);

	return (dt_printf(dtp, fp, format, s));
}

int
dt_print_umod(dtrace_hdl_t *dtp, FILE *fp, const char *format, caddr_t addr)
{
	/* LINTED - alignment */
	uint64_t pid = ((uint64_t *)addr)[0];
	/* LINTED - alignment */
	uint64_t pc = ((uint64_t *)addr)[1];
	int err = 0;

	char objname[PATH_MAX], c[PATH_MAX * 2];
	struct ps_prochandle *P;

	if (format == NULL)
		format = "  %-50s";

	/*
	 * See the comment in dt_print_ustack() for the rationale for
	 * printing raw addresses in the vectored case.
	 */
	if (dtp->dt_vector == NULL)
		P = dt_proc_grab(dtp, pid, PGRAB_RDONLY | PGRAB_FORCE, 0);
	else
		P = NULL;

	if (P != NULL)
		dt_proc_lock(dtp, P); /* lock handle while we perform lookups */

	if (P != NULL && Pobjname(P, pc, objname, sizeof (objname)) != NULL) {
		(void) snprintf(c, sizeof (c), "%s", dt_basename(objname));
	} else {
		(void) snprintf(c, sizeof (c), "0x%llx", (u_longlong_t)pc);
	}

	err = dt_printf(dtp, fp, format, c);

	if (P != NULL) {
		dt_proc_unlock(dtp, P);
		dt_proc_release(dtp, P);
	}

	return (err);
}

static int
dt_print_sym(dtrace_hdl_t *dtp, FILE *fp, const char *format, caddr_t addr)
{
	/* LINTED - alignment */
	uint64_t pc = *((uint64_t *)addr);
	dtrace_syminfo_t dts;
	GElf_Sym sym;
	char c[PATH_MAX * 2];

	if (format == NULL)
		format = "  %-50s";

	if (dtrace_lookup_by_addr(dtp, pc, &sym, &dts) == 0) {
		(void) snprintf(c, sizeof (c), "%s`%s",
		    dts.dts_object, dts.dts_name);
	} else {
		/*
		 * We'll repeat the lookup, but this time we'll specify a
		 * NULL GElf_Sym -- indicating that we're only interested in
		 * the containing module.
		 */
		if (dtrace_lookup_by_addr(dtp, pc, NULL, &dts) == 0) {
			(void) snprintf(c, sizeof (c), "%s`0x%llx",
			    dts.dts_object, (u_longlong_t)pc);
		} else {
			(void) snprintf(c, sizeof (c), "0x%llx",
			    (u_longlong_t)pc);
		}
	}

	if (dt_printf(dtp, fp, format, c) < 0)
		return (-1);

	return (0);
}

int
dt_print_mod(dtrace_hdl_t *dtp, FILE *fp, const char *format, caddr_t addr)
{
	/* LINTED - alignment */
	uint64_t pc = *((uint64_t *)addr);
	dtrace_syminfo_t dts;
	char c[PATH_MAX * 2];

	if (format == NULL)
		format = "  %-50s";

	if (dtrace_lookup_by_addr(dtp, pc, NULL, &dts) == 0) {
		(void) snprintf(c, sizeof (c), "%s", dts.dts_object);
	} else {
		(void) snprintf(c, sizeof (c), "0x%llx", (u_longlong_t)pc);
	}

	if (dt_printf(dtp, fp, format, c) < 0)
		return (-1);

	return (0);
}

typedef struct dt_normal {
	dtrace_aggvarid_t dtnd_id;
	uint64_t dtnd_normal;
} dt_normal_t;

static int
dt_normalize_agg(const dtrace_aggdata_t *aggdata, void *arg)
{
	dt_normal_t *normal = arg;
	dtrace_aggdesc_t *agg = aggdata->dtada_desc;
	dtrace_aggvarid_t id = normal->dtnd_id;

	if (agg->dtagd_nrecs == 0)
		return (DTRACE_AGGWALK_NEXT);

	if (agg->dtagd_varid != id)
		return (DTRACE_AGGWALK_NEXT);

	((dtrace_aggdata_t *)aggdata)->dtada_normal = normal->dtnd_normal;
	return (DTRACE_AGGWALK_NORMALIZE);
}

static int
dt_normalize(dtrace_hdl_t *dtp, caddr_t base, dtrace_recdesc_t *rec)
{
	dt_normal_t normal;
	caddr_t addr;

	/*
	 * We (should) have two records:  the aggregation ID followed by the
	 * normalization value.
	 */
	addr = base + rec->dtrd_offset;

	if (rec->dtrd_size != sizeof (dtrace_aggvarid_t))
		return (dt_set_errno(dtp, EDT_BADNORMAL));

	/* LINTED - alignment */
	normal.dtnd_id = *((dtrace_aggvarid_t *)addr);
	rec++;

	if (rec->dtrd_action != DTRACEACT_LIBACT)
		return (dt_set_errno(dtp, EDT_BADNORMAL));

	if (rec->dtrd_arg != DT_ACT_NORMALIZE)
		return (dt_set_errno(dtp, EDT_BADNORMAL));

	addr = base + rec->dtrd_offset;

	switch (rec->dtrd_size) {
	case sizeof (uint64_t):
		/* LINTED - alignment */
		normal.dtnd_normal = *((uint64_t *)addr);
		break;
	case sizeof (uint32_t):
		/* LINTED - alignment */
		normal.dtnd_normal = *((uint32_t *)addr);
		break;
	case sizeof (uint16_t):
		/* LINTED - alignment */
		normal.dtnd_normal = *((uint16_t *)addr);
		break;
	case sizeof (uint8_t):
		normal.dtnd_normal = *((uint8_t *)addr);
		break;
	default:
		return (dt_set_errno(dtp, EDT_BADNORMAL));
	}

	(void) dtrace_aggregate_walk(dtp, dt_normalize_agg, &normal);

	return (0);
}

static int
dt_denormalize_agg(const dtrace_aggdata_t *aggdata, void *arg)
{
	dtrace_aggdesc_t *agg = aggdata->dtada_desc;
	dtrace_aggvarid_t id = *((dtrace_aggvarid_t *)arg);

	if (agg->dtagd_nrecs == 0)
		return (DTRACE_AGGWALK_NEXT);

	if (agg->dtagd_varid != id)
		return (DTRACE_AGGWALK_NEXT);

	return (DTRACE_AGGWALK_DENORMALIZE);
}

static int
dt_clear_agg(const dtrace_aggdata_t *aggdata, void *arg)
{
	dtrace_aggdesc_t *agg = aggdata->dtada_desc;
	dtrace_aggvarid_t id = *((dtrace_aggvarid_t *)arg);

	if (agg->dtagd_nrecs == 0)
		return (DTRACE_AGGWALK_NEXT);

	if (agg->dtagd_varid != id)
		return (DTRACE_AGGWALK_NEXT);

	return (DTRACE_AGGWALK_CLEAR);
}

typedef struct dt_trunc {
	dtrace_aggvarid_t dttd_id;
	uint64_t dttd_remaining;
} dt_trunc_t;

static int
dt_trunc_agg(const dtrace_aggdata_t *aggdata, void *arg)
{
	dt_trunc_t *trunc = arg;
	dtrace_aggdesc_t *agg = aggdata->dtada_desc;
	dtrace_aggvarid_t id = trunc->dttd_id;

	if (agg->dtagd_nrecs == 0)
		return (DTRACE_AGGWALK_NEXT);

	if (agg->dtagd_varid != id)
		return (DTRACE_AGGWALK_NEXT);

	if (trunc->dttd_remaining == 0)
		return (DTRACE_AGGWALK_REMOVE);

	trunc->dttd_remaining--;
	return (DTRACE_AGGWALK_NEXT);
}

static int
dt_trunc(dtrace_hdl_t *dtp, caddr_t base, dtrace_recdesc_t *rec)
{
	dt_trunc_t trunc;
	caddr_t addr;
	int64_t remaining;
	int (*func)(dtrace_hdl_t *, dtrace_aggregate_f *, void *);

	/*
	 * We (should) have two records:  the aggregation ID followed by the
	 * number of aggregation entries after which the aggregation is to be
	 * truncated.
	 */
	addr = base + rec->dtrd_offset;

	if (rec->dtrd_size != sizeof (dtrace_aggvarid_t))
		return (dt_set_errno(dtp, EDT_BADTRUNC));

	/* LINTED - alignment */
	trunc.dttd_id = *((dtrace_aggvarid_t *)addr);
	rec++;

	if (rec->dtrd_action != DTRACEACT_LIBACT)
		return (dt_set_errno(dtp, EDT_BADTRUNC));

	if (rec->dtrd_arg != DT_ACT_TRUNC)
		return (dt_set_errno(dtp, EDT_BADTRUNC));

	addr = base + rec->dtrd_offset;

	switch (rec->dtrd_size) {
	case sizeof (uint64_t):
		/* LINTED - alignment */
		remaining = *((int64_t *)addr);
		break;
	case sizeof (uint32_t):
		/* LINTED - alignment */
		remaining = *((int32_t *)addr);
		break;
	case sizeof (uint16_t):
		/* LINTED - alignment */
		remaining = *((int16_t *)addr);
		break;
	case sizeof (uint8_t):
		remaining = *((int8_t *)addr);
		break;
	default:
		return (dt_set_errno(dtp, EDT_BADNORMAL));
	}

	if (remaining < 0) {
		func = dtrace_aggregate_walk_valsorted;
		remaining = -remaining;
	} else {
		func = dtrace_aggregate_walk_valrevsorted;
	}

	assert(remaining >= 0);
	trunc.dttd_remaining = remaining;

	(void) func(dtp, dt_trunc_agg, &trunc);

	return (0);
}

static int
dt_print_datum(dtrace_hdl_t *dtp, FILE *fp, dtrace_recdesc_t *rec,
    caddr_t addr, size_t size, uint64_t normal)
{
	int err;
	dtrace_actkind_t act = rec->dtrd_action;

	switch (act) {
	case DTRACEACT_STACK:
		return (dt_print_stack(dtp, fp, NULL, addr,
		    rec->dtrd_arg, rec->dtrd_size / rec->dtrd_arg));

	case DTRACEACT_USTACK:
	case DTRACEACT_JSTACK:
		return (dt_print_ustack(dtp, fp, NULL, addr, rec->dtrd_arg));

	case DTRACEACT_USYM:
	case DTRACEACT_UADDR:
		return (dt_print_usym(dtp, fp, addr, act));

	case DTRACEACT_UMOD:
		return (dt_print_umod(dtp, fp, NULL, addr));

	case DTRACEACT_SYM:
		return (dt_print_sym(dtp, fp, NULL, addr));

	case DTRACEACT_MOD:
		return (dt_print_mod(dtp, fp, NULL, addr));

	case DTRACEAGG_QUANTIZE:
		return (dt_print_quantize(dtp, fp, addr, size, normal));

	case DTRACEAGG_LQUANTIZE:
		return (dt_print_lquantize(dtp, fp, addr, size, normal));

	case DTRACEAGG_AVG:
		return (dt_print_average(dtp, fp, addr, size, normal));

	case DTRACEAGG_STDDEV:
		return (dt_print_stddev(dtp, fp, addr, size, normal));

	default:
		break;
	}

	switch (size) {
	case sizeof (uint64_t):
		err = dt_printf(dtp, fp, " %16lld",
		    /* LINTED - alignment */
		    (long long)*((uint64_t *)addr) / normal);
		break;
	case sizeof (uint32_t):
		/* LINTED - alignment */
		err = dt_printf(dtp, fp, " %8d", *((uint32_t *)addr) /
		    (uint32_t)normal);
		break;
	case sizeof (uint16_t):
		/* LINTED - alignment */
		err = dt_printf(dtp, fp, " %5d", *((uint16_t *)addr) /
		    (uint32_t)normal);
		break;
	case sizeof (uint8_t):
		err = dt_printf(dtp, fp, " %3d", *((uint8_t *)addr) /
		    (uint32_t)normal);
		break;
	default:
		err = dt_print_bytes(dtp, fp, addr, size, 50, 0);
		break;
	}

	return (err);
}

int
dt_print_aggs(const dtrace_aggdata_t **aggsdata, int naggvars, void *arg)
{
	int i, aggact = 0;
	dt_print_aggdata_t *pd = arg;
	const dtrace_aggdata_t *aggdata = aggsdata[0];
	dtrace_aggdesc_t *agg = aggdata->dtada_desc;
	FILE *fp = pd->dtpa_fp;
	dtrace_hdl_t *dtp = pd->dtpa_dtp;
	dtrace_recdesc_t *rec;
	dtrace_actkind_t act;
	caddr_t addr;
	size_t size;

	/*
	 * Iterate over each record description in the key, printing the traced
	 * data, skipping the first datum (the tuple member created by the
	 * compiler).
	 */
	for (i = 1; i < agg->dtagd_nrecs; i++) {
		rec = &agg->dtagd_rec[i];
		act = rec->dtrd_action;
		addr = aggdata->dtada_data + rec->dtrd_offset;
		size = rec->dtrd_size;

		if (DTRACEACT_ISAGG(act)) {
			aggact = i;
			break;
		}

		if (dt_print_datum(dtp, fp, rec, addr, size, 1) < 0)
			return (-1);

		if (dt_buffered_flush(dtp, NULL, rec, aggdata,
		    DTRACE_BUFDATA_AGGKEY) < 0)
			return (-1);
	}

	assert(aggact != 0);

	for (i = (naggvars == 1 ? 0 : 1); i < naggvars; i++) {
		uint64_t normal;

		aggdata = aggsdata[i];
		agg = aggdata->dtada_desc;
		rec = &agg->dtagd_rec[aggact];
		act = rec->dtrd_action;
		addr = aggdata->dtada_data + rec->dtrd_offset;
		size = rec->dtrd_size;

		assert(DTRACEACT_ISAGG(act));
		normal = aggdata->dtada_normal;

		if (dt_print_datum(dtp, fp, rec, addr, size, normal) < 0)
			return (-1);

		if (dt_buffered_flush(dtp, NULL, rec, aggdata,
		    DTRACE_BUFDATA_AGGVAL) < 0)
			return (-1);

		if (!pd->dtpa_allunprint)
			agg->dtagd_flags |= DTRACE_AGD_PRINTED;
	}

	if (dt_printf(dtp, fp, "\n") < 0)
		return (-1);

	if (dt_buffered_flush(dtp, NULL, NULL, aggdata,
	    DTRACE_BUFDATA_AGGFORMAT | DTRACE_BUFDATA_AGGLAST) < 0)
		return (-1);

	return (0);
}

int
dt_print_agg(const dtrace_aggdata_t *aggdata, void *arg)
{
	dt_print_aggdata_t *pd = arg;
	dtrace_aggdesc_t *agg = aggdata->dtada_desc;
	dtrace_aggvarid_t aggvarid = pd->dtpa_id;

	if (pd->dtpa_allunprint) {
		if (agg->dtagd_flags & DTRACE_AGD_PRINTED)
			return (0);
	} else {
		/*
		 * If we're not printing all unprinted aggregations, then the
		 * aggregation variable ID denotes a specific aggregation
		 * variable that we should print -- skip any other aggregations
		 * that we encounter.
		 */
		if (agg->dtagd_nrecs == 0)
			return (0);

		if (aggvarid != agg->dtagd_varid)
			return (0);
	}

	return (dt_print_aggs(&aggdata, 1, arg));
}

int
dt_setopt(dtrace_hdl_t *dtp, const dtrace_probedata_t *data,
    const char *option, const char *value)
{
	int len, rval;
	char *msg;
	const char *errstr;
	dtrace_setoptdata_t optdata;

	bzero(&optdata, sizeof (optdata));
	(void) dtrace_getopt(dtp, option, &optdata.dtsda_oldval);

	if (dtrace_setopt(dtp, option, value) == 0) {
		(void) dtrace_getopt(dtp, option, &optdata.dtsda_newval);
		optdata.dtsda_probe = data;
		optdata.dtsda_option = option;
		optdata.dtsda_handle = dtp;

		if ((rval = dt_handle_setopt(dtp, &optdata)) != 0)
			return (rval);

		return (0);
	}

	errstr = dtrace_errmsg(dtp, dtrace_errno(dtp));
	len = strlen(option) + strlen(value) + strlen(errstr) + 80;
	msg = alloca(len);

	(void) snprintf(msg, len, "couldn't set option \"%s\" to \"%s\": %s\n",
	    option, value, errstr);

	if ((rval = dt_handle_liberr(dtp, data, msg)) == 0)
		return (0);

	return (rval);
}

static int
dt_consume_cpu(dtrace_hdl_t *dtp, FILE *fp, int cpu, dtrace_bufdesc_t *buf,
    dtrace_consume_probe_f *efunc, dtrace_consume_rec_f *rfunc, void *arg)
{
	dtrace_epid_t id;
	size_t offs, start = buf->dtbd_oldest, end = buf->dtbd_size;
	int flow = (dtp->dt_options[DTRACEOPT_FLOWINDENT] != DTRACEOPT_UNSET);
	int quiet = (dtp->dt_options[DTRACEOPT_QUIET] != DTRACEOPT_UNSET);
	int rval, i, n;
	dtrace_epid_t last = DTRACE_EPIDNONE;
	dtrace_probedata_t data;
	uint64_t drops;
	caddr_t addr;

	bzero(&data, sizeof (data));
	data.dtpda_handle = dtp;
	data.dtpda_cpu = cpu;

again:
#if defined(__APPLE__)
	//XXX please don't nuke any of these dt_dprintf's... I need them to track the correctness
	//XXX of the function calls while I'm breaking the consume routines into collect/analyze 
	//XXX modules that can be called separately -- epmiller 05/04/07
	//XXX I'll pull them out once everything is 100%, it's about 95% now
	
	// EPM DEBUG PRINT
	// dt_dprintf("---0-------> dt_consume_cpu - %d start: %d end: %d\n",cpu, start, end);
#endif
	for (offs = start; offs < end; ) {
		dtrace_eprobedesc_t *epd;

		/*
		 * We're guaranteed to have an ID.
		 */
		id = *(uint32_t *)((uintptr_t)buf->dtbd_data + offs);
#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("---1-------> dt_consume_cpu - %d ID: %d\n",cpu, id);
#endif
		if (id == DTRACE_EPIDNONE) {
			/*
			 * This is filler to assure proper alignment of the
			 * next record; we simply ignore it.
			 */
			offs += sizeof (id);
			continue;
		}

		if ((rval = dt_epid_lookup(dtp, id, &data.dtpda_edesc,
		    &data.dtpda_pdesc)) != 0)
			return (rval);
#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("-----> dt_epid_lookup - %d ID: %d\n",cpu, id);
#endif
		epd = data.dtpda_edesc;
		data.dtpda_data = buf->dtbd_data + offs;

		if (data.dtpda_edesc->dtepd_uarg != DT_ECB_DEFAULT) {
			rval = dt_handle(dtp, &data);

			if (rval == DTRACE_CONSUME_NEXT)
				goto nextepid;

			if (rval == DTRACE_CONSUME_ERROR)
				return (-1);
		}

		if (flow)
			(void) dt_flowindent(dtp, &data, last, buf, offs);

		rval = (*efunc)(&data, arg);

		if (flow) {
			if (data.dtpda_flow == DTRACEFLOW_ENTRY)
				data.dtpda_indent += 2;
		}

		if (rval == DTRACE_CONSUME_NEXT)
			goto nextepid;

		if (rval == DTRACE_CONSUME_ABORT)
			return (dt_set_errno(dtp, EDT_DIRABORT));

		if (rval != DTRACE_CONSUME_THIS)
			return (dt_set_errno(dtp, EDT_BADRVAL));

		for (i = 0; i < epd->dtepd_nrecs; i++) {
			dtrace_recdesc_t *rec = &epd->dtepd_rec[i];
			dtrace_actkind_t act = rec->dtrd_action;
#if defined(__APPLE__)
			// EPM DEBUG PRINT
			// dt_dprintf("----------> dt_consume_cpu - %d ACT: %d\n",cpu, act);
#endif
			data.dtpda_data = buf->dtbd_data + offs +
			    rec->dtrd_offset;
			addr = data.dtpda_data;
            
			if (act == DTRACEACT_LIBACT) {
				uint64_t arg = rec->dtrd_arg;
				dtrace_aggvarid_t id;
                                
				switch (arg) {
				case DT_ACT_CLEAR:
					/* LINTED - alignment */
					id = *((dtrace_aggvarid_t *)addr);
					(void) dtrace_aggregate_walk(dtp,
					    dt_clear_agg, &id);
					continue;

				case DT_ACT_DENORMALIZE:
					/* LINTED - alignment */
					id = *((dtrace_aggvarid_t *)addr);
					(void) dtrace_aggregate_walk(dtp,
					    dt_denormalize_agg, &id);
					continue;

				case DT_ACT_FTRUNCATE:
					if (fp == NULL)
						continue;

					(void) fflush(fp);
					(void) ftruncate(fileno(fp), 0);
					(void) fseeko(fp, 0, SEEK_SET);
					continue;

				case DT_ACT_NORMALIZE:
					if (i == epd->dtepd_nrecs - 1)
						return (dt_set_errno(dtp,
						    EDT_BADNORMAL));

					if (dt_normalize(dtp,
					    buf->dtbd_data + offs, rec) != 0)
						return (-1);

					i++;
					continue;

				case DT_ACT_SETOPT: {
					uint64_t *opts = dtp->dt_options;
					dtrace_recdesc_t *valrec;
					uint32_t valsize;
					caddr_t val;
					int rv;

					if (i == epd->dtepd_nrecs - 1) {
						return (dt_set_errno(dtp,
						    EDT_BADSETOPT));
					}

					valrec = &epd->dtepd_rec[++i];
					valsize = valrec->dtrd_size;

					if (valrec->dtrd_action != act ||
					    valrec->dtrd_arg != arg) {
						return (dt_set_errno(dtp,
						    EDT_BADSETOPT));
					}

					if (valsize > sizeof (uint64_t)) {
						val = buf->dtbd_data + offs +
						    valrec->dtrd_offset;
					} else {
						val = "1";
					}

					rv = dt_setopt(dtp, &data, addr, val);

					if (rv != 0)
						return (-1);

					flow = (opts[DTRACEOPT_FLOWINDENT] !=
					    DTRACEOPT_UNSET);
					quiet = (opts[DTRACEOPT_QUIET] !=
					    DTRACEOPT_UNSET);

					continue;
				}

				case DT_ACT_TRUNC:
					if (i == epd->dtepd_nrecs - 1)
						return (dt_set_errno(dtp,
						    EDT_BADTRUNC));

					if (dt_trunc(dtp,
					    buf->dtbd_data + offs, rec) != 0)
						return (-1);

					i++;
					continue;

				default:
					continue;
				}
			}

			rval = (*rfunc)(&data, rec, arg);

#if defined(__APPLE__)
            if (act == DTRACEACT_APPLEBINARY) {
                /* 
                 * Let's skip the records following a compound
                 * APPLEBINARY action and go to the next record
                 * since the default handling behavior is to 
                 * do nothing.
                 */
                uint64_t arg = rec->dtrd_arg;
				uint16_t following_recs = 
                (uint16_t)((arg >> 16)&0xFFFF);
                
                i += following_recs;
                goto nextrec;
            }
#endif
			if (rval == DTRACE_CONSUME_NEXT)
				continue;

			if (rval == DTRACE_CONSUME_ABORT)
				return (dt_set_errno(dtp, EDT_DIRABORT));

			if (rval != DTRACE_CONSUME_THIS)
				return (dt_set_errno(dtp, EDT_BADRVAL));

			if (act == DTRACEACT_STACK) {
				int depth = rec->dtrd_arg;

				if (dt_print_stack(dtp, fp, NULL, addr, depth,
				    rec->dtrd_size / depth) < 0)
					return (-1);
				goto nextrec;
			}

			if (act == DTRACEACT_USTACK ||
			    act == DTRACEACT_JSTACK) {
				if (dt_print_ustack(dtp, fp, NULL,
				    addr, rec->dtrd_arg) < 0)
					return (-1);
				goto nextrec;
			}

			if (act == DTRACEACT_SYM) {
				if (dt_print_sym(dtp, fp, NULL, addr) < 0)
					return (-1);
				goto nextrec;
			}

			if (act == DTRACEACT_MOD) {
				if (dt_print_mod(dtp, fp, NULL, addr) < 0)
					return (-1);
				goto nextrec;
			}

			if (act == DTRACEACT_USYM || act == DTRACEACT_UADDR) {
				if (dt_print_usym(dtp, fp, addr, act) < 0)
					return (-1);
				goto nextrec;
			}

			if (act == DTRACEACT_UMOD) {
				if (dt_print_umod(dtp, fp, NULL, addr) < 0)
					return (-1);
				goto nextrec;
			}

			if (DTRACEACT_ISPRINTFLIKE(act)) {
				void *fmtdata;
				int (*func)(dtrace_hdl_t *, FILE *, void *,
				    const dtrace_probedata_t *,
				    const dtrace_recdesc_t *, uint_t,
				    const void *buf, size_t);

				if ((fmtdata = dt_format_lookup(dtp,
				    rec->dtrd_format)) == NULL)
					goto nofmt;

				switch (act) {
				case DTRACEACT_PRINTF:
					func = dtrace_fprintf;
					break;
				case DTRACEACT_PRINTA:
					func = dtrace_fprinta;
					break;
				case DTRACEACT_SYSTEM:
					func = dtrace_system;
					break;
				case DTRACEACT_FREOPEN:
					func = dtrace_freopen;
					break;
				}

				n = (*func)(dtp, fp, fmtdata, &data,
				    rec, epd->dtepd_nrecs - i,
				    (uchar_t *)buf->dtbd_data + offs,
				    buf->dtbd_size - offs);

				if (n < 0)
					return (-1); /* errno is set for us */

				if (n > 0)
					i += n - 1;
				goto nextrec;
			}

nofmt:
			if (act == DTRACEACT_PRINTA) {
				dt_print_aggdata_t pd;
				dtrace_aggvarid_t *aggvars;
				int j, naggvars = 0;
				size_t size = ((epd->dtepd_nrecs - i) *
				    sizeof (dtrace_aggvarid_t));

				if ((aggvars = dt_alloc(dtp, size)) == NULL)
					return (-1);

				/*
				 * This might be a printa() with multiple
				 * aggregation variables.  We need to scan
				 * forward through the records until we find
				 * a record from a different statement.
				 */
				for (j = i; j < epd->dtepd_nrecs; j++) {
					dtrace_recdesc_t *nrec;
					caddr_t naddr;

					nrec = &epd->dtepd_rec[j];

					if (nrec->dtrd_uarg != rec->dtrd_uarg)
						break;

					if (nrec->dtrd_action != act) {
						return (dt_set_errno(dtp,
						    EDT_BADAGG));
					}

					naddr = buf->dtbd_data + offs +
					    nrec->dtrd_offset;

					aggvars[naggvars++] =
					    /* LINTED - alignment */
					    *((dtrace_aggvarid_t *)naddr);
				}

				i = j - 1;
				bzero(&pd, sizeof (pd));
				pd.dtpa_dtp = dtp;
				pd.dtpa_fp = fp;

				assert(naggvars >= 1);

				if (naggvars == 1) {
					pd.dtpa_id = aggvars[0];
					dt_free(dtp, aggvars);

					if (dt_printf(dtp, fp, "\n") < 0 ||
					    dtrace_aggregate_walk_sorted(dtp,
					    dt_print_agg, &pd) < 0)
						return (-1);
					goto nextrec;
				}

				if (dt_printf(dtp, fp, "\n") < 0 ||
				    dtrace_aggregate_walk_joined(dtp, aggvars,
				    naggvars, dt_print_aggs, &pd) < 0) {
					dt_free(dtp, aggvars);
					return (-1);
				}

				dt_free(dtp, aggvars);
				goto nextrec;
			}

			switch (rec->dtrd_size) {
			case sizeof (uint64_t):
				n = dt_printf(dtp, fp,
				    quiet ? "%lld" : " %16lld",
				    /* LINTED - alignment */
				    *((unsigned long long *)addr));
				break;
			case sizeof (uint32_t):
				n = dt_printf(dtp, fp, quiet ? "%d" : " %8d",
				    /* LINTED - alignment */
				    *((uint32_t *)addr));
				break;
			case sizeof (uint16_t):
				n = dt_printf(dtp, fp, quiet ? "%d" : " %5d",
				    /* LINTED - alignment */
				    *((uint16_t *)addr));
				break;
			case sizeof (uint8_t):
				n = dt_printf(dtp, fp, quiet ? "%d" : " %3d",
				    *((uint8_t *)addr));
				break;
			default:
				n = dt_print_bytes(dtp, fp, addr,
				    rec->dtrd_size, 33, quiet);
				break;
			}

			if (n < 0)
				return (-1); /* errno is set for us */

nextrec:
			if (dt_buffered_flush(dtp, &data, rec, NULL, 0) < 0)
				return (-1); /* errno is set for us */
		}

		/*
		 * Call the record callback with a NULL record to indicate
		 * that we're done processing this EPID.
		 */
		rval = (*rfunc)(&data, NULL, arg);
nextepid:
		offs += epd->dtepd_size;
		last = id;
#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("---E-------> dt_consume_cpu - %d ID: %d\n",cpu, id);
#endif
	}

	if (buf->dtbd_oldest != 0 && start == buf->dtbd_oldest) {
		end = buf->dtbd_oldest;
		start = 0;
#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("---2-------> dt_consume_cpu - %d AGAIN!!\n");
#endif
		goto again;
	}

	if ((drops = buf->dtbd_drops) == 0)
		return (0);

	/*
	 * Explicitly zero the drops to prevent us from processing them again.
	 */
	buf->dtbd_drops = 0;
        
#if defined(__APPLE__)
	// EPM DEBUG PRINT
	// dt_dprintf("-XXXXX----> dt_consume_cpu - %d DROPS!!\n",cpu);
#endif
	return (dt_handle_cpudrop(dtp, cpu, DTRACEDROP_PRINCIPAL, drops));
}

typedef struct dt_begin {
	dtrace_consume_probe_f *dtbgn_probefunc;
	dtrace_consume_rec_f *dtbgn_recfunc;
	void *dtbgn_arg;
	dtrace_handle_err_f *dtbgn_errhdlr;
	void *dtbgn_errarg;
	int dtbgn_beginonly;
} dt_begin_t;

static int
dt_consume_begin_probe(const dtrace_probedata_t *data, void *arg)
{
	dt_begin_t *begin = (dt_begin_t *)arg;
	dtrace_probedesc_t *pd = data->dtpda_pdesc;

	int r1 = (strcmp(pd->dtpd_provider, "dtrace") == 0);
	int r2 = (strcmp(pd->dtpd_name, "BEGIN") == 0);

	if (begin->dtbgn_beginonly) {
		if (!(r1 && r2))
			return (DTRACE_CONSUME_NEXT);
	} else {
		if (r1 && r2)
			return (DTRACE_CONSUME_NEXT);
	}

	/*
	 * We have a record that we're interested in.  Now call the underlying
	 * probe function...
	 */
	return (begin->dtbgn_probefunc(data, begin->dtbgn_arg));
}

static int
dt_consume_begin_record(const dtrace_probedata_t *data,
    const dtrace_recdesc_t *rec, void *arg)
{
	dt_begin_t *begin = (dt_begin_t *)arg;

	return (begin->dtbgn_recfunc(data, rec, begin->dtbgn_arg));
}

static int
dt_consume_begin_error(const dtrace_errdata_t *data, void *arg)
{
	dt_begin_t *begin = (dt_begin_t *)arg;
	dtrace_probedesc_t *pd = data->dteda_pdesc;

	int r1 = (strcmp(pd->dtpd_provider, "dtrace") == 0);
	int r2 = (strcmp(pd->dtpd_name, "BEGIN") == 0);

	if (begin->dtbgn_beginonly) {
		if (!(r1 && r2))
			return (DTRACE_HANDLE_OK);
	} else {
		if (r1 && r2)
			return (DTRACE_HANDLE_OK);
	}

	return (begin->dtbgn_errhdlr(data, begin->dtbgn_errarg));
}

static int
dt_consume_begin(dtrace_hdl_t *dtp, FILE *fp, dtrace_bufdesc_t *buf,
    dtrace_consume_probe_f *pf, dtrace_consume_rec_f *rf, void *arg)
{
	/*
	 * There's this idea that the BEGIN probe should be processed before
	 * everything else, and that the END probe should be processed after
	 * anything else.  In the common case, this is pretty easy to deal
	 * with.  However, a situation may arise where the BEGIN enabling and
	 * END enabling are on the same CPU, and some enabling in the middle
	 * occurred on a different CPU.  To deal with this (blech!) we need to
	 * consume the BEGIN buffer up until the end of the BEGIN probe, and
	 * then set it aside.  We will then process every other CPU, and then
	 * we'll return to the BEGIN CPU and process the rest of the data
	 * (which will inevitably include the END probe, if any).  Making this
	 * even more complicated (!) is the library's ERROR enabling.  Because
	 * this enabling is processed before we even get into the consume call
	 * back, any ERROR firing would result in the library's ERROR enabling
	 * being processed twice -- once in our first pass (for BEGIN probes),
	 * and again in our second pass (for everything but BEGIN probes).  To
	 * deal with this, we interpose on the ERROR handler to assure that we
	 * only process ERROR enablings induced by BEGIN enablings in the
	 * first pass, and that we only process ERROR enablings _not_ induced
	 * by BEGIN enablings in the second pass.
	 */
	dt_begin_t begin;
	processorid_t cpu = dtp->dt_beganon;
	dtrace_bufdesc_t nbuf;
	int rval, i;
	static int max_ncpus;
	dtrace_optval_t size;

	dtp->dt_beganon = -1;

	if (dt_ioctl(dtp, DTRACEIOC_BUFSNAP, buf) == -1) {
		/*
		 * We really don't expect this to fail, but it is at least
		 * technically possible for this to fail with ENOENT.  In this
		 * case, we just drive on...
		 */
		if (errno == ENOENT)
			return (0);
#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("1) dt consume begin: buf snap failed... returning\n");
#endif
		return (dt_set_errno(dtp, errno));
	}

	if (!dtp->dt_stopped || buf->dtbd_cpu != dtp->dt_endedon) {
#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("]]]] dt_consume_cpu on: %d, simple case\n",cpu);
#endif
		/*
		 * This is the simple case.  We're either not stopped, or if
		 * we are, we actually processed any END probes on another
		 * CPU.  We can simply consume this buffer and return.
		 */
		return (dt_consume_cpu(dtp, fp, cpu, buf, pf, rf, arg));
	}

	begin.dtbgn_probefunc = pf;
	begin.dtbgn_recfunc = rf;
	begin.dtbgn_arg = arg;
	begin.dtbgn_beginonly = 1;

	/*
	 * We need to interpose on the ERROR handler to be sure that we
	 * only process ERRORs induced by BEGIN.
	 */
	begin.dtbgn_errhdlr = dtp->dt_errhdlr;
	begin.dtbgn_errarg = dtp->dt_errarg;
	dtp->dt_errhdlr = dt_consume_begin_error;
	dtp->dt_errarg = &begin;
#if defined(__APPLE__)
	// EPM DEBUG PRINT
	// dt_dprintf("]]]] dt_consume_cpu: (begin) : %d\n", cpu);
#endif
	rval = dt_consume_cpu(dtp, fp, cpu, buf, dt_consume_begin_probe,
	    dt_consume_begin_record, &begin);

	dtp->dt_errhdlr = begin.dtbgn_errhdlr;
	dtp->dt_errarg = begin.dtbgn_errarg;

	if (rval != 0)
		return (rval);

	/*
	 * Now allocate a new buffer.  We'll use this to deal with every other
	 * CPU.
	 */
	bzero(&nbuf, sizeof (dtrace_bufdesc_t));
	(void) dtrace_getopt(dtp, "bufsize", &size);
	if ((nbuf.dtbd_data = malloc(size)) == NULL)
		return (dt_set_errno(dtp, EDT_NOMEM));

	if (max_ncpus == 0)
		max_ncpus = dt_sysconf(dtp, _SC_CPUID_MAX) + 1;

	for (i = 0; i < max_ncpus; i++) {
		nbuf.dtbd_cpu = i;

		if (i == cpu)
			continue;

		if (dt_ioctl(dtp, DTRACEIOC_BUFSNAP, &nbuf) == -1) {
			/*
			 * If we failed with ENOENT, it may be because the
			 * CPU was unconfigured -- this is okay.  Any other
			 * error, however, is unexpected.
			 */
			if (errno == ENOENT)
				continue;
#if defined(__APPLE__)
			// EPM DEBUG PRINT
			// dt_dprintf("2) dt consume begin: buf snap failed... returning\n");
#endif
			free(nbuf.dtbd_data);

			return (dt_set_errno(dtp, errno));
		}

#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("]]]] dt_consume_cpu: (every other) : %d\n", i);
#endif
		if ((rval = dt_consume_cpu(dtp, fp,
		    i, &nbuf, pf, rf, arg)) != 0) {
			free(nbuf.dtbd_data);
			return (rval);
		}
	}

	free(nbuf.dtbd_data);

	/*
	 * Okay -- we're done with the other buffers.  Now we want to
	 * reconsume the first buffer -- but this time we're looking for
	 * everything _but_ BEGIN.  And of course, in order to only consume
	 * those ERRORs _not_ associated with BEGIN, we need to reinstall our
	 * ERROR interposition function...
	 */
	begin.dtbgn_beginonly = 0;

	assert(begin.dtbgn_errhdlr == dtp->dt_errhdlr);
	assert(begin.dtbgn_errarg == dtp->dt_errarg);
	dtp->dt_errhdlr = dt_consume_begin_error;
	dtp->dt_errarg = &begin;

#if defined(__APPLE__)
	// EPM DEBUG PRINT
	// dt_dprintf("]]]] dt_consume_cpu: (reconsume first) : %d\n", cpu);
#endif
	rval = dt_consume_cpu(dtp, fp, cpu, buf, dt_consume_begin_probe,
	    dt_consume_begin_record, &begin);

	dtp->dt_errhdlr = begin.dtbgn_errhdlr;
	dtp->dt_errarg = begin.dtbgn_errarg;

	return (rval);
}

int
dtrace_consume(dtrace_hdl_t *dtp, FILE *fp,
    dtrace_consume_probe_f *pf, dtrace_consume_rec_f *rf, void *arg)
{
	dtrace_bufdesc_t *buf = &dtp->dt_buf;
	dtrace_optval_t size;
	static int max_ncpus;
	int i, rval;
	dtrace_optval_t interval = dtp->dt_options[DTRACEOPT_SWITCHRATE];
	hrtime_t now = gethrtime();

	if (dtp->dt_lastswitch != 0) {
		if (now - dtp->dt_lastswitch < interval) {
#if defined(__APPLE__)
			// EPM DEBUG PRINT
			// dt_dprintf("rrrrr consume: It's not time to consume yet...%lld < %lld returning\n",now - dtp->dt_lastswitch,interval);
#endif
			return (0);
		}
		dtp->dt_lastswitch += interval;
	} else {
		dtp->dt_lastswitch = now;
	}

	if (!dtp->dt_active) {
#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("act act act dtrace no longer active... returning\n");
#endif
		return (dt_set_errno(dtp, EINVAL));
	}
	if (max_ncpus == 0)
		max_ncpus = dt_sysconf(dtp, _SC_CPUID_MAX) + 1;

	if (pf == NULL)
		pf = (dtrace_consume_probe_f *)dt_nullprobe;

	if (rf == NULL)
		rf = (dtrace_consume_rec_f *)dt_nullrec;

	if (buf->dtbd_data == NULL) {
		(void) dtrace_getopt(dtp, "bufsize", &size);
		if ((buf->dtbd_data = malloc(size)) == NULL)
			return (dt_set_errno(dtp, EDT_NOMEM));

		buf->dtbd_size = size;
	}
#if defined(__APPLE__)
	// EPM DEBUG PRINT
	// dt_dprintf("Time: CONSUME: timing...E %lld : I %lld\n",now - dtp->dt_lastswitch, interval);
#endif
	/*
	 * If we have just begun, we want to first process the CPU that
	 * executed the BEGIN probe (if any).
	 */
	if (dtp->dt_active && dtp->dt_beganon != -1) {
		buf->dtbd_cpu = dtp->dt_beganon;
		if ((rval = dt_consume_begin(dtp, fp, buf, pf, rf, arg)) != 0)
			return (rval);
	}

	for (i = 0; i < max_ncpus; i++) {
		buf->dtbd_cpu = i;

		/*
		 * If we have stopped, we want to process the CPU on which the
		 * END probe was processed only _after_ we have processed
		 * everything else.
		 */
		if (dtp->dt_stopped && (i == dtp->dt_endedon))
			continue;

		if (dt_ioctl(dtp, DTRACEIOC_BUFSNAP, buf) == -1) {
			/*
			 * If we failed with ENOENT, it may be because the
			 * CPU was unconfigured -- this is okay.  Any other
			 * error, however, is unexpected.
			 */
			if (errno == ENOENT)
				continue;
#if defined(__APPLE__)
			// EPM DEBUG PRINT
			// dt_dprintf("1) consume: buf snap failed... returning\n");
#endif
			return (dt_set_errno(dtp, errno));
		}

#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("]]]] calling dt_consume_cpu on %d, in everything else loop\n",i);
#endif
		if ((rval = dt_consume_cpu(dtp, fp, i, buf, pf, rf, arg)) != 0)
			return (rval);
	}

	if (!dtp->dt_stopped)
		return (0);

	buf->dtbd_cpu = dtp->dt_endedon;

	if (dt_ioctl(dtp, DTRACEIOC_BUFSNAP, buf) == -1) {
		/*
		 * This _really_ shouldn't fail, but it is strictly speaking
		 * possible for this to return ENOENT if the CPU that called
		 * the END enabling somehow managed to become unconfigured.
		 * It's unclear how the user can possibly expect anything
		 * rational to happen in this case -- the state has been thrown
		 * out along with the unconfigured CPU -- so we'll just drive
		 * on...
		 */
		if (errno == ENOENT)
			return (0);
#if defined(__APPLE__)
		// EPM DEBUG PRINT
		// dt_dprintf("2) consume: buf snap failed... returning\n");
#endif
		return (dt_set_errno(dtp, errno));
	}

#if defined(__APPLE__)
	// EPM DEBUG PRINT
	// dt_dprintf("]]]] calling dt_consume_cpu on %d...ended-on\n",dtp->dt_endedon);
#endif
	return (dt_consume_cpu(dtp, fp, dtp->dt_endedon, buf, pf, rf, arg));
}

#if defined(__APPLE__)
#pragma mark vvv modified functions. separate collect/analyze phases vvv
// These functions are not used by dtrace per se. 
// They are to be used by other tools like Shark and XRay -- epmiller 05/04/07
// --> dtp->dt_buf is the BEGIN probe data buf
// cpu_bufs lists the rest of the cpu buffers other than the one BEGIN happened on
int
dtrace_collect(dtrace_hdl_t *dtp, dtrace_bufdesc_t ***cpu_bufs)
{
	dtrace_bufdesc_t *buf;
	dtrace_optval_t size;
	static int max_ncpus = 0;
	int i, cpuct=0;
	dtrace_optval_t interval = dtp->dt_options[DTRACEOPT_SWITCHRATE];
	hrtime_t now;
	hrtime_t elapsed = 0ULL;

get_time:
	now = gethrtime();

	if (dtp->dt_lastswitch != 0) {
		elapsed = now - dtp->dt_lastswitch;
		i=0;
		
#if should_force_interval_compliance
		while (elapsed < interval) {
		//interval and elapsed are in nanoseconds, usleep measures in microseconds hence the /1000
			usleep((interval-elapsed)/1000); //artificial delay to slow things down to the original speed
			now = gethrtime();
			elapsed = now - dtp->dt_lastswitch;
			i++;
		}
#endif //should_force_interval_compliance

		if (elapsed < interval) {
			#if defined(__APPLE__)
			dt_dprintf("RRRRR collect: It's not time to collect yet...%lld < %lld returning\n",elapsed, interval);
			#endif
			return (0);
		}
		dtp->dt_lastswitch += interval;
	} else {
		dtp->dt_lastswitch = now;
	}

	if (!dtp->dt_active) {
		#if defined(__APPLE__)
		dt_dprintf("NANA collect: It's not active anymore... returning\n");
		#endif
		return (dt_set_errno(dtp, EINVAL));
	}
	
	if (max_ncpus == 0)
		max_ncpus = dt_sysconf(dtp, _SC_CPUID_MAX) + 1;

	#if defined(__APPLE__)
	dt_dprintf("Time: collect: timing (%d)...E %lld : I %lld\n",i,now - dtp->dt_lastswitch, interval);
	#endif
	//need the buffer size
	(void) dtrace_getopt(dtp, "bufsize", &size); 

	//allocate max_ncpus cpu-buffers POINTERS... plus 1 more for the "beginning buffer"
	if(*cpu_bufs == NULL) {
		*cpu_bufs = calloc(max_ncpus+1,sizeof(dtrace_bufdesc_t *));
		for (i = 0; i < max_ncpus+1; i++) {
			//allocate a buffer for each cpu...
			if((*cpu_bufs)[i] == NULL) {
				(*cpu_bufs)[i] = calloc(1, sizeof(dtrace_bufdesc_t));
				if((*cpu_bufs)[i]==NULL)
					return (dt_set_errno(dtp, EDT_NOMEM));
				//not allocating a actual buffer for the 0th cpu buffer
				//that entry will be used to store the buffer containing begin-enablement
				//allocated when we collect a "begin buffer", and copied into
				if(i>0) { 
					(*cpu_bufs)[i]->dtbd_data = malloc(size);
					if((*cpu_bufs)[i]->dtbd_data==NULL)
						return (dt_set_errno(dtp, EDT_NOMEM));
				} else {
					(*cpu_bufs)[i]->dtbd_data = NULL; //i==0
				}
			}
		}
	}

	for (i = 1; i < max_ncpus+1; i++) {
		//get a new buffer full...
		buf = (*cpu_bufs)[i];
		buf->dtbd_size = size; //max size of the buffer
		buf->dtbd_cpu = i-1;   //cpu we're interested in this time (still zero-based in the kernel)
				
		if (dt_ioctl(dtp, DTRACEIOC_BUFSNAP, buf) == -1) {
			/*
			 * If we failed with ENOENT, it may be because the
			 * CPU was unconfigured -- this is okay.  Any other
			 * error, however, is unexpected.
			 */
			if (errno == ENOENT)
				continue;
			#if defined(__APPLE__)
			dt_dprintf("IOIOIO collect: buf snap failed... returning\n",i-1,buf->dtbd_size);
			#endif
			return (dt_set_errno(dtp, errno));
		}
		cpuct++;
		#if defined(__APPLE__)
		dt_dprintf("**** collect: cpu:%d, sz: %d\n",i-1,buf->dtbd_size);
		#endif
		//if we find the beginning buffer, we need to dupe it
		//and store it in the 0th slot
		if(dtp->dt_beganon > -1 && (*cpu_bufs)[0]->dtbd_data==NULL && buf->dtbd_size > 0) {
			#if defined(__APPLE__)
			dt_dprintf("**B** collect: begincpu:%d, sz: %d\n",dtp->dt_beganon,buf->dtbd_size);
			#endif
			(*cpu_bufs)[0]->dtbd_data = malloc(size);
			if((*cpu_bufs)[0]->dtbd_data==NULL)
				return (dt_set_errno(dtp, EDT_NOMEM));
			(*cpu_bufs)[0]->dtbd_cpu = dtp->dt_beganon;
			(*cpu_bufs)[0]->dtbd_size = buf->dtbd_size;
			bcopy(buf->dtbd_data,(*cpu_bufs)[0]->dtbd_data,buf->dtbd_size);
		}
	}
	
	return (cpuct);
}

static int
dt_analyze_begin(dtrace_hdl_t *dtp, FILE *fp, dtrace_bufdesc_t *beginbuf, dtrace_bufdesc_t **otherbufs,
    dtrace_consume_probe_f *pf, dtrace_consume_rec_f *rf, void *arg)
{
	dt_begin_t begin;
	processorid_t cpu = dtp->dt_beganon;
	int rval;

	begin.dtbgn_probefunc = pf;
	begin.dtbgn_recfunc = rf;
	begin.dtbgn_arg = arg;
	begin.dtbgn_beginonly = 1;

	/*
	 * We need to interpose on the ERROR handler to be sure that we
	 * only process ERRORs induced by BEGIN.
	 */
	begin.dtbgn_errhdlr = dtp->dt_errhdlr;
	begin.dtbgn_errarg = dtp->dt_errarg;
	dtp->dt_errhdlr = dt_consume_begin_error;
	dtp->dt_errarg = &begin;

	#if defined(__APPLE__)
	dt_dprintf(">>>>>>> in analyze_begin: calling dt_consume_cpu = %d (begin bits)\n",cpu);
	#endif
	rval = dt_consume_cpu(dtp, fp, cpu, beginbuf, dt_consume_begin_probe,
	    dt_consume_begin_record, &begin);

	dtp->dt_beganon = -1;
	dtp->dt_errhdlr = begin.dtbgn_errhdlr;
	dtp->dt_errarg = begin.dtbgn_errarg;

	return (rval);
}

int
dtrace_analyze(dtrace_hdl_t *dtp, FILE *fp, dtrace_bufdesc_t **cpu_bufs,
    dtrace_bufdesc_t **agg_bufs, dtrace_consume_probe_f *pf, dtrace_consume_rec_f *rf, void *arg)
{
	static int max_ncpus=0;
	int i, rval;
	static int begincpu=-1;
	dtrace_bufdesc_t *buf = NULL;
	dt_begin_t begin;

	if (max_ncpus == 0)
		max_ncpus = dt_sysconf(dtp, _SC_CPUID_MAX) + 1;

	if (pf == NULL)
		pf = (dtrace_consume_probe_f *)dt_nullprobe;

	if (rf == NULL)
		rf = (dtrace_consume_rec_f *)dt_nullrec;


	if(agg_bufs)
		if (dtrace_aggregate_hash(dtp, agg_bufs) == -1)
			return (DTRACE_WORKSTATUS_ERROR);

	/*
	 * If we have just begun, we want to first process the CPU that
	 * executed the BEGIN probe (if any).
	 * XXX dt_consume_begin trys and gets buffers back from the kernel.
	 * XXX need to write a 'dt_analyze_begin' that processes 
	 * XXX the same way, but from a passed-in buffer
	 * XXX dt_consume_cpu does not try to retrieve data  
	 */
	if(cpu_bufs) {
		for (i = 1; i < max_ncpus+1; i++) {
			#if defined(__APPLE__)
			dt_dprintf("XX cpu:%d, stopped:%d, end:%d\n",i-1,dtp->dt_stopped,dtp->dt_endedon);
			dt_dprintf("XXX cpu:%d sz:%d\n",i-1,(cpu_bufs[i])->dtbd_size);
			#endif
			if (dtp->dt_stopped) {
				if (dtp->dt_beganon > -1) begincpu = dtp->dt_beganon;
				#if defined(__APPLE__)
				dt_dprintf("XXXX cpu:%d, bgn: %d(%d) sz: %d\n",
					i-1,begincpu,dtp->dt_beganon,(cpu_bufs[begincpu+1])->dtbd_size);
				dt_dprintf("---- end:%d sz: %d\n",dtp->dt_endedon,(cpu_bufs[dtp->dt_endedon+1])->dtbd_size);
				#endif
				if (i == dtp->dt_beganon+1) {
					if((cpu_bufs[begincpu+1])->dtbd_size == 0) { //false positive
						#if defined(__APPLE__)
						dt_dprintf(">>false positive begin: %d\n",begincpu);
						#endif
						dtp->dt_beganon = -1;
						begincpu = -1;
						continue;
					}
					buf = cpu_bufs[0];//[dtp->dt_beganon+1];//was copied in collect phase
					//buf->dtbd_cpu = begincpu;
					if(buf->dtbd_size > 0)
						if ((rval = dt_analyze_begin(dtp, fp, buf, cpu_bufs, pf, rf, arg)) != 0)
							return (rval);
					if(i != dtp->dt_endedon+1)
						continue;
				}

				//only run over all the cpus 1 time...
				if(i == dtp->dt_endedon+1) {
					if(i != max_ncpus) 
						continue;
					else
						break;
				}
			}
							
			//a cpu that is neither a began-on or ended-on cpu
				buf = cpu_bufs[i];
				//buf->dtbd_cpu = i-1;
			if(!dtp->dt_stopped && buf->dtbd_size > 0) {
				#if defined(__APPLE__)
				dt_dprintf(">>>>> calling dt_consume_cpu = %d (loop)\n",i-1);
				#endif
				if ((rval = dt_consume_cpu(dtp, fp, i-1, buf, pf, rf, arg)) != 0) {
					return (rval);
				}
				if(dtp->dt_beganon == i-1) {//we just processed the begin buffer
					dtp->dt_beganon = -1;
					begincpu = -1;
					#if defined(__APPLE__)
					dt_dprintf(">>>>> consumed the begin buffer:%d (loop)\n",i-1);
					#endif
				}
			}
		}
		
		if(dtp->dt_stopped && dtp->dt_beganon == -1 && begincpu > -1) {
			//analyze the remainder of the buffer collected from the cpu we began on
			buf = cpu_bufs[0];//[begincpu];			
			///buf->dtbd_cpu = begincpu;

			begin.dtbgn_probefunc = pf;
			begin.dtbgn_recfunc = rf;
			begin.dtbgn_beginonly = 0;
			begin.dtbgn_arg = arg;

			begin.dtbgn_errhdlr = dtp->dt_errhdlr;
			begin.dtbgn_errarg = dtp->dt_errarg;
			dtp->dt_errhdlr = dt_consume_begin_error;
			dtp->dt_errarg = &begin;
		
			#if defined(__APPLE__)
			dt_dprintf(">>>>>>> analyze: calling dt_consume_cpu - remainder of: %d\n",begincpu);
			#endif
			rval = dt_consume_cpu(dtp, fp, begincpu, buf, dt_consume_begin_probe,
				dt_consume_begin_record, &begin);
			if (rval != 0) {
				return (rval);
			}
			
			dtp->dt_errhdlr = begin.dtbgn_errhdlr;
			dtp->dt_errarg = begin.dtbgn_errarg;
			
			if(dtp->dt_endedon == begincpu) { //we just handled the end...
				dtp->dt_endedon = -1;
				#if defined(__APPLE__)
				dt_dprintf(">>>>>>> analyze: just handled the end condition on cpu: %d (same as begin cpu)\n",begincpu);
				#endif
			}
			begincpu = -1; //finally finished with the begin buffer
		}

		if(dtp->dt_stopped && dtp->dt_endedon > -1 && dtp->dt_endedon != begincpu) {
			buf = cpu_bufs[dtp->dt_endedon+1];
			#if defined(__APPLE__)
			dt_dprintf(">>>>>>> calling dt_consume_cpu = %d (ended on)\n",dtp->dt_endedon);
			#endif
			return (dt_consume_cpu(dtp, fp, dtp->dt_endedon, buf, pf, rf, arg));
		}
	}
	return (0);
}
#pragma mark ^^^ modified functions. test separate collect/analyze phases ^^^
#endif /* __APPLE__ */

