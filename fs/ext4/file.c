// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext4/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 */

#include "linux/dbg.h"
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/filelock.h>
#include <linux/quotaops.h>
#include <linux/pagevec.h>
#include <linux/uio.h>
#include <linux/mman.h>
#include <linux/backing-dev.h>
#include "ext4.h"
#include "ext4_jbd2.h"
#include "xattr.h"
#include "acl.h"
#include "truncate.h"

/*
 * Returns %true if the given DIO request should be attempted with DIO, or
 * %false if it should fall back to buffered I/O.
 *
 * DIO isn't well specified; when it's unsupported (either due to the request
 * being misaligned, or due to the file not supporting DIO at all), filesystems
 * either fall back to buffered I/O or return EINVAL.  For files that don't use
 * any special features like encryption or verity, ext4 has traditionally
 * returned EINVAL for misaligned DIO.  iomap_dio_rw() uses this convention too.
 * In this case, we should attempt the DIO, *not* fall back to buffered I/O.
 *
 * In contrast, in cases where DIO is unsupported due to ext4 features, ext4
 * traditionally falls back to buffered I/O.
 *
 * This function implements the traditional ext4 behavior in all these cases.
 */
/**
 * ext4_should_use_dio - 判断是否应使用直接I/O (DIO) 方式进行读写
 * @iocb: 内核I/O控制块，包含文件指针、读写偏移量等信息
 * @iter: 用户空间数据迭代器，包含数据缓冲区信息
 *
 * 该函数通过检查inode的直接I/O对齐要求，以及当前读写位置和数据缓冲区
 * 的对齐情况，来决定当前操作是否可以使用直接I/O。
 *
 * 返回值: 如果应使用直接I/O则返回true，否则返回false。
 */
static bool ext4_should_use_dio(struct kiocb *iocb, struct iov_iter *iter)
{
	/* 获取与iocb关联的文件对应的inode结构体 */
	struct inode *inode = file_inode(iocb->ki_filp);
	
	/* 获取该inode的直接I/O对齐字节数要求 */
	u32 dio_align = ext4_dio_alignment(inode);

	/* 如果对齐要求为0，表示不支持直接I/O，返回false */
	if (dio_align == 0)
		return false;

	/* 如果对齐要求为1，表示无特殊对齐限制，直接返回true */
	if (dio_align == 1)
		return true;

	/* 
	 * 检查读写偏移量与数据缓冲区地址是否均满足对齐要求。
	 * 使用按位或运算符(|)将两者的对齐情况合并，只要有一个未对齐，
	 * IS_ALIGNED宏就会判定为未对齐，从而返回false；全部对齐则返回true。
	 */
	return IS_ALIGNED(iocb->ki_pos | iov_iter_alignment(iter), dio_align);
}


/**
 * ext4_dio_read_iter - ext4文件系统直接I/O(DIO)读取操作的迭代器
 * @iocb: 内核异步I/O控制块，包含文件偏移量、标志等信息
 * @to: 描述用户空间目标缓冲区的迭代器
 *
 * 该函数用于处理ext4文件系统的直接I/O读取请求。如果条件不允许使用
 * 直接I/O，则会回退到缓冲I/O(Buffered I/O)模式。
 *
 * 返回值: 读取的字节数，或负的错误码
 */
static ssize_t ext4_dio_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	/* 用于存储读取操作的返回值 */
	ssize_t ret;
	/* 获取文件对应的inode结构体，用于后续的文件级锁操作 */
	struct inode *inode = file_inode(iocb->ki_filp);

	/* 检查是否设置了无等待(IOCB_NOWAIT)标志 */
	if (iocb->ki_flags & IOCB_NOWAIT) {
		/* 以非阻塞方式尝试获取共享锁，若获取失败则直接返回EAGAIN错误 */
		if (!inode_trylock_shared(inode))
			return -EAGAIN;
	} else {
		/* 阻塞式获取inode的共享锁，允许并发读取 */
		inode_lock_shared(inode);
	}

	/* 判断当前读写操作和文件属性是否适合使用直接I/O */
	if (!ext4_should_use_dio(iocb, to)) {
		/* 不适合直接I/O，释放之前获取的inode共享锁 */
		inode_unlock_shared(inode);
		/*
		 * 如果在inode上执行的操作不受直接I/O支持，
		 * 则回退到缓冲I/O。在此处需要清除IOCB_DIRECT标志，
		 * 以确保在调用generic_file_read_iter()时不会进入
		 * 直接I/O的代码路径。
		 */
		iocb->ki_flags &= ~IOCB_DIRECT;
		/* 走常规的缓冲读取路径 */
		return generic_file_read_iter(iocb, to);
	}

	/* 执行基于iomap的直接I/O读写操作 */
	ret = iomap_dio_rw(iocb, to, &ext4_iomap_ops, NULL, 0, NULL, 0);
	/* 直接I/O操作完成，释放inode共享锁 */
	inode_unlock_shared(inode);

	/* 更新文件的最后访问时间 */
	file_accessed(iocb->ki_filp);
	/* 返回读取到的字节数或错误码 */
	return ret;
}


