/*
 *  arch/arm/include/asm/domain.h
 *
 *  Copyright (C) 1999 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_PROC_DOMAIN_H
#define __ASM_PROC_DOMAIN_H

#ifndef __ASSEMBLY__
#include <asm/barrier.h>
#endif

/*
 * Domain numbers
 *
 *  DOMAIN_IO     - domain 2 includes all IO only
 *  DOMAIN_USER   - domain 1 includes all user memory only
 *  DOMAIN_KERNEL - domain 0 includes all kernel memory only
 *
 * The domain numbering depends on whether we support 36 physical
 * address for I/O or not.  Addresses above the 32 bit boundary can
 * only be mapped using supersections and supersections can only
 * be set for domain 0.  We could just default to DOMAIN_IO as zero,
 * but there may be systems with supersection support and no 36-bit
 * addressing.  In such cases, we want to map system memory with
 * supersections to reduce TLB misses and footprint.
 *
 * 36-bit addressing and supersections are only available on
 * CPUs based on ARMv6+ or the Intel XSC3 core.
 */
#ifndef CONFIG_IO_36
#define DOMAIN_KERNEL	0
#define DOMAIN_TABLE	0
#define DOMAIN_USER	1
#define DOMAIN_IO	2
#else
#define DOMAIN_KERNEL	2
#define DOMAIN_TABLE	2
#define DOMAIN_USER	1
#define DOMAIN_IO	0
#endif

/*
 * Domain types
 */
#define DOMAIN_NOACCESS	0
#define DOMAIN_CLIENT	1
#ifdef CONFIG_CPU_USE_DOMAINS
#define DOMAIN_MANAGER	3
#else
#define DOMAIN_MANAGER	1
#endif

#define domain_val(dom,type)	((type) << (2*(dom)))

#ifndef __ASSEMBLY__

#ifdef CONFIG_CPU_USE_DOMAINS
/*!!C -------------------------------------------------
 * domain 에 대해 복습 필요 
 * http://stackcanary.com/?p=632
 *----------------------------------------------------*/
static inline void set_domain(unsigned val)
{
	asm volatile(
	"mcr	p15, 0, %0, c3, c0	@ set domain"
	  : : "r" (val));
	isb();
}

/*!!C -------------------------------------------------
 * http://stackcanary.com/?p=632 복습 후 아래 코드를 봐야 함.
 *
 * 아하!!!
 * thread 마다 domain 설정이 다른 것임.
 * page table entry 의 domain 필드를 통해 해당 thread 의
 * domain 설정(cp15)을 조회하여 client 일 경우는 entry 의
 * AP 필드를 또 조회하게 됨.
 * CP15:c3:c0 레지스터에 설정된 domain 은 현재 thread 의
 * thread_info.cpu_domain 과 같은 값이다.
 * 이는 context switch 될 때마다 바꾸어주어야할 값일 듯.
 *----------------------------------------------------*/
#define modify_domain(dom,type)					\
	do {							\
	struct thread_info *thread = current_thread_info();	\
	unsigned int domain = thread->cpu_domain;		\
	domain &= ~domain_val(dom, DOMAIN_MANAGER);		\  /*!!C domain 에서 clear */
	thread->cpu_domain = domain | domain_val(dom, type);	\  /*!!C 주어진 type 으로 설정 */
	set_domain(thread->cpu_domain);				\
	} while (0)

#else
static inline void set_domain(unsigned val) { }
static inline void modify_domain(unsigned dom, unsigned type)	{ }
#endif

/*
 * Generate the T (user) versions of the LDR/STR and related
 * instructions (inline assembly)
 */
#ifdef CONFIG_CPU_USE_DOMAINS
#define TUSER(instr)	#instr "t"
#else
#define TUSER(instr)	#instr
#endif

#else /* __ASSEMBLY__ */

/*
 * Generate the T (user) versions of the LDR/STR and related
 * instructions
 */
#ifdef CONFIG_CPU_USE_DOMAINS
#define TUSER(instr)	instr ## t
#else
#define TUSER(instr)	instr
#endif

#endif /* __ASSEMBLY__ */

#endif /* !__ASM_PROC_DOMAIN_H */
