/*
 *  linux/arch/arm/mm/mmu.c
 *
 *  Copyright (C) 1995-2005 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/sizes.h>

#include <asm/cp15.h>
#include <asm/cputype.h>
#include <asm/sections.h>
#include <asm/cachetype.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/tlb.h>
#include <asm/highmem.h>
#include <asm/system_info.h>
#include <asm/traps.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/pci.h>

#include "mm.h"
#include "tcm.h"

/*
 * empty_zero_page is a special page that is used for
 * zero-initialized data and COW.
 */
struct page *empty_zero_page;
EXPORT_SYMBOL(empty_zero_page);

/*
 * The pmd table for the upper-most set of pages.
 */
pmd_t *top_pmd;

#define CPOLICY_UNCACHED	0
#define CPOLICY_BUFFERED	1
#define CPOLICY_WRITETHROUGH	2
#define CPOLICY_WRITEBACK	3
#define CPOLICY_WRITEALLOC	4

static unsigned int cachepolicy __initdata = CPOLICY_WRITEBACK;
static unsigned int ecc_mask __initdata = 0;
pgprot_t pgprot_user;
pgprot_t pgprot_kernel;
pgprot_t pgprot_hyp_device;
pgprot_t pgprot_s2;
pgprot_t pgprot_s2_device;

EXPORT_SYMBOL(pgprot_user);
EXPORT_SYMBOL(pgprot_kernel);

struct cachepolicy {
	const char	policy[16];
	unsigned int	cr_mask;
	pmdval_t	pmd;
	pteval_t	pte;
	pteval_t	pte_s2;
};

#ifdef CONFIG_ARM_LPAE
#define s2_policy(policy)	policy
#else
#define s2_policy(policy)	0
#endif

static struct cachepolicy cache_policies[] __initdata = {
	{
		.policy		= "uncached",
		.cr_mask	= CR_W|CR_C,
		.pmd		= PMD_SECT_UNCACHED,
		.pte		= L_PTE_MT_UNCACHED,
		.pte_s2		= s2_policy(L_PTE_S2_MT_UNCACHED),
	}, {
		.policy		= "buffered",
		.cr_mask	= CR_C,
		.pmd		= PMD_SECT_BUFFERED,
		.pte		= L_PTE_MT_BUFFERABLE,
		.pte_s2		= s2_policy(L_PTE_S2_MT_UNCACHED),
	}, {
		.policy		= "writethrough",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WT,
		.pte		= L_PTE_MT_WRITETHROUGH,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITETHROUGH),
	}, {
		.policy		= "writeback",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WB,
		.pte		= L_PTE_MT_WRITEBACK,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITEBACK),
	}, {
		.policy		= "writealloc",
		.cr_mask	= 0,
		.pmd		= PMD_SECT_WBWA,
		.pte		= L_PTE_MT_WRITEALLOC,
		.pte_s2		= s2_policy(L_PTE_S2_MT_WRITEBACK),
	}
};

#ifdef CONFIG_CPU_CP15
/*
 * These are useful for identifying cache coherency
 * problems by allowing the cache or the cache and
 * writebuffer to be turned off.  (Note: the write
 * buffer should not be on and the cache off).
 */
static int __init early_cachepolicy(char *p)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cache_policies); i++) {
		int len = strlen(cache_policies[i].policy);

		if (memcmp(p, cache_policies[i].policy, len) == 0) {
			cachepolicy = i;
			cr_alignment &= ~cache_policies[i].cr_mask;
			cr_no_alignment &= ~cache_policies[i].cr_mask;
			break;
		}
	}
	if (i == ARRAY_SIZE(cache_policies))
		printk(KERN_ERR "ERROR: unknown or unsupported cache policy\n");
	/*
	 * This restriction is partly to do with the way we boot; it is
	 * unpredictable to have memory mapped using two different sets of
	 * memory attributes (shared, type, and cache attribs).  We can not
	 * change these attributes once the initial assembly has setup the
	 * page tables.
	 */
	if (cpu_architecture() >= CPU_ARCH_ARMv6) {
		printk(KERN_WARNING "Only cachepolicy=writeback supported on ARMv6 and later\n");
		cachepolicy = CPOLICY_WRITEBACK;
	}
	flush_cache_all();
	set_cr(cr_alignment);
	return 0;
}
early_param("cachepolicy", early_cachepolicy);

static int __init early_nocache(char *__unused)
{
	char *p = "buffered";
	printk(KERN_WARNING "nocache is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nocache", early_nocache);

static int __init early_nowrite(char *__unused)
{
	char *p = "uncached";
	printk(KERN_WARNING "nowb is deprecated; use cachepolicy=%s\n", p);
	early_cachepolicy(p);
	return 0;
}
early_param("nowb", early_nowrite);

#ifndef CONFIG_ARM_LPAE
static int __init early_ecc(char *p)
{
	if (memcmp(p, "on", 2) == 0)
		ecc_mask = PMD_PROTECTION;
	else if (memcmp(p, "off", 3) == 0)
		ecc_mask = 0;
	return 0;
}
early_param("ecc", early_ecc);
#endif

static int __init noalign_setup(char *__unused)
{
	cr_alignment &= ~CR_A;
	cr_no_alignment &= ~CR_A;
	set_cr(cr_alignment);
	return 1;
}
__setup("noalign", noalign_setup);

#ifndef CONFIG_SMP
void adjust_cr(unsigned long mask, unsigned long set)
{
	unsigned long flags;

	mask &= ~CR_A;

	set &= mask;

	local_irq_save(flags);

	cr_no_alignment = (cr_no_alignment & ~mask) | set;
	cr_alignment = (cr_alignment & ~mask) | set;

	set_cr((get_cr() & ~mask) | set);

	local_irq_restore(flags);
}
#endif

#else /* ifdef CONFIG_CPU_CP15 */

static int __init early_cachepolicy(char *p)
{
	pr_warning("cachepolicy kernel parameter not supported without cp15\n");
}
early_param("cachepolicy", early_cachepolicy);

static int __init noalign_setup(char *__unused)
{
	pr_warning("noalign kernel parameter not supported without cp15\n");
}
__setup("noalign", noalign_setup);

#endif /* ifdef CONFIG_CPU_CP15 / else */

#define PROT_PTE_DEVICE		L_PTE_PRESENT|L_PTE_YOUNG|L_PTE_DIRTY|L_PTE_XN
#define PROT_PTE_S2_DEVICE	PROT_PTE_DEVICE
#define PROT_SECT_DEVICE	PMD_TYPE_SECT|PMD_SECT_AP_WRITE

/*!!Q-----------------------------------------------------------------
 * MT_DEVICE, MT_DEVICE_NONSHARED 등 메모리가 사용되는 목적에 맞는
 * 타입이 결정되고 각 타입별로 필요한 속성들이 mem_type 자료구조에
 * 설정된다.
 * mem_type 자료구조의 각 변수와 속성이 무엇을 의미하는지 정확히 모르겠네.
 -------------------------------------------------------------------*/
static struct mem_type mem_types[] = {
	[MT_DEVICE] = {		  /* Strongly ordered / ARMv6 shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_SHARED |
				  L_PTE_SHARED,
		.prot_pte_s2	= s2_policy(PROT_PTE_S2_DEVICE) |
				  s2_policy(L_PTE_S2_MT_DEV_SHARED) |
				  L_PTE_SHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_S,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_NONSHARED] = { /* ARMv6 non-shared device */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_NONSHARED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_CACHED] = {	  /* ioremap_cached */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_CACHED,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE | PMD_SECT_WB,
		.domain		= DOMAIN_IO,
	},
	[MT_DEVICE_WC] = {	/* ioremap_wc */
		.prot_pte	= PROT_PTE_DEVICE | L_PTE_MT_DEV_WC,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PROT_SECT_DEVICE,
		.domain		= DOMAIN_IO,
	},
	[MT_UNCACHED] = {
		.prot_pte	= PROT_PTE_DEVICE,
		.prot_l1	= PMD_TYPE_TABLE,
		.prot_sect	= PMD_TYPE_SECT | PMD_SECT_XN,
		.domain		= DOMAIN_IO,
	},
	[MT_CACHECLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
#ifndef CONFIG_ARM_LPAE
	[MT_MINICLEAN] = {
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN | PMD_SECT_MINICACHE,
		.domain    = DOMAIN_KERNEL,
	},
#endif
	[MT_LOW_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_USER,
	},
	[MT_HIGH_VECTORS] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_USER | L_PTE_RDONLY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_USER,
	},
	[MT_MEMORY] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_ROM] = {
		.prot_sect = PMD_TYPE_SECT,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_NONCACHED] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_BUFFERABLE,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_DTCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_ITCM] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_SO] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY |
				L_PTE_MT_UNCACHED | L_PTE_XN,
		.prot_l1   = PMD_TYPE_TABLE,
		.prot_sect = PMD_TYPE_SECT | PMD_SECT_AP_WRITE | PMD_SECT_S |
				PMD_SECT_UNCACHED | PMD_SECT_XN,
		.domain    = DOMAIN_KERNEL,
	},
	[MT_MEMORY_DMA_READY] = {
		.prot_pte  = L_PTE_PRESENT | L_PTE_YOUNG | L_PTE_DIRTY,
		.prot_l1   = PMD_TYPE_TABLE,
		.domain    = DOMAIN_KERNEL,
	},
};

