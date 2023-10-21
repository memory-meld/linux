#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
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

struct task_struct *helper_pid_task(pid_t pid)
{
	rcu_read_lock();
	struct task_struct *task = pid_task(find_vpid(pid), PIDTYPE_PID);
	rcu_read_unlock();
	return task;
}

void helper_init_irq_work(struct irq_work *work,
			  void (*func)(struct irq_work *))
{
	init_irq_work(work, func);
}

void helper_init_delayed_work(struct delayed_work *work,
			      void (*func)(struct work_struct *work))
{
	INIT_DELAYED_WORK(work, func);
}

u32 helper_num_online_cpus(void)
{
	return num_online_cpus();
}

// can only be called in the pebs event handler, because this function relies
// it's been called with mm being the profiled task's memory
// it's similar to how perf_virt_to_phys works
bool helper_in_mmap_region(u64 va)
{
	struct mm_struct *mm = current->mm;
	return mm && mm->brk < va && va < mm->mmap_base;
}

inline static u64 squares64(u64 ctr, u64 key)
{
	u64 t, x, y, z;
	y = x = ctr * key;
	z = y + key;
	x = x * x + y;
	x = (x >> 32) | (x << 32);
	x = x * x + z;
	x = (x >> 32) | (x << 32);
	x = x * x + y;
	x = (x >> 32) | (x << 32);
	t = x = x * x + z;
	x = (x >> 32) | (x << 32);
	return t ^ ((x * x + y) >> 32);
}

static u64 random(void)
{
	static u64 counter = 0x183ac9a532e05;
	return squares64(counter++, 0x8f7fbcc8696b378b);
}

u64 helper_find_random_candidate(struct task_struct *task, u64 *buf, u64 len)
{
#define BUFLEN (32)
	struct mm_struct *mm = NULL;
	if (!task || !(mm = task->mm, mm)) {
		return 0;
	}
	mmap_read_lock(mm);
	struct vm_area_struct *vma = NULL;
	struct vm_area_struct *mmap_vma[BUFLEN] = {};
	ulong mmap_vma_len = 0, total_len = 0;
	// only show mmap region
	VMA_ITERATOR(vmi, mm, mm->brk);
	for_each_vma(vmi, vma)
	{
		// only show mmap region
		if (!(vma->vm_end < mm->mmap_base && mmap_vma_len < BUFLEN))
			break;
		ulong len = vma->vm_end - vma->vm_start;
		// skip small vma
		if (len < 3 * HPAGE_SIZE)
			continue;
		// skip code segments
		if (vma->vm_file && (vma->vm_flags & VM_EXEC))
			continue;
		// skip read-only
		if (!(vma->vm_flags & VM_WRITE))
			continue;
		// skip stack
		if (vma->vm_flags & VM_GROWSDOWN)
			continue;
		pr_debug("%s: vma [0x%lx, 0x%lx) len 0x%lx flags 0x%lx",
			 __func__, vma->vm_start, vma->vm_end, len,
			 vma->vm_flags);
		total_len += len;
		mmap_vma[mmap_vma_len++] = vma;
	}
	pr_debug("%s: total len 0x%lx", __func__, total_len);
	u64 failed = 0;
	for (u64 i = 0; i < len; ++i) {
		ulong found = 0;
		for (ulong retry = 0; retry < 3 && !found; ++retry) {
			ulong offset = random() % total_len;
			for (struct vm_area_struct **p = mmap_vma;
			     p < mmap_vma + mmap_vma_len; ++p) {
				struct vm_area_struct *vma = *p;
				ulong len = vma->vm_end - vma->vm_start;
				if (len < offset) {
					offset -= len;
					continue;
				}
				ulong boundary = round_down(
					vma->vm_start + offset, HPAGE_SIZE);
				if (boundary >= vma->vm_start &&
				    boundary + HPAGE_SIZE <= vma->vm_end) {
					found = boundary;
					// pr_info("%s: found 0x%lx", __func__,
					// 	found);
				} else {
					// fail, no problem, we will retry several times
					failed += 1;
				}
				// if return 0, means we fail to find a boundary
				break;
			}
		}
		buf[i] = found;
	}
	mmap_read_unlock(mm);
	return failed;
#undef BUFLEN
}

int helper_dram_node(void)
{
	static int dram = NUMA_NO_NODE;
	// we might have a memoryless node, which makes dram_node not being 0
	if (dram == NUMA_NO_NODE) {
		int nid = NUMA_NO_NODE;
		for_each_online_node(nid) {
			if (!node_present_pages(nid)) {
				continue;
			}
			dram = nid;
			break;
		}
	}
	return dram;
}

int helper_pmem_node(void)
{
	return helper_dram_node() + 1;
}

bool helper_node_has_space(int nid)
{
	struct zone *normal =
		&NODE_DATA(helper_pmem_node())->node_zones[ZONE_NORMAL];
	return normal && zone_page_state(normal, NR_FREE_PAGES) -
				 high_wmark_pages(normal);
}

u64 helper_interrupt_context_level(void)
{
	return interrupt_context_level();
}
