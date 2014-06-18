/*
 *  linux/arch/arm/boot/compressed/head.S
 *
 *  Copyright (C) 1996-2002 Russell King
 *  Copyright (C) 2004 Hyok S. Choi (MPU support)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/linkage.h>
#include <asm/assembler.h>

/*!!C-----------------------------------------------------------------
 * Let's Start. ARM11A
 -------------------------------------------------------------------*/

	.arch	armv7-a
/*
 * Debugging stuff
 *
 * Note that these macros must not contain any code which is not
 * 100% relocatable.  Any attempt to do so will result in a crash.
 * Please select one of the following when turning on debugging.
 */
#ifdef DEBUG

#if defined(CONFIG_DEBUG_ICEDCC)

/*!!Q-----------------------------------------------------------------
 * loadsp, rb, tmp 에서 tmp 는 주소값을 나타낸다.
 * define 에 따라 특정 주소값을 명시적으로 줄 수도 있고, * tmp 를 통해 인자로 줄 수도 있고,
 * co-processor 를 사용할 경우에는 loadsp 가 아무일도 안할 수도 있다.
 -------------------------------------------------------------------*/
#if defined(CONFIG_CPU_V6) || defined(CONFIG_CPU_V6K) || defined(CONFIG_CPU_V7)
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb  /*!!C rb 는 실제로 사용되지는 않는다. length 를 의미하는게 아니다. */
		mcr	p14, 0, \ch, c0, c5, 0
		.endm
#elif defined(CONFIG_CPU_XSCALE)
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c8, c0, 0
		.endm
#else
		.macro	loadsp, rb, tmp
		.endm
		.macro	writeb, ch, rb
		mcr	p14, 0, \ch, c1, c0, 0
		.endm
#endif

#else

#include CONFIG_DEBUG_LL_INCLUDE

		.macro	writeb,	ch, rb
		senduart \ch, \rb
		.endm

#if defined(CONFIG_ARCH_SA1100)
		.macro	loadsp, rb, tmp
		mov	\rb, #0x80000000	@ physical base address
#ifdef CONFIG_DEBUG_LL_SER3
		add	\rb, \rb, #0x00050000	@ Ser3
#else
		add	\rb, \rb, #0x00010000	@ Ser1
#endif
		.endm
#elif defined(CONFIG_ARCH_S3C24XX)
		.macro loadsp, rb, tmp
		mov	\rb, #0x50000000
		add	\rb, \rb, #0x4000 * CONFIG_S3C_LOWLEVEL_UART_PORT
		.endm
#else
		.macro	loadsp,	rb, tmp
		addruart \rb, \tmp
		.endm
#endif
#endif
#endif

		.macro	kputc,val
		mov	r0, \val
		bl	putc
		.endm

		.macro	kphex,val,len
		mov	r0, \val
		mov	r1, #\len
		bl	phex
		.endm

		.macro	debug_reloc_start
#ifdef DEBUG
		kputc	#'\n'
		kphex	r6, 8		/* processor id */
		kputc	#':'
		kphex	r7, 8		/* architecture id */
#ifdef CONFIG_CPU_CP15
		kputc	#':'
		mrc	p15, 0, r0, c1, c0
		kphex	r0, 8		/* control reg */
#endif
		kputc	#'\n'
		kphex	r5, 8		/* decompressed kernel start */
		kputc	#'-'
		kphex	r9, 8		/* decompressed kernel end  */
		kputc	#'>'
		kphex	r4, 8		/* kernel execution address */
		kputc	#'\n'
#endif
		.endm

		.macro	debug_reloc_end
#ifdef DEBUG
		kphex	r5, 8		/* end of kernel */
		kputc	#'\n'
		mov	r0, r4
		bl	memdump		/* dump 256 bytes at start of kernel */
#endif
		.endm

		.section ".start", #alloc, #execinstr
/*
 * sort out different calling conventions
 */
		.align      /*!!C default = 4 byte */
		.arm				@ Always enter in ARM state
start:
		.type	start,#function

        /*!!C-----------------------------------------------------------------
         * iamroot 에 댓글 찾아보면 이유를 알 수 있음.
         * 매직넘버의 위치를 잘못알고 있는
         * 특정 boot loader 를 위한 코드
         -------------------------------------------------------------------*/
		.rept	7
		mov	r0, r0
		.endr

   /*!!C-----------------------------------------------------------------
    * thumb 은 2 개의 instruction 을 수행하지만
    * ARM 은 2 개를 맞추기 위해서 무의미한 mov 를 하나 추가
    * 이 또한 magic number 의 위치를 맞추기 위한 방법인 듯.
    -------------------------------------------------------------------*/
   ARM(		mov	r0, r0		)

   /*!!C-----------------------------------------------------------------
    * 20:  ea000002    b    0x30
    -------------------------------------------------------------------*/
   ARM(		b	1f		)
 THUMB(		adr	r12, BSYM(1f)	)
 THUMB(		bx	r12		)

		.word	0x016f2818		@ Magic numbers to help the loader
        /*!!Q-----------------------------------------------------------------
         * start 와 _edata 는 언제 누가 설정해주는건가 ?
         * -> linker script 에 지정할 수 있음 
         -------------------------------------------------------------------*/
		.word	start			@ absolute load/run zImage address
		.word	_edata			@ zImage end address
 THUMB(		.thumb			)
1:
       /*!!C-----------------------------------------------------------------
        * 30:  e10f9000    mrs r9, cpsr
        -------------------------------------------------------------------*/
		mrs	r9, cpsr
#ifdef CONFIG_ARM_VIRT_EXT

       /*!!C-----------------------------------------------------------------
        * 34:  eb0000e1    bl 0x38c0
        -------------------------------------------------------------------*/
		bl	__hyp_stub_install	@ get into SVC mode, reversibly
#endif
        /*!!C-----------------------------------------------------------------
         * r1, r2 는 boot loader 에서 넘어옴.
         * r2 는 각종 메모리 정보 및 H/W 정보를 저장한 구조체.
         -------------------------------------------------------------------*/
		mov	r7, r1			@ save architecture ID
		mov	r8, r2			@ save atags pointer

		/*
		 * Booting from Angel - need to enter SVC mode and disable
		 * FIQs/IRQs (numeric definitions from angel arm.h source).
		 * We only do this if we were in user mode on entry.
		 */
		mrs	r2, cpsr		@ get current mode
        /*!!C-----------------------------------------------------------------
         * supervisor mode(0x3)
         * 현재는 SVC 모드이므로, 이의 결과로 zero flag 가
         * 0 이 된다. (and 연산의 결과가 0 이 아니므로)
         -------------------------------------------------------------------*/
		tst	r2, #3			@ not user?

        /*!!C-----------------------------------------------------------------
         * user mode 가 아니면 not_angel 로 분기.
         * not equal 은 zero flag 가 설정(1)안되어 있다는 말임.
         * 현재는 user mode 가 아니므로 not_angel 로 분기하게 됨.
         -------------------------------------------------------------------*/

        /*!!C-----------------------------------------------------------------
         * 48:  1a000001   bne  0x54
         -------------------------------------------------------------------*/
		bne	not_angel
		mov	r0, #0x17		@ angel_SWIreason_EnterSVC
 ARM(		swi	0x123456	)	@ angel_SWI_ARM
 THUMB(		svc	0xab		)	@ angel_SWI_THUMB
not_angel:

        /*!!C-----------------------------------------------------------------
         * 54:  e10f0000    mrs  r0, cpsr
         -------------------------------------------------------------------*/
		safe_svcmode_maskall r0 /*!!C 2014-06-07 */

        /*!!C-----------------------------------------------------------------
         * 84:  e16ff009    msr  spsr_cxsf, r9
         -------------------------------------------------------------------*/
		msr	spsr_cxsf, r9		@ Save the CPU boot mode in
						@ SPSR
		/*
		 * Note that some cache flushing and other stuff may
		 * be needed here - is there an Angel SWI call for this?
		 */

		/*
		 * some architecture specific code can be inserted
		 * by the linker here, but it should preserve r7, r8, and r9.
		 */

        /*!!C-----------------------------------------------------------------
         * System.map 을 보면 _text 가 어디에서부터인지 확인할 수 있다.
         * C0008000  T  _text
         -------------------------------------------------------------------*/
		.text