const struct mem_type *get_mem_type(unsigned int type)
{
	return type < ARRAY_SIZE(mem_types) ? &mem_types[type] : NULL;
}
EXPORT_SYMBOL(get_mem_type);

/*
 * Adjust the PMD section entries according to the CPU in use.
 */
/*!!C -------------------------------------------------
 * mem_type 이라는 것이 결국 memory 영역이 이용될 수 있는 
 * 다양한 type 에 대한 성질이나 속성들을 설정해놓은 자료구조이다.
 * 이러한 자료구조는 Memory 영역을 가리키는 Page Table Entry 의
 * 설정값들을 결정하게 된다.
 * (이전에 보았듯이 Page Table Entry 에는 AP, Bufferable, 
 *  Domain, Cacheable 등 bit 단위로 메모리의 특징이 설정되어 있다.)
 * build_mem_type_table 함수에서는 이러한 Entry 의 값들로
 * 사용될 mem_type 의 자료구조 설정들을 현재 사용하는 
 * CPU 를 고려하여 조정해주는 역할을 수행한다.
 *
 * 아래 함수는 길어보이지만 결국은 섹션(Section or L1 or PMD), 
 * 테이블 엔트리(L2 or PTE), 그리고 도메인에 대한 보호설정을
 * 해주는 것이 전부이다.
 *----------------------------------------------------*/
static void __init build_mem_type_table(void)
{
	struct cachepolicy *cp;
    /*!!C -------------------------------------------------
     * 컨트롤 레지스터의 값을 읽어온다.
     *----------------------------------------------------*/
	unsigned int cr = get_cr();
	pteval_t user_pgprot, kern_pgprot, vecs_pgprot;
	pteval_t hyp_device_pgprot, s2_pgprot, s2_device_pgprot;
	int cpu_arch = cpu_architecture();
	int i;

	if (cpu_arch < CPU_ARCH_ARMv6) {
#if defined(CONFIG_CPU_DCACHE_DISABLE)
		if (cachepolicy > CPOLICY_BUFFERED)
			cachepolicy = CPOLICY_BUFFERED;
#elif defined(CONFIG_CPU_DCACHE_WRITETHROUGH)
		if (cachepolicy > CPOLICY_WRITETHROUGH)
			cachepolicy = CPOLICY_WRITETHROUGH;
#endif
	}
	if (cpu_arch < CPU_ARCH_ARMv5) {
		if (cachepolicy >= CPOLICY_WRITEALLOC)
			cachepolicy = CPOLICY_WRITEBACK;
		ecc_mask = 0;
	}

    /*!!C -------------------------------------------------
     * write allocation 이 추가되면서 기존의
     * CPOLICY_WRITEBACK 은 Write-Back cache with No-Write Allocation 의 의미로,
     * CPOLICY_WRITEALLOC 은 Write-Back cache with Write Allocation 의미로 사용됨
     *
     *  - write allocate 방식
     *    write miss 가 발생했을 때, 우선 메인메모리로부터
     *    해당 block을 fetch 하여 cache 내의 block 에 allocate 한다.
     *    그러면, write hit 가 가능해지는데 write hit 에 의하여
     *    cache 내 해당 block 에 write 를 수행한다.
     *    그리고, 또 다른 miss 가 발생했을 때 modified 된 block 은
     *    write buffer 에 write 된다.
     *  - write miss 가 발생했을 때, 우선 write buffer 에 write 한다.
     *    그 후, write buffer 에 write 된 데이터는 메인메모리에
     *    write 를 수행한다. (메인메모리로부터 해당 block 을
     *    fetch 하여 cache 내의 block 에 allocate 를 수행하지 않는다)
     *
     * https://www.google.co.kr/url?sa=t&rct=j&q=&esrc=s&source=web&cd=2&    \
     *   ved=0CCwQFjAB&url=http%3A%2F%2Fmpu.yonsei.ac.kr%2Fxe%2F%3Fmodule%   \
     *   3Dfile%26act%3DprocFileDownload%26file_srl%3D11170%26sid%           \
     *   3Dee4ab3e31f204e2a7804a59e9ce68727&ei=kCyBVLjcIdfZ8gXcwIHwDQ&       \
     *   usg=AFQjCNHnxCTS23ucZARkdTCDhJG_FFdQug&sig2=y74iWbEaH8O4IaDlQQ9_cw& \
     *   bvm=bv.80642063,bs.1,d.dGc&cad=rjt
     *
     * smp 일 때 왜 cachepolicy 를 write allocate 로 사용하는지.
     *   http://lists.infradead.org/pipermail/linux-arm-kernel/2014-May/260008.html
     *----------------------------------------------------*/
	if (is_smp())
		cachepolicy = CPOLICY_WRITEALLOC;

	/*
	 * Strip out features not present on earlier architectures.
	 * Pre-ARMv5 CPUs don't have TEX bits.  Pre-ARMv6 CPUs or those
	 * without extended page tables don't have the 'Shared' bit.
	 */
    /*!!C -------------------------------------------------
     * 버전이 낮은 architecture 들에 대해 지원안되는 기능등을
     * 가리키는 bit 는 빼버림.
     * ARRAY_SIZE 매크로 유용할 듯...
     *----------------------------------------------------*/
	if (cpu_arch < CPU_ARCH_ARMv5)
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_TEX(7);
	if ((cpu_arch < CPU_ARCH_ARMv6 || !(cr & CR_XP)) && !cpu_is_xsc3())
		for (i = 0; i < ARRAY_SIZE(mem_types); i++)
			mem_types[i].prot_sect &= ~PMD_SECT_S;

	/*
	 * ARMv5 and lower, bit 4 must be set for page tables (was: cache
	 * "update-able on write" bit on ARM610).  However, Xscale and
	 * Xscale3 require this bit to be cleared.
	 */
	if (cpu_is_xscale() || cpu_is_xsc3()) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			mem_types[i].prot_sect &= ~PMD_BIT4;
			mem_types[i].prot_l1 &= ~PMD_BIT4;
		}
	} else if (cpu_arch < CPU_ARCH_ARMv6) {
		for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
			if (mem_types[i].prot_l1)
				mem_types[i].prot_l1 |= PMD_BIT4;
			if (mem_types[i].prot_sect)
				mem_types[i].prot_sect |= PMD_BIT4;
		}
	}

	/*
	 * Mark the device areas according to the CPU/architecture.
	 */
	if (cpu_is_xsc3() || (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP))) {
		if (!cpu_is_xsc3()) {
			/*
			 * Mark device regions on ARMv6+ as execute-never
			 * to prevent speculative instruction fetches.
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_XN;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_XN;
		}
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/*
			 * For ARMv7 with TEX remapping,
			 * - shared device is SXCB=1100
			 * - nonshared device is SXCB=0100
			 * - write combine device mem is SXCB=0001
			 * (Uncached Normal memory)
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(1);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
		} else if (cpu_is_xsc3()) {
			/*
			 * For Xscale3,
			 * - shared device is TEXCB=00101
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Inner/Outer Uncacheable in xsc3 parlance)
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_TEX(1) | PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		} else {
			/*
			 * For ARMv6 and ARMv7 without TEX remapping,
			 * - shared device is TEXCB=00001
			 * - nonshared device is TEXCB=01000
			 * - write combine device mem is TEXCB=00100
			 * (Uncached Normal in ARMv6 parlance).
			 */
			mem_types[MT_DEVICE].prot_sect |= PMD_SECT_BUFFERED;
			mem_types[MT_DEVICE_NONSHARED].prot_sect |= PMD_SECT_TEX(2);
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_TEX(1);
		}
	} else {
		/*
		 * On others, write combining is "Uncached/Buffered"
		 */
		mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_BUFFERABLE;
	}

	/*
	 * Now deal with the memory-type mappings
	 */
	cp = &cache_policies[cachepolicy];
	vecs_pgprot = kern_pgprot = user_pgprot = cp->pte;
	s2_pgprot = cp->pte_s2;
	hyp_device_pgprot = mem_types[MT_DEVICE].prot_pte;
	s2_device_pgprot = mem_types[MT_DEVICE].prot_pte_s2;

	/*
	 * We don't use domains on ARMv6 (since this causes problems with
	 * v6/v7 kernels), so we must use a separate memory type for user
	 * r/o, kernel r/w to map the vectors page.
	 */
