#include <linux/memcontrol.h>
#include <linux/node.h>
#include <linux/seq_buf.h>
#include <linux/htmm.h>

#include "../internal.h"

/* memcg interfaces for htmm */
static int memcg_htmm_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));

	if (memcg->htmm_enabled)
		seq_printf(m, "[enabled] disabled\n");
	else
		seq_printf(m, "enabled [disabled]\n");

	return 0;
}

static ssize_t memcg_htmm_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	int nid;

	if (sysfs_streq(buf, "enabled"))
		memcg->htmm_enabled = true;
	else if (sysfs_streq(buf, "disabled"))
		memcg->htmm_enabled = false;
	else
		return -EINVAL;

	if (memcg->htmm_enabled) {
		kmigraterd_init();
	} else {
		kmigraterd_stop();
	}
	for_each_node_state (nid, N_MEMORY) {
		struct pglist_data *pgdat = NODE_DATA(nid);

		if (memcg->htmm_enabled) {
			WRITE_ONCE(pgdat->kswapd_failures, MAX_RECLAIM_RETRIES);
			add_memcg_to_kmigraterd(memcg, nid);
		} else {
			WRITE_ONCE(pgdat->kswapd_failures, 0);
			del_memcg_from_kmigraterd(memcg, nid);
		}
	}

	return nbytes;
}

static struct cftype memcg_htmm_file[] = {
	{
		.name = "htmm_enabled",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memcg_htmm_show,
		.write = memcg_htmm_write,
	},
	{}, /* terminate */
};

static int __init mem_cgroup_htmm_init(void)
{
	WARN_ON(cgroup_add_dfl_cftypes(&memory_cgrp_subsys, memcg_htmm_file));
	return 0;
}
subsys_initcall(mem_cgroup_htmm_init);

static int memcg_access_map_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	struct seq_buf s;
	int i;

	seq_buf_init(&s, kmalloc(PAGE_SIZE, GFP_KERNEL), PAGE_SIZE);
	if (!s.buffer)
		return 0;
	for (i = 20; i > 15; i--) {
		seq_buf_printf(&s, "skewness_idx_map[%2d]: %10lu\n", i,
			       memcg->access_map[i]);
	}

	for (i = 15; i >= 0; i--) {
		seq_buf_printf(
			&s,
			"skewness_idx_map[%2d]: %10lu  hotness_hg[%2d]: %10lu  ebp_hotness_hg[%2d]: %10lu\n",
			i, memcg->access_map[i], i, memcg->hotness_hg[i], i,
			memcg->ebp_hotness_hg[i]);
	}

	seq_puts(m, s.buffer);
	kfree(s.buffer);

	return 0;
}

static struct cftype memcg_access_map_file[] = {
	{
		.name = "access_map",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memcg_access_map_show,
	},
	{},
};

static int __init mem_cgroup_access_map_init(void)
{
	WARN_ON(cgroup_add_dfl_cftypes(&memory_cgrp_subsys,
				       memcg_access_map_file));
	return 0;
}
subsys_initcall(mem_cgroup_access_map_init);

static int memcg_hotness_stat_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	struct seq_buf s;
	unsigned long hot = 0, warm = 0, cold = 0;
	int i;

	seq_buf_init(&s, kmalloc(PAGE_SIZE, GFP_KERNEL), PAGE_SIZE);
	if (!s.buffer)
		return 0;

	for (i = 15; i >= 0; i--) {
		if (i >= memcg->active_threshold)
			hot += memcg->hotness_hg[i];
		else if (i >= memcg->warm_threshold)
			warm += memcg->hotness_hg[i];
		else
			cold += memcg->hotness_hg[i];
	}

	seq_buf_printf(&s, "hot %lu warm %lu cold %lu\n", hot, warm, cold);

	seq_puts(m, s.buffer);
	kfree(s.buffer);

	return 0;
}

static struct cftype memcg_hotness_stat_file[] = {
	{
		.name = "hotness_stat",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = memcg_hotness_stat_show,
	},
	{},
};

static int __init mem_cgroup_hotness_stat_init(void)
{
	WARN_ON(cgroup_add_dfl_cftypes(&memory_cgrp_subsys,
				       memcg_hotness_stat_file));
	return 0;
}
subsys_initcall(mem_cgroup_hotness_stat_init);

static int memcg_per_node_max_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	struct cftype *cur_file = seq_cft(m);
	int nid = cur_file->numa_node_id;
	unsigned long max = READ_ONCE(memcg->nodeinfo[nid]->max_nr_base_pages);

	if (max == ULONG_MAX)
		seq_puts(m, "max\n");
	else
		seq_printf(m, "%llu\n", (u64)max * PAGE_SIZE);

	return 0;
}

static ssize_t memcg_per_node_max_write(struct kernfs_open_file *of, char *buf,
					size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	struct cftype *cur_file = of_cft(of);
	int nid = cur_file->numa_node_id;
	unsigned long max, nr_dram_pages = 0;
	int err, n;

	buf = strstrip(buf);
	err = page_counter_memparse(buf, "max", &max);
	if (err)
		return err;

	xchg(&memcg->nodeinfo[nid]->max_nr_base_pages, max);

	for_each_node_state (n, N_MEMORY) {
		if (node_is_toptier(n)) {
			if (memcg->nodeinfo[n]->max_nr_base_pages != ULONG_MAX)
				nr_dram_pages +=
					memcg->nodeinfo[n]->max_nr_base_pages;
		}
	}
	if (nr_dram_pages)
		memcg->max_nr_dram_pages = nr_dram_pages;

	return nbytes;
}

static int pgdat_memcg_htmm_init(struct pglist_data *pgdat)
{
	pgdat->memcg_htmm_file = kzalloc(sizeof(struct cftype) * 2, GFP_KERNEL);
	if (!pgdat->memcg_htmm_file) {
		printk("error: fails to allocate pgdat->memcg_htmm_file\n");
		return -ENOMEM;
	}
#ifdef CONFIG_LOCKDEP
	lockdep_register_key(&(pgdat->memcg_htmm_file->lockdep_key));
#endif
	return 0;
}

int mem_cgroup_per_node_htmm_init(void)
{
	int nid;

	for_each_node_state (nid, N_MEMORY) {
		struct pglist_data *pgdat = NODE_DATA(nid);

		if (!pgdat || pgdat->memcg_htmm_file)
			continue;
		if (pgdat_memcg_htmm_init(pgdat))
			continue;

		snprintf(pgdat->memcg_htmm_file[0].name, MAX_CFTYPE_NAME,
			 "max_at_node%d", nid);
		pgdat->memcg_htmm_file[0].flags = CFTYPE_NOT_ON_ROOT;
		pgdat->memcg_htmm_file[0].seq_show = memcg_per_node_max_show;
		pgdat->memcg_htmm_file[0].write = memcg_per_node_max_write;
		pgdat->memcg_htmm_file[0].numa_node_id = nid;

		WARN_ON(cgroup_add_dfl_cftypes(&memory_cgrp_subsys,
					       pgdat->memcg_htmm_file));
	}
	return 0;
}
subsys_initcall(mem_cgroup_per_node_htmm_init);
