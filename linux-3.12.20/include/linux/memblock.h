#ifndef _LINUX_MEMBLOCK_H
#define _LINUX_MEMBLOCK_H
#ifdef __KERNEL__

#ifdef CONFIG_HAVE_MEMBLOCK
/*
 * Logical memory blocks.
 *
 * Copyright (C) 2001 Peter Bergner, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/mm.h>

#define INIT_MEMBLOCK_REGIONS	128

struct memblock_region {
	phys_addr_t base;
	phys_addr_t size;
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
	int nid;
#endif
};

struct memblock_type {
	unsigned long cnt;	/* number of regions */
	unsigned long max;	/* size of the allocated array */
	phys_addr_t total_size;	/* size of all regions */
	struct memblock_region *regions;
};

struct memblock {
	phys_addr_t current_limit;

    /*!!C -------------------------------------------------
     * 물리적으로 쓸수 있는 영역 설정.
     * 여기에는 물리적인 메모리 덩어리들(region) 정보가 설정되어 있음.
     * 메모리가 필요할 때는 여기에서 짤라서 사용하면 되는 것.
     * 다만 이 덩어리안에는 reserved 영역들이 있는데
     * 이 부분들은 할당하여 사용하면 안되는 page table, kernel image 
     * dtb 등을 위해 예약된 공간이다.
     *----------------------------------------------------*/
	struct memblock_type memory;

    /*!!C -------------------------------------------------
     * 사용하지 말아야할 부분 설정.
     * kernel, page table, dtb 등을 위해 예약해둔 공간에 대한
     * 정보가 설정된다. 또한 vector table 등 신규로 할당되는
     * 메모리 영역도 여기에 등록된다.
     * 이 영역들도 물리메모리이므로 물론 위의 memory region 들에
     * 속하게 된다.
     *----------------------------------------------------*/
	struct memblock_type reserved;
};

extern struct memblock memblock;
extern int memblock_debug;

#define memblock_dbg(fmt, ...) \
	if (memblock_debug) printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)

phys_addr_t memblock_find_in_range_node(phys_addr_t start, phys_addr_t end,
				phys_addr_t size, phys_addr_t align, int nid);
phys_addr_t memblock_find_in_range(phys_addr_t start, phys_addr_t end,
				   phys_addr_t size, phys_addr_t align);
phys_addr_t get_allocated_memblock_reserved_regions_info(phys_addr_t *addr);
void memblock_allow_resize(void);
int memblock_add_node(phys_addr_t base, phys_addr_t size, int nid);
int memblock_add(phys_addr_t base, phys_addr_t size);
int memblock_remove(phys_addr_t base, phys_addr_t size);
int memblock_free(phys_addr_t base, phys_addr_t size);
int memblock_reserve(phys_addr_t base, phys_addr_t size);
void memblock_trim_memory(phys_addr_t align);

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
int memblock_search_pfn_nid(unsigned long pfn, unsigned long *start_pfn,
			    unsigned long  *end_pfn);
void __next_mem_pfn_range(int *idx, int nid, unsigned long *out_start_pfn,
			  unsigned long *out_end_pfn, int *out_nid);

/**
 * for_each_mem_pfn_range - early memory pfn range iterator
 * @i: an integer used as loop variable
 * @nid: node selector, %MAX_NUMNODES for all nodes
 * @p_start: ptr to ulong for start pfn of the range, can be %NULL
 * @p_end: ptr to ulong for end pfn of the range, can be %NULL
 * @p_nid: ptr to int for nid of the range, can be %NULL
 *
 * Walks over configured memory ranges.
 */
#define for_each_mem_pfn_range(i, nid, p_start, p_end, p_nid)		\
	for (i = -1, __next_mem_pfn_range(&i, nid, p_start, p_end, p_nid); \
	     i >= 0; __next_mem_pfn_range(&i, nid, p_start, p_end, p_nid))
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

void __next_free_mem_range(u64 *idx, int nid, phys_addr_t *out_start,
			   phys_addr_t *out_end, int *out_nid);

/**
 * for_each_free_mem_range - iterate through free memblock areas
 * @i: u64 used as loop variable
 * @nid: node selector, %MAX_NUMNODES for all nodes
 * @p_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @p_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @p_nid: ptr to int for nid of the range, can be %NULL
 *
 * Walks over free (memory && !reserved) areas of memblock.  Available as
 * soon as memblock is initialized.
 */
#define for_each_free_mem_range(i, nid, p_start, p_end, p_nid)		\
	for (i = 0,							\
	     __next_free_mem_range(&i, nid, p_start, p_end, p_nid);	\
	     i != (u64)ULLONG_MAX;					\
	     __next_free_mem_range(&i, nid, p_start, p_end, p_nid))

void __next_free_mem_range_rev(u64 *idx, int nid, phys_addr_t *out_start,
			       phys_addr_t *out_end, int *out_nid);

/**
 * for_each_free_mem_range_reverse - rev-iterate through free memblock areas
 * @i: u64 used as loop variable
 * @nid: node selector, %MAX_NUMNODES for all nodes
 * @p_start: ptr to phys_addr_t for start address of the range, can be %NULL
 * @p_end: ptr to phys_addr_t for end address of the range, can be %NULL
 * @p_nid: ptr to int for nid of the range, can be %NULL
 *
 * Walks over free (memory && !reserved) areas of memblock in reverse
 * order.  Available as soon as memblock is initialized.
 */
