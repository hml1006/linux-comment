// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/fs/file_table.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *  Copyright (C) 1997 David S. Miller (davem@caip.rutgers.edu)
 */

#include <linux/string.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/filelock.h>
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/eventpoll.h>
#include <linux/rcupdate.h>
#include <linux/mount.h>
#include <linux/capability.h>
#include <linux/cdev.h>
#include <linux/fsnotify.h>
#include <linux/sysctl.h>
#include <linux/percpu_counter.h>
#include <linux/percpu.h>
#include <linux/task_work.h>
#include <linux/swap.h>
#include <linux/kmemleak.h>

#include <linux/atomic.h>

#include "internal.h"

/* sysctl tunables... */
static struct files_stat_struct files_stat = {
	.max_files = NR_FILE
};

/* SLAB cache for file structures */
static struct kmem_cache *filp_cachep __ro_after_init;
static struct kmem_cache *bfilp_cachep __ro_after_init;

static struct percpu_counter nr_files __cacheline_aligned_in_smp;

/* Container for backing file with optional user path */
struct backing_file {
	struct file file;
	union {
		struct path user_path;
		freeptr_t bf_freeptr;
	};
};

#define backing_file(f) container_of(f, struct backing_file, file)

const struct path *backing_file_user_path(const struct file *f)
{
	return &backing_file(f)->user_path;
}
EXPORT_SYMBOL_GPL(backing_file_user_path);

void backing_file_set_user_path(struct file *f, const struct path *path)
{
	backing_file(f)->user_path = *path;
}
EXPORT_SYMBOL_GPL(backing_file_set_user_path);

/**
 * 释放文件结构体内存和相关资源
 * @f: 指向要释放的文件结构体的指针
 * 
 * 该函数负责释放文件结构体及其关联的资源，包括：
 * 1. 调用安全模块的文件释放钩子
 * 2. 更新系统中打开文件计数器（如果不是特殊模式）
 * 3. 释放文件相关的凭证
 * 4. 根据文件模式释放文件结构体或后备文件资源
 */
static inline void file_free(struct file *f)
{
	// 调用安全模块的文件释放钩子
	security_file_free(f);
	
	// 如果文件不是FMODE_NOACCOUNT模式，则减少系统中打开文件计数
	if (likely(!(f->f_mode & FMODE_NOACCOUNT)))
		percpu_counter_dec(&nr_files);
	
	// 释放文件相关的凭证
	put_cred(f->f_cred);
	
	// 如果文件处于FMODE_BACKING模式
	if (unlikely(f->f_mode & FMODE_BACKING)) {
		// 释放后备文件的路径引用
		path_put(backing_file_user_path(f));
		// 从后备文件的缓存中释放文件结构体
		kmem_cache_free(bfilp_cachep, backing_file(f));
	} else {
		// 从普通文件缓存中释放文件结构体
		kmem_cache_free(filp_cachep, f);
	}
}


/*
 * Return the total number of open files in the system
 */
static long get_nr_files(void)
{
	return percpu_counter_read_positive(&nr_files);
}

/*
 * Return the maximum number of open files in the system
 */
unsigned long get_max_files(void)
{
	return files_stat.max_files;
}
EXPORT_SYMBOL_GPL(get_max_files);

#if defined(CONFIG_SYSCTL) && defined(CONFIG_PROC_FS)

/*
 * Handle nr_files sysctl
 */
static int proc_nr_files(const struct ctl_table *table, int write, void *buffer,
			 size_t *lenp, loff_t *ppos)
{
	files_stat.nr_files = percpu_counter_sum_positive(&nr_files);
	return proc_doulongvec_minmax(table, write, buffer, lenp, ppos);
}

static const struct ctl_table fs_stat_sysctls[] = {
	{
		.procname	= "file-nr",
		.data		= &files_stat,
		.maxlen		= sizeof(files_stat),
		.mode		= 0444,
		.proc_handler	= proc_nr_files,
	},
	{
		.procname	= "file-max",
		.data		= &files_stat.max_files,
		.maxlen		= sizeof(files_stat.max_files),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
		.extra1		= SYSCTL_LONG_ZERO,
		.extra2		= SYSCTL_LONG_MAX,
	},
	{
		.procname	= "nr_open",
		.data		= &sysctl_nr_open,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= &sysctl_nr_open_min,
		.extra2		= &sysctl_nr_open_max,
	},
};

