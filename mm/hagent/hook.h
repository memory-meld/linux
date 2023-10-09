#ifndef __MM_HAGENT_HOOK_H
#define __MM_HAGENT_HOOK_H

#include <linux/ptrace.h>

typedef asmlinkage long (*sys_call_ptr_t)(struct pt_regs *);

void syscall_hook_install(int id, sys_call_ptr_t new);
void syscall_hook_remove(int id);
void syscall_hook_remove_all(void);
sys_call_ptr_t syscall_hook_get_original(int id);

#endif
