#ifndef __MM_HAGENT_HOOKED_H
#define __MM_HAGENT_HOOKED_H

#include <linux/linkage.h>
#include <linux/ptrace.h>

asmlinkage __noreturn long hagent_hooked_exit_group(struct pt_regs const *regs);
asmlinkage long hagent_hooked_mmap(struct pt_regs const *regs);

#endif
