/*
 *  linux/arch/arm/kernel/devtree.c
 *
 *  Copyright (C) 2009 Canonical Ltd. <jeremy.kerr@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>

#include <asm/cputype.h>
#include <asm/setup.h>
#include <asm/page.h>
#include <asm/smp_plat.h>
#include <asm/mach/arch.h>
#include <asm/mach-types.h>

void __init early_init_dt_add_memory_arch(u64 base, u64 size)
{
	arm_add_memory(base, size);
}

void * __init early_init_dt_alloc_memory_arch(u64 size, u64 align)
{
	return alloc_bootmem_align(size, align);
}

void __init arm_dt_memblock_reserve(void)
{
	u64 *reserve_map, base, size;

	if (!initial_boot_params)
		return;

	/* Reserve the dtb region */
	memblock_reserve(virt_to_phys(initial_boot_params),
			 be32_to_cpu(initial_boot_params->totalsize));

	/*
	 * Process the reserve map.  This will probably overlap the initrd
	 * and dtb locations which are already reserved, but overlaping
	 * doesn't hurt anything
	 */
	reserve_map = ((void*)initial_boot_params) +
			be32_to_cpu(initial_boot_params->off_mem_rsvmap);
	while (1) {
		base = be64_to_cpup(reserve_map++);
		size = be64_to_cpup(reserve_map++);
		if (!size)
			break;
		memblock_reserve(base, size);
	}
}

/*
 * arm_dt_init_cpu_maps - Function retrieves cpu nodes from the device tree
 * and builds the cpu logical map array containing MPIDR values related to
 * logical cpus
 *
 * Updates the cpu possible mask with the number of parsed cpu nodes
 */
void __init arm_dt_init_cpu_maps(void)
{
	/*
	 * Temp logical map is initialized with UINT_MAX values that are
	 * considered invalid logical map entries since the logical map must
	 * contain a list of MPIDR[23:0] values where MPIDR[31:24] must
	 * read as 0.
	 */
	struct device_node *cpu, *cpus;
	u32 i, j, cpuidx = 1;
	u32 mpidr = is_smp() ? read_cpuid_mpidr() & MPIDR_HWID_BITMASK : 0;

	u32 tmp_map[NR_CPUS] = { [0 ... NR_CPUS-1] = MPIDR_INVALID };
	bool bootcpu_valid = false;
	cpus = of_find_node_by_path("/cpus");

	if (!cpus)
		return;

	for_each_child_of_node(cpus, cpu) {
		u32 hwid;

		if (of_node_cmp(cpu->type, "cpu"))
			continue;

		pr_debug(" * %s...\n", cpu->full_name);
		/*
		 * A device tree containing CPU nodes with missing "reg"
		 * properties is considered invalid to build the
		 * cpu_logical_map.
		 */
		if (of_property_read_u32(cpu, "reg", &hwid)) {
			pr_debug(" * %s missing reg property\n",
				     cpu->full_name);
			return;
		}

		/*
		 * 8 MSBs must be set to 0 in the DT since the reg property
		 * defines the MPIDR[23:0].
		 */
		if (hwid & ~MPIDR_HWID_BITMASK)
			return;

		/*
		 * Duplicate MPIDRs are a recipe for disaster.
		 * Scan all initialized entries and check for
		 * duplicates. If any is found just bail out.
		 * temp values were initialized to UINT_MAX
		 * to avoid matching valid MPIDR[23:0] values.
		 */
		for (j = 0; j < cpuidx; j++)
			if (WARN(tmp_map[j] == hwid, "Duplicate /cpu reg "
						     "properties in the DT\n"))
				return;

		/*
		 * Build a stashed array of MPIDR values. Numbering scheme
		 * requires that if detected the boot CPU must be assigned
		 * logical id 0. Other CPUs get sequential indexes starting
		 * from 1. If a CPU node with a reg property matching the
		 * boot CPU MPIDR is detected, this is recorded so that the
		 * logical map built from DT is validated and can be used
		 * to override the map created in smp_setup_processor_id().
		 */
		if (hwid == mpidr) {
			i = 0;
			bootcpu_valid = true;
		} else {
			i = cpuidx++;
		}

		if (WARN(cpuidx > nr_cpu_ids, "DT /cpu %u nodes greater than "
					       "max cores %u, capping them\n",
					       cpuidx, nr_cpu_ids)) {
			cpuidx = nr_cpu_ids;
			break;
		}

		tmp_map[i] = hwid;
	}

	if (!bootcpu_valid) {
		pr_warn("DT missing boot CPU MPIDR[23:0], fall back to default cpu_logical_map\n");
		return;
	}

	/*
	 * Since the boot CPU node contains proper data, and all nodes have
	 * a reg property, the DT CPU list can be considered valid and the
	 * logical map created in smp_setup_processor_id() can be overridden
	 */
	for (i = 0; i < cpuidx; i++) {
		set_cpu_possible(i, true);
		cpu_logical_map(i) = tmp_map[i];
		pr_debug("cpu logical map 0x%x\n", cpu_logical_map(i));
	}
}

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

