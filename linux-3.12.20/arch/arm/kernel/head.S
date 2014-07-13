/*
 *  linux/arch/arm/kernel/head.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Kernel startup code for all 32-bit CPUs
 */
#include <linux/linkage.h>
#include <linux/init.h>

#include <asm/assembler.h>
#include <asm/cp15.h>
#include <asm/domain.h>
#include <asm/ptrace.h>
#include <asm/asm-offsets.h>
#include <asm/memory.h>
#include <asm/thread_info.h>
#include <asm/pgtable.h>

#if defined(CONFIG_DEBUG_LL) && !defined(CONFIG_DEBUG_SEMIHOSTING)
#include CONFIG_DEBUG_LL_INCLUDE
#endif

/*
 * swapper_pg_dir is the virtual address of the initial page table.
 * We place the page tables 16K below KERNEL_RAM_VADDR.  Therefore, we must
 * make sure that KERNEL_RAM_VADDR is correctly set.  Currently, we expect
 * the least significant 16 bits to be 0x8000, but we could probably
 * relax this restriction to KERNEL_RAM_VADDR >= PAGE_OFFSET + 0x4000.
 */
/*!!C-----------------------------------------------------------------
 * KERNEL_RAM_VADDR = 0xC0008000
 -------------------------------------------------------------------*/
#define KERNEL_RAM_VADDR	(PAGE_OFFSET + TEXT_OFFSET)
#if (KERNEL_RAM_VADDR & 0xffff) != 0x8000
#error KERNEL_RAM_VADDR must start at 0xXXXX8000
#endif

/*!!C-----------------------------------------------------------------
 * CONFIG_ARM_LPAE = Large Page ...
 * => Page Table Level 2 로는 cover 가 안됨.
 * => 그래서 0x5000 으로 확장
 -------------------------------------------------------------------*/
#ifdef CONFIG_ARM_LPAE
	/* LPAE requires an additional page for the PGD */
#define PG_DIR_SIZE	0x5000
#define PMD_ORDER	3
#else
#define PG_DIR_SIZE	0x4000
#define PMD_ORDER	2
#endif

	.globl	swapper_pg_dir
	.equ	swapper_pg_dir, KERNEL_RAM_VADDR - PG_DIR_SIZE  /*!!C 0xc0004000 = start */

	.macro	pgtbl, rd, phys
	add	\rd, \phys, #TEXT_OFFSET - PG_DIR_SIZE
	.endm

/*
 * Kernel startup entry point.
 * ---------------------------
 *
 * This is normally called from the decompressor code.  The requirements
 * are: MMU = off, D-cache = off, I-cache = dont care, r0 = 0,
 * r1 = machine nr, r2 = atags or dtb pointer.
 *
 * This code is mostly position independent, so if you link the kernel at
 * 0xc0008000, you call this at __pa(0xc0008000).
 *
 * See linux/arch/arm/tools/mach-types for the complete list of machine
 * numbers for r1.
 *
 * We're trying to keep crap to a minimum; DO NOT add any machine specific
 * crap here - that's what the boot loader (or in extreme, well justified
 * circumstances, zImage) is for.
 */
	.arm

	__HEAD

    /*!!C-----------------------------------------------------------------
     * 외부에서 볼 수 있는 symbol
     -------------------------------------------------------------------*/
ENTRY(stext)

 THUMB(	adr	r9, BSYM(1f)	)	@ Kernel is always entered in ARM.
 THUMB(	bx	r9		)	@ If this is a Thumb-2 kernel,
 THUMB(	.thumb			)	@ switch to Thumb now.
 THUMB(1:			)

#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install
#endif
	@ ensure svc mode and all interrupts masked
	safe_svcmode_maskall r9

    /*!!C-----------------------------------------------------------------
     * r1 = architecture no
     * r2 = dtb pointer
     -------------------------------------------------------------------*/
	mrc	p15, 0, r9, c0, c0		@ get processor id

    /*!!C-----------------------------------------------------------------
     * 자신의 cpuid 에 해당하는 procinfo 를 찾음 
     -------------------------------------------------------------------*/
	bl	__lookup_processor_type		@ r5=procinfo r9=cpuid
	movs	r10, r5				@ invalid processor (r5=0)?
 THUMB( it	eq )		@ force fixup-able long branch encoding
	beq	__error_p			@ yes, error 'p'