/*!!C -------------------------------------------------
 * 이 for each 문은 looping 을 돌면서 p_start 와 p_end 를
 * 얻어내는 매크로이다. free memblock 을 찾아서 start 와 
 * end 를 찾아내는 것이다.
 * for 문 처음 시작할 때 __next_free_mem_range_rev 를 
 * 호출하고, 매번 looping 하면서 같은 __next_free_mem_range_rev 
 * 함수를 호출한다.
 * 이 함수 내에서 다음 looping 을 결정하는 i 의 값이 결정된다.
 * i 값은 메모리를 할당할 영역을 찾으면 변경된다.
 *----------------------------------------------------*/
#define for_each_free_mem_range_reverse(i, nid, p_start, p_end, p_nid)	\
	for (i = (u64)ULLONG_MAX,					\
	     __next_free_mem_range_rev(&i, nid, p_start, p_end, p_nid);	\
	     i != (u64)ULLONG_MAX;					\
	     __next_free_mem_range_rev(&i, nid, p_start, p_end, p_nid))

#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
int memblock_set_node(phys_addr_t base, phys_addr_t size, int nid);

static inline void memblock_set_region_node(struct memblock_region *r, int nid)
{
	r->nid = nid;
}

static inline int memblock_get_region_node(const struct memblock_region *r)
{
	return r->nid;
}
#else
static inline void memblock_set_region_node(struct memblock_region *r, int nid)
{
}

static inline int memblock_get_region_node(const struct memblock_region *r)
{
	return 0;
}
#endif /* CONFIG_HAVE_MEMBLOCK_NODE_MAP */

phys_addr_t memblock_alloc_nid(phys_addr_t size, phys_addr_t align, int nid);
phys_addr_t memblock_alloc_try_nid(phys_addr_t size, phys_addr_t align, int nid);

phys_addr_t memblock_alloc(phys_addr_t size, phys_addr_t align);

/* Flags for memblock_alloc_base() amd __memblock_alloc_base() */
#define MEMBLOCK_ALLOC_ANYWHERE	(~(phys_addr_t)0)
#define MEMBLOCK_ALLOC_ACCESSIBLE	0

phys_addr_t memblock_alloc_base(phys_addr_t size, phys_addr_t align,
				phys_addr_t max_addr);
phys_addr_t __memblock_alloc_base(phys_addr_t size, phys_addr_t align,
				  phys_addr_t max_addr);
phys_addr_t memblock_phys_mem_size(void);
phys_addr_t memblock_mem_size(unsigned long limit_pfn);
phys_addr_t memblock_start_of_DRAM(void);
phys_addr_t memblock_end_of_DRAM(void);
void memblock_enforce_memory_limit(phys_addr_t memory_limit);
int memblock_is_memory(phys_addr_t addr);
int memblock_is_region_memory(phys_addr_t base, phys_addr_t size);
int memblock_is_reserved(phys_addr_t addr);
int memblock_is_region_reserved(phys_addr_t base, phys_addr_t size);

extern void __memblock_dump_all(void);

static inline void memblock_dump_all(void)
{
	if (memblock_debug)
		__memblock_dump_all();
}

/**
 * memblock_set_current_limit - Set the current allocation limit to allow
 *                         limiting allocations to what is currently
 *                         accessible during boot
 * @limit: New limit value (physical address)
 */
void memblock_set_current_limit(phys_addr_t limit);


/*
 * pfn conversion functions
 *
 * While the memory MEMBLOCKs should always be page aligned, the reserved
 * MEMBLOCKs may not be. This accessor attempt to provide a very clear
 * idea of what they return for such non aligned MEMBLOCKs.
 */

/**
 * memblock_region_memory_base_pfn - Return the lowest pfn intersecting with the memory region
 * @reg: memblock_region structure
 */
static inline unsigned long memblock_region_memory_base_pfn(const struct memblock_region *reg)
{
	return PFN_UP(reg->base);
}

/**
 * memblock_region_memory_end_pfn - Return the end_pfn this region
 * @reg: memblock_region structure
 */
static inline unsigned long memblock_region_memory_end_pfn(const struct memblock_region *reg)
{
	return PFN_DOWN(reg->base + reg->size);
}

/**
 * memblock_region_reserved_base_pfn - Return the lowest pfn intersecting with the reserved region
 * @reg: memblock_region structure
 */
static inline unsigned long memblock_region_reserved_base_pfn(const struct memblock_region *reg)
{
	return PFN_DOWN(reg->base);
}

/**
 * memblock_region_reserved_end_pfn - Return the end_pfn this region
 * @reg: memblock_region structure
 */
static inline unsigned long memblock_region_reserved_end_pfn(const struct memblock_region *reg)
{
	return PFN_UP(reg->base + reg->size);
}

#define for_each_memblock(memblock_type, region)					\
	for (region = memblock.memblock_type.regions;				\
	     region < (memblock.memblock_type.regions + memblock.memblock_type.cnt);	\
	     region++)


#ifdef CONFIG_ARCH_DISCARD_MEMBLOCK
#define __init_memblock __meminit
#define __initdata_memblock __meminitdata
#else
#define __init_memblock
#define __initdata_memblock
#endif

#else
static inline phys_addr_t memblock_alloc(phys_addr_t size, phys_addr_t align)
{
	return 0;
}

#endif /* CONFIG_HAVE_MEMBLOCK */

#endif /* __KERNEL__ */

#endif /* _LINUX_MEMBLOCK_H */