#ifndef CONFIG_ARM_LPAE
	if (cpu_arch == CPU_ARCH_ARMv6)
		vecs_pgprot |= L_PTE_MT_VECTORS;
#endif

	/*
	 * ARMv6 and above have extended page tables.
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6 && (cr & CR_XP)) {
#ifndef CONFIG_ARM_LPAE
		/*
		 * Mark cache clean areas and XIP ROM read only
		 * from SVC mode and no access from userspace.
		 */
		mem_types[MT_ROM].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_MINICLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_APX|PMD_SECT_AP_WRITE;
#endif

		if (is_smp()) {
			/*
			 * Mark memory with the "shared" attribute
			 * for SMP systems
			 */
			user_pgprot |= L_PTE_SHARED;
			kern_pgprot |= L_PTE_SHARED;
			vecs_pgprot |= L_PTE_SHARED;
			s2_pgprot |= L_PTE_SHARED;
			mem_types[MT_DEVICE_WC].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_WC].prot_pte |= L_PTE_SHARED;
			mem_types[MT_DEVICE_CACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_DEVICE_CACHED].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_DMA_READY].prot_pte |= L_PTE_SHARED;
			mem_types[MT_MEMORY_NONCACHED].prot_sect |= PMD_SECT_S;
			mem_types[MT_MEMORY_NONCACHED].prot_pte |= L_PTE_SHARED;
		}
	}

	/*
	 * Non-cacheable Normal - intended for memory areas that must
	 * not cause dirty cache line writebacks when used
	 */
	if (cpu_arch >= CPU_ARCH_ARMv6) {
		if (cpu_arch >= CPU_ARCH_ARMv7 && (cr & CR_TRE)) {
			/* Non-cacheable Normal is XCB = 001 */
			mem_types[MT_MEMORY_NONCACHED].prot_sect |=
				PMD_SECT_BUFFERED;
		} else {
			/* For both ARMv6 and non-TEX-remapping ARMv7 */
			mem_types[MT_MEMORY_NONCACHED].prot_sect |=
				PMD_SECT_TEX(1);
		}
	} else {
		mem_types[MT_MEMORY_NONCACHED].prot_sect |= PMD_SECT_BUFFERABLE;
	}

#ifdef CONFIG_ARM_LPAE
	/*
	 * Do not generate access flag faults for the kernel mappings.
	 */
	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		mem_types[i].prot_pte |= PTE_EXT_AF;
		if (mem_types[i].prot_sect)
			mem_types[i].prot_sect |= PMD_SECT_AF;
	}
	kern_pgprot |= PTE_EXT_AF;
	vecs_pgprot |= PTE_EXT_AF;
#endif

	for (i = 0; i < 16; i++) {
		pteval_t v = pgprot_val(protection_map[i]);
		protection_map[i] = __pgprot(v | user_pgprot);
	}

	mem_types[MT_LOW_VECTORS].prot_pte |= vecs_pgprot;
	mem_types[MT_HIGH_VECTORS].prot_pte |= vecs_pgprot;

	pgprot_user   = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | user_pgprot);
	pgprot_kernel = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG |
				 L_PTE_DIRTY | kern_pgprot);
	pgprot_s2  = __pgprot(L_PTE_PRESENT | L_PTE_YOUNG | s2_pgprot);
	pgprot_s2_device  = __pgprot(s2_device_pgprot);
	pgprot_hyp_device  = __pgprot(hyp_device_pgprot);

	mem_types[MT_LOW_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_HIGH_VECTORS].prot_l1 |= ecc_mask;
	mem_types[MT_MEMORY].prot_sect |= ecc_mask | cp->pmd;
	mem_types[MT_MEMORY].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_DMA_READY].prot_pte |= kern_pgprot;
	mem_types[MT_MEMORY_NONCACHED].prot_sect |= ecc_mask;
	mem_types[MT_ROM].prot_sect |= cp->pmd;

	switch (cp->pmd) {
	case PMD_SECT_WT:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WT;
		break;
	case PMD_SECT_WB:
	case PMD_SECT_WBWA:
		mem_types[MT_CACHECLEAN].prot_sect |= PMD_SECT_WB;
		break;
	}
	printk("Memory policy: ECC %sabled, Data cache %s\n",
		ecc_mask ? "en" : "dis", cp->policy);

	for (i = 0; i < ARRAY_SIZE(mem_types); i++) {
		struct mem_type *t = &mem_types[i];
		if (t->prot_l1)
			t->prot_l1 |= PMD_DOMAIN(t->domain);
		if (t->prot_sect)
			t->prot_sect |= PMD_DOMAIN(t->domain);
	}
}

#ifdef CONFIG_ARM_DMA_MEM_BUFFERABLE
pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);
#endif

#define vectors_base()	(vectors_high() ? 0xffff0000 : 0)

static void __init *early_alloc_aligned(unsigned long sz, unsigned long align)
{
	void *ptr = __va(memblock_alloc(sz, align));
	memset(ptr, 0, sz);
	return ptr;
}

static void __init *early_alloc(unsigned long sz)
{
	return early_alloc_aligned(sz, sz);
}

static pte_t * __init early_pte_alloc(pmd_t *pmd, unsigned long addr, unsigned long prot)
{
	if (pmd_none(*pmd)) {
		pte_t *pte = early_alloc(PTE_HWTABLE_OFF + PTE_HWTABLE_SIZE);
		__pmd_populate(pmd, __pa(pte), prot);
	}
	BUG_ON(pmd_bad(*pmd));
	return pte_offset_kernel(pmd, addr);
}

static void __init alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  const struct mem_type *type)
{
	pte_t *pte = early_pte_alloc(pmd, addr, type->prot_l1);
	do {
		set_pte_ext(pte, pfn_pte(pfn, __pgprot(type->prot_pte)), 0);
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);
}

