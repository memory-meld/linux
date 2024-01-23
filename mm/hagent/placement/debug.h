#ifndef HAGENT_PLACEMENT_DEBUG_H
#define HAGENT_PLACEMENT_DEBUG_H

#include <linux/fs.h>

#include "utils.h"

static inline int __filp_close(struct file **file)
{
	if (file && !IS_ERR_OR_NULL(*file))
		TRY(filp_close(*file, NULL));
	return 0;
}

// DEFINE_CLASS(file, struct file *, __filp_close(&_T),
// 	     filp_open(filename, flags, mode), const char *filename, int flags,
// 	     umode_t mode);

static inline ssize_t debug_write_file(char const *name, void *buf, size_t len)
{
	if (!name || !buf) {
		return -EINVAL;
	}
	struct file *file __cleanup(__filp_close) =
		TRY(filp_open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644));
	return TRY(kernel_write(file, buf, len, 0));
}

#endif // !HAGENT_PLACEMENT_DEBUG_H
