/*
 *  linux/arch/arm/kernel/head-common.S
 *
 *  Copyright (C) 1994-2002 Russell King
 *  Copyright (c) 2003 ARM Limited
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define ATAG_CORE 0x54410001
#define ATAG_CORE_SIZE ((2*4 + 3*4) >> 2)
#define ATAG_CORE_SIZE_EMPTY ((2*4) >> 2)

#ifdef CONFIG_CPU_BIG_ENDIAN
#define OF_DT_MAGIC 0xd00dfeed
#else
#define OF_DT_MAGIC 0xedfe0dd0 /* 0xd00dfeed in big-endian */
#endif

/*
 * Exception handling.  Something went wrong and we can't proceed.  We
 * ought to tell the user, but since we don't have any guarantee that
 * we're even running on the right architecture, we do virtually nothing.
 *
 * If CONFIG_DEBUG_LL is set we try to print out something about the error
 * and hope for the best (useful if bootloader fails to pass a proper
 * machine ID for example).
 */
	__HEAD

/* Determine validity of the r2 atags pointer.  The heuristic requires
 * that the pointer be aligned, in the first 16k of physical RAM and
 * that the ATAG_CORE marker is first and present.  If CONFIG_OF_FLATTREE
 * is selected, then it will also accept a dtb pointer.  Future revisions
 * of this function may be more lenient with the physical address and
 * may also be able to move the ATAGS block if necessary.
 *
 * Returns:
 *  r2 either valid atags pointer, valid dtb pointer, or zero
 *  r5, r6 corrupted
 */
/*!!C-----------------------------------------------------------------
 * r1 = machine no, r2 = atags or dtb,
 * r8 = phys_offset, r9 = cpuid, r10 = procinfo
 -------------------------------------------------------------------*/