static void __init __map_init_section(pmd_t *pmd, unsigned long addr,
			unsigned long end, phys_addr_t phys,
			const struct mem_type *type)
{
	pmd_t *p = pmd;

#ifndef CONFIG_ARM_LPAE
	/*
	 * In classic MMU format, puds and pmds are folded in to
	 * the pgds. pmd_offset gives the PGD entry. PGDs refer to a
	 * group of L1 entries making up one logical pointer to
	 * an L2 table (2MB), where as PMDs refer to the individual
	 * L1 entries (1MB). Hence increment to get the correct
	 * offset for odd 1MB sections.
	 * (See arch/arm/include/asm/pgtable-2level.h)
	 */
	if (addr & SECTION_SIZE)
		pmd++;
#endif
    /*!!C -------------------------------------------------
     * addr ~ end 에 해당하는 pmd entry 들을 phys | prot_sect 값으로
     * 설정한다. 이게 곧 mapping 작업이다.
     *----------------------------------------------------*/
	do {
		*pmd = __pmd(phys | type->prot_sect);
		phys += SECTION_SIZE;
	} while (pmd++, addr += SECTION_SIZE, addr != end);

    /*!!C -------------------------------------------------
     * 수정된 내용을 메모리에 반영 
     *----------------------------------------------------*/
	flush_pmd_entry(p);
}

static void __init alloc_init_pmd(pud_t *pud, unsigned long addr,
				      unsigned long end, phys_addr_t phys,
				      const struct mem_type *type)
{
	pmd_t *pmd = pmd_offset(pud, addr);
	unsigned long next;

	do {
		/*
		 * With LPAE, we must loop over to map
		 * all the pmds for the given range.
		 */
		next = pmd_addr_end(addr, end);

		/*
		 * Try a section mapping - addr, next and phys must all be
		 * aligned to a section boundary.
		 */
		if (type->prot_sect &&
				((addr | next | phys) & ~SECTION_MASK) == 0) {
            /*!!C -------------------------------------------------
             * 우리는 prot_sect 가 설정되어 있고, addr/next/phys 가
             * section boundary 로 align 되어 있다고 볼 수 있으므로
             * 여기를 탈까 ? -> yes
             *----------------------------------------------------*/
			__map_init_section(pmd, addr, next, phys, type);
		} else {
            /*!!C -------------------------------------------------
             * prot_sect 에 Section 설정이 안되어 있거나,
             * section 단위로 align 안되어 있으면 2 level 을 타도록.
             *----------------------------------------------------*/
			alloc_init_pte(pmd, addr, next,
						__phys_to_pfn(phys), type);
		}

		phys += next - addr;

	} while (pmd++, addr = next, addr != end);
}

static void __init alloc_init_pud(pgd_t *pgd, unsigned long addr,
				  unsigned long end, phys_addr_t phys,
				  const struct mem_type *type)
{
    /*!!C -------------------------------------------------
     * 2-level 에서는 pud = pmd = pgd
     *----------------------------------------------------*/
	pud_t *pud = pud_offset(pgd, addr);
	unsigned long next;

	do {
		next = pud_addr_end(addr, end);
        /*!!C -------------------------------------------------
         * 2-level 에서는 결국 alloc_init_pud, alloc_init_pmd 의
         * depth 는 무시해도 될 것 같은데...
         *----------------------------------------------------*/
		alloc_init_pmd(pud, addr, next, phys, type);
		phys += next - addr;
	} while (pud++, addr = next, addr != end);
}

#ifndef CONFIG_ARM_LPAE
static void __init create_36bit_mapping(struct map_desc *md,
					const struct mem_type *type)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	pgd_t *pgd;

	addr = md->virtual;
	phys = __pfn_to_phys(md->pfn);
	length = PAGE_ALIGN(md->length);

	if (!(cpu_architecture() >= CPU_ARCH_ARMv6 || cpu_is_xsc3())) {
		printk(KERN_ERR "MM: CPU does not support supersection "
		       "mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/* N.B.	ARMv6 supersections are only defined to work with domain 0.
	 *	Since domain assignments can in fact be arbitrary, the
	 *	'domain == 0' check below is required to insure that ARMv6
	 *	supersections are only allocated for domain 0 regardless
	 *	of the actual domain assignments in use.
	 */
	if (type->domain) {
		printk(KERN_ERR "MM: invalid domain in supersection "
		       "mapping for 0x%08llx at 0x%08lx\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	if ((addr | length | __pfn_to_phys(md->pfn)) & ~SUPERSECTION_MASK) {
		printk(KERN_ERR "MM: cannot create mapping for 0x%08llx"
		       " at 0x%08lx invalid alignment\n",
		       (long long)__pfn_to_phys((u64)md->pfn), addr);
		return;
	}

	/*
	 * Shift bits [35:32] of address into bits [23:20] of PMD
	 * (See ARMv6 spec).
	 */
	phys |= (((md->pfn >> (32 - PAGE_SHIFT)) & 0xF) << 20);

	pgd = pgd_offset_k(addr);
	end = addr + length;
	do {
		pud_t *pud = pud_offset(pgd, addr);
		pmd_t *pmd = pmd_offset(pud, addr);
		int i;

		for (i = 0; i < 16; i++)
			*pmd++ = __pmd(phys | type->prot_sect | PMD_SECT_SUPER);

		addr += SUPERSECTION_SIZE;
		phys += SUPERSECTION_SIZE;
		pgd += SUPERSECTION_SIZE >> PGDIR_SHIFT;
	} while (addr != end);
}
#endif	/* !CONFIG_ARM_LPAE */

/*
 * Create the page directory entries and any necessary
 * page tables for the mapping specified by `md'.  We
 * are able to cope here with varying sizes and address
 * offsets, and we take full advantage of sections and
 * supersections.
 */
/*!!C -------------------------------------------------
 * 인자로 넘어온 map_desc 로 표현되는 메모리 영역에 대해
 * pgd 부터 pte 까지 page table entry 들을 map_desc.type 에
 * 맞는 mem_types 정보로 설정하는 작업.
 * 주어지는 물리 메모리 영역을 map_desc.virtual 주소로
 * mapping 하는 작업이다.
 *----------------------------------------------------*/
static void __init create_mapping(struct map_desc *md)
{
	unsigned long addr, length, end;
	phys_addr_t phys;
	const struct mem_type *type;
	pgd_t *pgd;

    /*!!C -------------------------------------------------
     * md 가 가리키는 메모리가 vector table 이 아니고,
     * user task space 영역일 경우에는 mapping 을 하지 않는다.
     * prepare_page_table 함수에서 user 영역 초기화는 이미 수행했다.
     *----------------------------------------------------*/
	if (md->virtual != vectors_base() && md->virtual < TASK_SIZE) {
		printk(KERN_WARNING "BUG: not creating mapping for 0x%08llx"
		       " at 0x%08lx in user region\n",
		       (long long)__pfn_to_phys((u64)md->pfn), md->virtual);
		return;
	}

    /*!!C -------------------------------------------------
     * ROM 이나 DEVICE 메모리 타입일 경우에는 vmalloc 영역을
     * 벗어난 영역에 대해서는 mapping 을 원래 못하는데...
     *----------------------------------------------------*/
	if ((md->type == MT_DEVICE || md->type == MT_ROM) &&
	    md->virtual >= PAGE_OFFSET &&
	    (md->virtual < VMALLOC_START || md->virtual >= VMALLOC_END)) {
		printk(KERN_WARNING "BUG: mapping for 0x%08llx"
		       " at 0x%08lx out of vmalloc space\n",
		       (long long)__pfn_to_phys((u64)md->pfn), md->virtual);
	}

	type = &mem_types[md->type];

#ifndef CONFIG_ARM_LPAE
	/*
	 * Catch 36-bit addresses
	 */
	if (md->pfn >= 0x100000) {
		create_36bit_mapping(md, type);
		return;
	}
#endif

    /*!!C -------------------------------------------------
     * addr = 4 k 단위로 짤라낸 가상주소 
     * phys = 4 k 단위로 짤라낸 물리주소 
     *----------------------------------------------------*/
	addr = md->virtual & PAGE_MASK;
	phys = __pfn_to_phys(md->pfn);

    /*!!C -------------------------------------------------
     * md->lenght 를 page 크기(4k) 단위로 align
     *----------------------------------------------------*/
	length = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));

    /*!!C -------------------------------------------------
     * type->prot_l1 == 0 이고, 즉 section 으로 사용하려고 하면서 
     * addr, phys, length 가 모두 section size 로 align 되어 있지 않으면 
     *----------------------------------------------------*/
	if (type->prot_l1 == 0 && ((addr | phys | length) & ~SECTION_MASK)) {
		printk(KERN_WARNING "BUG: map for 0x%08llx at 0x%08lx can not "
		       "be mapped using pages, ignoring.\n",
		       (long long)__pfn_to_phys(md->pfn), addr);
		return;
	}

    /*!!C -------------------------------------------------
     * 인자로 넘어온 memory 영역을 cover 하는 pgd entry 들을
     * 구하려면 일단 시작 주소에 해당하는 pgd entry 를 구해야지. -> pgd
     * (인자로 넘어온 memory 영역은 여러개의 pgd entry 가
     *  필요할 수 있음.)
     *----------------------------------------------------*/
	pgd = pgd_offset_k(addr);

    /*!!C -------------------------------------------------
     * 4 k align 된 시작주소에다 4 k align 된 length 를 더해서
     * 인자 메모리 덩어리의 끝(end)을 구함.
     *----------------------------------------------------*/
	end = addr + length;
	do {
        /*!!C -------------------------------------------------
         * next = 이번꺼 다음의 2 MB 단위의 다음 boundary 주소 
         *----------------------------------------------------*/
		unsigned long next = pgd_addr_end(addr, end);

        /*!!C -------------------------------------------------
         * pgd entry 하나에 대한 설정 작업 
         *   -> pgd 가 하나라면 section 을 사용할 때는 1 개의 pmd
         *   -> section 을 사용하지 않는다면 pte 할당하고 설정작업 
         *
         * pgd  : addr 의 page directory offset
         * addr : 2 MB 씩 이동하는 시작주소 (mapping 할 가상주소)
         * next : addr 의 다음 2 MB 시작 주소 
         * phys : 현재 addr 의 물리주소 
         * type : MT_MEMORY 에 해당하는 mem_types
         *----------------------------------------------------*/
		alloc_init_pud(pgd, addr, next, phys, type);

        /*!!C -------------------------------------------------
         * 물리주소에도 2 MB 를 더해준다.
         *----------------------------------------------------*/
		phys += next - addr;

		addr = next;
	} while (pgd++, addr != end);
}

