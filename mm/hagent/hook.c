// coming from: arch/x86/kernel/cpu/common.c
//   > void native_write_cr0(unsigned long val)
#include <linux/printk.h>
#include <linux/kprobes.h>

#include "hook.h"

inline void write_cr0_nocheck(unsigned long val)
{
	asm volatile("mov %0,%%cr0" : "+r"(val) : : "memory");
}

static void enable_write_protection(void)
{
	write_cr0_nocheck(read_cr0() | X86_CR0_WP);
}

static void disable_write_protection(void)
{
	write_cr0_nocheck(read_cr0() & ~X86_CR0_WP);
}

static sys_call_ptr_t *sys_call_table;
static sys_call_ptr_t sys_call_backup[__NR_syscalls];

void syscall_hook_install(int id, sys_call_ptr_t new)
{
	typedef unsigned long (*kallsyms_lookup_name_t)(const char *);
	static kallsyms_lookup_name_t kallsyms_lookup_name = NULL;
	if (!kallsyms_lookup_name) {
		struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
		register_kprobe(&kp);
		kallsyms_lookup_name = (kallsyms_lookup_name_t)kp.addr;
		unregister_kprobe(&kp);
		BUG_ON(!kallsyms_lookup_name);
	}
	if (!sys_call_table) {
		sys_call_table = (sys_call_ptr_t *)kallsyms_lookup_name(
			"sys_call_table");
		BUG_ON(!sys_call_table);
	}
	// backup old symbols
	sys_call_ptr_t *old = &sys_call_table[id];
	sys_call_backup[id] = *old;
	disable_write_protection();
	// install hooked version
	*old = new;
	enable_write_protection();
	pr_info("successfully hooked syscall %d %px->%px", id, new, old);
}

void syscall_hook_remove(int id)
{
	if (!sys_call_table) {
		pr_err("no hook has ever been installed");
		return;
	}
	sys_call_ptr_t old = sys_call_backup[id];
	sys_call_ptr_t *new = &sys_call_table[id];
	if (!old) {
		pr_err("trying to remove a unhooked syscall %d", id);
		return;
	}
	disable_write_protection();
	// restore original symbols
	*new = old;
	enable_write_protection();
	pr_info("successfully restored syscall %d %px->%px", id, new, old);
}

void syscall_hook_remove_all(void)
{
	for (int i = 0; i < __NR_syscalls; ++i) {
		sys_call_ptr_t old = sys_call_backup[i];
		if (!old) {
			continue;
		}
		syscall_hook_remove(i);
	}
	pr_info("successfully removed all syscall hooks");
}

sys_call_ptr_t syscall_hook_get_original(int id)
{
	if (!sys_call_table || !sys_call_backup[id]) {
		pr_err("no hook for %d has ever been installed", id);
		return NULL;
	}
	return sys_call_backup[id];
}