static int __init init_fs_stat_sysctls(void)
{
	register_sysctl_init("fs", fs_stat_sysctls);
	if (IS_ENABLED(CONFIG_BINFMT_MISC)) {
		struct ctl_table_header *hdr;

		hdr = register_sysctl_mount_point("fs/binfmt_misc");
		kmemleak_not_leak(hdr);
	}
	return 0;
}
fs_initcall(init_fs_stat_sysctls);
#endif

static int init_file(struct file *f, int flags, const struct cred *cred)
{
	int error;

	f->f_cred = get_cred(cred);
	error = security_file_alloc(f);
	if (unlikely(error)) {
		put_cred(f->f_cred);
		return error;
	}

	spin_lock_init(&f->f_lock);
	/*
	 * Note that f_pos_lock is only used for files raising
	 * FMODE_ATOMIC_POS and directories. Other files such as pipes
	 * don't need it and since f_pos_lock is in a union may reuse
	 * the space for other purposes. They are expected to initialize
	 * the respective member when opening the file.
	 */
	mutex_init(&f->f_pos_lock);
	memset(&f->__f_path, 0, sizeof(f->f_path));
	memset(&f->f_ra, 0, sizeof(f->f_ra));

	f->f_flags	= flags;
	f->f_mode	= OPEN_FMODE(flags);
	/*
	 * Disable permission and pre-content events for all files by default.
	 * They may be enabled later by fsnotify_open_perm_and_set_mode().
	 */
	file_set_fsnotify_mode(f, FMODE_NONOTIFY_PERM);

	f->f_op		= NULL;
	f->f_mapping	= NULL;
	f->private_data = NULL;
	f->f_inode	= NULL;
	f->f_owner	= NULL;
#ifdef CONFIG_EPOLL
	f->f_ep		= NULL;
#endif

	f->f_iocb_flags = 0;
	f->f_pos	= 0;
	f->f_wb_err	= 0;
	f->f_sb_err	= 0;

	/*
	 * We're SLAB_TYPESAFE_BY_RCU so initialize f_ref last. While
	 * fget-rcu pattern users need to be able to handle spurious
	 * refcount bumps we should reinitialize the reused file first.
	 */
	file_ref_init(&f->f_ref, 1);
	return 0;
}

/* Find an unused file structure and return a pointer to it.
 * Returns an error pointer if some error happend e.g. we over file
 * structures limit, run out of memory or operation is not permitted.
 *
 * Be very careful using this.  You are responsible for
 * getting write access to any mount that you might assign
 * to this filp, if it is opened for write.  If this is not
 * done, you will imbalance int the mount's writer count
 * and a warning at __fput() time.
 */
/**
 * alloc_empty_file - 分配一个空的文件结构体
 * @flags: 文件的打开标志
 * @cred: 凭证信息，用于权限控制
 *
 * 该函数用于从缓存中分配一个新的 file 结构体，并进行初始化。
 * 同时会检查系统打开文件的数量是否超过上限。
 *
 * 返回值: 成功时返回指向新分配的 file 结构体的指针，
 *         失败时返回相应的 ERR_PTR 错误指针（如 -ENOMEM 或 -ENFILE）。
 */
struct file *alloc_empty_file(int flags, const struct cred *cred)
{
	/* 记录上次触发文件数上限时的最大文件数，避免重复打印日志 */
	static long old_max;
	/* 指向新分配的文件结构体的指针 */
	struct file *f;
	/* 用于存储初始化等操作的错误码 */
	int error;

	/*
	 * 特权用户可以突破 max_files 的限制
	 */
	if (unlikely(get_nr_files() >= files_stat.max_files) &&
	    !capable(CAP_SYS_ADMIN)) {
		/*
		 * percpu_counters（每CPU计数器）是不精确的。
		 * 在我们真正走向失败之前，做一个开销较大的精确检查。
		 */
		if (percpu_counter_sum_positive(&nr_files) >= files_stat.max_files)
			goto over;
	}

	/* 从文件结构体专属的 slab 缓存中分配内存 */
	f = kmem_cache_alloc(filp_cachep, GFP_KERNEL);
	/* 内存分配失败 */
	if (unlikely(!f))
		return ERR_PTR(-ENOMEM);

	/* 初始化分配到的文件结构体 */
	error = init_file(f, flags, cred);
	/* 初始化失败，释放之前分配的内存 */
	if (unlikely(error)) {
		kmem_cache_free(filp_cachep, f);
		return ERR_PTR(error);
	}

	/* 增加系统全局的已打开文件计数器 */
	percpu_counter_inc(&nr_files);

