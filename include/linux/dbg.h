#ifndef __COMMENT_DBG__
#define __COMMENT_DBG__

#include <linux/kern_levels.h>
#include <linux/printk.h>
#include <linux/sched.h>

extern bool vfs_dbg_enabled;
static inline void vfs_dbg_enable(void)
{
        vfs_dbg_enabled = true;
        printk(KERN_INFO "vfs debug enabled\n");
}

static inline void vfs_dbg_disable(void)
{
        vfs_dbg_enabled = false;
        printk(KERN_INFO "vfs debug disabled\n");
}

static inline bool is_vfs_dbg_enabled(void)
{
        return vfs_dbg_enabled;
}

#define VFS_DBG_PROC "cat"

#define vfs_dbg(fmt, ...)       do { \
        if (strncmp(current->comm, VFS_DBG_PROC, strlen(VFS_DBG_PROC)) == 0 || is_vfs_dbg_enabled()){ \
                printk(KERN_INFO "[%s] "fmt, __func__, ##__VA_ARGS__); \
        } \
} while (0)

#endif  // __COMMENT_DBG__