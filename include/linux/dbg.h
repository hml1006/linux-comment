#ifndef __COMMENT_DBG__
#define __COMMENT_DBG__

#include <linux/kern_levels.h>
#include <linux/printk.h>

#define VFS_DBG_DIR     "/root"
#define vfs_dbg(pathname, fmt, ...)       do { \
        if (strncmp(pathname, VFS_DBG_DIR, strlen(VFS_DBG_DIR)) == 0){ \
                pr_debug("[%s][%s] "fmt, __func__, pathname, ##__VA_ARGS__); \
        } \
} while (0)

#endif  // __COMMENT_DBG__