#ifdef CONFIG_FS_DAX
static ssize_t ext4_dax_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!inode_trylock_shared(inode))
			return -EAGAIN;
	} else {
		inode_lock_shared(inode);
	}
	/*
	 * Recheck under inode lock - at this point we are sure it cannot
	 * change anymore
	 */
	if (!IS_DAX(inode)) {
		inode_unlock_shared(inode);
		/* Fallback to buffered IO in case we cannot support DAX */
		return generic_file_read_iter(iocb, to);
	}
	ret = dax_iomap_rw(iocb, to, &ext4_iomap_ops);
	inode_unlock_shared(inode);

	file_accessed(iocb->ki_filp);
	return ret;
}
#endif

/**
 * ext4_file_read_iter - ext4文件读取的迭代器接口
 * @iocb: 内核异步I/O控制块，包含了文件指针、偏移量等I/O操作相关信息
 * @to: 目标iov_iter结构体，描述了用户空间缓冲区的位置和长度
 *
 * 该函数是ext4文件系统中处理文件读取操作的核心入口。
 * 它会根据文件系统的当前状态以及文件的存储和访问模式（如DAX、直接I/O），
 * 将读取请求分发到相应的具体处理函数中。
 *
 * 返回值: 读取的字节数，若出错则返回相应的负错误码
 */
static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	/* 从异步I/O控制块中获取对应的inode结构体 */
	struct inode *inode = file_inode(iocb->ki_filp);

	/* 检查文件系统是否被强制关闭 */
	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO; /* 若已强制关闭，返回I/O错误码 */

	/* 如果请求读取的长度为0，则直接返回0 */
	if (!iov_iter_count(to))
		return 0; /* skip atime - 跳过访问时间的更新 */

#ifdef CONFIG_FS_DAX
	/* 如果启用了DAX（直接访问）配置且当前文件支持DAX模式 */
	if (IS_DAX(inode))
		return ext4_dax_read_iter(iocb, to); /* 走DAX读取路径 */
#endif
	/* 如果I/O控制块标志中包含直接I/O（IOCB_DIRECT）标志 */
	if (iocb->ki_flags & IOCB_DIRECT)
		return ext4_dio_read_iter(iocb, to); /* 走直接I/O读取路径 */

	/* 默认情况：走通用的缓冲区缓存读取路径 */
	return generic_file_read_iter(iocb, to);
}


static ssize_t ext4_file_splice_read(struct file *in, loff_t *ppos,
				     struct pipe_inode_info *pipe,
				     size_t len, unsigned int flags)
{
	struct inode *inode = file_inode(in);

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;
	return filemap_splice_read(in, ppos, pipe, len, flags);
}

/*
 * Called when an inode is released. Note that this is different
 * from ext4_file_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */
static int ext4_release_file(struct inode *inode, struct file *filp)
{
	if (ext4_test_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE)) {
		ext4_alloc_da_blocks(inode);
		ext4_clear_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);
	}
	/* if we are the last writer on the inode, drop the block reservation */
	if ((filp->f_mode & FMODE_WRITE) &&
			(atomic_read(&inode->i_writecount) == 1) &&
			!EXT4_I(inode)->i_reserved_data_blocks) {
		down_write(&EXT4_I(inode)->i_data_sem);
		ext4_discard_preallocations(inode);
		up_write(&EXT4_I(inode)->i_data_sem);
	}
	if (is_dx(inode) && filp->private_data)
		ext4_htree_free_dir_info(filp->private_data);

	return 0;
}

/*
 * This tests whether the IO in question is block-aligned or not.
 * Ext4 utilizes unwritten extents when hole-filling during direct IO, and they
 * are converted to written only after the IO is complete.  Until they are
 * mapped, these blocks appear as holes, so dio_zero_block() will assume that
 * it needs to zero out portions of the start and/or end block.  If 2 AIO
 * threads are at work on the same unwritten block, they must be synchronized
 * or one thread will zero the other's data, causing corruption.
 */
static bool
ext4_unaligned_io(struct inode *inode, struct iov_iter *from, loff_t pos)
{
	struct super_block *sb = inode->i_sb;
	unsigned long blockmask = sb->s_blocksize - 1;

	if ((pos | iov_iter_alignment(from)) & blockmask)
		return true;

	return false;
}

static bool
ext4_extending_io(struct inode *inode, loff_t offset, size_t len)
{
	if (offset + len > i_size_read(inode) ||
	    offset + len > EXT4_I(inode)->i_disksize)
		return true;
	return false;
}

/* Is IO overwriting allocated or initialized blocks? */
static bool ext4_overwrite_io(struct inode *inode,
			      loff_t pos, loff_t len, bool *unwritten)
{
	struct ext4_map_blocks map;
	unsigned int blkbits = inode->i_blkbits;
	int err, blklen;

	if (pos + len > i_size_read(inode))
		return false;

	map.m_lblk = pos >> blkbits;
	map.m_len = EXT4_MAX_BLOCKS(len, pos, blkbits);
	blklen = map.m_len;

	err = ext4_map_blocks(NULL, inode, &map, 0);
	if (err != blklen)
		return false;
	/*
	 * 'err==len' means that all of the blocks have been preallocated,
	 * regardless of whether they have been initialized or not. We need to
	 * check m_flags to distinguish the unwritten extents.
	 */
	*unwritten = !(map.m_flags & EXT4_MAP_MAPPED);
	return true;
}

