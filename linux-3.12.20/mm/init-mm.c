#include <linux/mm_types.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/cpumask.h>

#include <linux/atomic.h>
#include <asm/pgtable.h>
#include <asm/mmu.h>

#ifndef INIT_MM_CONTEXT
#define INIT_MM_CONTEXT(name)
#endif

/*!!C
  Task가 사용하는 메모리들을 다 정의해놓은 메모리 구조.
  나중에 struct task_struct에서 자기 task가 가지고 있는 메모리를
  이 메모리 구조를 통해 가지고 있는다.
 */
/* !!Q
 * swapper_pg_dir 는 어디에서 설정되어 있는가...?
 * mm_struct init_mm 에 swapper_pg_dir 은 arch/arm/kernel/head.S에 아래와 같이 정의 되어 있다.
 * 	.globl	swapper_pg_dir
 *	.equ	swapper_pg_dir, KERNEL_RAM_VADDR - PG_DIR_SIZE
 */
/* 2015/02/07 study 는 여기에서 종료 */
struct mm_struct init_mm = {
	.mm_rb		= RB_ROOT,
	.pgd		= swapper_pg_dir,
	.mm_users	= ATOMIC_INIT(2),
	.mm_count	= ATOMIC_INIT(1),
	.mmap_sem	= __RWSEM_INITIALIZER(init_mm.mmap_sem),
	.page_table_lock =  __SPIN_LOCK_UNLOCKED(init_mm.page_table_lock),
	.mmlist		= LIST_HEAD_INIT(init_mm.mmlist),
	INIT_MM_CONTEXT(init_mm)
};
