/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2021 Arm Ltd.
 */

#include <linux/linkage.h>
#include <asm/asm-uaccess.h>

	.text
	.align	2

/*
 * Clear user space with zero.
 *
 * Parameters:
 *	x0 - to
 *	x1 - n
 * Returns:
 *	x0 - bytes not cleared
 *
 * Alignment fixed up by hardware.
 */
ENTRY(__arch_clear_user)
	add	x2, x0, x1

	subs	x1, x1, #8
	b.mi	2f

1:	.p2align 4
USER(9f, sttr	xzr, [x0])
	add	x0, x0, #8
	subs	x1, x1, #8
	b.hi	1b
USER(9f, sttr	xzr, [x2, #-8])
	mov	x0, #0
	ret

2:	tbz	x1, #2, 3f
USER(9f, sttr	wzr, [x0])
USER(8f, sttr	wzr, [x2, #-4])
	mov	x0, #0
	ret

3:	tbz	x1, #1, 4f
USER(9f, sttrh	wzr, [x0])
4:	tbz	x1, #0, 5f
USER(7f, sttrb	wzr, [x2, #-1])
5:	mov	x0, #0
	ret

	// Exception fixups
7:	sub	x0, x2, #5	// Adjust for faulting on the final byte...
8:	add	x0, x0, #4	// ...or the second word of the 4-7 byte case
9:	sub	x0, x2, x0
	ret
ENDPROC(__arch_clear_user)