	/* 返回成功初始化的文件结构体指针 */
	return f;

over:
	/* 文件句柄耗尽 - 报告该情况 */
	if (get_nr_files() > old_max) {
		/* 打印内核日志，提示文件数已达上限 */
		pr_info("VFS: file-max limit %lu reached\n", get_max_files());
		/* 更新历史最大文件数记录，防止日志刷屏 */
		old_max = get_nr_files();
	}
	/* 返回 -ENFILE 错误，表示系统打开的文件总数已满 */
	return ERR_PTR(-ENFILE);
}


/*
 * Variant of alloc_empty_file() that doesn't check and modify nr_files.
 *
 * This is only for kernel internal use, and the allocate file must not be
 * installed into file tables or such.
 */
struct file *alloc_empty_file_noaccount(int flags, const struct cred *cred)
{
	struct file *f;
	int error;

	f = kmem_cache_alloc(filp_cachep, GFP_KERNEL);
	if (unlikely(!f))
		return ERR_PTR(-ENOMEM);

	error = init_file(f, flags, cred);
	if (unlikely(error)) {
		kmem_cache_free(filp_cachep, f);
		return ERR_PTR(error);
	}

	f->f_mode |= FMODE_NOACCOUNT;

	return f;
}

/*
 * Variant of alloc_empty_file() that allocates a backing_file container
 * and doesn't check and modify nr_files.
 *
 * This is only for kernel internal use, and the allocate file must not be
 * installed into file tables or such.
 */
struct file *alloc_empty_backing_file(int flags, const struct cred *cred)
{
	struct backing_file *ff;
	int error;

	ff = kmem_cache_alloc(bfilp_cachep, GFP_KERNEL);
	if (unlikely(!ff))
		return ERR_PTR(-ENOMEM);

	error = init_file(&ff->file, flags, cred);
	if (unlikely(error)) {
		kmem_cache_free(bfilp_cachep, ff);
		return ERR_PTR(error);
	}

	ff->file.f_mode |= FMODE_BACKING | FMODE_NOACCOUNT;
	return &ff->file;
}
EXPORT_SYMBOL_GPL(alloc_empty_backing_file);

/**
 * file_init_path - initialize a 'struct file' based on path
 *
 * @file: the file to set up
 * @path: the (dentry, vfsmount) pair for the new file
 * @fop: the 'struct file_operations' for the new file
 */
static void file_init_path(struct file *file, const struct path *path,
			   const struct file_operations *fop)
{
	file->__f_path = *path;
	file->f_inode = path->dentry->d_inode;
	file->f_mapping = path->dentry->d_inode->i_mapping;
	file->f_wb_err = filemap_sample_wb_err(file->f_mapping);
	file->f_sb_err = file_sample_sb_err(file);
	if (fop->llseek)
		file->f_mode |= FMODE_LSEEK;
	if ((file->f_mode & FMODE_READ) &&
	     likely(fop->read || fop->read_iter))
		file->f_mode |= FMODE_CAN_READ;
	if ((file->f_mode & FMODE_WRITE) &&
	     likely(fop->write || fop->write_iter))
		file->f_mode |= FMODE_CAN_WRITE;
	file->f_iocb_flags = iocb_flags(file);
	file->f_mode |= FMODE_OPENED;
	file->f_op = fop;
	if ((file->f_mode & (FMODE_READ | FMODE_WRITE)) == FMODE_READ)
		i_readcount_inc(path->dentry->d_inode);
}

/**
 * alloc_file - allocate and initialize a 'struct file'
 *
 * @path: the (dentry, vfsmount) pair for the new file
 * @flags: O_... flags with which the new file will be opened
 * @fop: the 'struct file_operations' for the new file
 */
static struct file *alloc_file(const struct path *path, int flags,
		const struct file_operations *fop)
{
	struct file *file;

	file = alloc_empty_file(flags, current_cred());
	if (!IS_ERR(file))
		file_init_path(file, path, fop);
	return file;
}

static inline int alloc_path_pseudo(const char *name, struct inode *inode,
				    struct vfsmount *mnt, struct path *path)
{
	path->dentry = d_alloc_pseudo(mnt->mnt_sb, &QSTR(name));
	if (!path->dentry)
		return -ENOMEM;
	path->mnt = mntget(mnt);
	d_instantiate(path->dentry, inode);
	return 0;
}

struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt,
			       const char *name, int flags,
			       const struct file_operations *fops)
{
	int ret;
	struct path path;
	struct file *file;

	ret = alloc_path_pseudo(name, inode, mnt, &path);
	if (ret)
		return ERR_PTR(ret);

	file = alloc_file(&path, flags, fops);
	if (IS_ERR(file)) {
		ihold(inode);
		path_put(&path);
		return file;
	}
	/*
	 * Disable all fsnotify events for pseudo files by default.
	 * They may be enabled by caller with file_set_fsnotify_mode().
	 */
	file_set_fsnotify_mode(file, FMODE_NONOTIFY);
	return file;
}
EXPORT_SYMBOL(alloc_file_pseudo);

struct file *alloc_file_pseudo_noaccount(struct inode *inode,
					 struct vfsmount *mnt, const char *name,
					 int flags,
					 const struct file_operations *fops)
{
	int ret;
	struct path path;
	struct file *file;

	ret = alloc_path_pseudo(name, inode, mnt, &path);
	if (ret)
		return ERR_PTR(ret);

	file = alloc_empty_file_noaccount(flags, current_cred());
	if (IS_ERR(file)) {
		ihold(inode);
		path_put(&path);
		return file;
	}
	file_init_path(file, &path, fops);
	/*
	 * Disable all fsnotify events for pseudo files by default.
	 * They may be enabled by caller with file_set_fsnotify_mode().
	 */
	file_set_fsnotify_mode(file, FMODE_NONOTIFY);
	return file;
}
EXPORT_SYMBOL_GPL(alloc_file_pseudo_noaccount);

struct file *alloc_file_clone(struct file *base, int flags,
				const struct file_operations *fops)
{
	struct file *f;

	f = alloc_file(&base->f_path, flags, fops);
	if (!IS_ERR(f)) {
		path_get(&f->f_path);
		f->f_mapping = base->f_mapping;
	}
	return f;
}

/* the real guts of fput() - releasing the last reference to file
 */
static void __fput(struct file *file)
{
	// 从文件结构中获取目录项(dentry)、虚拟文件系统安装点(vfsmount)、inode节点和文件模式
	struct dentry *dentry = file->f_path.dentry;
	struct vfsmount *mnt = file->f_path.mnt;
	struct inode *inode = file->f_inode;
	fmode_t mode = file->f_mode;

	// 如果文件未打开，则直接跳转到out标签处执行
	if (unlikely(!(file->f_mode & FMODE_OPENED)))
		goto out;

	// 可能会睡眠，确保当前进程可以安全地睡眠
	might_sleep();

	// 通知系统文件关闭事件
	fsnotify_close(file);
	/*
	 * eventpoll_release()函数应该在文件清理链中首先被调用。
	 * 这是必要的，因为它处理与epoll相关的资源清理。
	 */
	eventpoll_release(file);
	// 移除文件上的所有锁
	locks_remove_file(file);

	// 调用安全模块的文件释放钩子
	security_file_release(file);
	// 如果文件被设置为异步模式(FASYNC)
	if (unlikely(file->f_flags & FASYNC)) {
		// 且文件操作结构中有fasync方法
		if (file->f_op->fasync)
			// 调用fasync方法取消异步通知
			file->f_op->fasync(-1, file, 0);
	}
	// 如果文件操作结构中有release方法
	if (file->f_op->release)
		// 调用release方法释放文件资源
		file->f_op->release(inode, file);
	// 如果是字符设备文件，且不是通过路径打开的，且有关联的字符设备
	if (unlikely(S_ISCHR(inode->i_mode) && inode->i_cdev != NULL &&
		     !(mode & FMODE_PATH))) {
		// 减少字符设备引用计数
		cdev_put(inode->i_cdev);
	}
	// 减少文件操作结构的引用计数
	fops_put(file->f_op);
	// 释放文件属主信息
	file_f_owner_release(file);
	// 减少文件访问权限的引用计数
	put_file_access(file);
	// 减少目录项的引用计数
	dput(dentry);
	// 如果文件模式需要卸载
	if (unlikely(mode & FMODE_NEED_UNMOUNT))
		// 在文件释放时卸载文件系统
		dissolve_on_fput(mnt);
	// 减少虚拟文件系统安装点的引用计数
	mntput(mnt);
out:
	// 释放文件结构本身
	file_free(file);
}


static LLIST_HEAD(delayed_fput_list);
static void delayed_fput(struct work_struct *unused)
{
	struct llist_node *node = llist_del_all(&delayed_fput_list);
	struct file *f, *t;

	llist_for_each_entry_safe(f, t, node, f_llist)
		__fput(f);
}

static void ____fput(struct callback_head *work)
{
	__fput(container_of(work, struct file, f_task_work));
}