#ifdef CONFIG_ARM_LPAE
	mrc	p15, 0, r3, c0, c1, 4		@ read ID_MMFR0
	and	r3, r3, #0xf			@ extract VMSA support
	cmp	r3, #5				@ long-descriptor translation table format?
 THUMB( it	lo )				@ force fixup-able long branch encoding
	blo	__error_p			@ only classic page table format
#endif

#ifndef CONFIG_XIP_KERNEL
    /*!!C-----------------------------------------------------------------
     *  2:	.long	.
     *      .long	PAGE_OFFSET
     -------------------------------------------------------------------*/
	adr	r3, 2f
	ldmia	r3, {r4, r8}
	sub	r4, r3, r4			@ (PHYS_OFFSET - PAGE_OFFSET)

    /*!!C-----------------------------------------------------------------
     * physical RAM start address = PHYS_OFFSET
     * = PAGE_OFFSET + offset(r4)
     -------------------------------------------------------------------*/
	add	r8, r8, r4			@ PHYS_OFFSET, page_offset(0xc0000000)
#else
	ldr	r8, =PHYS_OFFSET		@ always constant in this case
#endif

	/*
	 * r1 = machine no, r2 = atags or dtb,
	 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
	 */
	bl	__vet_atags
#ifdef CONFIG_SMP_ON_UP
	bl	__fixup_smp
#endif

    /*!!C-----------------------------------------------------------------
     * http://cateee.net/lkddb/web-lkddb/ARM_PATCH_PHYS_VIRT.html
     * pv_table 이란 무엇인가에 대해 좀더 상세한 복습 필요.
     -------------------------------------------------------------------*/
#ifdef CONFIG_ARM_PATCH_PHYS_VIRT
	bl	__fixup_pv_table
