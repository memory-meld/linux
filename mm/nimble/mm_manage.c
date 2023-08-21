/*
 * A syscall used to move pages between two nodes.
 */

#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/nodemask.h>
#include <linux/rmap.h>
#include <linux/security.h>
#include <linux/syscalls.h>

#include "nimble.h"

extern int get_nodes(nodemask_t *nodes, const unsigned long __user *nmask,
		     unsigned long maxnode);

SYSCALL_DEFINE6(mm_manage, pid_t, pid, unsigned long, nr_pages, unsigned long,
		maxnode, const unsigned long __user *, old_nodes,
		const unsigned long __user *, new_nodes, int, flags)
{
	const struct cred *cred = current_cred(), *tcred;
	struct task_struct *task;
	struct mm_struct *mm = NULL;
	int err;
	nodemask_t task_nodes;
	nodemask_t *old;
	nodemask_t *new;
	NODEMASK_SCRATCH(scratch);

	if (!scratch)
		return -ENOMEM;

	old = &scratch->mask1;
	new = &scratch->mask2;

	err = get_nodes(old, old_nodes, maxnode);
	if (err)
		goto out;

	err = get_nodes(new, new_nodes, maxnode);
	if (err)
		goto out;

	/* Check flags */
	if (flags & ~MPOL_NF_VALID)
		return -EINVAL;

	/* Find the mm_struct */
	rcu_read_lock();
	task = pid ? find_task_by_vpid(pid) : current;
	if (!task) {
		rcu_read_unlock();
		err = -ESRCH;
		goto out;
	}
	get_task_struct(task);

	err = -EINVAL;
	/*
	 * Check if this process has the right to modify the specified
	 * process. The right exists if the process has administrative
	 * capabilities, superuser privileges or the same
	 * userid as the target process.
	 */
	tcred = __task_cred(task);
	if (!uid_eq(cred->euid, tcred->suid) &&
	    !uid_eq(cred->euid, tcred->uid) &&
	    !uid_eq(cred->uid, tcred->suid) && !uid_eq(cred->uid, tcred->uid) &&
	    !capable(CAP_SYS_NICE)) {
		rcu_read_unlock();
		err = -EPERM;
		goto out_put;
	}
	rcu_read_unlock();

	err = security_task_movememory(task);
	if (err)
		goto out_put;

	task_nodes = cpuset_mems_allowed(task);
	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm) {
		err = -EINVAL;
		goto out;
	}
	if (test_bit(MMF_MM_MANAGE, &mm->flags)) {
		mmput(mm);
		goto out;
	} else {
		set_bit(MMF_MM_MANAGE, &mm->flags);
	}

	if (flags & MPOL_NF_SHRINK_LISTS)
		shrink_lists(task, mm, old, new, nr_pages);

	if (flags & MPOL_NF_MOVE)
		err = -EINVAL;
		// err = do_mm_manage(task, mm, old, new, nr_pages, flags);

	clear_bit(MMF_MM_MANAGE, &mm->flags);
	mmput(mm);
out:
	NODEMASK_SCRATCH_FREE(scratch);

	return err;

out_put:
	put_task_struct(task);
	goto out;
}