/*
 * Create the architecture specific mappings
 */
void __init iotable_init(struct map_desc *io_desc, int nr)
{
	struct map_desc *md;
	struct vm_struct *vm;
	struct static_vm *svm;

	if (!nr)
		return;

	svm = early_alloc_aligned(sizeof(*svm) * nr, __alignof__(*svm));

	for (md = io_desc; nr; md++, nr--) {
		create_mapping(md);

		vm = &svm->vm;
		vm->addr = (void *)(md->virtual & PAGE_MASK);
		vm->size = PAGE_ALIGN(md->length + (md->virtual & ~PAGE_MASK));
		vm->phys_addr = __pfn_to_phys(md->pfn);
		vm->flags = VM_IOREMAP | VM_ARM_STATIC_MAPPING;
		vm->flags |= VM_ARM_MTYPE(md->type);
		vm->caller = iotable_init;
		add_static_vm_early(svm++);
	}
}

void __init vm_reserve_area_early(unsigned long addr, unsigned long size,
				  void *caller)
{
	struct vm_struct *vm;
	struct static_vm *svm;

	svm = early_alloc_aligned(sizeof(*svm), __alignof__(*svm));

	vm = &svm->vm;
	vm->addr = (void *)addr;
	vm->size = size;
	vm->flags = VM_IOREMAP | VM_ARM_EMPTY_MAPPING;
	vm->caller = caller;
	add_static_vm_early(svm);
}

#ifndef CONFIG_ARM_LPAE

/*
 * The Linux PMD is made of two consecutive section entries covering 2MB
 * (see definition in include/asm/pgtable-2level.h).  However a call to
 * create_mapping() may optimize static mappings by using individual
 * 1MB section mappings.  This leaves the actual PMD potentially half
 * initialized if the top or bottom section entry isn't used, leaving it
 * open to problems if a subsequent ioremap() or vmalloc() tries to use
 * the virtual space left free by that unused section entry.
 *
 * Let's avoid the issue by inserting dummy vm entries covering the unused
 * PMD halves once the static mappings are in place.
 */

static void __init pmd_empty_section_gap(unsigned long addr)
{
	vm_reserve_area_early(addr, SECTION_SIZE, pmd_empty_section_gap);
}

static void __init fill_pmd_gaps(void)
{
	struct static_vm *svm;
	struct vm_struct *vm;
	unsigned long addr, next = 0;
	pmd_t *pmd;

	list_for_each_entry(svm, &static_vmlist, list) {
		vm = &svm->vm;
		addr = (unsigned long)vm->addr;
		if (addr < next)
			continue;

		/*
		 * Check if this vm starts on an odd section boundary.
		 * If so and the first section entry for this PMD is free
		 * then we block the corresponding virtual address.
		 */
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr);
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr & PMD_MASK);
		}

		/*
		 * Then check if this vm ends on an odd section boundary.
		 * If so and the second section entry for this PMD is empty
		 * then we block the corresponding virtual address.
		 */
		addr += vm->size;
		if ((addr & ~PMD_MASK) == SECTION_SIZE) {
			pmd = pmd_off_k(addr) + 1;
			if (pmd_none(*pmd))
				pmd_empty_section_gap(addr);
		}

		/* no need to look at any vm entry until we hit the next PMD */
		next = (addr + PMD_SIZE - 1) & PMD_MASK;
	}
}

#else
#define fill_pmd_gaps() do { } while (0)
#endif

#if defined(CONFIG_PCI) && !defined(CONFIG_NEED_MACH_IO_H)
static void __init pci_reserve_io(void)
{
	struct static_vm *svm;

	svm = find_static_vm_vaddr((void *)PCI_IO_VIRT_BASE);
	if (svm)
		return;

	vm_reserve_area_early(PCI_IO_VIRT_BASE, SZ_2M, pci_reserve_io);
}
#else
#define pci_reserve_io() do { } while (0)
#endif

#ifdef CONFIG_DEBUG_LL
void __init debug_ll_io_init(void)
{
	struct map_desc map;

	debug_ll_addr(&map.pfn, &map.virtual);
	if (!map.pfn || !map.virtual)
		return;
	map.pfn = __phys_to_pfn(map.pfn);
	map.virtual &= PAGE_MASK;
	map.length = PAGE_SIZE;
	map.type = MT_DEVICE;
	iotable_init(&map, 1);
}
#endif

/*!!C -------------------------------------------------
 * 왜 240 M를 뺐을까 ?
 * x86 과 같이 896 MB 를 제외한 128 MB 를 highmem 으로
 * 사용하기에는 ARM 에서 16 MB (high exception vector)가
 * 추가로 제약되어 256 MB 로 Align 한 것으로 보인다.
 *
 * vmalloc_min = 4080 MB - 240 MB - 8 MB = 3832 MB
 * VMALLOC_END = 0xff000000
 * VMALLOC_OFFSET = 0x800000
 * 0xff000000 - 0xf0000000 - 0x800000 = 0xef800000
 * vmalloc_min = 0xef800000
 *----------------------------------------------------*/
