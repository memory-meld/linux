#ifndef __MM_NIMBLE_H
#define __MM_NIMBLE_H

#include <linux/mm.h>
#include <linux/mm_inline.h>

#include "../internal.h"

extern int shrink_lists(struct task_struct *p, struct mm_struct *mm,
			const nodemask_t *from, const nodemask_t *to,
			unsigned long nr_to_scan);

extern void nimble_shrink_active_list(unsigned long nr_to_scan, struct lruvec *lruvec,
			       struct scan_control *sc, enum lru_list lru);

extern unsigned long nimble_shrink_inactive_list(unsigned long nr_to_scan,
					  struct lruvec *lruvec,
					  struct scan_control *sc,
					  enum lru_list lru);

extern unsigned long mem_cgroup_node_nr_lru_pages(struct mem_cgroup *memcg,
						  int nid,
						  unsigned int lru_mask,
						  bool tree);

#define LRU_MASK_ALL		((1 << NR_LRU_LISTS) - 1)
#define LRU_MASK_ACTIVE		(BIT(LRU_ACTIVE_ANON) | BIT(LRU_ACTIVE_FILE))
#define LRU_MASK_INACTIVE	(BIT(LRU_INACTIVE_ANON) | BIT(LRU_INACTIVE_FILE))

static inline unsigned long
lruvec_size_memcg_node(enum lru_list lru, struct mem_cgroup *memcg, int nid)
{
	if (nid >= MAX_NUMNODES)
		return 0;

	VM_BUG_ON(lru < 0 || lru >= NR_LRU_LISTS);
	return mem_cgroup_node_nr_lru_pages(memcg, nid, BIT(lru), false);
}

static inline unsigned long memcg_size_node(struct mem_cgroup *memcg, int nid)
{
	if (nid >= MAX_NUMNODES)
		return 0;

	return mem_cgroup_node_nr_lru_pages(memcg, nid, LRU_MASK_ALL, false);
}

static inline unsigned long
active_inactive_size_memcg_node(struct mem_cgroup *memcg, int nid, bool active)
{
	if (nid >= MAX_NUMNODES)
		return 0;

	if (active)
		return mem_cgroup_node_nr_lru_pages(memcg, nid, LRU_MASK_ACTIVE, false);
	else
		return mem_cgroup_node_nr_lru_pages(memcg, nid, LRU_MASK_INACTIVE, false);
}

#endif
