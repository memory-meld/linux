#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/htmm.h>

/* sysfs htmm */
unsigned int htmm_sample_period = 199;
unsigned int htmm_inst_sample_period = 100007;
unsigned int htmm_thres_hot = 1;
unsigned int htmm_cooling_period = 2000000;
unsigned int htmm_adaptation_period = 100000;
unsigned int htmm_split_period = 2; /* used to shift the wss of memcg */
unsigned int ksampled_min_sample_ratio = 50; // 50%
unsigned int ksampled_max_sample_ratio = 10; // 10%
unsigned int htmm_demotion_period_in_ms = 500;
unsigned int htmm_promotion_period_in_ms = 500;
unsigned int htmm_thres_split = 2;
unsigned int htmm_nowarm = 0; // enabled: 0, disabled: 1
unsigned int htmm_util_weight = 10; // no impact (unused)
unsigned int htmm_mode = 1;
unsigned int htmm_gamma = 4; /* 0.4; divide this by 10 */
bool htmm_cxl_mode = false;
bool htmm_skip_cooling = true;
// unit: 4KiB, default: 10GB
unsigned int htmm_thres_cooling_alloc = 256 * 1024 * 10;
unsigned int ksampled_soft_cpu_quota = 30; // 3 %

static ssize_t htmm_sample_period_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_sample_period);
}

static ssize_t htmm_sample_period_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	int err;
	unsigned int period;

	err = kstrtouint(buf, 10, &period);
	if (err)
		return err;

	WRITE_ONCE(htmm_sample_period, period);
	return count;
}

static struct kobj_attribute htmm_sample_period_attr =
	__ATTR(htmm_sample_period, 0644, htmm_sample_period_show,
	       htmm_sample_period_store);

static ssize_t htmm_inst_sample_period_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_inst_sample_period);
}

static ssize_t htmm_inst_sample_period_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	int err;
	unsigned int period;

	err = kstrtouint(buf, 10, &period);
	if (err)
		return err;

	WRITE_ONCE(htmm_inst_sample_period, period);
	return count;
}

static struct kobj_attribute htmm_inst_sample_period_attr =
	__ATTR(htmm_inst_sample_period, 0644, htmm_inst_sample_period_show,
	       htmm_inst_sample_period_store);

static ssize_t htmm_split_period_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_split_period);
}

static ssize_t htmm_split_period_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	int err;
	unsigned int thres;

	err = kstrtouint(buf, 10, &thres);
	if (err)
		return err;

	WRITE_ONCE(htmm_split_period, thres);
	return count;
}

static struct kobj_attribute htmm_split_period_attr =
	__ATTR(htmm_split_period, 0644, htmm_split_period_show,
	       htmm_split_period_store);

static ssize_t htmm_thres_hot_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_thres_hot);
}

static ssize_t htmm_thres_hot_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	int err;
	unsigned int thres;

	err = kstrtouint(buf, 10, &thres);
	if (err)
		return err;

	WRITE_ONCE(htmm_thres_hot, thres);
	return count;
}

static struct kobj_attribute htmm_thres_hot_attr =
	__ATTR(htmm_thres_hot, 0644, htmm_thres_hot_show, htmm_thres_hot_store);

static ssize_t htmm_cooling_period_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_cooling_period);
}

static ssize_t htmm_cooling_period_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	int err;
	unsigned int period;

	err = kstrtouint(buf, 10, &period);
	if (err)
		return err;

	WRITE_ONCE(htmm_cooling_period, period);
	return count;
}

static struct kobj_attribute htmm_cooling_period_attr =
	__ATTR(htmm_cooling_period, 0644, htmm_cooling_period_show,
	       htmm_cooling_period_store);

static ssize_t ksampled_min_sample_ratio_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	return sysfs_emit(buf, "%u\n", ksampled_min_sample_ratio);
}

static ssize_t ksampled_min_sample_ratio_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	int err;
	unsigned int interval;

	err = kstrtouint(buf, 10, &interval);
	if (err)
		return err;

	WRITE_ONCE(ksampled_min_sample_ratio, interval);
	return count;
}

static struct kobj_attribute ksampled_min_sample_ratio_attr =
	__ATTR(ksampled_min_sample_ratio, 0644, ksampled_min_sample_ratio_show,
	       ksampled_min_sample_ratio_store);

static ssize_t ksampled_max_sample_ratio_show(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      char *buf)
{
	return sysfs_emit(buf, "%u\n", ksampled_max_sample_ratio);
}

static ssize_t ksampled_max_sample_ratio_store(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	int err;
	unsigned int interval;

	err = kstrtouint(buf, 10, &interval);
	if (err)
		return err;

	WRITE_ONCE(ksampled_max_sample_ratio, interval);
	return count;
}

static struct kobj_attribute ksampled_max_sample_ratio_attr =
	__ATTR(ksampled_max_sample_ratio, 0644, ksampled_max_sample_ratio_show,
	       ksampled_max_sample_ratio_store);

