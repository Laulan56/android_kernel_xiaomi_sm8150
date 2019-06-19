/*
 * arm64-specific implementation of refcount_t. Based on x86 version and
 * PAX_REFCOUNT from PaX/grsecurity.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ASM_REFCOUNT_H
#define __ASM_REFCOUNT_H

#include <linux/refcount.h>

#include <asm/atomic.h>

static __always_inline void refcount_add(int i, refcount_t *r)
{
	__refcount_add_lt(i, &r->refs);
}

static __always_inline void refcount_inc(refcount_t *r)
{
	__refcount_add_lt(1, &r->refs);
}

static __always_inline void refcount_dec(refcount_t *r)
{
	__refcount_sub_le(1, &r->refs);
}

static __always_inline __must_check bool refcount_sub_and_test(unsigned int i,
							       refcount_t *r)
{
	bool ret = __refcount_sub_lt(i, &r->refs) == 0;

	if (ret) {
		smp_acquire__after_ctrl_dep();
		return true;
	}
	return false;
}

static __always_inline __must_check bool refcount_dec_and_test(refcount_t *r)
{
	return refcount_sub_and_test(1, r);
}

static __always_inline __must_check bool refcount_add_not_zero(unsigned int i,
							       refcount_t *r)
{
	return __refcount_add_not_zero(i, &r->refs) != 0;
}

static __always_inline __must_check bool refcount_inc_not_zero(refcount_t *r)
{
	return __refcount_add_not_zero(1, &r->refs) != 0;
}

#endif
