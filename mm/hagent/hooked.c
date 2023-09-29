#include "asm-generic/bug.h"
#include "linux/printk.h"
#include <linux/err.h>

#include "hagent.h"
#include "hook.h"

// see arch/x86/entry/calling.h:
//                          di                  si                 dx
// long mmap(unsigned long addr, unsigned long len, unsigned long prot,
// 	  unsigned long flags, unsigned long fd, unsigned long off);
// 	                  cx                 r8                 r9
asmlinkage long hagent_hooked_mmap(struct pt_regs const *regs)

{
	long err = syscall_hook_get_original(__NR_mmap)(regs);
	if (IS_ERR_VALUE(err)) {
		return err;
	}
	// syscall executed successfully
	// inject hook logic
	hagent_callback_mmap(current->tgid, regs->di, regs->si, err);
	return err;
}

asmlinkage __noreturn long hagent_hooked_exit_group(struct pt_regs const *regs)

{
	if (current) {
		hagent_callback_exit_group(current->tgid);
	} else {
		pr_err("%s: `current` is null", __func__);
	}
	syscall_hook_get_original(__NR_exit_group)(regs);
}