/**
 * setup_machine_fdt - Machine setup when an dtb was passed to the kernel
 * @dt_phys: physical address of dt blob
 *
 * If a dtb was passed to the kernel in r2, then use it to choose the
 * correct machine_desc and to setup the system.
 *
 * dtb 가 r2 레지스터를 통해 kernel 에 전달되면, 해당 dtb 는
 * 적합한 machine desc 를 선택하여 system 을 초기화하고 셋업하는데 사용된다.
 */

/*!!C -------------------------------------------------
 * 아래 그림이 dtb binary 의 구조이다.
 *
 * dtb base -> +----------------------------+
 *             |  struct boot_param_header  |
 *             |     - magic                |
 *             |     - totalsize            |
 *             |     - off_dt_sturct -----------+
 *             |     - off_dt_string --------------+
 *             |       ...                  |   |  |
 *             +----------------------------+   |  |
 *             |      memory reserve map    |   |  |
 *             +----------------------------+ <-+  |
 *             |                            |      |
 *             |  dt struct (tlv)           |      |
 *             |     - tag (4 byte)         |      |         tag   = 이번 tlv 가 노드의 시작인지, property 인지.. 
 *             |     - size (4 byte)        |      |         size  = length of value
 *             |     - noff (4 byte) ------------------+     noff  = start of property name string
 *             |     - value (size byte)    |      |   |     value = property value
 *             |                            |      |   |
 *             |     - tag                  |      |   |
 *             |     - size                 |      |   |
 *             |     - noff ------------------------------+
 *             |     - value                |      |   |  |
 *             |                            |      |   |  |
 *             |     ...                    |      |   |  |
 *             +----------------------------+ <----+   |  |
 *             |                            |          |  |
 *             |  dt string                 |          |  |
 *             |     - key name str 1 <----------------+  |
 *             |     - key name str 2 <-------------------+
 *             |     ...                    |
 *         +-> +----------------------------+
 *         |
 *         +-- base + totalsize
 *
 * dt struct 부분은 tlv 의 연속이라고 보면 된다.
 * dt struct 에서 각 요소의 의미는 다음과 같다.
 *  - tag   = 이번 tlv 가 노드의 시작인지, property 인지, 노드의 끝인지를 나타낸다.
 *  - size  = value 의 길이 
 *  - noff  = key string 이 저장된 위치 
 *  - value = value 값이 저장된다.
 *
 * 사용자가 arch/arm/boot/dts 디렉토리의 dts(dtsi) 파일에
 * 마치 xml 과 같이 시스템 설정들을 해놓으면
 * dtc 라는 컴파일러를 통해 위와 같은 구조의 dtb binary 가 만들어진다.
 *
 * dt struct 의 tag 값은 다음과 같은 것들이 올 수 있다.
 *   #define OF_DT_BEGIN_NODE	0x1         // 노드의 시작 
 *   #define OF_DT_END_NODE		0x2         // 노드의 끝 
 *   #define OF_DT_PROP		    0x3         // property tag
 *   #define OF_DT_NOP		    0x4         // nothing
 *
 * dts 파일을 xml 형태로 생각해보면 쉽다.
 * 이렇게 xml 형식으로 이해해보면 dtb 에 flat 하게 붙여진 정보들은
 * 결국 tree 형태로 재구성할 수 있다는 것을 예측할 수 있다.
 * 모든 노드에는 property 가 있을 수 있고, 하위 노드가 포함될 수 있다.
 *
 *   [tag]                      : tag = OF_DT_BEGIN_NODE, root 노드 시작 
 *       "/"                    : root 노드의 path(key)는 "/"
 *       [tag]                  : tag = OF_DT_PROP, root 노드의 property 
 *           "samsung,smdk5420" : property value     
 *       [tag]                  : tag = OF_DT_BEGIN_NODE, 하위노드의 시작  
 *           "memory"           : 하위노드의 path 는 "memory"
 *           [tag]              : tag = OF_DT_PROP, property value tag
 *               "prop value"
 *           [tag]              : tag = OF_DT_PROP, property value tag
 *               "prop value"
 *           [tag]              : tag = OF_DT_PROP, property value tag
 *               "prop value"
 *       [/tag]                 : tag = OF_DT_END_NODE, 하위노드 끝 
 *   [/tag]                     : tag = OF_DT_END_NODE, root 노드 끝 
 *----------------------------------------------------*/
const struct machine_desc * __init setup_machine_fdt(unsigned int dt_phys)
{
	struct boot_param_header *devtree;
	const struct machine_desc *mdesc, *mdesc_best = NULL;
	unsigned int score, mdesc_score = ~1;
	unsigned long dt_root;
	const char *model;

#ifdef CONFIG_ARCH_MULTIPLATFORM
	DT_MACHINE_START(GENERIC_DT, "Generic DT based system")
	MACHINE_END

	mdesc_best = &__mach_desc_GENERIC_DT;
#endif

	if (!dt_phys)
		return NULL;

	devtree = phys_to_virt(dt_phys);

	/* check device tree validity */
	/*!!C
	 * be32_to_cpu()는 big endian 32bit를 읽어서 CPU가 인식할 수
	 * 있는 값으로 변환시켜줌
	 */
	if (be32_to_cpu(devtree->magic) != OF_DT_HEADER)
		return NULL;

	/* Search the mdescs for the 'best' compatible value match */
	initial_boot_params = devtree;

	/*!!C
	 * 우선 다음 소스는 dt_struct 의 시작점을 가리킨다 나머지는 dtb 특집필요 
	 * 처음 정보는 property 일듯하다.
	 */
	dt_root = of_get_flat_dt_root();

	/*!!C
	 * 컴파일 과정에 arch.info.init 섹션에 각 machine의 description을
	 * 넣어놓고 본 함수에서 받은 device tree blobb 와 비교하여 가장 적은 score 를 
	 * 받는 machine description을 선택한다
	 *  best = 가장작은 스코어
	 */
	for_each_machine_desc(mdesc) {
		score = of_flat_dt_match(dt_root, mdesc->dt_compat);
		if (score > 0 && score < mdesc_score) {
			mdesc_best = mdesc;
			mdesc_score = score;
		}
	}
	if (!mdesc_best) {
		const char *prop;
		long size;

		early_print("\nError: unrecognized/unsupported "
			    "device tree compatible list:\n[ ");

		prop = of_get_flat_dt_prop(dt_root, "compatible", &size);
		while (size > 0) {
			early_print("'%s' ", prop);
			size -= strlen(prop) + 1;
			prop += strlen(prop) + 1;
		}
		early_print("]\n\n");

		dump_machine_table(); /* does not return */
	}

	/*!!C
	 * blob 에서 "model" property 를 가져온다
	 */
	model = of_get_flat_dt_prop(dt_root, "model", NULL);
	if (!model)
		model = of_get_flat_dt_prop(dt_root, "compatible", NULL);
	if (!model)
		model = "<unknown>";
	pr_info("Machine: %s, model: %s\n", mdesc_best->name, model);

    /*!!C -------------------------------------------------
     * cloudrain21 추가 
     *
     * of_scan_flag_dt 함수는 dtb tree 의 root 부터
     * node 들만 쭈욱 따라가면서 첫번째 인자인 callback 함수를
     * 차례로 호출해준다.
     * callback 함수 내에서는 인자로 제공된 node pointer 를 이용하여
     * 해당 node 의 property 들 중 원하는 것을 찾아서 초기화 작업에 이용하게 된다.
     *----------------------------------------------------*/
	/* Retrieve various information from the /chosen node */
    /*!!C -------------------------------------------------
     * early_init_dt_scan_chosen 함수에서 chosen 노드일 경우
     * bootargs 프로퍼티의 value 를 .init.data section 을
     * 사용하는 boot_command_line 변수에 저장해준다.
     *----------------------------------------------------*/
	of_scan_flat_dt(early_init_dt_scan_chosen, boot_command_line);

	/* Initialize {size,address}-cells info */
    /*!!C -------------------------------------------------
     * cell size 와 addr 를 얻어서 dt_root_size_cells 와
     * dt_root_addr_cells 에 저장한다.
     *----------------------------------------------------*/
	of_scan_flat_dt(early_init_dt_scan_root, NULL);

	/* Setup memory, calling early_init_dt_add_memory_arch */
    /*!!C -------------------------------------------------
     * 위에서 얻은 cell 정보와 memory 노드 등의 정보를 이용하여
     * struct meminfo 자료구조를 채운다.
     * meminfo 를 보면 memory bank 들의 크기와 위치를 알 수 있게 된다.
     *----------------------------------------------------*/
	of_scan_flat_dt(early_init_dt_scan_memory, NULL);

	/* Change machine number to match the mdesc we're using */
	__machine_arch_type = mdesc_best->nr;

	return mdesc_best;
}