#ifdef CONFIG_AUTO_ZRELADDR
		@ determine final kernel image address

        /*!!C-----------------------------------------------------------------
         * a0:  e1a0400f    mov r4, pc
         -------------------------------------------------------------------*/
		mov	r4, pc

        /*!!C-----------------------------------------------------------------
         * 상위 5 bit 만 빼냄 
         * (zImage 를 어디에 올렸더라도 128 MB 경계를 기준으로 생각함 !!!)
         -------------------------------------------------------------------*/
		and	r4, r4, #0xf8000000

        /*!!C-----------------------------------------------------------------
         * arch/arm/Makefile 에 정의되어 있음.
         * 컴파일 옵션으로 -DTEXT_OFFSET 으로 주어짐.
         * textofs-y   := 0x00008000  ; 32 KB
         -------------------------------------------------------------------*/
		add	r4, r4, #TEXT_OFFSET
#else
        /*!!C-----------------------------------------------------------------
         * mach-exynos/Makefile.boot 에 zreladdr-y += 0x40008000
         -------------------------------------------------------------------*/
		ldr	r4, =zreladdr
#endif

		/*!!C-----------------------------------------------------------------
		 * Set up a page table only if it won't overwrite ourself.
		 * That means r4 < pc && r4 - 16k page directory > &_end.
		 * Given that r4 > &_end is most unfrequent, we add a rough
		 * additional 1MB of room for a possible appended DTB.
         *
         * page table 이 이 코드를 overwrite 하지 않는 경우에만 page table 을 셋팅한다.
         * 즉, cache_on 과정으로 들어간다.
         *
         *  case(1) r0 >= r4 인 경우 
         *          이는 zImage 가 최소한 0x8000 offset 보다는 위쪽에 로딩된 경우임.
         *          위와 같은 경우는 cache_on 에 의해 overwrite 될 일이 없으므로
         *          바로 cache_on 으로 가면 된다.
         *          cache_on 내에서 page directory 를 위해 (1) 구간을 사용한다. 
         *
         *          +--------------+-> 128m end
         *          |              |
         *          |              |
         *          +--------------+ _end
         *          |              |
         *          |    zImage    |-> r0 = pc
         *          |              |
         *          +--------------+
         *          |              |
         *          |              |-> r4 = +0x8000 (32k) (uncompressed image start)
         *          |     (1)      |
         *          +--------------+-> 128m start
         *
         *
         *  case(2) r0 < r4 인 경우 첫번째 
         *          r0 값을 pc + zImage size + 16k + 1m 로 조정하여 r4 와 비교했을 때
         *          그래도 r4 가 크다면 case(1) 과 마찬가지로 그냥 cache on 으로 가면 된다.
         *          하지만, 이는 발생할 수 없는 case 이다.
         *          (pc + zImage size + 16k + 1m) 의 값이 (pc & 0xf8 + 0x8000)보다 작을 수는
         *          없기 때문이다.  zImage size 만해도 2m 가 넘기 때문에...
         *          dtb 1m 가 없고 zImage size 가 아주 작다면 가능한 얘기이긴 하지만 unfrequent 하다.
         *
         *          +--------------+-> 128m end
         *          |              |
         *          |              |
         *          |              |-> r4 = +0x8000 (32k) (uncompressed image start)
         *          |              |
         *          |              |-> r0 = pc + zImage size + 16k + 1m
         *          |              |
         *          +--------------+ _end
         *          |              |
         *          |    zImage    |
         *          |              |-> r0 = pc
         *          +--------------+
         *          |              |
         *          |              |
         *          +--------------+-> 128m start
         *
         *
         *  case(3) r0 < r4 인 경우 두번째 
         *          zImage 를 r4 지점보다 낮을 곳에 올리게 되면 아래와 같이 uncompressed
         *          image start 지점이 zImage 를 overwrite 할 수 있다.
         *          이 때는 cache on 으로 가지 않고 restart label 로 간다. 
         *
         *          +--------------+-> 128m end
         *          |              |
         *          |              |
         *          |              +-> r0 = pc + zImage size + 16k + 1m
         *          |              |
         *          +--------------+ _end
         *          |              |
         *          |              |-> r4 = +0x8000 (32k) (uncompressed image start)
         *          |    zImage    |
         *          |              |-> r0 = pc
         *          +--------------+
         *          |              |
         *          |              |
         *          +--------------+-> 128m start
         *
         *
         * 결국 위의 3 개 상황 중에서 best 는 case(1)의 경우이다.
         * zImage 를 memory 에 올릴 때 로딩 위치를 잘 잡아주면 쓸데없이 재배치하고 이런
         * 수고들이 덜어져서 booting 속도도 빠를 것이다.
         * 위와 같은 상황들 때문에 Document/arm/Booting 문서를 보면 다음과 같은 문구들이 있다.
         *
         * 4b. Setup the device tree
         *     A safe location is just above the 128MiB boundary from start of RAM.
         *     dtb 는 128m 경계 바로 위에 위치시켜야 안전하다.
         *
         * 6. Calling the kernel image
         * ---------------------------
         * ...
         * The zImage may also be placed in system RAM and called there.  The
         * kernel should be placed in the first 128MiB of RAM.  It is recommended
         * that it is loaded above 32MiB in order to avoid the need to relocate
         * prior to decompression, which will make the boot process slightly
         * faster.
         * 되도록이면 zImage 는 RAM 의 첫번째 128m 에 위치시키되, 32m 넘는 곳에 위치시켜라.
         * 그러면 쓸데없는 재배치과정이 없어져서 booting 속도가 약간 빨라진다.
         -------------------------------------------------------------------*/

        /*!!C-----------------------------------------------------------------
         * ac:  e1a0000f   mov  r0, pc
         -------------------------------------------------------------------*/
		mov	r0, pc
		cmp	r0, r4

        /*!!C-----------------------------------------------------------------
         * r0 < r4 이면 ldrcc 가 수행된다.
         * (carry flag = 0)
         * r0 가 r4 보다 크다는 얘기는 현재 수행 코드 위치가
         * 압축해제된 image 가 놓일 위치를 넘어설 수 있다는 의미임.
         * 아래의 ldrcc 에서 blcs 까지는 넘어서지 않았을 때만 수행됨.
         -------------------------------------------------------------------*/
		ldrcc	r0, LC0+32      /*!!C _end - restart + 16384 + 1024*1024 */
		                        /*!!C  = zImage size + page directory size + dtb  */
		addcc	r0, r0, pc
		cmpcc	r4, r0
        /*!!C-----------------------------------------------------------------
         * r4 < r0 이면 orrcc 가 수행된다.
         * (carry flag = 0)
         -------------------------------------------------------------------*/
		orrcc	r4, r4, #1		@ remember we skipped cache_on

        /*!!C-----------------------------------------------------------------
         * c4:  2b00008d   blcs  0x300
         -------------------------------------------------------------------*/
		blcs	cache_on

