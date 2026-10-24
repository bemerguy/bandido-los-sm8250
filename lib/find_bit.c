/* bit search implementation
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * Copyright (C) 2008 IBM Corporation
 * 'find_last_bit' is written by Rusty Russell <rusty@rustcorp.com.au>
 * (Inspired by David Howell's find_next_bit implementation)
 *
 * Rewritten by Yury Norov <yury.norov@gmail.com> to decrease
 * size and improve performance, 2015.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/kernel.h>

/*
 * Common helper for find_bit() function family
 * @FETCH: The expression that fetches and pre-processes each word of bitmap(s)
 * @MUNGE: The expression that post-processes a word containing found bit (may be empty)
 * @size: The bitmap size in bits
 */
#define FIND_FIRST_BIT(FETCH, MUNGE, size)					\
({										\
	unsigned long idx, val, sz = (size);					\
										\
	for (idx = 0; idx * BITS_PER_LONG < sz; idx++) {			\
		val = (FETCH);							\
		if (val) {							\
			sz = min(idx * BITS_PER_LONG + __ffs(MUNGE(val)), sz);	\
			break;							\
		}								\
	}									\
										\
	sz;									\
})

/*
 * Common helper for find_next_bit() function family
 * @FETCH: The expression that fetches and pre-processes each word of bitmap(s)
 * @MUNGE: The expression that post-processes a word containing found bit (may be empty)
 * @size: The bitmap size in bits
 * @start: The bitnumber to start searching at
 */
#define FIND_NEXT_BIT(FETCH, MUNGE, size, start)				\
({										\
	unsigned long mask, idx, tmp, sz = (size), __start = (start);		\
										\
	if (unlikely(__start >= sz))						\
		goto out;							\
										\
	mask = MUNGE(BITMAP_FIRST_WORD_MASK(__start));				\
	idx = __start / BITS_PER_LONG;						\
										\
	for (tmp = (FETCH) & mask; !tmp; tmp = (FETCH)) {			\
		if ((idx + 1) * BITS_PER_LONG >= sz)				\
			goto out;						\
		idx++;								\
	}									\
										\
	sz = min(idx * BITS_PER_LONG + __ffs(MUNGE(tmp)), sz);			\
out:										\
	sz;									\
})

unsigned long _find_next_bit(const unsigned long *addr, unsigned long size,
			    unsigned long offset)
{
	return FIND_NEXT_BIT(addr[idx], /* nop */, size, offset);
}
EXPORT_SYMBOL(_find_next_bit);

unsigned long _find_next_zero_bit(const unsigned long *addr, unsigned long size,
				 unsigned long offset)
{
	return FIND_NEXT_BIT(~addr[idx], /* nop */, size, offset);
}
EXPORT_SYMBOL(_find_next_zero_bit);

unsigned long _find_next_and_bit(const unsigned long *addr1,
		const unsigned long *addr2, unsigned long size,
		unsigned long offset)
{
	return FIND_NEXT_BIT(addr1[idx] & addr2[idx], /* nop */, size, offset);
}
EXPORT_SYMBOL(_find_next_and_bit);

#ifndef find_first_bit
/*
 * Find the first set bit in a memory region.
 */
unsigned long find_first_bit(const unsigned long *addr, unsigned long size)
{
	return FIND_FIRST_BIT(addr[idx], /* nop */, size);
}
EXPORT_SYMBOL(find_first_bit);
#endif

#ifndef find_first_zero_bit
/*
 * Find the first cleared bit in a memory region.
 */
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	return FIND_FIRST_BIT(~addr[idx], /* nop */, size);
}
EXPORT_SYMBOL(find_first_zero_bit);
#endif

#ifndef find_last_bit
unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	if (size) {
		unsigned long val = BITMAP_LAST_WORD_MASK(size);
		unsigned long idx = (size-1) / BITS_PER_LONG;

		do {
			val &= addr[idx];
			if (val)
				return idx * BITS_PER_LONG + __fls(val);

			val = ~0ul;
		} while (idx--);
	}
	return size;
}
EXPORT_SYMBOL(find_last_bit);
#endif

#ifdef __BIG_ENDIAN

#if !defined(find_next_bit_le) || !defined(find_next_zero_bit_le)
static inline unsigned long _find_next_bit_le(const unsigned long *addr1,
		const unsigned long *addr2, unsigned long nbits,
		unsigned long start, unsigned long invert)
{
	unsigned long tmp;

	if (unlikely(start >= nbits))
		return nbits;

	tmp = addr1[start / BITS_PER_LONG];
	if (addr2)
		tmp &= addr2[start / BITS_PER_LONG];
	tmp ^= invert;

	/* Handle 1st word. */
	tmp &= swab(BITMAP_FIRST_WORD_MASK(start));
	start = round_down(start, BITS_PER_LONG);

	while (!tmp) {
		start += BITS_PER_LONG;
		if (start >= nbits)
			return nbits;

		tmp = addr1[start / BITS_PER_LONG];
		if (addr2)
			tmp &= addr2[start / BITS_PER_LONG];
		tmp ^= invert;
	}

	return min(start + __ffs(swab(tmp)), nbits);
}
#endif

#ifndef find_next_zero_bit_le
unsigned long find_next_zero_bit_le(const void *addr, unsigned
		long size, unsigned long offset)
{
	return _find_next_bit_le(addr, NULL, size, offset, ~0UL);
}
EXPORT_SYMBOL(find_next_zero_bit_le);
#endif

#ifndef find_next_bit_le
unsigned long find_next_bit_le(const void *addr, unsigned
		long size, unsigned long offset)
{
	return _find_next_bit_le(addr, NULL, size, offset, 0UL);
}
EXPORT_SYMBOL(find_next_bit_le);
#endif

#endif /* __BIG_ENDIAN */
