/*
 * lib/smp_processor_id.c
 *
 * DEBUG_PREEMPT variant of smp_processor_id().
 */
#include <linux/export.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>

notrace unsigned int debug_smp_processor_id(void)
{
	unsigned long preempt_count = preempt_count();

    /*!!C
     * get current thread's cpu
     */
	int this_cpu = raw_smp_processor_id();

    /*!!C
     * preempt count 가 0 이 아니면 preemption 을 막기 위해
     * 빠져 나감.
     */
	if (likely(preempt_count))
		goto out;

	if (irqs_disabled())
		goto out;

	/*
	 * Kernel threads bound to a single CPU can safely use
	 * smp_processor_id():
	 */
	if (cpumask_equal(tsk_cpus_allowed(current), cpumask_of(this_cpu)))
		goto out;

	/*
	 * It is valid to assume CPU-locality during early bootup:
	 */
    /*!!C
     * currently SYSTEM_BOOTING
     */
	if (system_state != SYSTEM_RUNNING)
		goto out;

	/*
	 * Avoid recursion:
	 */
    /*!!C
     * 여기에서 thread_info 의 preempt_count 변수를 1 증가시켜서
     * debug_smp_processor_id() 함수에 다시 진입하는 것을 막기 위해서.
     * 이 함수 위쪽에서 검사하고 있음.
     */
	preempt_disable_notrace();

    /*!!C
     * printk 를 이용해서 kernel message 로 DOS 공격하는 것을 막기 위해
     * 5 초당 10 개 정도만 찍을 수 있도록 제한함.
     */
	if (!printk_ratelimit())
		goto out_enable;

	printk(KERN_ERR "BUG: using smp_processor_id() in preemptible [%08x] "
			"code: %s/%d\n",
			preempt_count() - 1, current->comm, current->pid);
	print_symbol("caller is %s\n", (long)__builtin_return_address(0));
	dump_stack();

out_enable:
    /*!!C
     * preempt count 를 원상복귀.
     */
	preempt_enable_no_resched_notrace();
out:
	return this_cpu;
}

EXPORT_SYMBOL(debug_smp_processor_id);