static ssize_t ext4_generic_write_checks(struct kiocb *iocb,
					 struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	if (unlikely(IS_IMMUTABLE(inode)))
		return -EPERM;

	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		return ret;

	/*
	 * If we have encountered a bitmap-format file, the size limit
	 * is smaller than s_maxbytes, which is for extent-mapped files.
	 */
	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

		if (iocb->ki_pos >= sbi->s_bitmap_maxbytes)
			return -EFBIG;
		iov_iter_truncate(from, sbi->s_bitmap_maxbytes - iocb->ki_pos);
	}

	return iov_iter_count(from);
}

static ssize_t ext4_write_checks(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret, count;

	count = ext4_generic_write_checks(iocb, from);
	if (count <= 0)
		return count;

	ret = file_modified(iocb->ki_filp);
	if (ret)
		return ret;
	return count;
}

static ssize_t ext4_buffered_write_iter(struct kiocb *iocb,
					struct iov_iter *from)
{
	ssize_t ret;
	struct inode *inode = file_inode(iocb->ki_filp);

	if (iocb->ki_flags & IOCB_NOWAIT)
		return -EOPNOTSUPP;

	inode_lock(inode);
	ret = ext4_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	ret = generic_perform_write(iocb, from);

out:
	inode_unlock(inode);
	if (unlikely(ret <= 0))
		return ret;
	return generic_write_sync(iocb, ret);
}

static ssize_t ext4_handle_inode_extension(struct inode *inode, loff_t offset,
					   ssize_t written, ssize_t count)
{
	handle_t *handle;

	lockdep_assert_held_write(&inode->i_rwsem);
	handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (ext4_update_inode_size(inode, offset + written)) {
		int ret = ext4_mark_inode_dirty(handle, inode);
		if (unlikely(ret)) {
			ext4_journal_stop(handle);
			return ret;
		}
	}

	if ((written == count) && inode->i_nlink)
		ext4_orphan_del(handle, inode);
	ext4_journal_stop(handle);

	return written;
}

/*
 * Clean up the inode after DIO or DAX extending write has completed and the
 * inode size has been updated using ext4_handle_inode_extension().
 */
static void ext4_inode_extension_cleanup(struct inode *inode, bool need_trunc)
{
	lockdep_assert_held_write(&inode->i_rwsem);
	if (need_trunc) {
		ext4_truncate_failed_write(inode);
		/*
		 * If the truncate operation failed early, then the inode may
		 * still be on the orphan list. In that case, we need to try
		 * remove the inode from the in-memory linked list.
		 */
		if (inode->i_nlink)
			ext4_orphan_del(NULL, inode);
		return;
	}
	/*
	 * If i_disksize got extended either due to writeback of delalloc
	 * blocks or extending truncate while the DIO was running we could fail
	 * to cleanup the orphan list in ext4_handle_inode_extension(). Do it
	 * now.
	 */
	if (ext4_inode_orphan_tracked(inode) && inode->i_nlink) {
		handle_t *handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);

		if (IS_ERR(handle)) {
			/*
			 * The write has successfully completed. Not much to
			 * do with the error here so just cleanup the orphan
			 * list and hope for the best.
			 */
			ext4_orphan_del(NULL, inode);
			return;
		}
		ext4_orphan_del(handle, inode);
		ext4_journal_stop(handle);
	}
}

static int ext4_dio_write_end_io(struct kiocb *iocb, ssize_t size,
				 int error, unsigned int flags)
{
	loff_t pos = iocb->ki_pos;
	struct inode *inode = file_inode(iocb->ki_filp);


	if (!error && size && (flags & IOMAP_DIO_UNWRITTEN) &&
			(iocb->ki_flags & IOCB_ATOMIC))
		error = ext4_convert_unwritten_extents_atomic(NULL, inode, pos,
							      size);
	else if (!error && size && flags & IOMAP_DIO_UNWRITTEN)
		error = ext4_convert_unwritten_extents(NULL, inode, pos, size);
	if (error)
		return error;
	/*
	 * Note that EXT4_I(inode)->i_disksize can get extended up to
	 * inode->i_size while the I/O was running due to writeback of delalloc
	 * blocks. But the code in ext4_iomap_alloc() is careful to use
	 * zeroed/unwritten extents if this is possible; thus we won't leave
	 * uninitialized blocks in a file even if we didn't succeed in writing
	 * as much as we intended. Also we can race with truncate or write
	 * expanding the file so we have to be a bit careful here.
	 */
	if (pos + size <= READ_ONCE(EXT4_I(inode)->i_disksize) &&
	    pos + size <= i_size_read(inode))
		return 0;
	error = ext4_handle_inode_extension(inode, pos, size, size);
	return error < 0 ? error : 0;
}

static const struct iomap_dio_ops ext4_dio_write_ops = {
	.end_io = ext4_dio_write_end_io,
};

/*
 * The intention here is to start with shared lock acquired then see if any
 * condition requires an exclusive inode lock. If yes, then we restart the
 * whole operation by releasing the shared lock and acquiring exclusive lock.
 *
 * - For unaligned_io we never take shared lock as it may cause data corruption
 *   when two unaligned IO tries to modify the same block e.g. while zeroing.
 *
 * - For extending writes case we don't take the shared lock, since it requires
 *   updating inode i_disksize and/or orphan handling with exclusive lock.
 *
 * - shared locking will only be true mostly with overwrites, including
 *   initialized blocks and unwritten blocks.
 *
 * - Otherwise we will switch to exclusive i_rwsem lock.
 */
