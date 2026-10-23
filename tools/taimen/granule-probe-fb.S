/*
 * 16K page probe for the Pixel 2 XL (taimen) that answers on the screen.
 * Derived from the Pixel 1 port's tools/granule-probe.S (PSCI-only answer).
 *
 * Google's ABL leaves its splash framebuffer at 0x9d600000 (reserved-memory
 * splash_region, 36 MiB) and keeps the panel refreshing from it.  We paint
 * byte patterns there, which read the same in any RGB layout:
 *   top band   (first 2 MiB)  0x00  -> black: proof the probe ran at all
 *   second band (next 4 MiB)  0xFF  -> white: 16K granule supported
 *                             0x60  -> grey:  16K granule NOT supported
 * then wait ~10 s on the generic timer and PSCI SYSTEM_RESET so the phone
 * reboots into Android on its own.  Hang on the Google logo = never ran.
 */
	.text
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0x80000			/* text_offset */
	.quad	_end - _head		/* image_size */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0, 0, 0			/* res2-res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

_start:
	msr	daifset, #0xf
	mrs	x9, id_aa64mmfr0_el1
	ubfx	x9, x9, #20, #4		/* TGran16: 0 = unsupported */

	/* band 1: 2 MiB of 0x00 at 0x9d600000 */
	mov	x0, #0x9d600000
	mov	x1, #0x200000
	mov	x2, #0
	bl	fill

	/* band 2: 4 MiB of 0xFF (supported) or 0x60 (unsupported) */
	mov	x0, #0x9d800000
	mov	x1, #0x400000
	mov	x2, #0xFFFFFFFFFFFFFFFF
	cbnz	x9, 1f
	mov	x2, #0x6060606060606060
1:	bl	fill

	/* keep the band visible ~10 s, then reset */
	mrs	x3, cntfrq_el0
	mov	x4, #10
	mul	x3, x3, x4
	mrs	x4, cntpct_el0
	add	x3, x3, x4
2:	mrs	x4, cntpct_el0
	cmp	x4, x3
	b.lo	2b
	/* ensure the stores are visible to the display engine */
	dsb	sy
	mov	w0, #0x0009		/* PSCI SYSTEM_RESET 0x84000009 */
	movk	w0, #0x8400, lsl #16
	smc	#0
3:	wfe
	b	3b

/* fill(x0 = dst, x1 = bytes, x2 = 8-byte pattern) */
fill:
	stp	x2, x2, [x0], #16
	subs	x1, x1, #16
	b.ne	fill
	dsb	sy
	ret
	.balign	4096
_end:
