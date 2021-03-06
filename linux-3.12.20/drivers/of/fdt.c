/*
 * Functions for working with the Flattened Device Tree data format
 *
 * Copyright 2009 Benjamin Herrenschmidt, IBM Corp
 * benh@kernel.crashing.org
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>

#include <asm/setup.h>  /* for COMMAND_LINE_SIZE */
#ifdef CONFIG_PPC
#include <asm/machdep.h>
#endif /* CONFIG_PPC */

#include <asm/page.h>

char *of_fdt_get_string(struct boot_param_header *blob, u32 offset)
{
	return ((char *)blob) +
		be32_to_cpu(blob->off_dt_strings) + offset;
}

/**
 * of_fdt_get_property - Given a node in the given flat blob, return
 * the property ptr 
 * tlv 관련한 사진은 11.29후기(임시쓰레드) 사진 참조
 */
void *of_fdt_get_property(struct boot_param_header *blob,
		       unsigned long node, const char *name,
		       unsigned long *size)
{
	unsigned long p = node;

	do {
		u32 tag = be32_to_cpup((__be32 *)p);
		u32 sz, noff;
		const char *nstr;

		p += 4;
		if (tag == OF_DT_NOP)
			continue;
		if (tag != OF_DT_PROP)
			return NULL;

		sz = be32_to_cpup((__be32 *)p);
		noff = be32_to_cpup((__be32 *)(p + 4));
		p += 8;
		if (be32_to_cpu(blob->version) < 0x10)
			p = ALIGN(p, sz >= 8 ? 8 : 4);

		nstr = of_fdt_get_string(blob, noff);
		if (nstr == NULL) {
			pr_warning("Can't find property index name !\n");
			return NULL;
		}
		if (strcmp(name, nstr) == 0) {
			if (size)
				*size = sz;
			return (void *)p;
		}
		p += sz;
		p = ALIGN(p, 4);
	} while (1);
}

/**
 * of_fdt_is_compatible - Return true if given node from the given blob has
 * compat in its compatible list
 * @blob: A device tree blob
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 *
 * On match, returns a non-zero value with smaller values returned for more
 * specific compatible values.
 */
int of_fdt_is_compatible(struct boot_param_header *blob,
		      unsigned long node, const char *compat)
{
	const char *cp;
	unsigned long cplen, l, score = 0;

	/*!!C
	 * node의 "compatible"에 해당하는 value의 포인터를 가지고 와서
	 * cp에 저장한다. blob은 string의 offset을 사용하기 위해서 필요하다.
	 */
	cp = of_fdt_get_property(blob, node, "compatible", &cplen);
	if (cp == NULL)
		return 0;
	while (cplen > 0) {
		score++;
		/*!!C
		 * cp와 compat의 값을 비교
		 * 비교횟수만큼 score가 올라가나 score의 의미에 대해선 좀 더
		 * 생각해볼 필요가 있음
		 */
		if (of_compat_cmp(cp, compat, strlen(compat)) == 0)
			return score;
		l = strlen(cp) + 1;
		cp += l;
		cplen -= l;
	}

	return 0;
}

/**
 * of_fdt_match - Return true if node matches a list of compatible values
 */
int of_fdt_match(struct boot_param_header *blob, unsigned long node,
                 const char *const *compat)
{
	unsigned int tmp, score = 0;

	if (!compat)
		return 0;

	while (*compat) {
			  /*!!C
			   * blob 과 source 의 compact 에 있는 모든 문자열을 비교한다.
			   * score 가 1이면 0번째 문자열 2면 1 번째 ...
			   */
		tmp = of_fdt_is_compatible(blob, node, *compat);
		if (tmp && (score == 0 || (tmp < score)))
			score = tmp;
		compat++;
	}

	return score;
}

static void *unflatten_dt_alloc(void **mem, unsigned long size,
				       unsigned long align)
{
	void *res;

	*mem = PTR_ALIGN(*mem, align);
	res = *mem;
	*mem += size;

	return res;
}

/**
 * unflatten_dt_node - Alloc and populate a device_node from the flat tree
 * @blob: The parent device tree blob
 * @mem: Memory chunk to use for allocating device nodes and properties
 * @p: pointer to node in flat tree
 * @dad: Parent struct device_node
 * @allnextpp: pointer to ->allnext from last allocated device_node
 * @fpsize: Size of the node path up at the current depth.
 */
static void * unflatten_dt_node(struct boot_param_header *blob,
				void *mem,
				void **p,
				struct device_node *dad,
				struct device_node ***allnextpp,
				unsigned long fpsize)
{
	struct device_node *np;
	struct property *pp, **prev_pp = NULL;
	char *pathp;
	u32 tag;
	unsigned int l, allocl;
	int has_name = 0;
	int new_format = 0;

	tag = be32_to_cpup(*p);
	if (tag != OF_DT_BEGIN_NODE) {
		pr_err("Weird tag at start of node: %x\n", tag);
		return mem;
	}
	*p += 4;
	pathp = *p;
	l = allocl = strlen(pathp) + 1;
	*p = PTR_ALIGN(*p + l, 4);

	/* version 0x10 has a more compact unit name here instead of the full
	 * path. we accumulate the full path size using "fpsize", we'll rebuild
	 * it later. We detect this because the first character of the name is
	 * not '/'.
	 */
	if ((*pathp) != '/') {
		new_format = 1;
		if (fpsize == 0) {
			/* root node: special case. fpsize accounts for path
			 * plus terminating zero. root node only has '/', so
			 * fpsize should be 2, but we want to avoid the first
			 * level nodes to have two '/' so we use fpsize 1 here
			 */
			fpsize = 1;
			allocl = 2;
			l = 1;
			*pathp = '\0';
		} else {
			/* account for '/' and path size minus terminal 0
			 * already in 'l'
			 */
			fpsize += l;
			allocl = fpsize;
		}
	}

	np = unflatten_dt_alloc(&mem, sizeof(struct device_node) + allocl,
				__alignof__(struct device_node));
	if (allnextpp) {
		char *fn;
		np->full_name = fn = ((char *)np) + sizeof(*np);
		if (new_format) {
			/* rebuild full path for new format */
			if (dad && dad->parent) {
				strcpy(fn, dad->full_name);
#ifdef DEBUG
				if ((strlen(fn) + l + 1) != allocl) {
					pr_debug("%s: p: %d, l: %d, a: %d\n",
						pathp, (int)strlen(fn),
						l, allocl);
				}
#endif
				fn += strlen(fn);
			}
			*(fn++) = '/';
		}
		memcpy(fn, pathp, l);

		prev_pp = &np->properties;
		**allnextpp = np;
		*allnextpp = &np->allnext;
		if (dad != NULL) {
			np->parent = dad;
			/* we temporarily use the next field as `last_child'*/
			if (dad->next == NULL)
				dad->child = np;
			else
				dad->next->sibling = np;
			dad->next = np;
		}
		kref_init(&np->kref);
	}
	/* process properties */
	while (1) {
		u32 sz, noff;
		char *pname;

		tag = be32_to_cpup(*p);
		if (tag == OF_DT_NOP) {
			*p += 4;
			continue;
		}
		if (tag != OF_DT_PROP)
			break;
		*p += 4;
		sz = be32_to_cpup(*p);
		noff = be32_to_cpup(*p + 4);
		*p += 8;
		if (be32_to_cpu(blob->version) < 0x10)
			*p = PTR_ALIGN(*p, sz >= 8 ? 8 : 4);

		pname = of_fdt_get_string(blob, noff);
		if (pname == NULL) {
			pr_info("Can't find property name in list !\n");
			break;
		}
		if (strcmp(pname, "name") == 0)
			has_name = 1;
		l = strlen(pname) + 1;
		pp = unflatten_dt_alloc(&mem, sizeof(struct property),
					__alignof__(struct property));
		if (allnextpp) {
			/* We accept flattened tree phandles either in
			 * ePAPR-style "phandle" properties, or the
			 * legacy "linux,phandle" properties.  If both
			 * appear and have different values, things
			 * will get weird.  Don't do that. */
			if ((strcmp(pname, "phandle") == 0) ||
			    (strcmp(pname, "linux,phandle") == 0)) {
				if (np->phandle == 0)
					np->phandle = be32_to_cpup((__be32*)*p);
			}
			/* And we process the "ibm,phandle" property
			 * used in pSeries dynamic device tree
			 * stuff */
			if (strcmp(pname, "ibm,phandle") == 0)
				np->phandle = be32_to_cpup((__be32 *)*p);
			pp->name = pname;
			pp->length = sz;
			pp->value = *p;
			*prev_pp = pp;
			prev_pp = &pp->next;
		}
		*p = PTR_ALIGN((*p) + sz, 4);
	}
	/* with version 0x10 we may not have the name property, recreate
	 * it here from the unit name if absent
	 */
	if (!has_name) {
		char *p1 = pathp, *ps = pathp, *pa = NULL;
		int sz;

		while (*p1) {
			if ((*p1) == '@')
				pa = p1;
			if ((*p1) == '/')
				ps = p1 + 1;
			p1++;
		}
		if (pa < ps)
			pa = p1;
		sz = (pa - ps) + 1;
		pp = unflatten_dt_alloc(&mem, sizeof(struct property) + sz,
					__alignof__(struct property));
		if (allnextpp) {
			pp->name = "name";
			pp->length = sz;
			pp->value = pp + 1;
			*prev_pp = pp;
			prev_pp = &pp->next;
			memcpy(pp->value, ps, sz - 1);
			((char *)pp->value)[sz - 1] = 0;
			pr_debug("fixed up name for %s -> %s\n", pathp,
				(char *)pp->value);
		}
	}
	if (allnextpp) {
		*prev_pp = NULL;
		np->name = of_get_property(np, "name", NULL);
		np->type = of_get_property(np, "device_type", NULL);

		if (!np->name)
			np->name = "<NULL>";
		if (!np->type)
			np->type = "<NULL>";
	}
	while (tag == OF_DT_BEGIN_NODE || tag == OF_DT_NOP) {
		if (tag == OF_DT_NOP)
			*p += 4;
		else
			mem = unflatten_dt_node(blob, mem, p, np, allnextpp,
						fpsize);
		tag = be32_to_cpup(*p);
	}
	if (tag != OF_DT_END_NODE) {
		pr_err("Weird tag at end of node: %x\n", tag);
		return mem;
	}
	*p += 4;
	return mem;
}

/**
 * __unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens a device-tree, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 * @blob: The blob to expand
 * @mynodes: The device_node tree created by the call
 * @dt_alloc: An allocator that provides a virtual address to memory
 * for the resulting tree
 */
/**C
 * device_node::data 필드의 값을 체우는 부분은 모른다.
 * of_allnodes 가 root node
 */
static void __unflatten_device_tree(struct boot_param_header *blob,
			     struct device_node **mynodes,
			     void * (*dt_alloc)(u64 siz, u64 align))
{
	unsigned long size;
	void *start, *mem;
	struct device_node **allnextp = mynodes;

	pr_debug(" -> unflatten_device_tree()\n");

	if (!blob) {
		pr_debug("No device tree pointer\n");
		return;
	}

	pr_debug("Unflattening device tree:\n");
	pr_debug("magic: %08x\n", be32_to_cpu(blob->magic));
	pr_debug("size: %08x\n", be32_to_cpu(blob->totalsize));
	pr_debug("version: %08x\n", be32_to_cpu(blob->version));

	if (be32_to_cpu(blob->magic) != OF_DT_HEADER) {
		pr_err("Invalid device tree blob header\n");
		return;
	}

	/* First pass, scan for size */
	start = ((void *)blob) + be32_to_cpu(blob->off_dt_struct);
	size = (unsigned long)unflatten_dt_node(blob, 0, &start, NULL, NULL, 0);
	size = ALIGN(size, 4);

	pr_debug("  size is %lx, allocating...\n", size);

	/* Allocate memory for the expanded device tree */
	mem = dt_alloc(size + 4, __alignof__(struct device_node));
	memset(mem, 0, size);

	*(__be32 *)(mem + size) = cpu_to_be32(0xdeadbeef);

	pr_debug("  unflattening %p...\n", mem);

	/* Second pass, do actual unflattening */
	start = ((void *)blob) + be32_to_cpu(blob->off_dt_struct);
	unflatten_dt_node(blob, mem, &start, NULL, &allnextp, 0);
	if (be32_to_cpup(start) != OF_DT_END)
		pr_warning("Weird tag at end of tree: %08x\n", be32_to_cpup(start));
	if (be32_to_cpup(mem + size) != 0xdeadbeef)
		pr_warning("End of tree marker overwritten: %08x\n",
			   be32_to_cpup(mem + size));
	*allnextp = NULL;

	pr_debug(" <- unflatten_device_tree()\n");
}

static void *kernel_tree_alloc(u64 size, u64 align)
{
	return kzalloc(size, GFP_KERNEL);
}

/**
 * of_fdt_unflatten_tree - create tree of device_nodes from flat blob
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 */
void of_fdt_unflatten_tree(unsigned long *blob,
			struct device_node **mynodes)
{
	struct boot_param_header *device_tree =
		(struct boot_param_header *)blob;
	__unflatten_device_tree(device_tree, mynodes, &kernel_tree_alloc);
}
EXPORT_SYMBOL_GPL(of_fdt_unflatten_tree);

/* Everything below here references initial_boot_params directly. */
int __initdata dt_root_addr_cells;
int __initdata dt_root_size_cells;

struct boot_param_header *initial_boot_params;

#ifdef CONFIG_OF_EARLY_FLATTREE

/**
 * of_scan_flat_dt - scan flattened tree blob and call callback on each.
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan the flattened device-tree, it is
 * used to extract the memory information at boot before we can
 * unflatten the tree
 *!!C
 * XML 을 참조
 * [tag]  [path]      [tag] [path] [tag] [property] [/tag] [/tag]
 *  beginNodeRoot     beginNode                    endNode endNodeRoot
 *  
 */
int __init of_scan_flat_dt(int (*it)(unsigned long node,
				     const char *uname, int depth,
				     void *data),
			   void *data)
{
	//!!C p = dt root
	unsigned long p = ((unsigned long)initial_boot_params) +
		be32_to_cpu(initial_boot_params->off_dt_struct);
	int rc = 0;
	int depth = -1;

    /*!!C
     * cloudrain21 추가 
     *
     * 결국 아래 코드는 dt root 부터 node 들만 찾아서
     * 각 node 에 맞는 path, depth 등의 값을 구해서
     * callback 함수인 it 를 호출해주기 위한 것이다.
     * callback 함수 it 는 of_scan_flat_dt 함수의 첫번째 인자로 
     * 넘겨주는 함수 이름을 따라가보면 된다.
     */
	do {
		u32 tag = be32_to_cpup((__be32 *)p);
		const char *pathp;

		p += 4;
		if (tag == OF_DT_END_NODE) {
			depth--;
			continue;
		}
		if (tag == OF_DT_NOP)
			continue;
		if (tag == OF_DT_END)
			break;
		if (tag == OF_DT_PROP) {
			u32 sz = be32_to_cpup((__be32 *)p);
			p += 8;
			if (be32_to_cpu(initial_boot_params->version) < 0x10)
				p = ALIGN(p, sz >= 8 ? 8 : 4);
			p += sz;
			p = ALIGN(p, 4);
			continue;
		}
		if (tag != OF_DT_BEGIN_NODE) {
			pr_err("Invalid tag %x in flat device tree!\n", tag);
			return -EINVAL;
		}
		depth++;
		pathp = (char *)p;
		p = ALIGN(p + strlen(pathp) + 1, 4);
		if (*pathp == '/')
			pathp = kbasename(pathp);
		rc = it(p, pathp, depth, data);
		if (rc != 0)
			break;
	} while (1);

	return rc;
}

/**
 * of_get_flat_dt_root - find the root node in the flat blob
 */
unsigned long __init of_get_flat_dt_root(void)
{
	/*!!C
	 * fdt 의 시작점을 찾아준다
	 *  * be32_to_cpu : cpu에 맞는 값으로 변경시켜주는 매크로.
	 */
	unsigned long p = ((unsigned long)initial_boot_params) +
		be32_to_cpu(initial_boot_params->off_dt_struct);

	while (be32_to_cpup((__be32 *)p) == OF_DT_NOP)
		p += 4;
	BUG_ON(be32_to_cpup((__be32 *)p) != OF_DT_BEGIN_NODE);
	p += 4;
	/*!!C
	 * 첫번째 노드를 찾아서 그노드의 처음 스티링을 건너뛴 다음 4byte align 자리 리턴
	 */
	return ALIGN(p + strlen((char *)p) + 1, 4);
}

/**
 * of_get_flat_dt_prop - Given a node in the flat blob, return the property ptr
 *
 * This function can be used within scan_flattened_dt callback to get
 * access to properties
 */
void *__init of_get_flat_dt_prop(unsigned long node, const char *name,
				 unsigned long *size)
{
	return of_fdt_get_property(initial_boot_params, node, name, size);
}

/**
 * of_flat_dt_is_compatible - Return true if given node has compat in compatible list
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 */
int __init of_flat_dt_is_compatible(unsigned long node, const char *compat)
{
	return of_fdt_is_compatible(initial_boot_params, node, compat);
}

/**
 * of_flat_dt_match - Return true if node matches a list of compatible values
 */
int __init of_flat_dt_match(unsigned long node, const char *const *compat)
{
	return of_fdt_match(initial_boot_params, node, compat);
}

struct fdt_scan_status {
	const char *name;
	int namelen;
	int depth;
	int found;
	int (*iterator)(unsigned long node, const char *uname, int depth, void *data);
	void *data;
};

/**
 * fdt_scan_node_by_path - iterator for of_scan_flat_dt_by_path function
 */
static int __init fdt_scan_node_by_path(unsigned long node, const char *uname,
					int depth, void *data)
{
	struct fdt_scan_status *st = data;

	/*
	 * if scan at the requested fdt node has been completed,
	 * return -ENXIO to abort further scanning
	 */
	if (depth <= st->depth)
		return -ENXIO;

	/* requested fdt node has been found, so call iterator function */
	if (st->found)
		return st->iterator(node, uname, depth, st->data);

	/* check if scanning automata is entering next level of fdt nodes */
	if (depth == st->depth + 1 &&
	    strncmp(st->name, uname, st->namelen) == 0 &&
	    uname[st->namelen] == 0) {
		st->depth += 1;
		if (st->name[st->namelen] == 0) {
			st->found = 1;
		} else {
			const char *next = st->name + st->namelen + 1;
			st->name = next;
			st->namelen = strcspn(next, "/");
		}
		return 0;
	}

	/* scan next fdt node */
	return 0;
}

/**
 * of_scan_flat_dt_by_path - scan flattened tree blob and call callback on each
 *			     child of the given path.
 * @path: path to start searching for children
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan the flattened device-tree starting from the
 * node given by path. It is used to extract information (like reserved
 * memory), which is required on ealy boot before we can unflatten the tree.
 */
int __init of_scan_flat_dt_by_path(const char *path,
	int (*it)(unsigned long node, const char *name, int depth, void *data),
	void *data)
{
	struct fdt_scan_status st = {path, 0, -1, 0, it, data};
	int ret = 0;

	if (initial_boot_params)
                ret = of_scan_flat_dt(fdt_scan_node_by_path, &st);

	if (!st.found)
		return -ENOENT;
	else if (ret == -ENXIO)	/* scan has been completed */
		return 0;
	else
		return ret;
}

#ifdef CONFIG_BLK_DEV_INITRD
/**
 * early_init_dt_check_for_initrd - Decode initrd location from flat tree
 * @node: reference to node containing initrd location ('chosen')
 */
/*!!C
 * cloudrain21
 *
 * node 정보에서 initrd start, end 프로퍼티를 읽어서
 * initrd 관련 global 변수에 설정해준다.
 */
void __init early_init_dt_check_for_initrd(unsigned long node)
{
	u64 start, end;
	unsigned long len;
	__be32 *prop;

	pr_debug("Looking for initrd properties... ");

    /*!!C -------------------------------------------------
     * node 의 property 에서 linux,initrd-start 키에 해당하는
     * value 를 찾아서 prop 으로 리턴한다.
     *----------------------------------------------------*/
	prop = of_get_flat_dt_prop(node, "linux,initrd-start", &len);
	if (!prop)
		return;

    /*!!C -------------------------------------------------
     * 32bit 이든 64bit 이든 value 들 여러개 설정된 것을 
     * 마지막 2 개 4byte 값들을 64 bit 하나로 합쳐서 리턴.
     *
     * 0x11111111,0x22222222,0x33333333,0x44444444
     * -> return 0x3333333344444444
     *----------------------------------------------------*/
	start = of_read_number(prop, len/4);

	prop = of_get_flat_dt_prop(node, "linux,initrd-end", &len);
	if (!prop)
		return;
	end = of_read_number(prop, len/4);

    /*!!C -------------------------------------------------
     * 결국 위에서 구한 start 와 end 64 bit 값을 이용하여
     * initrd start, size 를 구한다.
     * 32 bit 일 경우에는 u32 로 짤라내기 때문에 마지막
     * 4 byte 설정값이 start 에 들어가게 된다.
     *----------------------------------------------------*/
	early_init_dt_setup_initrd_arch(start, end);
	pr_debug("initrd_start=0x%llx  initrd_end=0x%llx\n",
		 (unsigned long long)start, (unsigned long long)end);
}
#else
inline void early_init_dt_check_for_initrd(unsigned long node)
{
}
#endif /* CONFIG_BLK_DEV_INITRD */