static ssize_t htmm_demotion_period_show(struct kobject *kobj,
					 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_demotion_period_in_ms);
}

static ssize_t htmm_demotion_period_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	int err;
	unsigned int thres;

	err = kstrtouint(buf, 10, &thres);
	if (err)
		return err;

	WRITE_ONCE(htmm_demotion_period_in_ms, thres);
	return count;
}

static struct kobj_attribute htmm_demotion_period_attr =
	__ATTR(htmm_demotion_period_in_ms, 0644, htmm_demotion_period_show,
	       htmm_demotion_period_store);

static ssize_t htmm_promotion_period_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_promotion_period_in_ms);
}

static ssize_t htmm_promotion_period_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	int err;
	unsigned int thres;

	err = kstrtouint(buf, 10, &thres);
	if (err)
		return err;

	WRITE_ONCE(htmm_promotion_period_in_ms, thres);
	return count;
}

static struct kobj_attribute htmm_promotion_period_attr =
	__ATTR(htmm_promotion_period_in_ms, 0644, htmm_promotion_period_show,
	       htmm_promotion_period_store);

static ssize_t ksampled_soft_cpu_quota_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "%u\n", ksampled_soft_cpu_quota);
}

static ssize_t ksampled_soft_cpu_quota_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	int err;
	unsigned int sp_count;

	err = kstrtouint(buf, 10, &sp_count);
	if (err)
		return err;

	WRITE_ONCE(ksampled_soft_cpu_quota, sp_count);
	return count;
}

static struct kobj_attribute ksampled_soft_cpu_quota_attr =
	__ATTR(ksampled_soft_cpu_quota, 0644, ksampled_soft_cpu_quota_show,
	       ksampled_soft_cpu_quota_store);

static ssize_t htmm_thres_split_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_thres_split);
}

static ssize_t htmm_thres_split_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int err;
	unsigned int thres;

	err = kstrtouint(buf, 10, &thres);
	if (err)
		return err;

	WRITE_ONCE(htmm_thres_split, thres);
	return count;
}

static struct kobj_attribute htmm_thres_split_attr = __ATTR(
	htmm_thres_split, 0644, htmm_thres_split_show, htmm_thres_split_store);

static ssize_t htmm_nowarm_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_nowarm);
}

static ssize_t htmm_nowarm_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count)
{
	int err;
	unsigned int thres;

	err = kstrtouint(buf, 10, &thres);
	if (err)
		return err;

	WRITE_ONCE(htmm_nowarm, thres);
	return count;
}

static struct kobj_attribute htmm_nowarm_attr =
	__ATTR(htmm_nowarm, 0644, htmm_nowarm_show, htmm_nowarm_store);

static ssize_t htmm_adaptation_period_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_adaptation_period);
}

static ssize_t htmm_adaptation_period_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	int err;
	unsigned int period;

	err = kstrtouint(buf, 10, &period);
	if (err)
		return err;

	WRITE_ONCE(htmm_adaptation_period, period);
	return count;
}

static struct kobj_attribute htmm_adaptation_period_attr =
	__ATTR(htmm_adaptation_period, 0644, htmm_adaptation_period_show,
	       htmm_adaptation_period_store);

static ssize_t htmm_util_weight_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_util_weight);
}

static ssize_t htmm_util_weight_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int err;
	unsigned int util_w;

	err = kstrtouint(buf, 10, &util_w);
	if (err)
		return err;

	WRITE_ONCE(htmm_util_weight, util_w);
	return count;
}

static struct kobj_attribute htmm_util_weight_attr = __ATTR(
	htmm_util_weight, 0644, htmm_util_weight_show, htmm_util_weight_store);

static ssize_t htmm_gamma_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_gamma);
}

static ssize_t htmm_gamma_store(struct kobject *kobj,
				struct kobj_attribute *attr, const char *buf,
				size_t count)
{
	int err;
	unsigned int g;

	err = kstrtouint(buf, 10, &g);
	if (err)
		return err;

	WRITE_ONCE(htmm_gamma, g);
	return count;
}

static struct kobj_attribute htmm_gamma_attr =
	__ATTR(htmm_gamma, 0644, htmm_gamma_show, htmm_gamma_store);

static ssize_t htmm_cxl_mode_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	if (htmm_cxl_mode)
		return sysfs_emit(buf, "CXL-emulated: %s\n",
				  "[enabled] disabled");
	else
		return sysfs_emit(buf, "CXL-emulated: %s\n",
				  "enabled [disabled]");
}

static ssize_t htmm_cxl_mode_store(struct kobject *kobj,
				   struct kobj_attribute *attr, const char *buf,
				   size_t count)
{
	if (sysfs_streq(buf, "enabled"))
		htmm_cxl_mode = true;
	else if (sysfs_streq(buf, "disabled"))
		htmm_cxl_mode = false;
	else
		return -EINVAL;

	return count;
}