static void * __initdata vmalloc_min =
	(void *)(VMALLOC_END - (240 << 20) - VMALLOC_OFFSET);

/*
 * vmalloc=size forces the vmalloc area to be exactly 'size'
 * bytes. This can be used to increase (or decrease) the vmalloc
 * area - the default is 240m.
 */
static int __init early_vmalloc(char *arg)
{
	unsigned long vmalloc_reserve = memparse(arg, NULL);

	if (vmalloc_reserve < SZ_16M) {
		vmalloc_reserve = SZ_16M;
		printk(KERN_WARNING
			"vmalloc area too small, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}

	if (vmalloc_reserve > VMALLOC_END - (PAGE_OFFSET + SZ_32M)) {
		vmalloc_reserve = VMALLOC_END - (PAGE_OFFSET + SZ_32M);
		printk(KERN_WARNING
			"vmalloc area is too big, limiting to %luMB\n",
			vmalloc_reserve >> 20);
	}

	vmalloc_min = (void *)(VMALLOC_END - vmalloc_reserve);
	return 0;
}
early_param("vmalloc", early_vmalloc);

phys_addr_t arm_lowmem_limit __initdata = 0;

/*!!C -------------------------------------------------
 * lowmem, vmalloc(highmem) 등 kernel virtual memory layout 에
 * 대한 좋은 답변 
 *  http://unix.stackexchange.com/questions/5124/what-does-the-virtual-kernel-memory-layout-in-dmesg-imply
 *
 * OB 들 분석 자료 굿
 *  https://github.com/iamroot-arm10b/linux/blob/master/arch/arm/mm/mmu.c
 *----------------------------------------------------*/
void __init sanity_check_meminfo(void)
{
	phys_addr_t memblock_limit = 0;
	int i, j, highmem = 0;

    /*!!C -------------------------------------------------
     * vmalloc_limit = himem 의 시작
     * vmalloc_min = 0xEF800000 : highmem의 시작주소에서 8M 만큼 뺀 주소
     * pa(vmalloc_min -1)+1 = 0x4F800000
     * vmalloc_limit = 0x4F800000
     *----------------------------------------------------*/
	phys_addr_t vmalloc_limit = __pa(vmalloc_min - 1) + 1;

    /*!!C -------------------------------------------------
     * dtb 설정에 의해 meminfo
     * nr_bank = 1
     * 0x20000000 start (addr)
     * 0x80000000 size
     *----------------------------------------------------*/
	for (i = 0, j = 0; i < meminfo.nr_banks; i++) {
		struct membank *bank = &meminfo.bank[j];
		phys_addr_t size_limit;

		*bank = meminfo.bank[i];
		size_limit = bank->size;

        /*!!C -------------------------------------------------
         * vmalloc_limit는 물리메모리상의 vmalloc 상한선을 나타낸다.
         *----------------------------------------------------*/
		if (bank->start >= vmalloc_limit)
			highmem = 1;
		else
			size_limit = vmalloc_limit - bank->start;

		bank->highmem = highmem;

#ifdef CONFIG_HIGHMEM
		/*
		 * Split those memory banks which are partially overlapping
		 * the vmalloc area greatly simplifying things later.
		 */
        /*!!C -------------------------------------------------
         * bank start 가 vmalloc_limit 를 넘지 않은 경우라도
         * bank 의 size 를 고려하면 bank 영역이 highmem 영역으로
         * 침범이 가능하므로 침범한 것과 아닌 것을 둘로 나눔 
         *----------------------------------------------------*/
		if (!highmem && bank->size > size_limit) {
			if (meminfo.nr_banks >= NR_BANKS) {
				printk(KERN_CRIT "NR_BANKS too low, "
						 "ignoring high memory\n");
			} else {
				memmove(bank + 1, bank,
					(meminfo.nr_banks - i) * sizeof(*bank));
				meminfo.nr_banks++;
				i++;
                /*!!C -------------------------------------------------
                 * 현재 bank 는 둘로 쪼갠 것 중에서 첫번째를 가리키고
                 * 있으니 bank[1] 을 하면 쪼갠 것의 두번째 것을 가리킴.
                 * 즉, highmem 영역의 것에 해당하는 bank.
                 *----------------------------------------------------*/
				bank[1].size -= size_limit;
				bank[1].start = vmalloc_limit;
				bank[1].highmem = highmem = 1;
				j++;
			}
			bank->size = size_limit;
		}
#else
		/*
		 * Highmem banks not allowed with !CONFIG_HIGHMEM.
		 */
		if (highmem) {
			printk(KERN_NOTICE "Ignoring RAM at %.8llx-%.8llx "
			       "(!CONFIG_HIGHMEM).\n",
			       (unsigned long long)bank->start,
			       (unsigned long long)bank->start + bank->size - 1);
			continue;
		}

		/*
		 * Check whether this memory bank would partially overlap
		 * the vmalloc area.
		 */
		if (bank->size > size_limit) {
			printk(KERN_NOTICE "Truncating RAM at %.8llx-%.8llx "
			       "to -%.8llx (vmalloc region overlap).\n",
			       (unsigned long long)bank->start,
			       (unsigned long long)bank->start + bank->size - 1,
			       (unsigned long long)bank->start + size_limit - 1);
			bank->size = size_limit;
		}
#endif
		if (!bank->highmem) {
			phys_addr_t bank_end = bank->start + bank->size;

            /*!!C -------------------------------------------------
             * 모든 lowmem bank 중에서 가장 end 값이 큰 것을
             * arm_lowmem_limit 으로 저장.
             *----------------------------------------------------*/
			if (bank_end > arm_lowmem_limit)
				arm_lowmem_limit = bank_end;

			/*
			 * Find the first non-section-aligned page, and point
			 * memblock_limit at it. This relies on rounding the
			 * limit down to be section-aligned, which happens at
			 * the end of this function.
			 *
			 * With this algorithm, the start or end of almost any
			 * bank can be non-section-aligned. The only exception
			 * is that the start of the bank 0 must be section-
			 * aligned, since otherwise memory would need to be
			 * allocated when mapping the start of bank 0, which
			 * occurs before any free memory is mapped.
			 */
			if (!memblock_limit) {
				if (!IS_ALIGNED(bank->start, SECTION_SIZE))
					memblock_limit = bank->start;
				else if (!IS_ALIGNED(bank_end, SECTION_SIZE))
					memblock_limit = bank_end;
			}
		}
		j++;
	}
#ifdef CONFIG_HIGHMEM
	if (highmem) {
		const char *reason = NULL;

		if (cache_is_vipt_aliasing()) {
			/*
			 * Interactions between kmap and other mappings
			 * make highmem support with aliasing VIPT caches
			 * rather difficult.
			 */
			reason = "with VIPT aliasing cache";
		}
		if (reason) {
			printk(KERN_CRIT "HIGHMEM is not supported %s, ignoring high memory\n",
				reason);
			while (j > 0 && meminfo.bank[j - 1].highmem)
				j--;
		}
	}
#endif
	meminfo.nr_banks = j;

    /*!!Q -------------------------------------------------
     * high memory 의 시작지점을 구하는 것 같은데,
     *  1. 무조건 arm_lowmem_limit 의 다음이 high memory 라고
     *     정의되는건가 ? 맨 위쪽의 vmalloc_limit 와 다른점은 ?
     *  2. 왜 -1 을 한 후 +1 을 해주었을까 ? 
     *
     * high_memory는 low memory 영역 끝의 가상주소이다.
     *----------------------------------------------------*/
	high_memory = __va(arm_lowmem_limit - 1) + 1;

	/*
	 * Round the memblock limit down to a section size.  This
	 * helps to ensure that we will allocate memory from the
	 * last full section, which should be mapped.
	 */
	if (memblock_limit)
		memblock_limit = round_down(memblock_limit, SECTION_SIZE);
	if (!memblock_limit)
		memblock_limit = arm_lowmem_limit;

    /*!!C -------------------------------------------------
     * 이 memblock 의 끝(limit)을 current_limit 변수로 설정해둠.
     *----------------------------------------------------*/
	memblock_set_current_limit(memblock_limit);
}

/*!!C -------------------------------------------------
 * 아래 파일의 맨 위쪽 개발자 주석 참조.
 * arch/arm/include/asm/pgtable-2level.h
 *
 * pmd = pgd = L1 page table
 *----------------------------------------------------*/
static inline void prepare_page_table(void)
{
	unsigned long addr;
	phys_addr_t end;

	/*
	 * Clear out all the mappings below the kernel image.
	 */
    /*!!C -------------------------------------------------
     * Virtual Address 0 ~ MODULES_VADDR까지 영역에 대한
     * 페이지테이블 영역 Clear 
     * kernel 아래 부분 user 영역에 대한 clear 이다.
     * PAGE_OFFSET - the virtual address of the start of the kernel image
     * MODULES_VADDR = PAGE_OFFSET - SZ_8M
     *
     * 8 byte 씩 clear 한다. (4 byte 당 1 MB 해당)
     *  - 4 byte 2 개를 0 으로 설정하고,
     *  - tlb flush operation 을 수행한다.
     * 즉, 2 MB 를 담당하는 pgd entry (4byte 2개)를 초기화한다.
     *----------------------------------------------------*/
	for (addr = 0; addr < MODULES_VADDR; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

#ifdef CONFIG_XIP_KERNEL
	/* The XIP kernel is mapped in the module area -- skip over it */
	addr = ((unsigned long)_etext + PMD_SIZE - 1) & PMD_MASK;
#endif

    /*!!C -------------------------------------------------
     * MODULES_VADDR ~ PAGE_OFFSET 영역에 대한 페이지테이블 영역 Clear
     * 즉, module space 에 대한 clear
     *----------------------------------------------------*/
	for ( ; addr < PAGE_OFFSET; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Find the end of the first block of lowmem.
	 */
    /*!!C -------------------------------------------------
     * sanity_check_meminfo() 함수에서 모든 lowmem bank 중
     * 가장 end 값이 큰 것을 arm_lowmem_limit 으로 저장했었음.
     *----------------------------------------------------*/
	end = memblock.memory.regions[0].base + memblock.memory.regions[0].size;
	if (end >= arm_lowmem_limit)
		end = arm_lowmem_limit;

	/*
	 * Clear out all the kernel space mappings, except for the first
	 * memory bank, up to the vmalloc region.
	 */
    /*!!C -------------------------------------------------
     * low memory ~ high memory gap 영역-Gap(최대 8 M)에 대한 clear
     * 왜냐하면 VMALLOC_START 는 8 M align 한 자리이므로.
     *----------------------------------------------------*/
	for (addr = __phys_to_virt(end);
	     addr < VMALLOC_START; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));
}

#ifdef CONFIG_ARM_LPAE
/* the first page is reserved for pgd */
#define SWAPPER_PG_DIR_SIZE	(PAGE_SIZE + \
				 PTRS_PER_PGD * PTRS_PER_PMD * sizeof(pmd_t))
#else
#define SWAPPER_PG_DIR_SIZE	(PTRS_PER_PGD * sizeof(pgd_t))
#endif

/*
 * Reserve the special regions of memory
 */
void __init arm_mm_memblock_reserve(void)
{
	/*
	 * Reserve the page tables.  These are already in use,
	 * and can only be in node 0.
	 */
	memblock_reserve(__pa(swapper_pg_dir), SWAPPER_PG_DIR_SIZE);

#ifdef CONFIG_SA1111
	/*
	 * Because of the SA1111 DMA bug, we want to preserve our
	 * precious DMA-able memory...
	 */
	memblock_reserve(PHYS_OFFSET, __pa(swapper_pg_dir) - PHYS_OFFSET);
#endif
}

/*
 * Set up the device mappings.  Since we clear out the page tables for all
 * mappings above VMALLOC_START, we will remove any debug device mappings.
 * This means you have to be careful how you debug this function, or any
 * called function.  This means you can't use any function or debugging
 * method which may touch any device, otherwise the kernel _will_ crash.
 */
static void __init devicemaps_init(const struct machine_desc *mdesc)
{
	struct map_desc map;
	unsigned long addr;
	void *vectors;

	/*
	 * Allocate the vector page early.
	 */
    /*!!C -------------------------------------------------
     * vector table 을 위해 2 page 를 memblock 에서 할당.
     *----------------------------------------------------*/
	vectors = early_alloc(PAGE_SIZE * 2);

    /*!!C -------------------------------------------------
     * 할당한 8 KB 에 vector, kuser 정보등을 설정하고
     * current_thread 의 domain 을 설정함.
     *----------------------------------------------------*/
	early_trap_init(vectors);

    /*!!C -------------------------------------------------
     * VMALLOC_START 윗부분에 대한 page table entry 모두 clear
     *----------------------------------------------------*/
	for (addr = VMALLOC_START; addr; addr += PMD_SIZE)
		pmd_clear(pmd_off_k(addr));

	/*
	 * Map the kernel if it is XIP.
	 * It is always first in the modulearea.
	 */
#ifdef CONFIG_XIP_KERNEL
	map.pfn = __phys_to_pfn(CONFIG_XIP_PHYS_ADDR & SECTION_MASK);
	map.virtual = MODULES_VADDR;
	map.length = ((unsigned long)_etext - map.virtual + ~SECTION_MASK) & SECTION_MASK;
	map.type = MT_ROM;
	create_mapping(&map);
#endif

	/*
	 * Map the cache flushing regions.
	 */
#ifdef FLUSH_BASE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS);
	map.virtual = FLUSH_BASE;
	map.length = SZ_1M;
	map.type = MT_CACHECLEAN;
	create_mapping(&map);
#endif
#ifdef FLUSH_BASE_MINICACHE
	map.pfn = __phys_to_pfn(FLUSH_BASE_PHYS + SZ_1M);
	map.virtual = FLUSH_BASE_MINICACHE;
	map.length = SZ_1M;
	map.type = MT_MINICLEAN;
	create_mapping(&map);
#endif

	/*
	 * Create a mapping for the machine vectors at the high-vectors
	 * location (0xffff0000).  If we aren't using high-vectors, also
	 * create a mapping at the low-vectors virtual address.
	 */
    /*!!C -------------------------------------------------
     * 할당한 page 중 하나를 가상주소 0xffff0000 로 매핑하여
     * MT_HIGH_VECTORS type 의 메모리로 pte 를 초기화함.
     *----------------------------------------------------*/
	map.pfn = __phys_to_pfn(virt_to_phys(vectors));
	map.virtual = 0xffff0000;
	map.length = PAGE_SIZE;
#ifdef CONFIG_KUSER_HELPERS
	map.type = MT_HIGH_VECTORS;
#else
	map.type = MT_LOW_VECTORS;
#endif
	create_mapping(&map);

    /*!!C -------------------------------------------------
     * control register 를 통해 vector table 이 0xffff0000 에
     * 매핑하도록 설정되어 있지 않은 경우 low vector 로
     * 0x00000000 가상주소로 매핑한다.
     *----------------------------------------------------*/
	if (!vectors_high()) {
		map.virtual = 0;
		map.length = PAGE_SIZE * 2;
		map.type = MT_LOW_VECTORS;
		create_mapping(&map);
	}

	/* Now create a kernel read-only mapping */
    /*!!C -------------------------------------------------
     * 할당한 page 중 나머지 하나 mapping
     *----------------------------------------------------*/
	map.pfn += 1;
	map.virtual = 0xffff0000 + PAGE_SIZE;
	map.length = PAGE_SIZE;
	map.type = MT_LOW_VECTORS;
	create_mapping(&map);

	/*
	 * Ask the machine support to map in the statically mapped devices.
	 */
    /*!!C -------------------------------------------------
     * architecture 의존적인 mapped device 에 대한 메모리 영역에
     * 대한 mapping 작업.
     *----------------------------------------------------*/
	if (mdesc->map_io)
		mdesc->map_io();   /*!!C  exynos_init_io */
	else
		debug_ll_io_init();

    /////// 여기 할 차례 
	fill_pmd_gaps();

	/* Reserve fixed i/o space in VMALLOC region */
	pci_reserve_io();

	/*
	 * Finally flush the caches and tlb to ensure that we're in a
	 * consistent state wrt the writebuffer.  This also ensures that
	 * any write-allocated cache lines in the vector page are written
	 * back.  After this point, we can start to touch devices again.
	 */
	local_flush_tlb_all();
	flush_cache_all();
}

static void __init kmap_init(void)
{
#ifdef CONFIG_HIGHMEM
	pkmap_page_table = early_pte_alloc(pmd_off_k(PKMAP_BASE),
		PKMAP_BASE, _PAGE_KERNEL_TABLE);
#endif
}

static void __init map_lowmem(void)
{
	struct memblock_region *reg;

	/* Map all the lowmem memory banks. */
    /*!!C -------------------------------------------------
     * lowmem 영역에 해당하는 memory region 들에 대해 
     * 돌아가면서 create_mapping 수행 
     *----------------------------------------------------*/
	for_each_memblock(memory, reg) {
		phys_addr_t start = reg->base;
		phys_addr_t end = start + reg->size;
		struct map_desc map;

        /*!!C -------------------------------------------------
         * sanity_check_meminfo 함수에서 모든 lowmem bank 중에서
         * 가장 end 값이 큰 것을 arm_lowmem_limit 으로 저장했었음.
         *----------------------------------------------------*/
		if (end > arm_lowmem_limit)
			end = arm_lowmem_limit;
		if (start >= end)
			break;

        /*!!C -------------------------------------------------
         * pfn = page frame number
         *     = physical addr >> PAGE_SHIFT (4k)
         *----------------------------------------------------*/
		map.pfn = __phys_to_pfn(start);
		map.virtual = __phys_to_virt(start);
		map.length = end - start;
		map.type = MT_MEMORY;

        /*!!C -------------------------------------------------
         * 선택된 low memory region 을 담당하는 page table entry 를
         * MT_MEMORY type 에 근거하여 설정하는 작업 
         *----------------------------------------------------*/
		create_mapping(&map);
	}
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps, and sets up the zero page, bad page and bad page tables.
 */
/*!!C -------------------------------------------------
 * 요거 스터디 진행하면서 VMALLOC_START 는 어디인지,
 * highmem, lowmem 은 어디까지인지 반드시 주소 단위로
 * 그림을 그려볼 것. !!!
 * (memory map 이 머리속에 그려져야 한다.)
 *----------------------------------------------------*/
void __init paging_init(const struct machine_desc *mdesc)
{
	void *zero_page;

    /*!!C -------------------------------------------------
     * 커널이 메모리를 사용하는 모든 용례를 분류화하여 
     * 메모리 타입을 정의하였는데, 이러한 메모리 타입 별로 
     * 속성정보를 저장하고 있는 자료구조가 mem_type 이다.
     * memory 의 type 별로 속성들을 설정해두는 mem_type 
     * 자료구조의 값들을 현재 사용하는 cpu 에 맞게 조정해준다.
     *----------------------------------------------------*/
	build_mem_type_table();

    /*!!C -------------------------------------------------
     * 현재 초기화되지 않은 pgd(pmd) entry 초기화
     * (user 영역(3064 M), module 영역(8 M), VMALLOC gap (8 M))
     *
     * 모기향책 205, 210 page 의 그림 참조.
     *
     * 원래 3-level paging 에서는 entry 가 다음과 같다.
     *
     * +------------+-----------+----------+------------+
     * |   pgd      |    pmd    |   pte    |   offset   |
     * +------------+-----------+----------+------------+
     *              |           |          |<---------->|
     *              |           |            PAGE_SHIFT |
     *              |           |<--------------------->|
     *              |                PMD_SHIFT          |
     *              |<--------------------------------->|
     *                  PGDIR_SHIFT
     *
     * ARM 에서는 2-level paging 을 사용하므로 (pgd = pmd)
     *
     * +----------------+--------------+----------------+
     * |   pgd = pmd    |     pte      |    offset      |
     * +----------------+--------------+----------------+
     *    11 bit [31:21]  9 bit [20:12]   12 bit [11:0]
     *    2048 entry      512 entry
     *    PGDIR_SHIFT = PMD_SHIFT
     * 
     * page table 관련 iamroot 의 내용 쭈욱 읽어볼 것.
     *  http://www.iamroot.org/xe/Kernel_10_ARM/186592#comment_186781
     *  http://studyfoss.egloos.com/viewer/5008142
     *----------------------------------------------------*/
	prepare_page_table();


    /*!!C -------------------------------------------------
     * 참고 !!
     *
     * arch/arm/include/asm/pgtable-2level.h 에 있는 설명처럼
     * ARM H/W 페이지 테이블(2-Level)과 Linux 페이지 테이블(3-Level)을
     * 함께 고려한 그림을 완성해보자.
     * 
     *   16 KB = 8 byte * 2048 entry
     *
     *   11 bit (2048)     9 bit (512)    12 bit (4096)
     * +----------------+--------------+----------------+
     * |      pgd       |     pte      |    offset      |
     * +----------+-----+-------+------+--------+-------+
     *            |             |               |
     *            |             |               |
     *            |             +---------+     +-------------+
     *            |                       |                   |  +------------+
     *            |                       |                   |  |   page     |
     *            |   | kernel image   |  |   +-------------+ +->| ( 4 KB )   |
     * 0xc0008000 |   +----------------+  |   |     pte     |    |            |
     *            |   |                |  +-->|             |--->+------------+
     *   16 KB    |   |      pgd       |      |             |
     *            +-->| (2048 entry)   |----->+-------------+
     *                |section entry 10|       
     * 0xc0004000 +-->+----------------+
     *            |   |                |
     *            |                
     *            +--+
     *               |
     *  init_mm.pgd -+
     *
     *----------------------------------------------------*/

    /*!!C -------------------------------------------------
     * memblock 의 모든 region 중에서 lowmem 에 해당하는
     * region 만 선택하여 mapping 되는 pgd entry 를 초기화함.
     * user 영역, module, gap 영역은 위쪽 prepare_page_table 에서 수행했음.
     * page table 구조를 명확히 이해하고 코드를 보며 쉬울 것임.
     *
     * http://www.iamroot.org/xe/Kernel_10_ARM/186592#comment_186781
     *----------------------------------------------------*/
	map_lowmem();

    /*!!C -------------------------------------------------
     * kernel parameter 로 cma= .. 과 같이 설정되어야 
     * 사용하게 되는데, CMA(Contiguous Memory Allocation)는
     * 현재 사용되지 않는 기법이라고 하고 우리도 사용안함. pass
     *----------------------------------------------------*/
	dma_contiguous_remap();

    /*!!C -------------------------------------------------
     * 벡터테이블용 메모리를 할당하고 매핑한 다음, 특정 영역의
     * pgd 엔트리를 초기화한다.
     *----------------------------------------------------*/
	devicemaps_init(mdesc);
	kmap_init();
	tcm_init();

	top_pmd = pmd_off_k(0xffff0000);

	/* allocate the zero page. */
	zero_page = early_alloc(PAGE_SIZE);

	bootmem_init();

	empty_zero_page = virt_to_page(zero_page);
	__flush_dcache_page(NULL, empty_zero_page);
}