/**
 * early_init_dt_scan_root - fetch the top level address and size cells
 */

/*!!C
 * cloudrain21
 *
 *  node 의 프로퍼티 중에서 #size-cells 와 #address-cells 값을 읽어서
 *  global 변수에 저장한다.
 *  cell 에 대한 좀더 정확한 의미 파악 필요.
 *
 *  http://forum.falinux.com/zbxe/?mid=lecture_tip&comment_srl=518031&l=es&sort_index=readed_count&order_type=asc&document_srl=784583
 *  http://www.devicetree.org/Device_Tree_Usage
 */
int __init early_init_dt_scan_root(unsigned long node, const char *uname,
				   int depth, void *data)
{
	__be32 *prop;

	if (depth != 0)
		return 0;

	dt_root_size_cells = OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
	dt_root_addr_cells = OF_ROOT_NODE_ADDR_CELLS_DEFAULT;

	prop = of_get_flat_dt_prop(node, "#size-cells", NULL);
	if (prop)
		dt_root_size_cells = be32_to_cpup(prop);
	pr_debug("dt_root_size_cells = %x\n", dt_root_size_cells);

	prop = of_get_flat_dt_prop(node, "#address-cells", NULL);
	if (prop)
		dt_root_addr_cells = be32_to_cpup(prop);
	pr_debug("dt_root_addr_cells = %x\n", dt_root_addr_cells);

	/* break now */
	return 1;
}

/*!!C next cell의 value를 읽어들임? */
u64 __init dt_mem_next_cell(int s, __be32 **cellp)
{
	__be32 *p = *cellp;
    /* reg는 s만큼의 크기로 늘어남 (4byte로 추정) */

	*cellp = p + s;
	return of_read_number(p, s);
}

/**
 * early_init_dt_scan_memory - Look for an parse memory nodes
 */
/*!!C
 * cloudrain21
 *
 * memory 프로퍼티에 설정된 memory cell 들 중
 * 한 쌍의 주소와 크기를 bank 라고 함.
 * 이러한 bank 는 여러개, 즉 여러 cell 일 수 있고 이러한
 * 여러 bank 설정들은 global 자료구조인 meminfo 에 저장된다.
 */
/*!!C __init .init.data section에 저장해두고 사용 */
int __init early_init_dt_scan_memory(unsigned long node, const char *uname,
				     int depth, void *data)
{
    /* device_type이라는 node를 찾음 */
	char *type = of_get_flat_dt_prop(node, "device_type", NULL);
	__be32 *reg, *endp; // Cells
	unsigned long l;

	/* We are scanning "memory" nodes only */
	if (type == NULL) {
		/*
		 * The longtrail doesn't have a device_type on the
		 * /memory node, so look for the node called /memory@0.
		 */
        /*!!C depth가 1이 아니거나, node path가 memory@0가 아니면 리턴 */
		if (depth != 1 || strcmp(uname, "memory@0") != 0)
			return 0;
	} 
        /*!!C 타입이 memory가 아니면 역시 리턴 */
    else if (strcmp(type, "memory") != 0)
		return 0;

    /*!!C 아래는 memory type이거나 
       (depth는 1이어야 함) 
       node path가 memory@0 (longtrail)일 때 수행 */

	reg = of_get_flat_dt_prop(node, "linux,usable-memory", &l);
	if (reg == NULL) {
        /*!!C 우리는 이걸 타게 됨. exynos5420에는 linux,usable-memory가 없음*/
		reg = of_get_flat_dt_prop(node, "reg", &l); 
        /*!!C reg값을 읽어 옴 */
    }
	if (reg == NULL)
		return 0; // 답 없음

	endp = reg + (l / sizeof(__be32));
    /*!!C reg value의 Byte수를 4byte 단위의 cell 수로 변환해서 endp에 저장 */ 

	pr_debug("memory scan node %s, reg size %ld, data: %x %x %x %x,\n",
	    uname, l, reg[0], reg[1], reg[2], reg[3]);

    /*!!C Cell size >= dt_root_addr_cells + dt_root_size_cells 
      dt_root_addr_cells = #address-cells
      dt_root_size_cells = #size-cells
     */
    
	while ((endp - reg) >= (dt_root_addr_cells + dt_root_size_cells)) {
		u64 base, size;

		base = dt_mem_next_cell(dt_root_addr_cells, &reg);
		size = dt_mem_next_cell(dt_root_size_cells, &reg);

		if (size == 0)
			continue;
		pr_debug(" - %llx ,  %llx\n", (unsigned long long)base,
		    (unsigned long long)size);

		early_init_dt_add_memory_arch(base, size);
	}

	return 0;
}
/*!!C
 * Node chosen ( bootargs = "console=ttySAC2,115200 init=/linuxrc";)
 * arch/arm/boot/dts/exynos5-smdk5420.dts
 *
 * cloudrain21
 *  node 가 chosen 노드일 때 initrd 관련 설정을 읽어서 global 에 초기화하고,
 *  boot argument 프로퍼티가 있을 경우 이 value 를 global 변수인
 *  .init.data section 의 boot_command_line 에 넣어준다.
 *  이 boot_command_line string 은 후에 command 를 parse 할 때 사용될 것이다.
 */
int __init early_init_dt_scan_chosen(unsigned long node, const char *uname,
				     int depth, void *data)
{
	unsigned long l;
	char *p;

	pr_debug("search \"chosen\", depth: %d, uname: %s\n", depth, uname);

	if (depth != 1 || !data ||
	    (strcmp(uname, "chosen") != 0 && strcmp(uname, "chosen@0") != 0))
		return 0;

    /*!!C
     * chosen node 일 때만 이곳까지 올 것이고
     * 이 때의 node 는 chosen node 의 첫 property 를 가리킴.
     * chosen node 의 property 중에서 initrd 설정을 찾아서
     * global initrd 관련 start,size 변수에 넣어준다.
     */
	early_init_dt_check_for_initrd(node); /*!!C 14.11.29 */

	/* Retrieve command line */
    /*!!C
     * bootargs 에 해당하는 value 를 boot_command_line 에 저장.
     */
	p = of_get_flat_dt_prop(node, "bootargs", &l);
	if (p != NULL && l > 0)
		strlcpy(data, p, min((int)l, COMMAND_LINE_SIZE));

	/*
	 * CONFIG_CMDLINE is meant to be a default in case nothing else
	 * managed to set the command line, unless CONFIG_CMDLINE_FORCE
	 * is set in which case we override whatever was found earlier.
	 */
#ifdef CONFIG_CMDLINE
#ifndef CONFIG_CMDLINE_FORCE
	if (!((char *)data)[0])
#endif
        /*!!C
         * bootargs property 에서 가져온 것이 없다면
         * config 에 설정한 값을 boot_command_line 에 저장해둔다.
         */
		strlcpy(data, CONFIG_CMDLINE, COMMAND_LINE_SIZE);
#endif /* CONFIG_CMDLINE */

	pr_debug("Command line is: %s\n", (char*)data);

	/* break now */
	return 1;
}

#ifdef CONFIG_HAVE_MEMBLOCK
/*
 * called from unflatten_device_tree() to bootstrap devicetree itself
 * Architectures can override this definition if memblock isn't used
 */
void * __init __weak early_init_dt_alloc_memory_arch(u64 size, u64 align)
{
	return __va(memblock_alloc(size, align));
}
#endif

/**
 * unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 */
void __init unflatten_device_tree(void)
{
	__unflatten_device_tree(initial_boot_params, &of_allnodes,
				early_init_dt_alloc_memory_arch);

	/* Get pointer to "/chosen" and "/aliases" nodes for use everywhere */
	of_alias_scan(early_init_dt_alloc_memory_arch);
}

#endif /* CONFIG_OF_EARLY_FLATTREE */
