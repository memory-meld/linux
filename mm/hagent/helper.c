#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/irq_work.h>
#include <linux/workqueue.h>

#include "hook.h"

// see arch/x86/entry/calling.h:
//                          di                  si                 dx
// long mmap(unsigned long addr, unsigned long len, unsigned long prot,
// 	  unsigned long flags, unsigned long fd, unsigned long off);
// 	                  cx                 r8                 r9
extern void hagent_callback_mmap(struct pt_regs *regs);
asmlinkage long hagent_hooked_mmap(struct pt_regs *regs)

{
	long err = syscall_hook_get_original(__NR_mmap)(regs);
	if (IS_ERR_VALUE(err)) {
		return err;
	}
	// syscall executed successfully
	// inject hook logic
	hagent_callback_mmap(regs);
	return err;
}

extern void hagent_callback_exit_group(struct pt_regs *regs);
asmlinkage __noreturn long hagent_hooked_exit_group(struct pt_regs *regs)

{
	if (current) {
		hagent_callback_exit_group(regs);
	} else {
		pr_err("%s: `current` is null", __func__);
	}
	syscall_hook_get_original(__NR_exit_group)(regs);
}

void helper_install_hook(void)
{
	pr_info("hagent structure allocated");
	syscall_hook_install(__NR_exit_group, hagent_hooked_exit_group);
	pr_info("exit_group hook installed");
	syscall_hook_install(__NR_mmap, hagent_hooked_mmap);
}

void helper_remove_hook(void)
{
	syscall_hook_remove(__NR_mmap);
	pr_info("mmap hook removed");
	syscall_hook_remove(__NR_exit_group);
	pr_info("exit_group hook removed");
}

ulong helper_ram_size(void)
{
	return PAGE_SIZE * totalram_pages();
}

ulong helper_task_vsize(pid_t pid)
{
	rcu_read_lock();
	struct task_struct *task = pid_task(find_vpid(pid), PIDTYPE_PID);
	ulong vsize = task ? PAGE_SIZE * task->mm->total_vm : 0;
	rcu_read_unlock();
	return vsize;
}

void helper_init_irq_work(struct irq_work *work,
			  void (*func)(struct irq_work *))
{
	init_irq_work(work, func);
}

void helper_init_work(struct work_struct *work,
		      void (*func)(struct work_struct *work))
{
	INIT_WORK(work, func);
}

u32 helper_num_online_cpus(void)
{
	return num_online_cpus();
}