static DECLARE_DELAYED_WORK(delayed_fput_work, delayed_fput);

/*
 * If kernel thread really needs to have the final fput() it has done
 * to complete, call this.  The only user right now is the boot - we
 * *do* need to make sure our writes to binaries on initramfs has
 * not left us with opened struct file waiting for __fput() - execve()
 * won't work without that.  Please, don't add more callers without
 * very good reasons; in particular, never call that with locks
 * held and never call that from a thread that might need to do
 * some work on any kind of umount.
 */
void flush_delayed_fput(void)
{
	delayed_fput(NULL);
	flush_delayed_work(&delayed_fput_work);
}
EXPORT_SYMBOL_GPL(flush_delayed_fput);

static void __fput_deferred(struct file *file)
{
	struct task_struct *task = current;

	if (unlikely(!(file->f_mode & (FMODE_BACKING | FMODE_OPENED)))) {
		file_free(file);
		return;
	}

	if (likely(!in_interrupt() && !(task->flags & PF_KTHREAD))) {
		init_task_work(&file->f_task_work, ____fput);
		if (!task_work_add(task, &file->f_task_work, TWA_RESUME))
			return;
		/*
		 * After this task has run exit_task_work(),
		 * task_work_add() will fail.  Fall through to delayed
		 * fput to avoid leaking *file.
		 */
	}

	if (llist_add(&file->f_llist, &delayed_fput_list))
		schedule_delayed_work(&delayed_fput_work, 1);
}

void fput(struct file *file)
{
	if (unlikely(file_ref_put(&file->f_ref)))
		__fput_deferred(file);
}
EXPORT_SYMBOL(fput);

/*
 * synchronous analog of fput(); for kernel threads that might be needed
 * in some umount() (and thus can't use flush_delayed_fput() without
 * risking deadlocks), need to wait for completion of __fput() and know
 * for this specific struct file it won't involve anything that would
 * need them.  Use only if you really need it - at the very least,
 * don't blindly convert fput() by kernel thread to that.
 */
void __fput_sync(struct file *file)
{
	if (file_ref_put(&file->f_ref))
		__fput(file);
}
EXPORT_SYMBOL(__fput_sync);

/*
 * Equivalent to __fput_sync(), but optimized for being called with the last
 * reference.
 *
 * See file_ref_put_close() for details.
 */
/**
 * fput_close_sync - 同步关闭文件并减少文件引用计数
 * @file: 要关闭的文件指针
 * 
 * 该函数用于安全地关闭一个文件，它会减少文件的引用计数，并在引用计数降为0时
 * 执行实际的文件关闭操作。使用likely()宏优化性能，假设文件引用计数减少
 * 并成功关闭的情况是常见的。
 */
void fput_close_sync(struct file *file)
{
	/* 尝试减少文件引用计数，如果成功减少则执行实际的文件关闭操作 */
	if (likely(file_ref_put_close(&file->f_ref)))
		__fput(file);  /* 执行文件关闭的底层操作 */
}


/*
 * Equivalent to fput(), but optimized for being called with the last
 * reference.
 *
 * See file_ref_put_close() for details.
 */
void fput_close(struct file *file)
{
	if (file_ref_put_close(&file->f_ref))
		__fput_deferred(file);
}

void __init files_init(void)
{
	struct kmem_cache_args args = {
		.use_freeptr_offset = true,
		.freeptr_offset = offsetof(struct file, f_freeptr),
	};

	filp_cachep = kmem_cache_create("filp", sizeof(struct file), &args,
				SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				SLAB_ACCOUNT | SLAB_TYPESAFE_BY_RCU);

	args.freeptr_offset = offsetof(struct backing_file, bf_freeptr);
	bfilp_cachep = kmem_cache_create("bfilp", sizeof(struct backing_file),
				&args, SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				SLAB_ACCOUNT | SLAB_TYPESAFE_BY_RCU);
	percpu_counter_init(&nr_files, 0, GFP_KERNEL);
}

/*
 * One file with associated inode and dcache is very roughly 1K. Per default
 * do not use more than 10% of our memory for files.
 */
void __init files_maxfiles_init(void)
{
	unsigned long n;
	unsigned long nr_pages = totalram_pages();
	unsigned long memreserve = (nr_pages - nr_free_pages()) * 3/2;

	memreserve = min(memreserve, nr_pages - 1);
	n = ((nr_pages - memreserve) * (PAGE_SIZE / 1024)) / 10;

	files_stat.max_files = max_t(unsigned long, n, NR_FILE);
}
