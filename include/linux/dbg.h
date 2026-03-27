#ifndef __COMMENT_DBG__
#define __COMMENT_DBG__

#include <linux/kern_levels.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/blkdev.h>
#include <linux/string.h>

#define FILTER_FS_TYPE1         "ext4"
#define FILTER_BLK_DEV          "nvme0n1"
#define FILTER_DIR0             "/"
#define FILTER_DIR1             "mnt"
#define FILTER_DIR2             "nvme"
#define FILTER_PATH             "/mnt/"
extern bool vfs_dbg_enabled;
static inline void vfs_dbg_enable(void)
{
        if (!vfs_dbg_enabled) {                
                vfs_dbg_enabled = true;
                printk(KERN_INFO "=================vfs debug enabled===============\n");
        }
}

static inline void vfs_dbg_disable(void)
{
        if (vfs_dbg_enabled) {
                vfs_dbg_enabled = false;
                printk(KERN_INFO "*****************vfs debug disabled**************\n");
        }
}

static inline bool is_vfs_dbg_enabled(void)
{
        return vfs_dbg_enabled;
}

static inline bool is_dentry_nvme0n1(const struct dentry *dentry)
{
        if (((unsigned long long)dentry & (~0xFFULL)) && 
                ((unsigned long long)dentry->d_sb & (~0xFFULL)) && 
                ((unsigned long long)dentry->d_sb->s_bdev & (~0xFFULL)) && 
                !(strncmp(dentry->d_sb->s_bdev->bd_disk->disk_name, FILTER_BLK_DEV, strlen(FILTER_BLK_DEV)))) {
                return true;
        }

        return false;
}

static inline bool is_inode_nvme0n1(const struct inode *inode)
{
        if (((unsigned long long)inode & (~0xFFULL)) && 
                ((unsigned long long)inode->i_sb & (~0xFFULL)) &&
                ((unsigned long long)inode->i_sb->s_bdev & (~0xFFULL)) &&
                !(strncmp(inode->i_sb->s_bdev->bd_disk->disk_name, FILTER_BLK_DEV, strlen(FILTER_BLK_DEV)))) {
                return true;
        }

        return false;
}

static inline bool is_dentry_name_match(const struct dentry *dentry)
{
        if (((unsigned long long)dentry & (~0xFFULL)) &&
                ((!strncmp(dentry->d_name.name, FILTER_DIR1, strlen(FILTER_DIR1))) ||
                (!strncmp(dentry->d_name.name, FILTER_DIR2, strlen(FILTER_DIR2))))) {
                return true;
        }

        return false;
}

static inline bool is_sb_nvme0n1(const struct super_block *sb)
{
        if (((unsigned long long)sb & (~0xFFULL)) && 
                ((unsigned long long)sb->s_bdev & (~0xFFULL)) &&
                !(strncmp(sb->s_bdev->bd_disk->disk_name, FILTER_BLK_DEV, strlen(FILTER_BLK_DEV)))) {
                return true;
        }

        return false;
}

static inline bool is_blk_nvme0n1(const struct block_device *blk)
{
        if (((unsigned long long)blk & (~0xFFULL)) && 
                !(strncmp(blk->bd_disk->disk_name, FILTER_BLK_DEV, strlen(FILTER_BLK_DEV)))) {
                return true;
        }

        return false;
}

static inline bool is_inode_ext4(const struct inode *inode)
{
        if (((unsigned long long)inode & (~0xFFULL)) && 
                !(strncmp(inode->i_sb->s_type->name, FILTER_FS_TYPE1, strlen(FILTER_FS_TYPE1)))) {
                return true;
        }

        return false;
}

#define vfs_dbg(pathname, fmt, ...)       do { \
        if (is_vfs_dbg_enabled() && !strncmp(pathname, FILTER_PATH, strlen(FILTER_PATH))) { \
                printk(KERN_INFO "[%s] path: %s => "fmt, __func__, pathname, ##__VA_ARGS__); \
        } \
} while (0)

#define nd_dbg(nd, fmt, ...)       do { \
        if (is_vfs_dbg_enabled() && ((unsigned long long)nd & (~0xFFULL)) && \
                !strncmp(nd->pathname, FILTER_PATH, strlen(FILTER_PATH))) { \
                printk(KERN_INFO "[%s] path: %s => "fmt, __func__, nd->pathname, ##__VA_ARGS__); \
        } \
} while (0)

#define dentry_dbg(dentry, fmt, ...)       do { \
        if (is_vfs_dbg_enabled() && (is_dentry_name_match(dentry) || \
                is_dentry_nvme0n1(dentry))) { \
                printk(KERN_INFO "[%s] dentry: %s => "fmt, \
                        __func__, dentry->d_name.name, ##__VA_ARGS__); \
        } \
} while (0)

#define inode_dbg(inode, fmt, ...)       do { \
        if (is_vfs_dbg_enabled() && is_inode_ext4(inode) && is_inode_nvme0n1(inode)) { \
                printk(KERN_INFO "[%s] inode: %lu => "fmt, \
                        __func__, inode->i_ino, ##__VA_ARGS__); \
        } \
} while (0)

#define sb_dbg(sb, fmt, ...)       do { \
        if (is_vfs_dbg_enabled() && is_sb_nvme0n1(sb)){ \
                printk(KERN_INFO "[%s] s_bdev %s => "fmt, \
                        __func__, sb->s_bdev->bd_disk->disk_name, ##__VA_ARGS__); \
        } \
} while (0)

#define blk_dbg(blk, fmt, ...)       do { \
        if (is_vfs_dbg_enabled() && is_blk_nvme0n1(blk)){ \
                printk(KERN_INFO "[%s] blk %pg => "fmt, \
                        __func__, blk, ##__VA_ARGS__); \
        } \
} while (0)

#endif  // __COMMENT_DBG__