/**
 * ext4_dio_write_checks - ext4直接I/O写操作的检查与锁管理
 * @iocb: 内核I/O控制块，包含文件指针、偏移量及I/O标志等信息
 * @from: 用户空间数据迭代器，包含待写入的数据
 * @ilock_shared: 指向布尔值的指针，指示当前是否持有inode的共享锁；函数内可能会升级为排他锁
 * @extend: 指向布尔值的指针，用于返回本次写操作是否会扩展文件大小
 * @dio_flags: 指向整型的指针，用于返回直接I/O的执行标志（如强制等待）
 *
 * 该函数在执行ext4直接I/O写操作前进行一系列前置检查，包括通用写检查、
 * I/O对齐与覆盖情况判断，并根据检查结果决定是否需要将共享锁升级为排他锁，
 * 以及设置相应的DIO标志。若检查失败或中途需重试，会妥善处理锁的释放与重获取。
 *
 * Return: 成功时返回待写入的字节数，失败时返回负的错误码
 */
static ssize_t ext4_dio_write_checks(struct kiocb *iocb, struct iov_iter *from,
				     bool *ilock_shared, bool *extend,
				     int *dio_flags)
{
	/* 从iocb中获取对应的文件结构体指针 */
	struct file *file = iocb->ki_filp;
	/* 获取文件对应的inode结构体 */
	struct inode *inode = file_inode(file);
	/* 写入偏移量 */
	loff_t offset;
	/* 待写入的字节数 */
	size_t count;
	/* 函数返回值/执行结果 */
	ssize_t ret;
	/* 标识是否为覆盖写、是否非对齐I/O、是否写入未分配的extent */
	bool overwrite, unaligned_io, unwritten;

restart:
	/* 执行通用写操作检查（如权限、资源限制等），返回允许写入的字节数 */
	ret = ext4_generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	/* 获取当前的写入偏移量 */
	offset = iocb->ki_pos;
	/* 获取经通用检查后允许写入的字节数 */
	count = ret;

	/* 检查是否为非对齐I/O（偏移量或数据长度未按块大小对齐） */
	unaligned_io = ext4_unaligned_io(inode, from, offset);
	/* 检查本次写操作是否会扩展文件大小 */
	*extend = ext4_extending_io(inode, offset, count);
	/* 检查是否为覆盖写，并判断覆盖区域是否包含未分配的extent */
	overwrite = ext4_overwrite_io(inode, offset, count, &unwritten);

	/*
	 * 判断是否需要将共享锁升级为排他锁。以下情况必须持有排他锁：
	 * 1. 需要在 file_modified() 中修改安全信息（!IS_NOSEC(inode)）；
	 * 2. 写操作会扩展文件大小（*extend）；
	 * 3. 非覆盖写（!overwrite）；
	 * 4. 非对齐I/O且写入未分配的extent（unaligned_io && unwritten），
	 *    因为此时可能需要对部分块进行填零操作。
	 *
	 * 注意：只要非对齐写是纯粹的覆盖写，就允许在共享锁下进行。
	 * 否则，并发的非对齐写可能会因为DIO层的部分块填零操作
	 * 导致数据损坏，因此这类I/O必须排他执行。
	 */
	if (*ilock_shared &&
	    ((!IS_NOSEC(inode) || *extend || !overwrite ||
	     (unaligned_io && unwritten)))) {
		/* 如果指定了不等待（NOWAIT），直接返回 -EAGAIN 避免阻塞 */
		if (iocb->ki_flags & IOCB_NOWAIT) {
			ret = -EAGAIN;
			goto out;
		}
		/* 释放当前的共享锁 */
		inode_unlock_shared(inode);
		/* 更新标记，指示当前已不再是共享锁 */
		*ilock_shared = false;
		/* 重新获取排他锁 */
		inode_lock(inode);
		/* 拿到排他锁后，需重新进行各项检查，防止状态在换锁期间发生变化 */
		goto restart;
	}

	/*
	 * 锁状态确定后，决定直接I/O的标志和排他性要求。
	 * 我们不使用 DIO_OVERWRITE_ONLY 标志，因为相关行为已经在此处强制保证。
	 * 如果写操作是非覆盖写或扩展写，前面已经持有了排他锁，
	 * 因此需要排空所有正在执行的直接I/O，并设置强制等待标志。
	 */
	if (!*ilock_shared && (unaligned_io || *extend)) {
		/* 在排他锁下，如果遇到 NOWAIT 请求，直接返回 -EAGAIN */
		if (iocb->ki_flags & IOCB_NOWAIT) {
			ret = -EAGAIN;
			goto out;
		}
		/* 如果是非对齐I/O，且不是纯覆盖写或写了未分配的extent，需等待其他DIO完成 */
		if (unaligned_io && (!overwrite || unwritten))
			inode_dio_wait(inode);
		/* 设置强制等待标志，确保当前DIO提交后等待完成再返回 */
		*dio_flags = IOMAP_DIO_FORCE_WAIT;
	}

	/* 更新文件的修改时间等元数据，可能涉及安全标记的修改 */
	ret = file_modified(file);
	if (ret < 0)
		goto out;

	/* 检查全部通过，返回待写入的字节数 */
	return count;
out:
	/* 退出处理：根据当前锁的状态释放对应的inode锁 */
	if (*ilock_shared)
		inode_unlock_shared(inode);
	else
		inode_unlock(inode);
	return ret;
}


/**
 * ext4_dio_write_iter - ext4 文件系统直接 I/O (DIO) 写入操作的迭代器函数
 * @iocb: 异步 I/O 控制块，包含文件指针、偏移量等信息
 * @from: 用户空间的数据迭代器，包含待写入的数据
 *
 * 此函数负责处理 ext4 的直接 I/O 写入请求。它会根据写入是否扩展文件、
 * 是否允许等待等条件获取适当的 inode 锁，并在必要时回退到缓冲 I/O。
 * 对于扩展文件大小的写入，会将其加入孤儿 inode 列表以防止系统崩溃时数据损坏，
 * 最后在写入完成后进行相应的清理和页缓存无效化操作。
 *
 * 返回值: 成功写入的字节数，失败时返回负的错误码。
 */
static ssize_t ext4_dio_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret;                 // 函数返回值，用于存储写入字节数或错误码
	handle_t *handle;            // 事务处理句柄，用于日志记录
	struct inode *inode = file_inode(iocb->ki_filp); // 获取文件对应的 inode 结构
	loff_t offset = iocb->ki_pos; // 获取写入的起始偏移量
	size_t count = iov_iter_count(from); // 获取待写入的数据总字节数
	bool extend = false;         // 标记此次写入是否会扩展文件大小
	bool ilock_shared = true;    // 标记是否可以使用共享 inode 锁（初始假设可以）
	int dio_flags = 0;           // 直接 I/O 的标志位

	/*
	 * Quick check here without any i_rwsem lock to see if it is extending
	 * IO. A more reliable check is done in ext4_dio_write_checks() with
	 * proper locking in place.
	 * 此处不持有 i_rwsem 锁进行快速检查，判断是否为扩展文件大小的 I/O。
	 * 更可靠的检查会在 ext4_dio_write_checks() 中在适当加锁的状态下进行。
	 */
	if (offset + count > i_size_read(inode))
		ilock_shared = false;    // 如果写入超出当前文件大小，则需要排他锁，不能使用共享锁

	// 根据是否设置了 IOCB_NOWAIT 标志，选择非阻塞或阻塞方式获取 inode 锁
	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (ilock_shared) {
			// 尝试获取共享锁，若失败立即返回 -EAGAIN
			if (!inode_trylock_shared(inode))
				return -EAGAIN;
		} else {
			// 尝试获取排他锁，若失败立即返回 -EAGAIN
			if (!inode_trylock(inode))
				return -EAGAIN;
		}
	} else {
		if (ilock_shared)
			inode_lock_shared(inode); // 阻塞等待获取共享锁
		else
			inode_lock(inode);        // 阻塞等待获取排他锁
	}

	/* Fallback to buffered I/O if the inode does not support direct I/O.
	 * 如果 inode 不支持直接 I/O，则回退到缓冲 I/O。
	 */
	if (!ext4_should_use_dio(iocb, from)) {
		if (ilock_shared)
			inode_unlock_shared(inode); // 释放共享锁
		else
			inode_unlock(inode);        // 释放排他锁
		return ext4_buffered_write_iter(iocb, from); // 调用缓冲写入函数
	}

	/*
	 * Prevent inline data from being created since we are going to allocate
	 * blocks for DIO. We know the inode does not currently have inline data
	 * because ext4_should_use_dio() checked for it, but we have to clear
	 * the state flag before the write checks because a lock cycle could
	 * introduce races with other writers.
	 * 阻止创建内联数据，因为我们将要为 DIO 分配数据块。
	 * 我们知道该 inode 当前没有内联数据，因为 ext4_should_use_dio() 已经检查过，
	 * 但我们必须在写入检查之前清除该状态标志，因为锁的循环周期可能会引入与其他写入者的竞争。
	 */
	ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);

	// 执行 DIO 写入前的详细检查，可能会修改锁的状态、扩展标志和 dio_flags
	ret = ext4_dio_write_checks(iocb, from, &ilock_shared, &extend,
				    &dio_flags);
	if (ret <= 0)
		return ret; // 检查失败或无需写入，直接返回

	// 重新获取更新后的偏移量和待写入字节数
	offset = iocb->ki_pos;
	count = ret;

	// 如果此次写入需要扩展文件大小，则需启动事务并将其加入孤儿列表
	if (extend) {
		// 启动日志事务，预留 2 个块的空间
		handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle); // 事务启动失败，记录错误码
			goto out;              // 跳转到解锁并退出流程
		}

		// 将 inode 加入孤儿列表，防止写入过程中崩溃导致的数据不一致
		ret = ext4_orphan_add(handle, inode);
		ext4_journal_stop(handle); // 停止事务
		if (ret)
			goto out;          // 添加孤儿节点失败，跳转到退出流程
	}

	// 执行实际的直接 I/O 读写操作
	ret = iomap_dio_rw(iocb, from, &ext4_iomap_ops, &ext4_dio_write_ops,
			   dio_flags, NULL, 0);
	if (ret == -ENOTBLK)
		ret = 0; // -ENOTBLK 表示回退到缓冲 I/O，此处视为正常，将返回值置为 0

	// 如果是扩展写入，需要进行写入后的清理工作
	if (extend) {
		/*
		 * We always perform extending DIO write synchronously so by
		 * now the IO is completed and ext4_handle_inode_extension()
		 * was called. Cleanup the inode in case of error or race with
		 * writeback of delalloc blocks.
		 * 我们总是同步执行扩展 DIO 写入，因此到现在 IO 已完成，
		 * 且 ext4_handle_inode_extension() 已被调用。
		 * 在发生错误或与延迟分配块的回写发生竞争时，清理 inode。
		 */
		WARN_ON_ONCE(ret == -EIOCBQUEUED); // 扩展写入不应异步排队，触发警告
		ext4_inode_extension_cleanup(inode, ret < 0); // 扩展写入完成后的清理
	}

out:
	// 根据当前锁的状态释放对应的 inode 锁
	if (ilock_shared)
		inode_unlock_shared(inode);
	else
		inode_unlock(inode);

	// 如果 DIO 写入成功，但迭代器中仍有未写入的数据，则回退到缓冲 I/O 处理剩余部分
	if (ret >= 0 && iov_iter_count(from)) {
		ssize_t err;
		loff_t endbyte;

		/*
		 * There is no support for atomic writes on buffered-io yet,
		 * we should never fallback to buffered-io for DIO atomic
		 * writes.
		 * 目前缓冲 I/O 尚不支持原子写入，
		 * 我们绝不应该为 DIO 原子写入回退到缓冲 I/O。
		 */
		WARN_ON_ONCE(iocb->ki_flags & IOCB_ATOMIC); // 若为原子写入回退则触发警告

		offset = iocb->ki_pos; // 更新偏移量到 DIO 写入结束的位置
		// 使用缓冲 I/O 写入剩余数据
		err = ext4_buffered_write_iter(iocb, from);
		if (err < 0)
			return err; // 缓冲写入失败，返回错误码

		/*
		 * We need to ensure that the pages within the page cache for
		 * the range covered by this I/O are written to disk and
		 * invalidated. This is in attempt to preserve the expected
		 * direct I/O semantics in the case we fallback to buffered I/O
		 * to complete off the I/O request.
		 * 我们需要确保页缓存中涵盖此 I/O 范围的页已被写入磁盘并被无效化。
		 * 这是为了在回退到缓冲 I/O 完成 I/O 请求时，尽量保持预期的直接 I/O 语义。
		 */
		ret += err; // 累加成功写入的字节数
		endbyte = offset + err - 1; // 计算缓冲写入的结束字节位置
		// 将页缓存中的脏页回写至磁盘并等待完成
		err = filemap_write_and_wait_range(iocb->ki_filp->f_mapping,
						   offset, endbyte);
		if (!err)
			// 回写成功后，无效化该范围内的页缓存，确保后续读取直接访问磁盘
			invalidate_mapping_pages(iocb->ki_filp->f_mapping,
						 offset >> PAGE_SHIFT,
						 endbyte >> PAGE_SHIFT);
	}

	return ret; // 返回总写入字节数或错误码
}


#ifdef CONFIG_FS_DAX
static ssize_t
ext4_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret;
	size_t count;
	loff_t offset;
	handle_t *handle;
	bool extend = false;
	struct inode *inode = file_inode(iocb->ki_filp);

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!inode_trylock(inode))
			return -EAGAIN;
	} else {
		inode_lock(inode);
	}

	ret = ext4_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	offset = iocb->ki_pos;
	count = iov_iter_count(from);

	if (offset + count > EXT4_I(inode)->i_disksize) {
		handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			goto out;
		}

		ret = ext4_orphan_add(handle, inode);
		if (ret) {
			ext4_journal_stop(handle);
			goto out;
		}

		extend = true;
		ext4_journal_stop(handle);
	}

	ret = dax_iomap_rw(iocb, from, &ext4_iomap_ops);

	if (extend) {
		ret = ext4_handle_inode_extension(inode, offset, ret, count);
		ext4_inode_extension_cleanup(inode, ret < (ssize_t)count);
	}
out:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}
#endif

/**
 * ext4_file_write_iter - ext4 文件异步/同步写入操作的核心入口函数
 * @iocb: 内核I/O控制块，包含文件指针、写入偏移量、I/O标志等信息
 * @from: 用户空间数据迭代器，包含待写入的数据及长度
 *
 * 该函数是 ext4 文件系统中 file_operations.write_iter 的实现。
 * 根据文件系统当前状态、挂载选项（如DAX模式）以及I/O标志（如直接I/O、原子写入），
 * 将写入请求分发到对应的底层处理函数中执行。
 *
 * Return: 成功写入的字节数（ssize_t），失败时返回负的错误码
 */
static ssize_t
ext4_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	int ret;
	// 获取文件对应的 inode 结构，用于后续的文件属性和状态检查
	struct inode *inode = file_inode(iocb->ki_filp);

	// 检查文件系统是否处于紧急状态（如只读降级、致命错误等）
	ret = ext4_emergency_state(inode->i_sb);
	if (unlikely(ret))
		return ret;

#ifdef CONFIG_FS_DAX
	// 如果内核配置了 DAX (Direct Access) 模式，且当前 inode 启用了 DAX
	// 则直接通过 DAX 模式写入（绕过页缓存，直接访问持久内存）
	if (IS_DAX(inode))
		return ext4_dax_write_iter(iocb, from);
#endif

	// 处理原子写入（Atomic Write）请求
	if (iocb->ki_flags & IOCB_ATOMIC) {
		// 获取本次请求写入的数据长度
		size_t len = iov_iter_count(from);

		// 检查写入长度是否在文件系统支持的原子写入单元范围内
		// 若小于最小值或大于最大值，则直接返回无效参数错误
		if (len < EXT4_SB(inode->i_sb)->s_awu_min ||
		    len > EXT4_SB(inode->i_sb)->s_awu_max)
			return -EINVAL;

		// 进行通用的原子写入合法性校验
		ret = generic_atomic_write_valid(iocb, from);
		if (ret)
			return ret;
	}

	// 根据 I/O 控制标志判断是否为直接 I/O (Direct I/O)
	if (iocb->ki_flags & IOCB_DIRECT)
		// 直接 I/O 模式：绕过页缓存，直接与块设备进行数据交互
		return ext4_dio_write_iter(iocb, from);
	else
		// 缓冲 I/O 模式：数据先写入页缓存，由内核后台线程负责刷盘
		return ext4_buffered_write_iter(iocb, from);
}


#ifdef CONFIG_FS_DAX
static vm_fault_t ext4_dax_huge_fault(struct vm_fault *vmf, unsigned int order)
{
	int error = 0;
	vm_fault_t result;
	int retries = 0;
	handle_t *handle = NULL;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct super_block *sb = inode->i_sb;

	/*
	 * We have to distinguish real writes from writes which will result in a
	 * COW page; COW writes should *not* poke the journal (the file will not
	 * be changed). Doing so would cause unintended failures when mounted
	 * read-only.
	 *
	 * We check for VM_SHARED rather than vmf->cow_page since the latter is
	 * unset for order != 0 (i.e. only in do_cow_fault); for
	 * other sizes, dax_iomap_fault will handle splitting / fallback so that
	 * we eventually come back with a COW page.
	 */
	bool write = (vmf->flags & FAULT_FLAG_WRITE) &&
		(vmf->vma->vm_flags & VM_SHARED);
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	unsigned long pfn;

	if (write) {
		sb_start_pagefault(sb);
		file_update_time(vmf->vma->vm_file);
		filemap_invalidate_lock_shared(mapping);
retry:
		handle = ext4_journal_start_sb(sb, EXT4_HT_WRITE_PAGE,
					       EXT4_DATA_TRANS_BLOCKS(sb));
		if (IS_ERR(handle)) {
			filemap_invalidate_unlock_shared(mapping);
			sb_end_pagefault(sb);
			return VM_FAULT_SIGBUS;
		}
	} else {
		filemap_invalidate_lock_shared(mapping);
	}
	result = dax_iomap_fault(vmf, order, &pfn, &error, &ext4_iomap_ops);
	if (write) {
		ext4_journal_stop(handle);

		if ((result & VM_FAULT_ERROR) && error == -ENOSPC &&
		    ext4_should_retry_alloc(sb, &retries))
			goto retry;
		/* Handling synchronous page fault? */
		if (result & VM_FAULT_NEEDDSYNC)
			result = dax_finish_sync_fault(vmf, order, pfn);
		filemap_invalidate_unlock_shared(mapping);
		sb_end_pagefault(sb);
	} else {
		filemap_invalidate_unlock_shared(mapping);
	}

	return result;
}

static vm_fault_t ext4_dax_fault(struct vm_fault *vmf)
{
	return ext4_dax_huge_fault(vmf, 0);
}

static const struct vm_operations_struct ext4_dax_vm_ops = {
	.fault		= ext4_dax_fault,
	.huge_fault	= ext4_dax_huge_fault,
	.page_mkwrite	= ext4_dax_fault,
	.pfn_mkwrite	= ext4_dax_fault,
};
#else
#define ext4_dax_vm_ops	ext4_file_vm_ops
#endif

static const struct vm_operations_struct ext4_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite   = ext4_page_mkwrite,
};

static int ext4_file_mmap_prepare(struct vm_area_desc *desc)
{
	int ret;
	struct file *file = desc->file;
	struct inode *inode = file->f_mapping->host;
	struct dax_device *dax_dev = EXT4_SB(inode->i_sb)->s_daxdev;

	if (file->f_mode & FMODE_WRITE)
		ret = ext4_emergency_state(inode->i_sb);
	else
		ret = ext4_forced_shutdown(inode->i_sb) ? -EIO : 0;
	if (unlikely(ret))
		return ret;

	/*
	 * We don't support synchronous mappings for non-DAX files and
	 * for DAX files if underneath dax_device is not synchronous.
	 */
	if (!daxdev_mapping_supported(desc, file_inode(file), dax_dev))
		return -EOPNOTSUPP;

	file_accessed(file);
	if (IS_DAX(file_inode(file))) {
		desc->vm_ops = &ext4_dax_vm_ops;
		vma_desc_set_flags(desc, VMA_HUGEPAGE_BIT);
	} else {
		desc->vm_ops = &ext4_file_vm_ops;
	}
	return 0;
}

static int ext4_sample_last_mounted(struct super_block *sb,
				    struct vfsmount *mnt)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct path path;
	char buf[64], *cp;
	handle_t *handle;
	int err;

	sb_dbg(sb, "vfsmount %s\n", mnt->mnt_root->d_name.name);
	if (likely(ext4_test_mount_flag(sb, EXT4_MF_MNTDIR_SAMPLED)))
		return 0;

	if (ext4_emergency_state(sb) || sb_rdonly(sb) ||
	    !sb_start_intwrite_trylock(sb))
		return 0;

	ext4_set_mount_flag(sb, EXT4_MF_MNTDIR_SAMPLED);
	/*
	 * Sample where the filesystem has been mounted and
	 * store it in the superblock for sysadmin convenience
	 * when trying to sort through large numbers of block
	 * devices or filesystem images.
	 */
	path.mnt = mnt;
	path.dentry = mnt->mnt_root;
	cp = d_path(&path, buf, sizeof(buf));
	err = 0;
	if (IS_ERR(cp))
		goto out;

	handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
	err = PTR_ERR(handle);
	if (IS_ERR(handle))
		goto out;
	sb_dbg(sb, "journal handle %p\n", handle);
	BUFFER_TRACE(sbi->s_sbh, "get_write_access");
	err = ext4_journal_get_write_access(handle, sb, sbi->s_sbh,
					    EXT4_JTR_NONE);
	if (err)
		goto out_journal;
	lock_buffer(sbi->s_sbh);
	strtomem_pad(sbi->s_es->s_last_mounted, cp, 0);
	ext4_superblock_csum_set(sb);
	unlock_buffer(sbi->s_sbh);
	ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
out_journal:
	ext4_journal_stop(handle);
out:
	sb_end_intwrite(sb);
	return err;
}

static int ext4_file_open(struct inode *inode, struct file *filp)
{
	int ret;

	inode_dbg(inode, "name %s\n", filp->f_path.dentry->d_name.name);
	if (filp->f_mode & FMODE_WRITE)
		ret = ext4_emergency_state(inode->i_sb);
	else
		ret = ext4_forced_shutdown(inode->i_sb) ? -EIO : 0;
	if (unlikely(ret))
		return ret;

	ret = ext4_sample_last_mounted(inode->i_sb, filp->f_path.mnt);
	if (ret)
		return ret;

	ret = fscrypt_file_open(inode, filp);
	if (ret)
		return ret;

	ret = fsverity_file_open(inode, filp);
	if (ret)
		return ret;

	/*
	 * Set up the jbd2_inode if we are opening the inode for
	 * writing and the journal is present
	 */
	if (filp->f_mode & FMODE_WRITE) {
		ret = ext4_inode_attach_jinode(inode);
		if (ret < 0)
			return ret;
	}

	if (ext4_inode_can_atomic_write(inode))
		filp->f_mode |= FMODE_CAN_ATOMIC_WRITE;

	filp->f_mode |= FMODE_NOWAIT | FMODE_CAN_ODIRECT;
	return dquot_file_open(inode, filp);
}

/*
 * ext4_llseek() handles both block-mapped and extent-mapped maxbytes values
 * by calling generic_file_llseek_size() with the appropriate maxbytes
 * value for each.
 */
loff_t ext4_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	loff_t maxbytes = ext4_get_maxbytes(inode);

	switch (whence) {
	default:
		return generic_file_llseek_size(file, offset, whence,
						maxbytes, i_size_read(inode));
	case SEEK_HOLE:
		inode_lock_shared(inode);
		offset = iomap_seek_hole(inode, offset,
					 &ext4_iomap_report_ops);
		inode_unlock_shared(inode);
		break;
	case SEEK_DATA:
		inode_lock_shared(inode);
		offset = iomap_seek_data(inode, offset,
					 &ext4_iomap_report_ops);
		inode_unlock_shared(inode);
		break;
	}

	if (offset < 0)
		return offset;
	return vfs_setpos(file, offset, maxbytes);
}

const struct file_operations ext4_file_operations = {
	.llseek		= ext4_llseek,
	.read_iter	= ext4_file_read_iter,
	.write_iter	= ext4_file_write_iter,
	.iopoll		= iocb_bio_iopoll,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext4_compat_ioctl,
#endif
	.mmap_prepare	= ext4_file_mmap_prepare,
	.open		= ext4_file_open,
	.release	= ext4_release_file,
	.fsync		= ext4_sync_file,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= ext4_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= ext4_fallocate,
	.fop_flags	= FOP_MMAP_SYNC | FOP_BUFFER_RASYNC |
			  FOP_DIO_PARALLEL_WRITE |
			  FOP_DONTCACHE,
	.setlease	= generic_setlease,
};

const struct inode_operations ext4_file_inode_operations = {
	.setattr	= ext4_setattr,
	.getattr	= ext4_file_getattr,
	.listxattr	= ext4_listxattr,
	.get_inode_acl	= ext4_get_acl,
	.set_acl	= ext4_set_acl,
	.fiemap		= ext4_fiemap,
	.fileattr_get	= ext4_fileattr_get,
	.fileattr_set	= ext4_fileattr_set,
};