restart:	adr	r0, LC0
		ldmia	r0, {r1, r2, r3, r6, r10, r11, r12}
		ldr	sp, [r0, #28]

		/*
		 * We might be running at a different address.  We need
		 * to fix up various pointers.
		 */
		sub	r0, r0, r1		@ calculate the delta offset
		add	r6, r6, r0		@ _edata
		add	r10, r10, r0		@ inflated kernel size location

		/*
		 * The kernel build system appends the size of the
		 * decompressed kernel at the end of the compressed data
		 * in little-endian form.
		 */
		ldrb	r9, [r10, #0]
		ldrb	lr, [r10, #1]
		orr	r9, r9, lr, lsl #8
		ldrb	lr, [r10, #2]
		ldrb	r10, [r10, #3]
		orr	r9, r9, lr, lsl #16
		orr	r9, r9, r10, lsl #24

#ifndef CONFIG_ZBOOT_ROM
		/* malloc space is above the relocated stack (64k max) */
		add	sp, sp, r0
		add	r10, sp, #0x10000
#else
		/*
		 * With ZBOOT_ROM the bss/stack is non relocatable,
		 * but someone could still run this code from RAM,
		 * in which case our reference is _edata.
		 */
		mov	r10, r6
#endif

		mov	r5, #0			@ init dtb size to 0
#ifdef CONFIG_ARM_APPENDED_DTB
/*
 *   r0  = delta
 *   r2  = BSS start
 *   r3  = BSS end
 *   r4  = final kernel address (possibly with LSB set)
 *   r5  = appended dtb size (still unknown)
 *   r6  = _edata
 *   r7  = architecture ID
 *   r8  = atags/device tree pointer
 *   r9  = size of decompressed image
 *   r10 = end of this image, including  bss/stack/malloc space if non XIP
 *   r11 = GOT start
 *   r12 = GOT end
 *   sp  = stack pointer
 *
 * if there are device trees (dtb) appended to zImage, advance r10 so that the
 * dtb data will get relocated along with the kernel if necessary.
 */

		ldr	lr, [r6, #0]
#ifndef __ARMEB__
		ldr	r1, =0xedfe0dd0		@ sig is 0xd00dfeed big endian
#else
		ldr	r1, =0xd00dfeed
#endif
		cmp	lr, r1
		bne	dtb_check_done		@ not found

#ifdef CONFIG_ARM_ATAG_DTB_COMPAT
		/*
		 * OK... Let's do some funky business here.
		 * If we do have a DTB appended to zImage, and we do have
		 * an ATAG list around, we want the later to be translated
		 * and folded into the former here.  To be on the safe side,
		 * let's temporarily move  the stack away into the malloc
		 * area.  No GOT fixup has occurred yet, but none of the
		 * code we're about to call uses any global variable.
		*/
		add	sp, sp, #0x10000
		stmfd	sp!, {r0-r3, ip, lr}
		mov	r0, r8
		mov	r1, r6
		sub	r2, sp, r6
		bl	atags_to_fdt

		/*
		 * If returned value is 1, there is no ATAG at the location
		 * pointed by r8.  Try the typical 0x100 offset from start
		 * of RAM and hope for the best.
		 */
		cmp	r0, #1
		sub	r0, r4, #TEXT_OFFSET
		bic	r0, r0, #1
		add	r0, r0, #0x100
		mov	r1, r6
		sub	r2, sp, r6
		bleq	atags_to_fdt

		ldmfd	sp!, {r0-r3, ip, lr}
		sub	sp, sp, #0x10000
#endif

		mov	r8, r6			@ use the appended device tree

		/*
		 * Make sure that the DTB doesn't end up in the final
		 * kernel's .bss area. To do so, we adjust the decompressed
		 * kernel size to compensate if that .bss size is larger
		 * than the relocated code.
		 */
		ldr	r5, =_kernel_bss_size
		adr	r1, wont_overwrite
		sub	r1, r6, r1
		subs	r1, r5, r1
		addhi	r9, r9, r1

		/* Get the dtb's size */
		ldr	r5, [r6, #4]
#ifndef __ARMEB__
		/* convert r5 (dtb size) to little endian */
		eor	r1, r5, r5, ror #16
		bic	r1, r1, #0x00ff0000
		mov	r5, r5, ror #8
		eor	r5, r5, r1, lsr #8
#endif

		/* preserve 64-bit alignment */
		add	r5, r5, #7
		bic	r5, r5, #7

		/* relocate some pointers past the appended dtb */
		add	r6, r6, r5
		add	r10, r10, r5
		add	sp, sp, r5
dtb_check_done:
#endif

/*
 * Check to see if we will overwrite ourselves.
 *   r4  = final kernel address (possibly with LSB set)
 *   r9  = size of decompressed image
 *   r10 = end of this image, including  bss/stack/malloc space if non XIP
 * We basically want:
 *   r4 - 16k page directory >= r10 -> OK
 *   r4 + image length <= address of wont_overwrite -> OK
 * Note: the possible LSB in r4 is harmless here.
 */
		add	r10, r10, #16384
		cmp	r4, r10
		bhs	wont_overwrite
		add	r10, r4, r9
		adr	r9, wont_overwrite
		cmp	r10, r9
		bls	wont_overwrite

/*
 * Relocate ourselves past the end of the decompressed kernel.
 *   r6  = _edata
 *   r10 = end of the decompressed kernel
 * Because we always copy ahead, we need to do it from the end and go
 * backward in case the source and destination overlap.
 */
		/*
		 * Bump to the next 256-byte boundary with the size of
		 * the relocation code added. This avoids overwriting
		 * ourself when the offset is small.
		 */
		add	r10, r10, #((reloc_code_end - restart + 256) & ~255)
		bic	r10, r10, #255

		/* Get start of code we want to copy and align it down. */
		adr	r5, restart
		bic	r5, r5, #31

/* Relocate the hyp vector base if necessary */
#ifdef CONFIG_ARM_VIRT_EXT
		mrs	r0, spsr
		and	r0, r0, #MODE_MASK
		cmp	r0, #HYP_MODE
		bne	1f

		bl	__hyp_get_vectors
		sub	r0, r0, r5
		add	r0, r0, r10
		bl	__hyp_set_vectors
1:
#endif

		sub	r9, r6, r5		@ size to copy
		add	r9, r9, #31		@ rounded up to a multiple
		bic	r9, r9, #31		@ ... of 32 bytes
		add	r6, r9, r5
		add	r9, r9, r10

1:		ldmdb	r6!, {r0 - r3, r10 - r12, lr}
		cmp	r6, r5
		stmdb	r9!, {r0 - r3, r10 - r12, lr}
		bhi	1b

		/* Preserve offset to relocated code. */
		sub	r6, r9, r6

#ifndef CONFIG_ZBOOT_ROM
		/* cache_clean_flush may use the stack, so relocate it */
		add	sp, sp, r6
#endif

		tst	r4, #1
		bleq	cache_clean_flush

		adr	r0, BSYM(restart)
		add	r0, r0, r6
		mov	pc, r0

wont_overwrite:
/*
 * If delta is zero, we are running at the address we were linked at.
 *   r0  = delta
 *   r2  = BSS start
 *   r3  = BSS end
 *   r4  = kernel execution address (possibly with LSB set)
 *   r5  = appended dtb size (0 if not present)
 *   r7  = architecture ID
 *   r8  = atags pointer
 *   r11 = GOT start
 *   r12 = GOT end
 *   sp  = stack pointer
 */
		orrs	r1, r0, r5
		beq	not_relocated

		add	r11, r11, r0
		add	r12, r12, r0

#ifndef CONFIG_ZBOOT_ROM
		/*
		 * If we're running fully PIC === CONFIG_ZBOOT_ROM = n,
		 * we need to fix up pointers into the BSS region.
		 * Note that the stack pointer has already been fixed up.
		 */
		add	r2, r2, r0
		add	r3, r3, r0

		/*
		 * Relocate all entries in the GOT table.
		 * Bump bss entries to _edata + dtb size
		 */
1:		ldr	r1, [r11, #0]		@ relocate entries in the GOT
		add	r1, r1, r0		@ This fixes up C references
		cmp	r1, r2			@ if entry >= bss_start &&
		cmphs	r3, r1			@       bss_end > entry
		addhi	r1, r1, r5		@    entry += dtb size
		str	r1, [r11], #4		@ next entry
		cmp	r11, r12
		blo	1b

		/* bump our bss pointers too */
		add	r2, r2, r5
		add	r3, r3, r5

#else

		/*
		 * Relocate entries in the GOT table.  We only relocate
		 * the entries that are outside the (relocated) BSS region.
		 */
1:		ldr	r1, [r11, #0]		@ relocate entries in the GOT
		cmp	r1, r2			@ entry < bss_start ||
		cmphs	r3, r1			@ _end < entry
		addlo	r1, r1, r0		@ table.  This fixes up the
		str	r1, [r11], #4		@ C references.
		cmp	r11, r12
		blo	1b
#endif

not_relocated:	mov	r0, #0
1:		str	r0, [r2], #4		@ clear bss
		str	r0, [r2], #4
		str	r0, [r2], #4
		str	r0, [r2], #4
		cmp	r2, r3
		blo	1b

		/*
		 * Did we skip the cache setup earlier?
		 * That is indicated by the LSB in r4.
		 * Do it now if so.
		 */
		tst	r4, #1
		bic	r4, r4, #1
		blne	cache_on

/*
 * The C runtime environment should now be setup sufficiently.
 * Set up some pointers, and start decompressing.
 *   r4  = kernel execution address
 *   r7  = architecture ID
 *   r8  = atags pointer
 */
		mov	r0, r4
		mov	r1, sp			@ malloc space above stack
		add	r2, sp, #0x10000	@ 64k max
		mov	r3, r7
		bl	decompress_kernel
		bl	cache_clean_flush
		bl	cache_off
		mov	r1, r7			@ restore architecture number
		mov	r2, r8			@ restore atags pointer

#ifdef CONFIG_ARM_VIRT_EXT
		mrs	r0, spsr		@ Get saved CPU boot mode
		and	r0, r0, #MODE_MASK
		cmp	r0, #HYP_MODE		@ if not booted in HYP mode...
		bne	__enter_kernel		@ boot kernel directly

		adr	r12, .L__hyp_reentry_vectors_offset
		ldr	r0, [r12]
		add	r0, r0, r12

		bl	__hyp_set_vectors
		__HVC(0)			@ otherwise bounce to hyp mode

		b	.			@ should never be reached

		.align	2
.L__hyp_reentry_vectors_offset:	.long	__hyp_reentry_vectors - .
#else
		b	__enter_kernel
#endif

		.align	2
		.type	LC0, #object
LC0:		.word	LC0			@ r1
		.word	__bss_start		@ r2
		.word	_end			@ r3
		.word	_edata			@ r6
		.word	input_data_end - 4	@ r10 (inflated size location)
		.word	_got_start		@ r11
		.word	_got_end		@ ip
		.word	.L_user_stack_end	@ sp
		.word	_end - restart + 16384 + 1024*1024
		.size	LC0, . - LC0

#ifdef CONFIG_ARCH_RPC
		.globl	params
params:		ldr	r0, =0x10000100		@ params_phys for RPC
		mov	pc, lr
		.ltorg
		.align
#endif

/*
 * Turn on the cache.  We need to setup some page tables so that we
 * can have both the I and D caches on.
 *
 * We place the page tables 16k down from the kernel execution address,
 * and we hope that nothing else is using it.  If we're using it, we
 * will go pop!
 *
 * On entry,
 *  r4 = kernel execution address
 *  r7 = architecture number
 *  r8 = atags pointer
 * On exit,
 *  r0, r1, r2, r3, r9, r10, r12 corrupted
 * This routine must preserve:
 *  r4, r7, r8
 */
		.align	5

        /*!!C-----------------------------------------------------------------
         * 300:  e3a03008  mov r3, #8
         -------------------------------------------------------------------*/
cache_on:	mov	r3, #8			@ cache_on function

        /*!!C-----------------------------------------------------------------
         * 304: ea00088   b  0x52c
         -------------------------------------------------------------------*/
		b	call_cache_fn

/*
 * Initialize the highest priority protection region, PR7
 * to cover all 32bit address and cacheable and bufferable.
 */
__armv4_mpu_cache_on:
		mov	r0, #0x3f		@ 4G, the whole
		mcr	p15, 0, r0, c6, c7, 0	@ PR7 Area Setting
		mcr 	p15, 0, r0, c6, c7, 1

		mov	r0, #0x80		@ PR7
		mcr	p15, 0, r0, c2, c0, 0	@ D-cache on
		mcr	p15, 0, r0, c2, c0, 1	@ I-cache on
		mcr	p15, 0, r0, c3, c0, 0	@ write-buffer on

		mov	r0, #0xc000
		mcr	p15, 0, r0, c5, c0, 1	@ I-access permission
		mcr	p15, 0, r0, c5, c0, 0	@ D-access permission

		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c7, c5, 0	@ flush(inval) I-Cache
		mcr	p15, 0, r0, c7, c6, 0	@ flush(inval) D-Cache
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
						@ ...I .... ..D. WC.M
		orr	r0, r0, #0x002d		@ .... .... ..1. 11.1
		orr	r0, r0, #0x1000		@ ...1 .... .... ....

		mcr	p15, 0, r0, c1, c0, 0	@ write control reg

		mov	r0, #0
		mcr	p15, 0, r0, c7, c5, 0	@ flush(inval) I-Cache
		mcr	p15, 0, r0, c7, c6, 0	@ flush(inval) D-Cache
		mov	pc, lr

__armv3_mpu_cache_on:
		mov	r0, #0x3f		@ 4G, the whole
		mcr	p15, 0, r0, c6, c7, 0	@ PR7 Area Setting

		mov	r0, #0x80		@ PR7
		mcr	p15, 0, r0, c2, c0, 0	@ cache on
		mcr	p15, 0, r0, c3, c0, 0	@ write-buffer on

		mov	r0, #0xc000
		mcr	p15, 0, r0, c5, c0, 0	@ access permission

		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		/*
		 * ?? ARMv3 MMU does not allow reading the control register,
		 * does this really work on ARMv3 MPU?
		 */
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
						@ .... .... .... WC.M
		orr	r0, r0, #0x000d		@ .... .... .... 11.1
		/* ?? this overwrites the value constructed above? */
		mov	r0, #0
		mcr	p15, 0, r0, c1, c0, 0	@ write control reg

		/* ?? invalidate for the second time? */
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

#ifdef CONFIG_CPU_DCACHE_WRITETHROUGH
#define CB_BITS 0x08
#else
#define CB_BITS 0x0c
#endif

/*!!C-----------------------------------------------------------------
 *
 * www.iamroot.org/xe/Kernel_10_ARM/176798
 * recipes.egloos.com/5232056
 * 위의 url 에서 MMU 의 page table 공부부터 필요
 *
 *   Page Table
 *
 *                 Level 1                      Level 2 
 *   TTBR -> +------------------+
 *           | Section Entry  10|-------------------------> Section Page(1m)
 *           +------------------+
 *           | Coarse Page    01|---> Coarse Page Table --> Large(64k), Small(4k) Page
 *           +------------------+
 *           | Fine Page      11|---> Fine Page Table  ---> Large(64k), Small(4k), Tiny(1k) Page
 *           +------------------+
 *           | Fault          00|---> Invalid
 *           +------------------+
 *           |      ...         |
 *           +------------------+
 *                4096 entry
 *          (4 byte * 4096 = 16k)
 *
 *   Level 1 Page Table Entry 의 마지막 2 비트를 보고 해당 Entry 가 
 *   section 또는 large, small page 중 어떤 것을 가리킬지 결정한다.
 -------------------------------------------------------------------*/

/*!!C-----------------------------------------------------------------
 * r4 는 xxx + TEXT_OFFSET + 1 에 해당하는 값이 담겨있음.
 * (압축풀린 image 가 놓일 곳)
 -------------------------------------------------------------------*/

__setup_mmu:	sub	r3, r4, #16384		@ Page directory size

        /*!!Q-----------------------------------------------------------------
         * 왜 아래의 bit 들을 clear 할까 ?
         * -> page directory 시작주소를 16k 로 align 하여 r3 에 넣음.
         *    (0x8xxx) 에서 xxx 에 해당하는 것은 짤라버림.
         -------------------------------------------------------------------*/
		bic	r3, r3, #0xff		@ Align the pointer
		bic	r3, r3, #0x3f00
/*
 * Initialise the page tables, turning on the cacheable and bufferable
 * bits for the RAM area only.
 */
		mov	r0, r3

        /**!C-----------------------------------------------------------------
         * 18 bit = 256 KB
         * 18 bit 를 clear 해버림.
         * page directory 의 시작주소에서 하위 18 bit 를 짤라낸다는건
         * 128m 의 경계 첫 주소를 나타냄.
         * 주로 RAM 의 첫번째 128m 구역 안에 zImage 를 올리므로
         * 이는 start of RAM 이 된다.
         *
         * r9  = start of RAM
         * r10 = start of RAM + 256m
         -------------------------------------------------------------------*/
		mov	r9, r0, lsr #18
		mov	r9, r9, lsl #18		@ start of RAM
		add	r10, r9, #0x10000000	@ a reasonable RAM size. 256 MB

        /*!!C-----------------------------------------------------------------
         * XN (Excute Never)
         *    When the XN bit is 1, a Permission fault is generated
         *    if the processor attempts to execute an instruction fetched
         *    from the corresponding memory region.
         *
         * U (allow write to Update the cache)
         -------------------------------------------------------------------*/
		mov	r1, #0x12		@ XN|U + section mapping  1 0010

        /*!!C-----------------------------------------------------------------
         * AP = Access Permission
         * 11= read/write
         *
         * r1 = 1100 0001 0010 (0xC12)
         *      ^^      ^   ^
         *      ||      |   |
         *      AP     XN | U
         -------------------------------------------------------------------*/
		orr	r1, r1, #3 << 10	@ AP=11      1100 0000 0000(0xC00)

        /*!!C-----------------------------------------------------------------
         * page directory start + 16 K
         -------------------------------------------------------------------*/
		add	r2, r3, #16384

        /*!!C-----------------------------------------------------------------
         * 현재 시점에서 register 값 
         *
         *  r1 = 0xC12
         *  r4 = xxx + TEXT_OFFSET + 1  : start of uncompressed image + 1
         *  r3 = (r4 - 16k) & ~(0x3fff) : start of page directory 
         *  r2 = r3 + 16k
         *  r0 = r3 & ~(low 18bit)      : temporary
         *  r7, r8 = architecture id, atags
         *  r9 = start of RAM (128m 경계의 시작)
         *  r10 = start of RAM + 256m
         *
         *  +--------------+-> r10 = +0x10000000 (256m)
         *  |              |
         *  |              |
         *  |              |
         *  |              |-> r4 = +0x8001 (32k + 1)
         *  |              |-> r2 = +0x8000 (32k)
         *  |              |
         *  |              |-> r3 = +0x4000 (16k)
         *  |              |
         *  |              |-> r1 = +0xC12
         *  +--------------+-> r9 = +0
         *
         -------------------------------------------------------------------*/
1:		cmp	r1, r9			@ if virt > start of RAM
		cmphs	r10, r1			@   && end of RAM > virt

        /*!!C-----------------------------------------------------------------
         * U 와 AP(Access Permission) 만 남김 (bit clear)
         * before r1 = 1100 0001 0010 (0xC02)
         * after  r1 = 1100 0000 0010 (0xC02)
         -------------------------------------------------------------------*/
		bic	r1, r1, #0x1c		@ clear XN|U + C + B   0x1c(11100)

        /*!!C-----------------------------------------------------------------
         * 아래의 orrlo 와 orrhs 는 위의 cmphs 결과로 수행 
         * r6에는 __setup_mmu 호출 전에 #CB_BITS | 0x02 가 들어있었다.
         -------------------------------------------------------------------*/
		orrlo	r1, r1, #0x10		@ Set XN|U for non-RAM  1 0000

        /*!!C-----------------------------------------------------------------
         * #define CB_BITS 에 의해 0x08 아니면 0x0C
         * 0x08 이면 섹션테이블의 C=1, B=0 으로 세팅, 0x0C 이면 C=1, B=1 로 세팅.
         *
         *   C B
         *   0 0  : Cache Off, Write Buffer Off
         *   1 0  : Cache On(Write Through), Write Buffer On
         *   1 1  : Cache On(Write Back), Write Buffer On
         -------------------------------------------------------------------*/
		orrhs	r1, r1, r6		@ set RAM section settings    1110

        /*!!C-----------------------------------------------------------------
         * *r0++ = r1
         -------------------------------------------------------------------*/
		str	r1, [r0], #4		@ 1:1 mapping

        /*!!C-----------------------------------------------------------------
         * page directory 의 하나의 entry 는
         * 1 MB 를 가리킬 수 있으므로.
         * 그래서, 4096 * 1 MB = 4 GB
         -------------------------------------------------------------------*/
		add	r1, r1, #1048576    @ 1 MB

        /*!!C-----------------------------------------------------------------
         * 16 K 만큼 될 때까지 반복 
         * (page table 초기화)
         -------------------------------------------------------------------*/
		teq	r0, r2
		bne	1b

        /*!!C-----------------------------------------------------------------
         * 여기까지가 page directory 초기화 완료 
         -------------------------------------------------------------------*/
/*
 * If ever we are running from Flash, then we surely want the cache
 * to be enabled also for our execution instance...  We map 2MB of it
 * so there is no map overlap problem for up to 1 MB compressed kernel.
 * If the execution is in RAM then we would only be duplicating the above.
 */
        /*!!C-----------------------------------------------------------------
         * 1110 | 0100
         * B = Bufferable
         -------------------------------------------------------------------*/
		orr	r1, r6, #0x04		@ ensure B is set for this

        /*!!C-----------------------------------------------------------------
         * AP Setting
         -------------------------------------------------------------------*/
		orr	r1, r1, #3 << 10
		mov	r2, pc      /*!!C  (1) */

        /*!!C-----------------------------------------------------------------
         * currently r1 = 1100 0000 1110 (0xC0E)
         -------------------------------------------------------------------*/

        /*!!C-----------------------------------------------------------------
         * 20 bit(1 MB) clear
         -------------------------------------------------------------------*/
		mov	r2, r2, lsr #20

        /*!!C-----------------------------------------------------------------
         * r1 | (1) 에서 구한 pc의 상위 12 bit
         -------------------------------------------------------------------*/
		orr	r1, r1, r2, lsl #20

        /*!!C-----------------------------------------------------------------
         * 여기에서 r1 의 상태는? r1 = 12 bit pc + 0000 0000 + r1(1100 0000 1110)
         * 20140614
         -------------------------------------------------------------------*/

		add	r0, r3, r2, lsl #2
		str	r1, [r0], #4
		add	r1, r1, #1048576
		str	r1, [r0]
		mov	pc, lr
ENDPROC(__setup_mmu)

@ Enable unaligned access on v6, to allow better code generation
@ for the decompressor C code:
__armv6_mmu_cache_on:
		mrc	p15, 0, r0, c1, c0, 0	@ read SCTLR
		bic	r0, r0, #2		@ A (no unaligned access fault)
		orr	r0, r0, #1 << 22	@ U (v6 unaligned access model)
		mcr	p15, 0, r0, c1, c0, 0	@ write SCTLR
		b	__armv4_mmu_cache_on

__arm926ejs_mmu_cache_on:
#ifdef CONFIG_CPU_DCACHE_WRITETHROUGH
		mov	r0, #4			@ put dcache in WT mode
		mcr	p15, 7, r0, c15, c0, 0
#endif

__armv4_mmu_cache_on:
		mov	r12, lr
#ifdef CONFIG_MMU
		mov	r6, #CB_BITS | 0x12	@ U
		bl	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		orr	r0, r0, #0x5000		@ I-cache enable, RR cache replacement
		orr	r0, r0, #0x0030
#ifdef CONFIG_CPU_ENDIAN_BE8
		orr	r0, r0, #1 << 25	@ big-endian page tables
#endif
		bl	__common_mmu_cache_on
		mov	r0, #0
		mcr	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
#endif
		mov	pc, r12


/*!!C-----------------------------------------------------------------
 * 458:  e1a0c00e   mov  r12, lr
 -------------------------------------------------------------------*/
__armv7_mmu_cache_on:
		mov	r12, lr

#ifdef CONFIG_MMU
        /*!!C-----------------------------------------------------------------
         * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.ddi0363g/Bgbfhfjc.html
         *
         * The ID_MMFR0 provides information about the memory model,
         * memory management, and cache support operations of the processor
         *
         * MMFR(Memory Model Feature Register)
         * A15 TRM(Technical Reference Manual) 참조 
         -------------------------------------------------------------------*/
		mrc	p15, 0, r11, c0, c1, 4	@ read ID_MMFR0

        /*!!C-----------------------------------------------------------------
         * VMSA(Virtual Memory System Architecture)
         * config_MMU 가 설정되어 있다면 VMSA 일 것 같은데 ?
         * -> config_MMU 가 설정되어 있던 아니던 cp15 로
         *    좀더 정확한 체크를 위해 하는 것 같다.
         -------------------------------------------------------------------*/
		tst	r11, #0xf		@ VMSA

        /*!!C-----------------------------------------------------------------
         * r6 = 1110 (0xE)
         * VMSA 일 경우에만 아래 두줄 수행 
         * CB_BITS (Bufferable Flag) - Cache 정책이 Write Back 일 경우 
         -------------------------------------------------------------------*/
		movne	r6, #CB_BITS | 0x02	@ !XN
		blne	__setup_mmu  /*!!C 커널 압축하고 해제하고 실행하는데 필요한 page directory 설정 */

		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		tst	r11, #0xf		@ VMSA
		mcrne	p15, 0, r0, c8, c7, 0	@ flush I,D TLBs
#endif
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		bic	r0, r0, #1 << 28	@ clear SCTLR.TRE
		orr	r0, r0, #0x5000		@ I-cache enable, RR cache replacement
		orr	r0, r0, #0x003c		@ write buffer
		bic	r0, r0, #2		@ A (no unaligned access fault)
		orr	r0, r0, #1 << 22	@ U (v6 unaligned access model)
						@ (needed for ARM1176)
#ifdef CONFIG_MMU
#ifdef CONFIG_CPU_ENDIAN_BE8
		orr	r0, r0, #1 << 25	@ big-endian page tables
#endif
		mrcne   p15, 0, r6, c2, c0, 2   @ read ttb control reg
		orrne	r0, r0, #1		@ MMU enabled
		movne	r1, #0xfffffffd		@ domain 0 = client
		bic     r6, r6, #1 << 31        @ 32-bit translation system
		bic     r6, r6, #3 << 0         @ use only ttbr0
		mcrne	p15, 0, r3, c2, c0, 0	@ load page table pointer
		mcrne	p15, 0, r1, c3, c0, 0	@ load domain access control
		mcrne   p15, 0, r6, c2, c0, 2   @ load ttb control
#endif
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
		mcr	p15, 0, r0, c1, c0, 0	@ load control register
		mrc	p15, 0, r0, c1, c0, 0	@ and read it back
		mov	r0, #0
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
		mov	pc, r12

__fa526_cache_on:
		mov	r12, lr
		mov	r6, #CB_BITS | 0x12	@ U
		bl	__setup_mmu
		mov	r0, #0
		mcr	p15, 0, r0, c7, c7, 0	@ Invalidate whole cache
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c8, c7, 0	@ flush UTLB
		mrc	p15, 0, r0, c1, c0, 0	@ read control reg
		orr	r0, r0, #0x1000		@ I-cache enable
		bl	__common_mmu_cache_on
		mov	r0, #0
		mcr	p15, 0, r0, c8, c7, 0	@ flush UTLB
		mov	pc, r12

__common_mmu_cache_on:
#ifndef CONFIG_THUMB2_KERNEL
#ifndef DEBUG
		orr	r0, r0, #0x000d		@ Write buffer, mmu
#endif
		mov	r1, #-1
		mcr	p15, 0, r3, c2, c0, 0	@ load page table pointer
		mcr	p15, 0, r1, c3, c0, 0	@ load domain access control
		b	1f
		.align	5			@ cache line aligned
1:		mcr	p15, 0, r0, c1, c0, 0	@ load control register
		mrc	p15, 0, r0, c1, c0, 0	@ and read it back to
		sub	pc, lr, r0, lsr #32	@ properly flush pipeline
#endif

#define PROC_ENTRY_SIZE (4*5)

/*
 * Here follow the relocatable cache support functions for the
 * various processors.  This is a generic hook for locating an
 * entry and jumping to an instruction at the specified offset
 * from the start of the block.  Please note this is all position
 * independent code.
 *
 *  r1  = corrupted
 *  r2  = corrupted
 *  r3  = block offset
 *  r9  = corrupted
 *  r12 = corrupted
 */

/*!!C-----------------------------------------------------------------
 * adr 매크로는 proc_types(label)의 주소값을
 * r12 에 넣어준다.
 -------------------------------------------------------------------*/

/*!!C-----------------------------------------------------------------
 * 52c:  e28fc01c   ip, pc, #28
 -------------------------------------------------------------------*/
call_cache_fn:	adr	r12, proc_types

#ifdef CONFIG_CPU_CP15
		mrc	p15, 0, r9, c0, c0	@ get processor ID
#else
		ldr	r9, =CONFIG_PROCESSOR_ID
#endif
        /*!!C-----------------------------------------------------------------
         * 534:  e59c1000   ldr r1, [ip]
         -------------------------------------------------------------------*/
1:		ldr	r1, [r12, #0]		@ get value
		ldr	r2, [r12, #4]		@ get mask
		eor	r1, r1, r9		@ (real ^ match)
		tst	r1, r2			@       & mask

        /*!!C-----------------------------------------------------------------
         * processor ID 와 r12 에서 가져온 value 가 맞을 경우
         * cache on 함수를 수행하기 위해 new CPU Id 쪽으로 이동 
         -------------------------------------------------------------------*/
 ARM(		addeq	pc, r12, r3		) @ call cache function
 THUMB(		addeq	r12, r3			)
 THUMB(		moveq	pc, r12			) @ call cache function

        /*!!C-----------------------------------------------------------------
         * processor ID 와 r12 에서 가져온 value 가 맞지 않을 경우 
         * r12 + 20 씩 뛰면서 다음 value 를 가져오도록 한다.
         -------------------------------------------------------------------*/

        /*!!C-----------------------------------------------------------------
         * 548:  e28cc014   add ip, ip, #20
         -------------------------------------------------------------------*/
		add	r12, r12, #PROC_ENTRY_SIZE

        /*!!C-----------------------------------------------------------------
         * 54c:  eafffff8   b  0x534
         -------------------------------------------------------------------*/
		b	1b

/*
 * Table for cache operations.  This is basically:
 *   - CPU ID match
 *   - CPU ID mask
 *   - 'cache on' method instruction
 *   - 'cache off' method instruction
 *   - 'cache flush' method instruction
 *
 * We match an entry using: ((real_id ^ match) & mask) == 0
 *
 * Writethrough caches generally only need 'on' and 'off'
 * methods.  Writeback caches _must_ have the flush method
 * defined.
 */
		.align	2
		.type	proc_types,#object
proc_types:

        /*!!C-----------------------------------------------------------------
         * 550:  41000000
         -------------------------------------------------------------------*/
		.word	0x41000000		@ old ARM ID
		.word	0xff00f000
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41007000		@ ARM7/710
		.word	0xfff8fe00
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41807200		@ ARM720T (writethrough)
		.word	0xffffff00
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		mov	pc, lr
 THUMB(		nop				)

		.word	0x41007400		@ ARM74x
		.word	0xff00ff00
		W(b)	__armv3_mpu_cache_on
		W(b)	__armv3_mpu_cache_off
		W(b)	__armv3_mpu_cache_flush
		
		.word	0x41009400		@ ARM94x
		.word	0xff00ff00
		W(b)	__armv4_mpu_cache_on
		W(b)	__armv4_mpu_cache_off
		W(b)	__armv4_mpu_cache_flush

		.word	0x41069260		@ ARM926EJ-S (v5TEJ)
		.word	0xff0ffff0
		W(b)	__arm926ejs_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x00007000		@ ARM7 IDs
		.word	0x0000f000
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		@ Everything from here on will be the new ID system.

		.word	0x4401a100		@ sa110 / sa1100
		.word	0xffffffe0
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x6901b110		@ sa1110
		.word	0xfffffff0
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x56056900
		.word	0xffffff00		@ PXA9xx
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x56158000		@ PXA168
		.word	0xfffff000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x56050000		@ Feroceon
		.word	0xff0f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

#ifdef CONFIG_CPU_FEROCEON_OLD_ID
		/* this conflicts with the standard ARMv5TE entry */
		.long	0x41009260		@ Old Feroceon
		.long	0xff00fff0
		b	__armv4_mmu_cache_on
		b	__armv4_mmu_cache_off
		b	__armv5tej_mmu_cache_flush
#endif

		.word	0x66015261		@ FA526
		.word	0xff01fff1
		W(b)	__fa526_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__fa526_cache_flush

		@ These match on the architecture ID

		.word	0x00020000		@ ARMv4T
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x00050000		@ ARMv5TE
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv4_mmu_cache_flush

		.word	0x00060000		@ ARMv5TEJ
		.word	0x000f0000
		W(b)	__armv4_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv5tej_mmu_cache_flush

		.word	0x0007b000		@ ARMv6
		.word	0x000ff000
		W(b)	__armv6_mmu_cache_on
		W(b)	__armv4_mmu_cache_off
		W(b)	__armv6_mmu_cache_flush

        /*!!C-----------------------------------------------------------------
         * 6a4:  000f0000
         -------------------------------------------------------------------*/
		.word	0x000f0000		@ new CPU Id
		.word	0x000f0000

        /*!!C-----------------------------------------------------------------
         * W 의 의미 
         * assembler 로 하여금 32 bit encoding 을 하도록 강제함.
         * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0204j/ch04s11s01.html
         -------------------------------------------------------------------*/

        /*!!C-----------------------------------------------------------------
         * 6ac:  eaffff69   b  0x458
         * 6b0:  ea000021   b  0x73c
         * 6b4:  ea00004c   b  0x7ec
         -------------------------------------------------------------------*/
		W(b)	__armv7_mmu_cache_on
		W(b)	__armv7_mmu_cache_off
		W(b)	__armv7_mmu_cache_flush

		.word	0			@ unrecognised type
		.word	0
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)
		mov	pc, lr
 THUMB(		nop				)

		.size	proc_types, . - proc_types

		/*
		 * If you get a "non-constant expression in ".if" statement"
		 * error from the assembler on this line, check that you have
		 * not accidentally written a "b" instruction where you should
		 * have written W(b).
		 */
		.if (. - proc_types) % PROC_ENTRY_SIZE != 0
		.error "The size of one or more proc_types entries is wrong."
		.endif

/*
 * Turn off the Cache and MMU.  ARMv3 does not support
 * reading the control register, but ARMv4 does.
 *
 * On exit,
 *  r0, r1, r2, r3, r9, r12 corrupted
 * This routine must preserve:
 *  r4, r7, r8
 */
		.align	5
cache_off:	mov	r3, #12			@ cache_off function
		b	call_cache_fn

__armv4_mpu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0	@ turn MPU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c10, 4	@ drain write buffer
		mcr	p15, 0, r0, c7, c6, 0	@ flush D-Cache
		mcr	p15, 0, r0, c7, c5, 0	@ flush I-Cache
		mov	pc, lr

__armv3_mpu_cache_off:
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0, 0	@ turn MPU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

__armv4_mmu_cache_off:
#ifdef CONFIG_MMU
		mrc	p15, 0, r0, c1, c0
		bic	r0, r0, #0x000d
		mcr	p15, 0, r0, c1, c0	@ turn MMU and cache off
		mov	r0, #0
		mcr	p15, 0, r0, c7, c7	@ invalidate whole cache v4
		mcr	p15, 0, r0, c8, c7	@ invalidate whole TLB v4
#endif
		mov	pc, lr

__armv7_mmu_cache_off:
		mrc	p15, 0, r0, c1, c0
#ifdef CONFIG_MMU
		bic	r0, r0, #0x000d
#else
		bic	r0, r0, #0x000c
#endif
		mcr	p15, 0, r0, c1, c0	@ turn MMU and cache off
		mov	r12, lr
		bl	__armv7_mmu_cache_flush
		mov	r0, #0
#ifdef CONFIG_MMU
		mcr	p15, 0, r0, c8, c7, 0	@ invalidate whole TLB
#endif
		mcr	p15, 0, r0, c7, c5, 6	@ invalidate BTC
		mcr	p15, 0, r0, c7, c10, 4	@ DSB
		mcr	p15, 0, r0, c7, c5, 4	@ ISB
		mov	pc, r12

/*
 * Clean and flush the cache to maintain consistency.
 *
 * On exit,
 *  r1, r2, r3, r9, r10, r11, r12 corrupted
 * This routine must preserve:
 *  r4, r6, r7, r8
 */
		.align	5
cache_clean_flush:
		mov	r3, #16
		b	call_cache_fn

__armv4_mpu_cache_flush:
		mov	r2, #1
		mov	r3, #0
		mcr	p15, 0, ip, c7, c6, 0	@ invalidate D cache
		mov	r1, #7 << 5		@ 8 segments
1:		orr	r3, r1, #63 << 26	@ 64 entries
2:		mcr	p15, 0, r3, c7, c14, 2	@ clean & invalidate D index
		subs	r3, r3, #1 << 26
		bcs	2b			@ entries 63 to 0
		subs 	r1, r1, #1 << 5
		bcs	1b			@ segments 7 to 0

		teq	r2, #0
		mcrne	p15, 0, ip, c7, c5, 0	@ invalidate I cache
		mcr	p15, 0, ip, c7, c10, 4	@ drain WB
		mov	pc, lr
		
__fa526_cache_flush:
		mov	r1, #0
		mcr	p15, 0, r1, c7, c14, 0	@ clean and invalidate D cache
		mcr	p15, 0, r1, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv6_mmu_cache_flush:
		mov	r1, #0
		mcr	p15, 0, r1, c7, c14, 0	@ clean+invalidate D
		mcr	p15, 0, r1, c7, c5, 0	@ invalidate I+BTB
		mcr	p15, 0, r1, c7, c15, 0	@ clean+invalidate unified
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv7_mmu_cache_flush:
		mrc	p15, 0, r10, c0, c1, 5	@ read ID_MMFR1
		tst	r10, #0xf << 16		@ hierarchical cache (ARMv7)
		mov	r10, #0
		beq	hierarchical
		mcr	p15, 0, r10, c7, c14, 0	@ clean+invalidate D
		b	iflush
hierarchical:
		mcr	p15, 0, r10, c7, c10, 5	@ DMB
		stmfd	sp!, {r0-r7, r9-r11}
		mrc	p15, 1, r0, c0, c0, 1	@ read clidr
		ands	r3, r0, #0x7000000	@ extract loc from clidr
		mov	r3, r3, lsr #23		@ left align loc bit field
		beq	finished		@ if loc is 0, then no need to clean
		mov	r10, #0			@ start clean at cache level 0
loop1:
		add	r2, r10, r10, lsr #1	@ work out 3x current cache level
		mov	r1, r0, lsr r2		@ extract cache type bits from clidr
		and	r1, r1, #7		@ mask of the bits for current cache only
		cmp	r1, #2			@ see what cache we have at this level
		blt	skip			@ skip if no cache, or just i-cache
		mcr	p15, 2, r10, c0, c0, 0	@ select current cache level in cssr
		mcr	p15, 0, r10, c7, c5, 4	@ isb to sych the new cssr&csidr
		mrc	p15, 1, r1, c0, c0, 0	@ read the new csidr
		and	r2, r1, #7		@ extract the length of the cache lines
		add	r2, r2, #4		@ add 4 (line length offset)
		ldr	r4, =0x3ff
		ands	r4, r4, r1, lsr #3	@ find maximum number on the way size
		clz	r5, r4			@ find bit position of way size increment
		ldr	r7, =0x7fff
		ands	r7, r7, r1, lsr #13	@ extract max number of the index size
loop2:
		mov	r9, r4			@ create working copy of max way size
loop3:
 ARM(		orr	r11, r10, r9, lsl r5	) @ factor way and cache number into r11
 ARM(		orr	r11, r11, r7, lsl r2	) @ factor index number into r11
 THUMB(		lsl	r6, r9, r5		)
 THUMB(		orr	r11, r10, r6		) @ factor way and cache number into r11
 THUMB(		lsl	r6, r7, r2		)
 THUMB(		orr	r11, r11, r6		) @ factor index number into r11
		mcr	p15, 0, r11, c7, c14, 2	@ clean & invalidate by set/way
		subs	r9, r9, #1		@ decrement the way
		bge	loop3
		subs	r7, r7, #1		@ decrement the index
		bge	loop2
skip:
		add	r10, r10, #2		@ increment cache number
		cmp	r3, r10
		bgt	loop1
finished:
		ldmfd	sp!, {r0-r7, r9-r11}
		mov	r10, #0			@ swith back to cache level 0
		mcr	p15, 2, r10, c0, c0, 0	@ select current cache level in cssr
iflush:
		mcr	p15, 0, r10, c7, c10, 4	@ DSB
		mcr	p15, 0, r10, c7, c5, 0	@ invalidate I+BTB
		mcr	p15, 0, r10, c7, c10, 4	@ DSB
		mcr	p15, 0, r10, c7, c5, 4	@ ISB
		mov	pc, lr

__armv5tej_mmu_cache_flush:
1:		mrc	p15, 0, r15, c7, c14, 3	@ test,clean,invalidate D cache
		bne	1b
		mcr	p15, 0, r0, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r0, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv4_mmu_cache_flush:
		mov	r2, #64*1024		@ default: 32K dcache size (*2)
		mov	r11, #32		@ default: 32 byte line size
		mrc	p15, 0, r3, c0, c0, 1	@ read cache type
		teq	r3, r9			@ cache ID register present?
		beq	no_cache_id
		mov	r1, r3, lsr #18
		and	r1, r1, #7
		mov	r2, #1024
		mov	r2, r2, lsl r1		@ base dcache size *2
		tst	r3, #1 << 14		@ test M bit
		addne	r2, r2, r2, lsr #1	@ +1/2 size if M == 1
		mov	r3, r3, lsr #12
		and	r3, r3, #3
		mov	r11, #8
		mov	r11, r11, lsl r3	@ cache line size in bytes
no_cache_id:
		mov	r1, pc
		bic	r1, r1, #63		@ align to longest cache line
		add	r2, r1, r2
1:
 ARM(		ldr	r3, [r1], r11		) @ s/w flush D cache
 THUMB(		ldr     r3, [r1]		) @ s/w flush D cache
 THUMB(		add     r1, r1, r11		)
		teq	r1, r2
		bne	1b

		mcr	p15, 0, r1, c7, c5, 0	@ flush I cache
		mcr	p15, 0, r1, c7, c6, 0	@ flush D cache
		mcr	p15, 0, r1, c7, c10, 4	@ drain WB
		mov	pc, lr

__armv3_mmu_cache_flush:
__armv3_mpu_cache_flush:
		mov	r1, #0
		mcr	p15, 0, r1, c7, c0, 0	@ invalidate whole cache v3
		mov	pc, lr

/*
 * Various debugging routines for printing hex characters and
 * memory, which again must be relocatable.
 */
#ifdef DEBUG
		.align	2

        /*!!C-----------------------------------------------------------------
         * Mark the symbol as being a data object
         -------------------------------------------------------------------*/
		.type	phexbuf,#object
phexbuf:	.space	12

        /*!!Q-----------------------------------------------------------------
         * 현재위치와 label 사이의 차이.
         * .size 는 symbol table 에 추가적인 정보를 남기기 위해 사용됨.
         *
         * http://web.mit.edu/gnu/doc/html/as_7.html
         * ==> COFF format 에서만 의미있고 ELF Format 에서는 무시됨.
         *
         * 왜 12 byte 를 0 으로 ?
         * ==> type 이 object 니깐 code 영역이 아니라 data 영역을
         *     잡는다 ?
         -------------------------------------------------------------------*/
		.size	phexbuf, . - phexbuf

@ phex corrupts {r0, r1, r2, r3}
        /*!!C-----------------------------------------------------------------
         * r0 = 4 byte, r1 = 8(4 byte = 4 bit x 8)
         -------------------------------------------------------------------*/
phex:		adr	r3, phexbuf
		mov	r2, #0
		strb	r2, [r3, r1]
1:		subs	r1, r1, #1
		movmi	r0, r3      /*!!C move if negative */
		bmi	puts            /*!!C branch if negative */
		and	r2, r0, #15     /*!!C 하위 4 bit -> r2 */
		mov	r0, r0, lsr #4  /*!!C 4 bit 만큼 right shit */
		cmp	r2, #10
		addge	r2, r2, #7  /*!!C '9' 와 'A' 의 차이 = 7 */
		add	r2, r2, #'0'    /*!!C '0' = 0x30 */
		strb	r2, [r3, r1]
		b	1b

/*!!C-----------------------------------------------------------------
 * http://amitshah.bizhat.com/arm/arm_linux_boot-1.html 참고
 -------------------------------------------------------------------*/
@ puts corrupts {r0, r1, r2, r3}
puts:		loadsp	r3, r1
1:		ldrb	r2, [r0], #1
		teq	r2, #0
		moveq	pc, lr
2:		writeb	r2, r3
		mov	r1, #0x00020000 /*!!C 128 K */
3:		subs	r1, r1, #1  /*!!Q 왜 looping 을 돌까? 단순 delay 인 듯... */
		bne	3b
		teq	r2, #'\n'
		moveq	r2, #'\r'
		beq	2b
		teq	r0, #0
		bne	1b
		mov	pc, lr
@ putc corrupts {r0, r1, r2, r3}
putc:
		mov	r2, r0
		mov	r0, #0
		loadsp	r3, r1
		b	2b              /*!!C puts 의 일부 루틴만 사용하기 위해 2 로 분기 */

@ memdump corrupts {r0, r1, r2, r3, r10, r11, r12, lr}
memdump:	mov	r12, r0
		mov	r10, lr
		mov	r11, #0
2:		mov	r0, r11, lsl #2
		add	r0, r0, r12
		mov	r1, #8          /*!!C r0, r1(len=8) */
		bl	phex
		mov	r0, #':'
		bl	putc
1:		mov	r0, #' '
		bl	putc
		ldr	r0, [r12, r11, lsl #2]  /*!!C 4 byte 단위 load */
		mov	r1, #8
		bl	phex
		and	r0, r11, #7
		teq	r0, #3
		moveq	r0, #' '
		bleq	putc
		and	r0, r11, #7
		add	r11, r11, #1
		teq	r0, #7
		bne	1b
		mov	r0, #'\n'
		bl	putc
		cmp	r11, #64        /*!!C 4 x 64 = 256 */
		blt	2b
		mov	pc, r10
#endif

		.ltorg

#ifdef CONFIG_ARM_VIRT_EXT
.align 5
__hyp_reentry_vectors:
		W(b)	.			@ reset
		W(b)	.			@ undef
		W(b)	.			@ svc
		W(b)	.			@ pabort
		W(b)	.			@ dabort
		W(b)	__enter_kernel		@ hyp
		W(b)	.			@ irq
		W(b)	.			@ fiq
#endif /* CONFIG_ARM_VIRT_EXT */

__enter_kernel:
		mov	r0, #0			@ must be 0
 ARM(		mov	pc, r4	)		@ call kernel
 THUMB(		bx	r4	)		@ entry point is always ARM

reloc_code_end:

		.align
		.section ".stack", "aw", %nobits
.L_user_stack:	.space	4096
.L_user_stack_end:
