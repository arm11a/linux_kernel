/*
 * GCC stack protector support.
 *
 * Stack protector works by putting predefined pattern at the start of
 * the stack frame and verifying that it hasn't been overwritten when
 * returning from the function.  The pattern is called stack canary
 * and gcc expects it to be defined by a global variable called
 * "__stack_chk_guard" on ARM.  This unfortunately means that on SMP
 * we cannot have a different canary value per task.
 */

#ifndef _ASM_STACKPROTECTOR_H
#define _ASM_STACKPROTECTOR_H 1

#include <linux/random.h>
#include <linux/version.h>

extern unsigned long __stack_chk_guard;

/*
 * Initialize the stackprotector canary value.
 *
 * NOTE: this must only be called from functions that never return,
 * and it must always be inlined.
 */
/*!!C
 * optimization level 이 지정되지 않았어도 inline 화 시킨다.
 */
static __always_inline void boot_init_stack_canary(void)
{
	unsigned long canary;

	/* Try to get a semi random initial value. */
	get_random_bytes(&canary, sizeof(canary));
	canary ^= LINUX_VERSION_CODE;

    /*!!C
     * 초기화한 thread_info 에서 task_struct 를 가져와서
     * 그 안의 canary 변수를 설정한다.
     */
	current->stack_canary = canary;

    /*!!C
     * __stack_chk_guard 변수는 global 이라서 ARM 에서
     * 다수의 thread 가 공통으로 사용한다. 
     */
	__stack_chk_guard = current->stack_canary;
}

#endif	/* _ASM_STACKPROTECTOR_H */