static struct kobj_attribute htmm_cxl_mode_attr =
	__ATTR(htmm_cxl_mode, 0644, htmm_cxl_mode_show, htmm_cxl_mode_store);

static ssize_t htmm_mode_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	char const *msg = NULL;
	if (htmm_mode == HTMM_NO_MIG)
		msg = "[NO MIG-0], BASELINE-1, HUGEPAGE_OPT-2, HUGEPAGE_OPT_V2-3";
	else if (htmm_mode == HTMM_BASELINE)
		msg = "NO MIG-0, [BASELINE-1], HUGEPAGE_OPT-2, HUGEPAGE_OPT_V2";
	else if (htmm_mode == HTMM_HUGEPAGE_OPT)
		msg = "NO MIG-0, BASELINE-1, [HUGEPAGE_OPT-2], HUGEPAGE_OPT_V2-3";
	else if (htmm_mode == HTMM_HUGEPAGE_OPT_V2)
		msg = "NO MIG-0, BASELINE-1, HUGEPAGE_OPT-2, [HUGEPAGE_OPT_V2]";
	else
		BUG(); // unreachable
	return sysfs_emit(buf, "%s\n", msg);
}

static ssize_t htmm_mode_store(struct kobject *kobj,
			       struct kobj_attribute *attr, const char *buf,
			       size_t count)
{
	int err;
	unsigned int mode;

	err = kstrtouint(buf, 10, &mode);
	if (err)
		return err;

	switch (mode) {
	case HTMM_NO_MIG:
	case HTMM_BASELINE:
	case HTMM_HUGEPAGE_OPT:
	case HTMM_HUGEPAGE_OPT_V2:
		WRITE_ONCE(htmm_mode, mode);
		break;
	default:
		return -EINVAL;
	}
	return count;
}

static struct kobj_attribute htmm_mode_attr =
	__ATTR(htmm_mode, 0644, htmm_mode_show, htmm_mode_store);
/* sysfs related to newly allocated pages */
static ssize_t htmm_skip_cooling_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	if (htmm_skip_cooling)
		return sysfs_emit(buf, "[enabled] disabled\n");
	else
		return sysfs_emit(buf, "enabled [disabled]\n");
}

static ssize_t htmm_skip_cooling_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	if (sysfs_streq(buf, "enabled"))
		htmm_skip_cooling = true;
	else if (sysfs_streq(buf, "disabled"))
		htmm_skip_cooling = false;
	else
		return -EINVAL;

	return count;
}

static struct kobj_attribute htmm_skip_cooling_attr =
	__ATTR(htmm_skip_cooling, 0644, htmm_skip_cooling_show,
	       htmm_skip_cooling_store);

static ssize_t htmm_thres_cooling_alloc_show(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%u\n", htmm_thres_cooling_alloc);
}

static ssize_t htmm_thres_cooling_alloc_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	int err;
	unsigned int thres;

	err = kstrtouint(buf, 10, &thres);
	if (err)
		return err;

	WRITE_ONCE(htmm_thres_cooling_alloc, thres);
	return count;
}

static struct kobj_attribute htmm_thres_cooling_alloc_attr =
	__ATTR(htmm_thres_cooling_alloc, 0644, htmm_thres_cooling_alloc_show,
	       htmm_thres_cooling_alloc_store);

static struct attribute *htmm_attrs[] = {
	&htmm_sample_period_attr.attr,
	&htmm_inst_sample_period_attr.attr,
	&htmm_split_period_attr.attr,
	&htmm_thres_hot_attr.attr,
	&htmm_cooling_period_attr.attr,
	&htmm_adaptation_period_attr.attr,
	&ksampled_min_sample_ratio_attr.attr,
	&ksampled_max_sample_ratio_attr.attr,
	&htmm_demotion_period_attr.attr,
	&htmm_promotion_period_attr.attr,
	&ksampled_soft_cpu_quota_attr.attr,
	&htmm_thres_split_attr.attr,
	&htmm_nowarm_attr.attr,
	&htmm_util_weight_attr.attr,
	&htmm_mode_attr.attr,
	&htmm_gamma_attr.attr,
	&htmm_cxl_mode_attr.attr,
	&htmm_skip_cooling_attr.attr,
	&htmm_thres_cooling_alloc_attr.attr,
	NULL,
};

static const struct attribute_group htmm_attr_group = {
	.attrs = htmm_attrs,
};

static int __init htmm_init_sysfs(void)
{
	int err;
	struct kobject *htmm_kobj;

	htmm_kobj = kobject_create_and_add("htmm", mm_kobj);
	if (!htmm_kobj) {
		pr_err("failed to create htmm kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(htmm_kobj, &htmm_attr_group);
	if (err) {
		pr_err("failed to register numa group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(htmm_kobj);
	return err;
}
subsys_initcall(htmm_init_sysfs);