#endif
	bl	__create_page_tables

	/*
	 * The following calls CPU specific code in a position independent
	 * manner.  See arch/arm/mm/proc-*.S for details.  r10 = base of
	 * xxx_proc_info structure selected by __lookup_processor_type
	 * above.  On return, the CPU will be ready for the MMU to be
	 * turned on, and r0 will hold the CPU control register value.
	 */
	ldr	r13, =__mmap_switched		@ address to jump to after
						@ mmu has been enabled
	adr	lr, BSYM(1f)			@ return (PIC) address
	mov	r8, r4				@ set TTBR1 to swapper_pg_dir
 ARM(	add	pc, r10, #PROCINFO_INITFUNC	)
 THUMB(	add	r12, r10, #PROCINFO_INITFUNC	)
 THUMB(	mov	pc, r12				)
1:	b	__enable_mmu
ENDPROC(stext)
	.ltorg
#ifndef CONFIG_XIP_KERNEL
2:	.long	.
	.long	PAGE_OFFSET
#endif

/*
 * Setup the initial page tables.  We only setup the barest
 * amount which are required to get the kernel running, which
 * generally means mapping in the kernel code.
 *
 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
 *
 * Returns:
 *  r0, r3, r5-r7 corrupted
 *  r4 = page table (see ARCH_PGD_SHIFT in asm/memory.h)
 */
__create_page_tables:

    /*!!C-----------------------------------------------------------------
     * r4 = physical address of page table
     -------------------------------------------------------------------*/
	pgtbl	r4, r8				@ page table address

	/*
	 * Clear the swapper page table
	 */
	mov	r0, r4
	mov	r3, #0
	add	r6, r0, #PG_DIR_SIZE
1:	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	teq	r0, r6
	bne	1b

#ifdef CONFIG_ARM_LPAE
	/*
	 * Build the PGD table (first level) to point to the PMD table. A PGD
	 * entry is 64-bit wide.
	 */
	mov	r0, r4
	add	r3, r4, #0x1000			@ first PMD table address
	orr	r3, r3, #3			@ PGD block type
	mov	r6, #4				@ PTRS_PER_PGD
	mov	r7, #1 << (55 - 32)		@ L_PGD_SWAPPER
1:
#ifdef CONFIG_CPU_ENDIAN_BE8
	str	r7, [r0], #4			@ set top PGD entry bits
	str	r3, [r0], #4			@ set bottom PGD entry bits
#else
	str	r3, [r0], #4			@ set bottom PGD entry bits
	str	r7, [r0], #4			@ set top PGD entry bits
#endif
	add	r3, r3, #0x1000			@ next PMD table
	subs	r6, r6, #1
	bne	1b

	add	r4, r4, #0x1000			@ point to the PMD tables
#ifdef CONFIG_CPU_ENDIAN_BE8
	add	r4, r4, #4			@ we only write the bottom word
#endif
#endif

    /*!!C-----------------------------------------------------------------
     * r10 = procinfo
     * r7 = __cpu_mm_mmu_flags
     -------------------------------------------------------------------*/
	ldr	r7, [r10, #PROCINFO_MM_MMUFLAGS] @ mm_mmuflags

	/*
	 * Create identity mapping to cater for __enable_mmu.
	 * This identity mapping will be removed by paging_init().
	 */
    /*!!C-----------------------------------------------------------------
	 * .long	.
	 * .long	__turn_mmu_on
	 * .long	__turn_mmu_on_end
     -------------------------------------------------------------------*/
	adr	r0, __turn_mmu_on_loc
	ldmia	r0, {r3, r5, r6}
	sub	r0, r0, r3			@ virt->phys offset
	add	r5, r5, r0			@ phys __turn_mmu_on
	add	r6, r6, r0			@ phys __turn_mmu_on_end

    /*!!C-----------------------------------------------------------------
     * SECTION_SHIFT = 20
     -------------------------------------------------------------------*/
	mov	r5, r5, lsr #SECTION_SHIFT
	mov	r6, r6, lsr #SECTION_SHIFT

    /*!!C-----------------------------------------------------------------
     * r4 = page table start
     * r5 = phys __turn_mmu_on
     * r7 = __cpu_mm_mmu_flags
     *
     * Page Table Entry 들을 다음과 같은 값으로 모두 설정하는 작업.
     *  __turn_mmu_on 의 상위 12 bit | __cpu_mm_mmu_flags
     *      section base address       권한,cacheable,buffable
     -------------------------------------------------------------------*/

    /*!!C-----------------------------------------------------------------
     *              ...
     *          +---------+
     *          |    4    |  -> 1MB
     *    16 -->+---------+
     *          |    3    |  -> 1MB
     *    12 -->+---------+
     *          |    2    |  -> 1MB
     *     8 -->+---------+
     *          |    1    |  -> 1MB
     *     4 -->+---------+
     *              ...
     *    주소     index       section
     *
     * turn_mmu_on 의 물리주소를 이용해서 해당 page table entry 에
     * 대해 설정함.
     * 4096 개를 모두 초기화하는 것이 아니라 turn_mmu_on 부터 
     * turn_mmu_on_end 사이의 물리 영역을 가리키는 담당 page table entry 에
     * 대해서만 초기화.
     -------------------------------------------------------------------*/
1:	orr	r3, r7, r5, lsl #SECTION_SHIFT	@ flags + kernel base
	str	r3, [r4, r5, lsl #PMD_ORDER]	@ identity mapping
	cmp	r5, r6
	addlo	r5, r5, #1			@ next section
	blo	1b

	/*
	 * Map our RAM from the start to the end of the kernel .bss section.
	 */
    /*!!C-----------------------------------------------------------------
     * PMD_ORDER 를 뺀 이유는 4 byte align 하여 주소를 의미하도록 한 것임.
     -------------------------------------------------------------------*/
	add	r0, r4, #PAGE_OFFSET >> (SECTION_SHIFT - PMD_ORDER)

    /*!!C-----------------------------------------------------------------
     * _end = bss section end
     * r7 = procinfo 에서 가져온 mmu flags
     * r8 = physical offset
     -------------------------------------------------------------------*/
	ldr	r6, =(_end - 1)
	orr	r3, r8, r7
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ORDER)

    /*!!C-----------------------------------------------------------------
     * r0 가 4 byte씩 움직이면서 entry pointer 가 되고,
     * 각 entry 에는 physical offset | mmu flags 값들로 채워진다.
     * 그 범위가 bss end 에 해당하는 entry 까지.
     *
     * 20140712
     -------------------------------------------------------------------*/
1:	str	r3, [r0], #1 << PMD_ORDER
	add	r3, r3, #1 << SECTION_SHIFT
	cmp	r0, r6
	bls	1b

#ifdef CONFIG_XIP_KERNEL
	/*
	 * Map the kernel image separately as it is not located in RAM.
	 */
#define XIP_START XIP_VIRT_ADDR(CONFIG_XIP_PHYS_ADDR)
	mov	r3, pc
	mov	r3, r3, lsr #SECTION_SHIFT
	orr	r3, r7, r3, lsl #SECTION_SHIFT
	add	r0, r4,  #(XIP_START & 0xff000000) >> (SECTION_SHIFT - PMD_ORDER)
	str	r3, [r0, #((XIP_START & 0x00f00000) >> SECTION_SHIFT) << PMD_ORDER]!
	ldr	r6, =(_edata_loc - 1)
	add	r0, r0, #1 << PMD_ORDER
	add	r6, r4, r6, lsr #(SECTION_SHIFT - PMD_ORDER)
1:	cmp	r0, r6
	add	r3, r3, #1 << SECTION_SHIFT
	strls	r3, [r0], #1 << PMD_ORDER
	bls	1b
#endif

	/*
	 * Then map boot params address in r2 if specified.
	 * We map 2 sections in case the ATAGs/DTB crosses a section boundary.
	 */
	mov	r0, r2, lsr #SECTION_SHIFT
	movs	r0, r0, lsl #SECTION_SHIFT
	subne	r3, r0, r8
	addne	r3, r3, #PAGE_OFFSET
	addne	r3, r4, r3, lsr #(SECTION_SHIFT - PMD_ORDER)
	orrne	r6, r7, r0
	strne	r6, [r3], #1 << PMD_ORDER
	addne	r6, r6, #1 << SECTION_SHIFT
	strne	r6, [r3]

#if defined(CONFIG_ARM_LPAE) && defined(CONFIG_CPU_ENDIAN_BE8)
	sub	r4, r4, #4			@ Fixup page table pointer
						@ for 64-bit descriptors
#endif

#ifdef CONFIG_DEBUG_LL
#if !defined(CONFIG_DEBUG_ICEDCC) && !defined(CONFIG_DEBUG_SEMIHOSTING)
	/*
	 * Map in IO space for serial debugging.
	 * This allows debug messages to be output
	 * via a serial console before paging_init.
	 */
	addruart r7, r3, r0

	mov	r3, r3, lsr #SECTION_SHIFT
	mov	r3, r3, lsl #PMD_ORDER

	add	r0, r4, r3
	mov	r3, r7, lsr #SECTION_SHIFT
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
	orr	r3, r7, r3, lsl #SECTION_SHIFT
#ifdef CONFIG_ARM_LPAE
	mov	r7, #1 << (54 - 32)		@ XN
#ifdef CONFIG_CPU_ENDIAN_BE8
	str	r7, [r0], #4
	str	r3, [r0], #4
#else
	str	r3, [r0], #4
	str	r7, [r0], #4
#endif
#else
	orr	r3, r3, #PMD_SECT_XN
	str	r3, [r0], #4
#endif

#else /* CONFIG_DEBUG_ICEDCC || CONFIG_DEBUG_SEMIHOSTING */
	/* we don't need any serial debugging mappings */
	ldr	r7, [r10, #PROCINFO_IO_MMUFLAGS] @ io_mmuflags
#endif

#if defined(CONFIG_ARCH_NETWINDER) || defined(CONFIG_ARCH_CATS)
	/*
	 * If we're using the NetWinder or CATS, we also need to map
	 * in the 16550-type serial port for the debug messages
	 */
	add	r0, r4, #0xff000000 >> (SECTION_SHIFT - PMD_ORDER)
	orr	r3, r7, #0x7c000000
	str	r3, [r0]
#endif
#ifdef CONFIG_ARCH_RPC
	/*
	 * Map in screen at 0x02000000 & SCREEN2_BASE
	 * Similar reasons here - for debug.  This is
	 * only for Acorn RiscPC architectures.
	 */
	add	r0, r4, #0x02000000 >> (SECTION_SHIFT - PMD_ORDER)
	orr	r3, r7, #0x02000000
	str	r3, [r0]
	add	r0, r4, #0xd8000000 >> (SECTION_SHIFT - PMD_ORDER)
	str	r3, [r0]
#endif
#endif
#ifdef CONFIG_ARM_LPAE
	sub	r4, r4, #0x1000		@ point to the PGD table
	mov	r4, r4, lsr #ARCH_PGD_SHIFT
#endif
	mov	pc, lr
ENDPROC(__create_page_tables)
	.ltorg
	.align
__turn_mmu_on_loc:
	.long	.
	.long	__turn_mmu_on
	.long	__turn_mmu_on_end

#if defined(CONFIG_SMP)
	.text
ENTRY(secondary_startup)
	/*
	 * Common entry point for secondary CPUs.
	 *
	 * Ensure that we're in SVC mode, and IRQs are disabled.  Lookup
	 * the processor type - there is no need to check the machine type
	 * as it has already been validated by the primary processor.
	 */
#ifdef CONFIG_ARM_VIRT_EXT
	bl	__hyp_stub_install_secondary
#endif
	safe_svcmode_maskall r9

	mrc	p15, 0, r9, c0, c0		@ get processor id
	bl	__lookup_processor_type
	movs	r10, r5				@ invalid processor?
	moveq	r0, #'p'			@ yes, error 'p'
 THUMB( it	eq )		@ force fixup-able long branch encoding
	beq	__error_p

	/*
	 * Use the page tables supplied from  __cpu_up.
	 */
	adr	r4, __secondary_data
	ldmia	r4, {r5, r7, r12}		@ address to jump to after
	sub	lr, r4, r5			@ mmu has been enabled
	ldr	r4, [r7, lr]			@ get secondary_data.pgdir
	add	r7, r7, #4
	ldr	r8, [r7, lr]			@ get secondary_data.swapper_pg_dir
	adr	lr, BSYM(__enable_mmu)		@ return address
	mov	r13, r12			@ __secondary_switched address
 ARM(	add	pc, r10, #PROCINFO_INITFUNC	) @ initialise processor
						  @ (return control reg)
 THUMB(	add	r12, r10, #PROCINFO_INITFUNC	)
 THUMB(	mov	pc, r12				)
ENDPROC(secondary_startup)

	/*
	 * r6  = &secondary_data
	 */
ENTRY(__secondary_switched)
	ldr	sp, [r7, #4]			@ get secondary_data.stack
	mov	fp, #0
	b	secondary_start_kernel
ENDPROC(__secondary_switched)

	.align

	.type	__secondary_data, %object
__secondary_data:
	.long	.
	.long	secondary_data
	.long	__secondary_switched
#endif /* defined(CONFIG_SMP) */



/*
 * Setup common bits before finally enabling the MMU.  Essentially
 * this is just loading the page table pointer and domain access
 * registers.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags or dtb pointer
 *  r4  = page table (see ARCH_PGD_SHIFT in asm/memory.h)
 *  r9  = processor ID
 *  r13 = *virtual* address to jump to upon completion
 */
__enable_mmu:
#if defined(CONFIG_ALIGNMENT_TRAP) && __LINUX_ARM_ARCH__ < 6
	orr	r0, r0, #CR_A
#else
	bic	r0, r0, #CR_A
#endif
#ifdef CONFIG_CPU_DCACHE_DISABLE
	bic	r0, r0, #CR_C
#endif
#ifdef CONFIG_CPU_BPREDICT_DISABLE
	bic	r0, r0, #CR_Z
#endif
#ifdef CONFIG_CPU_ICACHE_DISABLE
	bic	r0, r0, #CR_I
#endif
#ifndef CONFIG_ARM_LPAE
	mov	r5, #(domain_val(DOMAIN_USER, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_KERNEL, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_TABLE, DOMAIN_MANAGER) | \
		      domain_val(DOMAIN_IO, DOMAIN_CLIENT))
	mcr	p15, 0, r5, c3, c0, 0		@ load domain access register
	mcr	p15, 0, r4, c2, c0, 0		@ load page table pointer
#endif
	b	__turn_mmu_on
ENDPROC(__enable_mmu)

/*
 * Enable the MMU.  This completely changes the structure of the visible
 * memory space.  You will not be able to trace execution through this.
 * If you have an enquiry about this, *please* check the linux-arm-kernel
 * mailing list archives BEFORE sending another post to the list.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags or dtb pointer
 *  r9  = processor ID
 *  r13 = *virtual* address to jump to upon completion
 *
 * other registers depend on the function called upon completion
 */
	.align	5
	.pushsection	.idmap.text, "ax"
ENTRY(__turn_mmu_on)
	mov	r0, r0
	instr_sync
	mcr	p15, 0, r0, c1, c0, 0		@ write control reg
	mrc	p15, 0, r3, c0, c0, 0		@ read id reg
	instr_sync
	mov	r3, r3
	mov	r3, r13
	mov	pc, r3
__turn_mmu_on_end:
ENDPROC(__turn_mmu_on)
	.popsection


#ifdef CONFIG_SMP_ON_UP
	__INIT

/*!!C-----------------------------------------------------------------
 * r1 = machine no, r2 = atags or dtb,
 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
 -------------------------------------------------------------------*/
__fixup_smp:
	and	r3, r9, #0x000f0000	@ architecture version

    /*!!C-----------------------------------------------------------------
     * 서로 같으면(둘다 0 또는 둘다 1) exclusive or = 거짓 = zero flag : 1
     * 서로 다르면 exclusive or =  참  = zero flag : 0
     *
     * r3 값이 0x000f0000 가 아니면 모두 xor 은 참 
     -------------------------------------------------------------------*/
	teq	r3, #0x000f0000		@ CPU ID supported?

    /*!!C-----------------------------------------------------------------
     * not supported -> r3 != 0x000f0000
     * cpu id not supported -> 무조건 uni-processor
     -------------------------------------------------------------------*/
	bne	__fixup_smp_on_up	@ no, assume UP

    /*!!C-----------------------------------------------------------------
     * supported -> r3 = 0x000f0000
     -------------------------------------------------------------------*/
    /*!!Q-----------------------------------------------------------------
     * one byte 단위로 처리하는 명령어도 있단다...
     * 찾아보자...
     -------------------------------------------------------------------*/
	bic	r3, r9, #0x00ff0000
	bic	r3, r3, #0x0000000f	@ mask 0xff00fff0
	mov	r4, #0x41000000
	orr	r4, r4, #0x0000b000
	orr	r4, r4, #0x00000020	@ val 0x4100b020

    /*!!C-----------------------------------------------------------------
     * cpuid 에서 0xff00fff0 bit 값이 0x4100b020 인지 검사.
     * 완전 똑같으면 zero flag = 1
     * ARM 11MPCore 이면 돌아간다.
     -------------------------------------------------------------------*/
	teq	r3, r4			@ ARM 11MPCore?
	moveq	pc, lr			@ yes, assume SMP

	mrc	p15, 0, r0, c0, c0, 5	@ read MPIDR (Multiprocessor Affinity Register)
	and	r0, r0, #0xc0000000	@ multiprocessing extensions and

    /*!!C-----------------------------------------------------------------
     * r0 의 msb 는 무조건 1 임 
     * r0 의 차상위 bit 가 1 이면 zero flag = 0
     * r0 의 차상위 bit 가 0 이면 zero flag = 1
     -------------------------------------------------------------------*/
	teq	r0, #0x80000000		@ not part of a uniprocessor system?

    /*!!C-----------------------------------------------------------------
     * r0 의 차상위 bit 가 1 인 경우 -> uni-processor 인 경우 
     -------------------------------------------------------------------*/
	bne    __fixup_smp_on_up	@ no, assume UP

	@ Core indicates it is SMP. Check for Aegis(이지스) SOC where a single
	@ Cortex-A9 CPU is present but SMP operations fault.

    /*!!C-----------------------------------------------------------------
     * 이지스 SOC 는 SMP 연산수행이 fault 이므로 바로 리턴.
     -------------------------------------------------------------------*/
	mov	r4, #0x41000000
	orr	r4, r4, #0x0000c000
	orr	r4, r4, #0x00000090
	teq	r3, r4			@ Check for ARM Cortex-A9
	movne	pc, lr			@ Not ARM Cortex-A9,

	@ If a future SoC *does* use 0x0 as the PERIPH_BASE, then the
	@ below address check will need to be #ifdef'd or equivalent
	@ for the Aegis platform.
	mrc	p15, 4, r0, c15, c0	@ get SCU base address
	teq	r0, #0x0		@ '0' on actual UP A9 hardware
	beq	__fixup_smp_on_up	@ So its an A9 UP
	ldr	r0, [r0, #4]		@ read SCU Config
	and	r0, r0, #0x3		@ number of CPUs
	teq	r0, #0x0		@ is 1?
	movne	pc, lr

__fixup_smp_on_up:
	adr	r0, 1f
	ldmia	r0, {r3 - r5}
	sub	r3, r0, r3
	add	r4, r4, r3
	add	r5, r5, r3
	b	__do_fixup_smp_on_up
ENDPROC(__fixup_smp)

	.align
1:	.word	.
	.word	__smpalt_begin
	.word	__smpalt_end

	.pushsection .data
	.globl	smp_on_up
smp_on_up:
	ALT_SMP(.long	1)
	ALT_UP(.long	0)
	.popsection
#endif

	.text
/*!!C-----------------------------------------------------------------
 * r3 = offset
 * r4 = physical __smpalt_begin
 * r5 = physical __smpalt_end
 -------------------------------------------------------------------*/

/*!!Q-----------------------------------------------------------------
 * fixup ?
 * http://www.iamroot.org/xe/Kernel_10_ARM/180812#comment_181156
 * up = uni-processor
 *
 * 이작업을 왜할까 ?
 * http://www.phonesdevelopers.com/1813228/
 -------------------------------------------------------------------*/
__do_fixup_smp_on_up:
	cmp	r4, r5

    /*!!C-----------------------------------------------------------------
     * table 의 끝을 만나면 
     * bl __fixup_smp 호출 자리로 돌아감.
     -------------------------------------------------------------------*/
	movhs	pc, lr
	ldmia	r4!, {r0, r6}

/*!!C-----------------------------------------------------------------
 * table 에서 첫번째와 두번째를 r0, r6 에 로드하여,
 * r0 의 physical 주소를 구해서 그 자리에 r6 값을 넣는다.
 -------------------------------------------------------------------*/
 ARM(	str	r6, [r0, r3]	)
 THUMB(	add	r0, r0, r3	)
#ifdef __ARMEB__
 THUMB(	mov	r6, r6, ror #16	)	@ Convert word order for big-endian.
#endif
 THUMB(	strh	r6, [r0], #2	)	@ For Thumb-2, store as two halfwords
 THUMB(	mov	r6, r6, lsr #16	)	@ to be robust against misaligned r3.
 THUMB(	strh	r6, [r0]	)
	b	__do_fixup_smp_on_up
ENDPROC(__do_fixup_smp_on_up)

ENTRY(fixup_smp)
	stmfd	sp!, {r4 - r6, lr}
	mov	r4, r0
	add	r5, r0, r1
	mov	r3, #0
	bl	__do_fixup_smp_on_up
	ldmfd	sp!, {r4 - r6, pc}
ENDPROC(fixup_smp)

#ifdef CONFIG_ARM_PATCH_PHYS_VIRT

/* __fixup_pv_table - patch the stub instructions with the delta between
 * PHYS_OFFSET and PAGE_OFFSET, which is assumed to be 16MiB aligned and
 * can be expressed by an immediate shifter operand. The stub instruction
 * has a form of '(add|sub) rd, rn, #imm'.
 */

/*!!C-----------------------------------------------------------------
 * http://lists.infradead.org/pipermail/linux-arm-kernel/2011-January/037089.html
 -------------------------------------------------------------------*/
	__HEAD
__fixup_pv_table:
	adr	r0, 1f
	ldmia	r0, {r3-r5, r7}
	sub	r3, r0, r3	@ PHYS_OFFSET - PAGE_OFFSET
	add	r4, r4, r3	@ adjust table start address
	add	r5, r5, r3	@ adjust table end address
	add	r7, r7, r3	@ adjust __pv_phys_offset address
	str	r8, [r7]	@ save computed PHYS_OFFSET to __pv_phys_offset

    /*!!C-----------------------------------------------------------------
     * r3 는 offset 
     -------------------------------------------------------------------*/
	mov	r6, r3, lsr #24	@ constant for add/sub instructions

    /*!!Q-----------------------------------------------------------------
     * r3 가 lsb 24 bit 를 clear 한 r6 값과 같으면 zero flag = 1
     * 한마디로 offset 이 16 MB aligned 인지를 검사.
     * 근데 왜 하필 16 MB 일까 ? 궁금한데...
     -------------------------------------------------------------------*/
	teq	r3, r6, lsl #24 @ must be 16MiB aligned
THUMB(	it	ne		@ cross section branch )
    /*!!C-----------------------------------------------------------------
     * offset 이 16 MB 단위가 아니면 에러.
     -------------------------------------------------------------------*/
	bne	__error

    /*!!C-----------------------------------------------------------------
     * offset 을 16 MB 로 나눈 값. (16 MB 가 몇개인가)
     * -> 이 값을 r7 + 4 자리에 저장해둠.
     -------------------------------------------------------------------*/

    /*!!C-----------------------------------------------------------------
     * r3 = offset (16MB aligned)
     * r4 = physical __pv_table_begin
     * r5 = physical __pv_table_end
     * r7 = physical __pv_phys_offset
     -------------------------------------------------------------------*/
	str	r6, [r7, #4]	@ save to __pv_offset
	b	__fixup_a_pv_table
ENDPROC(__fixup_pv_table)

	.align
1:	.long	.
	.long	__pv_table_begin
	.long	__pv_table_end
2:	.long	__pv_phys_offset

	.text

__fixup_a_pv_table:
#ifdef CONFIG_THUMB2_KERNEL
	lsls	r6, #24
	beq	2f
	clz	r7, r6
	lsr	r6, #24
	lsl	r6, r7
	bic	r6, #0x0080
	lsrs	r7, #1
	orrcs	r6, #0x0080
	orr	r6, r6, r7, lsl #12
	orr	r6, #0x4000
	b	2f
1:	add     r7, r3
	ldrh	ip, [r7, #2]
	and	ip, 0x8f00
	orr	ip, r6	@ mask in offset bits 31-24
	strh	ip, [r7, #2]
2:	cmp	r4, r5
	ldrcc	r7, [r4], #4	@ use branch for delay slot
	bcc	1b
	bx	lr
#else
    /*!!C-----------------------------------------------------------------
     * r3 = offset (16MB aligned)
     * r4 = physical __pv_table_begin
     * r5 = physical __pv_table_end
     -------------------------------------------------------------------*/
	b	2f
1:	ldr	ip, [r7, r3]
	bic	ip, ip, #0x000000ff

    /*!!C-----------------------------------------------------------------
     * r6 -> r3(offset)를 24 bit right shift 한 값.
     -------------------------------------------------------------------*/
     */
    /*!!Q-----------------------------------------------------------------
     * 결국 ip 의 하위 8 bit(immediate value) 를 r6 값으로 변경.
     * 그런 다음 table 의 원래 자리에 대체.
     *
     * -> pv table 이라는걸 좀더 알아봐야겠지만
     * physical 주소와 virtual 주소의 변환에 사용되는 것이 맞다면
     * 하위 1 byte 는 그 변환을 위해 사용되는 offset 성보를 
     * 넣어두는 듯 하다.
     *
     * pv table 이 어떻게 쓰이는지가 궁금하다.
     -------------------------------------------------------------------*/
	orr	ip, ip, r6	@ mask in offset bits 31-24
	str	ip, [r7, r3]
2:	cmp	r4, r5

    /*!!C-----------------------------------------------------------------
     * r4 < r5 이면 carry clear
     -------------------------------------------------------------------*/
	ldrcc	r7, [r4], #4	@ use branch for delay slot
	bcc	1b
	mov	pc, lr
#endif
ENDPROC(__fixup_a_pv_table)

ENTRY(fixup_pv_table)
	stmfd	sp!, {r4 - r7, lr}
	ldr	r2, 2f			@ get address of __pv_phys_offset
	mov	r3, #0			@ no offset
	mov	r4, r0			@ r0 = table start
	add	r5, r0, r1		@ r1 = table size
	ldr	r6, [r2, #4]		@ get __pv_offset
	bl	__fixup_a_pv_table
	ldmfd	sp!, {r4 - r7, pc}
ENDPROC(fixup_pv_table)

	.align
2:	.long	__pv_phys_offset

	.data
	.globl	__pv_phys_offset
	.type	__pv_phys_offset, %object
__pv_phys_offset:
	.long	0
	.size	__pv_phys_offset, . - __pv_phys_offset
__pv_offset:
	.long	0
#endif

#include "head-common.S"