__vet_atags:

    /*!!C-----------------------------------------------------------------
     * r2(dtb pointer)가 align 안되어 있으면 1f 로 뛴다.
     * 그리고, r2 = 0 로 바꾼 후 리턴. 
     -------------------------------------------------------------------*/
	tst	r2, #0x3			@ aligned?
	bne	1f

    /*!!C-----------------------------------------------------------------
     * dtb pointer 의 첫번째 값을 r5 로 로드 
     -------------------------------------------------------------------*/
	ldr	r5, [r2, #0]
#ifdef CONFIG_OF_FLATTREE

    /*!!C-----------------------------------------------------------------
     * dtb magic no 비교 
     -------------------------------------------------------------------*/
	ldr	r6, =OF_DT_MAGIC		@ is it a DTB?
	cmp	r5, r6
	beq	2f
#endif

    /*!!Q-----------------------------------------------------------------
     * atags 유효 검사
     * ATAG_CORE_SIZE = 5 (????)
     * ATAG_CORE = atags magic number
     *
     * 왜 ATAG_CORE_SIZE 를 ((2*4 + 3*4) >> 2)와 같이 계산하지 ?
     * 아래의 atag_header 와 atag_core 구조체를 보면 알 수 있다.
     * http://www.simtec.co.uk/products/SWLINUX/files/booting_article.html
     -------------------------------------------------------------------*/
	cmp	r5, #ATAG_CORE_SIZE		@ is first tag ATAG_CORE?
	cmpne	r5, #ATAG_CORE_SIZE_EMPTY
	bne	1f
	ldr	r5, [r2, #4]
	ldr	r6, =ATAG_CORE
	cmp	r5, r6
	bne	1f

2:	mov	pc, lr				@ atag/dtb pointer is ok

1:	mov	r2, #0
	mov	pc, lr
ENDPROC(__vet_atags)

/*
 * The following fragment of code is executed with the MMU on in MMU mode,
 * and uses absolute addresses; this is not position independent.
 *
 *  r0  = cp#15 control register
 *  r1  = machine ID
 *  r2  = atags/dtb pointer
 *  r9  = processor ID
 */
	__INIT
    /*!!C-----------------------------------------------------------------
     * 여기부터는 가상주소이다.
     * adr 로 가져온 label 값도 가상주소이다.
     * 가상주소가 아니라면 아마도 delta offset 을 구했을 것이다.
     -------------------------------------------------------------------*/
__mmap_switched:
	adr	r3, __mmap_switched_data

    /*!!C-----------------------------------------------------------------
     *  c04c8000 A __data_loc
     *  c04c8000 D _data
     *  c04c8000 D _sdata
     *  c04c8000 D init_thread_union
     *
     * _sdata 가 아직 뭔지는 모르겠지만, 이 주소가 __data_loc 와 __bss_start 와
     * 다르면 __data_loc 의 값들을 _sdata 로 copy 해준다.
     * 어떨 때 _data 와 _sdata 가 다를까 ?
     * 현재는 link script 에 . 으로 똑같이 설정되어 있는데, 이것을 다르게 설정해준다는
     * 의미가 뭔지...
     *
     * link script 를 보면 CONFIG_XIP_KERNEL (ROM) 일 경우와 아닐 경우
     * __data_loc 을 다르게 설정해주고 있다.
     * RAM 일 경우에는 r4=r5 이므로 아래 copy 과정을 건너뜀.
     *
     * fp = frame pointer
     -------------------------------------------------------------------*/
	ldmia	r3!, {r4, r5, r6, r7}
	cmp	r4, r5				@ Copy data segment if needed
1:	cmpne	r5, r6          // if r4 != r5
	ldrne	fp, [r4], #4    // if r5 != r6
	strne	fp, [r5], #4
	bne	1b

	mov	fp, #0				@ Clear BSS (and zero fp)
1:	cmp	r6, r7
	strcc	fp, [r6],#4
	bcc	1b

 ARM(	ldmia	r3, {r4, r5, r6, r7, sp})
 THUMB(	ldmia	r3, {r4, r5, r6, r7}	)
 THUMB(	ldr	sp, [r3, #16]		)

    /*!!C-----------------------------------------------------------------
     * 아래 3 개를 data 영역에 쭈르륵 저장  
	 * .long	processor_id			@ r4
	 * .long	__machine_arch_type		@ r5
	 * .long	__atags_pointer			@ r6
     *
	 * .long	cr_alignment			@ r7  ==> 0 
	 * .long	init_thread_union + THREAD_START_SP @ sp
     -------------------------------------------------------------------*/
	str	r9, [r4]			@ Save processor ID
	str	r1, [r5]			@ Save machine type
	str	r2, [r6]			@ Save atags pointer
	cmp	r7, #0

    /*!!C-----------------------------------------------------------------
     * align 은 여기서 신경안쓰겠다.
     * 신경쓰면 아래 두 자리에 r0(ctrl register 원래값)와
     * r4(ctrl register 를 A bit clear 한것) 를 저장.
     *  cr_alignment:
     *      .space	4
     *  cr_no_alignment:
     *      .space	4
     -------------------------------------------------------------------*/
	bicne	r4, r0, #CR_A			@ Clear 'A' bit
	stmneia	r7, {r0, r4}			@ Save control register values
	b	start_kernel
ENDPROC(__mmap_switched)

	.align	2
	.type	__mmap_switched_data, %object
__mmap_switched_data:
	.long	__data_loc			@ r4
	.long	_sdata				@ r5
	.long	__bss_start			@ r6
	.long	_end				@ r7
	.long	processor_id			@ r4
	.long	__machine_arch_type		@ r5
	.long	__atags_pointer			@ r6
#ifdef CONFIG_CPU_CP15
	.long	cr_alignment			@ r7
#else
	.long	0				@ r7
#endif
	.long	init_thread_union + THREAD_START_SP @ sp
	.size	__mmap_switched_data, . - __mmap_switched_data

/*
 * This provides a C-API version of __lookup_processor_type
 */
ENTRY(lookup_processor_type)
	stmfd	sp!, {r4 - r6, r9, lr}
	mov	r9, r0
	bl	__lookup_processor_type
	mov	r0, r5
	ldmfd	sp!, {r4 - r6, r9, pc}
ENDPROC(lookup_processor_type)

	__FINIT
	.text

/*
 * Read processor ID register (CP#15, CR0), and look up in the linker-built
 * supported processor list.  Note that we can't use the absolute addresses
 * for the __proc_info lists since we aren't running with the MMU on
 * (and therefore, we are not in the correct address space).  We have to
 * calculate the offset.
 *
 *	r9 = cpuid
 * Returns:
 *	r3, r4, r6 corrupted
 *	r5 = proc_info pointer in physical address space
 *	r9 = cpuid (preserved)
 */
__lookup_processor_type:
	adr	r3, __lookup_processor_type_data

    /*!!C-----------------------------------------------------------------
     * r3 = physical
     * r4 = virtual
     * r3 - r4 = offset
     -------------------------------------------------------------------*/
	ldmia	r3, {r4 - r6}
	sub	r3, r3, r4			@ get offset between virt&phys

    /*!!C-----------------------------------------------------------------
     * r5 = proc info begin
     * virtual(r5) + offset(r3) = physical(r5)
     * virtual(r6) + offset(r3) = physical(r6)
     * 20140715
     -------------------------------------------------------------------*/
	add	r5, r5, r3			@ convert virt addresses to
	add	r6, r6, r3			@ physical address space

    /*!!C-----------------------------------------------------------------
     * table 의 value 와 내 cpuid 와 비교.
     -------------------------------------------------------------------*/
1:	ldmia	r5, {r3, r4}			@ value, mask
	and	r4, r4, r9			@ mask wanted bits
	teq	r3, r4

    /*!!C-----------------------------------------------------------------
     * r3 = r4 이면 zero flag = 1 (2f 로 뛴다)
     * 전체 비트가 다 같아야 zero flag = 1
     -------------------------------------------------------------------*/
	beq	2f

    /*!!C-----------------------------------------------------------------
     * 빌드할 때 asm-offsets.h 에 size 가 결정되어서 작성되어 있다.
     -------------------------------------------------------------------*/
	add	r5, r5, #PROC_INFO_SZ		@ sizeof(proc_info_list)
	cmp	r5, r6
	blo	1b
	mov	r5, #0				@ unknown processor
2:	mov	pc, lr
ENDPROC(__lookup_processor_type)

/*
 * Look in <asm/procinfo.h> for information about the __proc_info structure.
 */
	.align	2
	.type	__lookup_processor_type_data, %object
__lookup_processor_type_data:
	.long	.
	.long	__proc_info_begin
	.long	__proc_info_end
	.size	__lookup_processor_type_data, . - __lookup_processor_type_data

__error_p:
#ifdef CONFIG_DEBUG_LL
	adr	r0, str_p1
	bl	printascii
	mov	r0, r9
	bl	printhex8
	adr	r0, str_p2
	bl	printascii
	b	__error
str_p1:	.asciz	"\nError: unrecognized/unsupported processor variant (0x"
str_p2:	.asciz	").\n"
	.align
#endif
ENDPROC(__error_p)

__error:
#ifdef CONFIG_ARCH_RPC
/*
 * Turn the screen red on a error - RiscPC only.
 */
	mov	r0, #0x02000000
	mov	r3, #0x11
	orr	r3, r3, r3, lsl #8
	orr	r3, r3, r3, lsl #16
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
	str	r3, [r0], #4
#endif
1:	mov	r0, r0
	b	1b
ENDPROC(__error)
