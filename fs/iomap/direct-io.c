// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 Red Hat, Inc.
 * Copyright (c) 2016-2025 Christoph Hellwig.
 */
#include <linux/bio-integrity.h>
#include <linux/blk-crypto.h>
#include <linux/fscrypt.h>
#include <linux/pagemap.h>
#include <linux/iomap.h>
#include <linux/task_io_accounting_ops.h>
#include <linux/fserror.h>
#include "internal.h"
#include "trace.h"

#include "../internal.h"

/*
 * Private flags for iomap_dio, must not overlap with the public ones in
 * iomap.h:
 */
#define IOMAP_DIO_NO_INVALIDATE	(1U << 26)
#define IOMAP_DIO_COMP_WORK	(1U << 27)
#define IOMAP_DIO_WRITE_THROUGH	(1U << 28)
#define IOMAP_DIO_NEED_SYNC	(1U << 29)
#define IOMAP_DIO_WRITE		(1U << 30)
#define IOMAP_DIO_USER_BACKED	(1U << 31)

struct iomap_dio {
	struct kiocb		*iocb;
	const struct iomap_dio_ops *dops;
	loff_t			i_size;
	loff_t			size;
	atomic_t		ref;
	unsigned		flags;
	int			error;
	size_t			done_before;
	bool			wait_for_completion;

	union {
		/* used during submission and for synchronous completion: */
		struct {
			struct iov_iter		*iter;
			struct task_struct	*waiter;
		} submit;

		/* used for aio completion: */
		struct {
			struct work_struct	work;
		} aio;
	};
};

static struct bio *iomap_dio_alloc_bio(const struct iomap_iter *iter,
		struct iomap_dio *dio, unsigned short nr_vecs, blk_opf_t opf)
{
	if (dio->dops && dio->dops->bio_set)
		return bio_alloc_bioset(iter->iomap.bdev, nr_vecs, opf,
					GFP_KERNEL, dio->dops->bio_set);
	return bio_alloc(iter->iomap.bdev, nr_vecs, opf, GFP_KERNEL);
}

static void iomap_dio_submit_bio(const struct iomap_iter *iter,
		struct iomap_dio *dio, struct bio *bio, loff_t pos)
{
	struct kiocb *iocb = dio->iocb;

	atomic_inc(&dio->ref);

	/* Sync dio can't be polled reliably */
	if ((iocb->ki_flags & IOCB_HIPRI) && !is_sync_kiocb(iocb)) {
		bio->bi_opf |= REQ_POLLED;
		WRITE_ONCE(iocb->private, bio);
	}

	if (dio->dops && dio->dops->submit_io) {
		dio->dops->submit_io(iter, bio, pos);
	} else {
		WARN_ON_ONCE(iter->iomap.flags & IOMAP_F_ANON_WRITE);
		blk_crypto_submit_bio(bio);
	}
}

static inline enum fserror_type iomap_dio_err_type(const struct iomap_dio *dio)
{
	if (dio->flags & IOMAP_DIO_WRITE)
		return FSERR_DIRECTIO_WRITE;
	return FSERR_DIRECTIO_READ;
}

static inline bool should_report_dio_fserror(const struct iomap_dio *dio)
{
	switch (dio->error) {
	case 0:
	case -EAGAIN:
	case -ENOTBLK:
		/* don't send fsnotify for success or magic retry codes */
		return false;
	default:
		return true;
	}
}

/**
 * iomap_dio_complete - 完成直接I/O（DIO）操作
 * @dio: 指向iomap_dio结构的指针，包含了直接I/O操作的上下文和状态信息
 *
 * 此函数在直接I/O操作完成后被调用，负责处理完成后的收尾工作，包括：
 * 调用特定文件系统的结束回调、报告文件系统错误、处理读取截断、
 * 使页缓存失效、更新文件偏移量、处理同步写入以及释放相关资源。
 *
 * Return: 返回完成的字节数（正数），或错误码（负数）
 */
ssize_t iomap_dio_complete(struct iomap_dio *dio)
{
	/* 获取直接I/O操作集（包含特定文件系统的结束处理回调等） */
	const struct iomap_dio_ops *dops = dio->dops;
	/* 获取关联的异步I/O控制块 */
	struct kiocb *iocb = dio->iocb;
	/* 获取I/O操作的起始偏移量 */
	loff_t offset = iocb->ki_pos;
	/* 初始化返回值为I/O操作过程中记录的错误码 */
	ssize_t ret = dio->error;

	/* 如果定义了操作集且包含结束I/O的回调函数，则调用该回调进行特定文件系统的处理 */
	if (dops && dops->end_io)
		ret = dops->end_io(iocb, dio->size, ret, dio->flags);

	/* 检查是否需要报告文件系统I/O错误，若需要则进行错误上报 */
	if (should_report_dio_fserror(dio))
		fserror_report_io(file_inode(iocb->ki_filp),
				  iomap_dio_err_type(dio), offset, dio->size,
				  dio->error, GFP_NOFS);

	/* 如果没有发生错误（likely提示编译器此分支发生的概率更高） */
	if (likely(!ret)) {
		/* 将返回值设置为成功传输的字节数 */
		ret = dio->size;

		/* 检查是否为短读（读取超出了文件当前大小） */
		if (offset + ret > dio->i_size &&
		    !(dio->flags & IOMAP_DIO_WRITE))
			/* 如果是短读，则将返回值截断为实际可读取的字节数 */
			ret = dio->i_size - offset;
	}

	/*
	 * 尝试再次使干净的页缓存失效。这些页缓存可能是由非直接读取预读缓存，
	 * 或者当写入源是文件的mmap映射区域时由get_user_pages()触发的缺页缓存。
	 * 这两种做法都相当疯狂，所以我们无法100%支持。如果失效操作失败，
	 * 那也没办法，数据依然已经成功写入了...
	 *
	 * 并且此页缓存失效操作必须在 ->end_io() 之后执行，因为某些文件系统
	 * 会在必要时在 ->end_io() 中将未写入的区段转换为实际分配的空间。否则，
	 * 并发的缓冲读取会从尚未写入的区段中缓存零数据。
	 */
	/* 如果没有错误、传输了数据、是写操作且未标记跳过失效操作，则执行写后页缓存失效 */
	if (!dio->error && dio->size && (dio->flags & IOMAP_DIO_WRITE) &&
	    !(dio->flags & IOMAP_DIO_NO_INVALIDATE))
		kiocb_invalidate_post_direct_write(iocb, dio->size);

	/* 标记直接I/O操作结束，唤醒等待该inode的DIO完成的其他进程 */
	inode_dio_end(file_inode(iocb->ki_filp));

	/* 如果返回值为正数，表示成功完成了部分或全部I/O操作 */
	if (ret > 0) {
		/* 更新异步I/O控制块中的文件偏移量，向前移动已完成的字节数 */
		iocb->ki_pos += ret;

		/*
		 * 如果这是DSYNC（数据同步）写入，确保在数据写入完成后，
		 * 将其冲刷到稳定存储中。
		 */
		if (dio->flags & IOMAP_DIO_NEED_SYNC)
			ret = generic_write_sync(iocb, ret);

		/* 如果同步操作成功，将之前已完成的字节数累加到当前返回值中 */
		if (ret > 0)
			ret += dio->done_before;
	}

	/* 跟踪记录直接I/O完成事件，用于内核调试和性能分析 */
	trace_iomap_dio_complete(iocb, dio->error, ret);

	/* 释放iomap_dio结构体占用的内存 */
	kfree(dio);

	/* 返回最终结果：成功传输的字节数或错误码 */
	return ret;
}

EXPORT_SYMBOL_GPL(iomap_dio_complete);

static void iomap_dio_complete_work(struct work_struct *work)
{
	struct iomap_dio *dio = container_of(work, struct iomap_dio, aio.work);
	struct kiocb *iocb = dio->iocb;

	iocb->ki_complete(iocb, iomap_dio_complete(dio));
}

/*
 * Set an error in the dio if none is set yet.  We have to use cmpxchg
 * as the submission context and the completion context(s) can race to
 * update the error.
 */
static inline void iomap_dio_set_error(struct iomap_dio *dio, int ret)
{
	cmpxchg(&dio->error, 0, ret);
}

/*
 * Called when dio->ref reaches zero from an I/O completion.
 */
static void iomap_dio_done(struct iomap_dio *dio)
{
	struct kiocb *iocb = dio->iocb;

	if (dio->wait_for_completion) {
		/*
		 * Synchronous I/O, task itself will handle any completion work
		 * that needs after IO. All we need to do is wake the task.
		 */
		struct task_struct *waiter = dio->submit.waiter;

		WRITE_ONCE(dio->submit.waiter, NULL);
		blk_wake_io_task(waiter);
		return;
	}

	/*
	 * Always run error completions in user context.  These are not
	 * performance critical and some code relies on taking sleeping locks
	 * for error handling.
	 */
	if (dio->error)
		dio->flags |= IOMAP_DIO_COMP_WORK;

	/*
	 * Never invalidate pages from this context to avoid deadlocks with
	 * buffered I/O completions when called from the ioend workqueue,
	 * or avoid sleeping when called directly from ->bi_end_io.
	 * Tough luck if you hit the tiny race with someone dirtying the range
	 * right between this check and the actual completion.
	 */
	if ((dio->flags & IOMAP_DIO_WRITE) &&
	    !(dio->flags & IOMAP_DIO_COMP_WORK)) {
		if (dio->iocb->ki_filp->f_mapping->nrpages)
			dio->flags |= IOMAP_DIO_COMP_WORK;
		else
			dio->flags |= IOMAP_DIO_NO_INVALIDATE;
	}

	if (dio->flags & IOMAP_DIO_COMP_WORK) {
		struct inode *inode = file_inode(iocb->ki_filp);

		/*
		 * Async DIO completion that requires filesystem level
		 * completion work gets punted to a work queue to complete as
		 * the operation may require more IO to be issued to finalise
		 * filesystem metadata changes or guarantee data integrity.
		 */
		INIT_WORK(&dio->aio.work, iomap_dio_complete_work);
		queue_work(inode->i_sb->s_dio_done_wq, &dio->aio.work);
		return;
	}

	WRITE_ONCE(iocb->private, NULL);
	iomap_dio_complete_work(&dio->aio.work);
}

/**
 * __iomap_dio_bio_end_io - 处理直接IO(direct I/O) bio的结束回调
 * @bio: 要处理的bio结构体
 * @inline_completion: 是否在inline上下文中完成bio
 * 
 * 此函数处理直接IO bio的完成工作，根据bio的类型和状态进行不同的清理操作。
 */
static void __iomap_dio_bio_end_io(struct bio *bio, bool inline_completion)
{
	struct iomap_dio *dio = bio->bi_private;  // 获取与bio关联的iomap_dio结构体

	if (bio_integrity(bio))
		fs_bio_integrity_free(bio);

	// 根据bio的标志位进行不同的处理
	if (dio->flags & IOMAP_DIO_BOUNCE) {  // 如果使用了bounce buffer
		bio_iov_iter_unbounce(bio, !!dio->error,  // 解除bounce buffer
				dio->flags & IOMAP_DIO_USER_BACKED);
		bio_put(bio);  // 释放bio
	} else if (dio->flags & IOMAP_DIO_USER_BACKED) {  // 如果是用户空间映射的bio
		bio_check_pages_dirty(bio);  // 检查页面是否被修改
	} else {  // 其他情况
		bio_release_pages(bio, false);  // 释放页面引用
		bio_put(bio);  // 释放bio
	}

	/* 不要再访问bio，因为我们已经放弃了它的引用 */

	// 减少dio的引用计数，如果减到0则完成dio
	if (atomic_dec_and_test(&dio->ref)) {
		/*
		 * 当已经在ioend完成工作队列中调用时，
		 * 避免为完成再进行一次上下文切换。
		 */
		if (inline_completion)
			dio->flags &= ~IOMAP_DIO_COMP_WORK;  // 清除完成工作标志
		iomap_dio_done(dio);  // 完成dio操作
	}
}


/**
 * iomap_dio_bio_end_io - 处理直接IO(DIO) bio的结束回调
 * @bio: 指向bio结构的指针，bio是块I/O操作的基本单位
 * 
 * 该函数作为bio完成时的回调函数，用于处理直接IO操作的结束工作。
 * 它会检查bio的执行状态，如果有错误则设置相应的错误码，
 * 然后调用__iomap_dio_bio_end_io继续处理后续工作。
 */
void iomap_dio_bio_end_io(struct bio *bio)
{
	// 从bio的私有字段获取iomap_dio结构体指针
	struct iomap_dio *dio = bio->bi_private;

	// 检查bio的执行状态，如果状态不为成功，则设置错误码
	if (bio->bi_status)
		iomap_dio_set_error(dio, blk_status_to_errno(bio->bi_status));
	
	// 调用内部函数处理bio的结束工作
	__iomap_dio_bio_end_io(bio, false);
}

EXPORT_SYMBOL_GPL(iomap_dio_bio_end_io);

u32 iomap_finish_ioend_direct(struct iomap_ioend *ioend)
{
	struct iomap_dio *dio = ioend->io_bio.bi_private;
	u32 vec_count = ioend->io_bio.bi_vcnt;

	if (ioend->io_error)
		iomap_dio_set_error(dio, ioend->io_error);
	__iomap_dio_bio_end_io(&ioend->io_bio, true);

	/*
	 * Return the number of bvecs completed as even direct I/O completions
	 * do significant per-folio work and we'll still want to give up the
	 * CPU after a lot of completions.
	 */
	return vec_count;
}

static int iomap_dio_zero(const struct iomap_iter *iter, struct iomap_dio *dio,
		loff_t pos, unsigned len)
{
	struct inode *inode = file_inode(dio->iocb->ki_filp);
	struct bio *bio;
	struct folio *zero_folio = largest_zero_folio();
	int nr_vecs = max(1, i_blocksize(inode) / folio_size(zero_folio));

	if (!len)
		return 0;

	/*
	 * This limit shall never be reached as most filesystems have a
	 * maximum blocksize of 64k.
	 */
	if (WARN_ON_ONCE(nr_vecs > BIO_MAX_VECS))
		return -EINVAL;

	bio = iomap_dio_alloc_bio(iter, dio, nr_vecs,
				  REQ_OP_WRITE | REQ_SYNC | REQ_IDLE);
	fscrypt_set_bio_crypt_ctx(bio, inode, pos, GFP_KERNEL);
	bio->bi_iter.bi_sector = iomap_sector(&iter->iomap, pos);
	bio->bi_private = dio;
	bio->bi_end_io = iomap_dio_bio_end_io;

	while (len > 0) {
		unsigned int io_len = min(len, folio_size(zero_folio));

		bio_add_folio_nofail(bio, zero_folio, io_len, 0);
		len -= io_len;
	}
	iomap_dio_submit_bio(iter, dio, bio, pos);

	return 0;
}

/**
 * iomap_dio_bio_iter_one - 处理单个I/O映射的直接IO bio迭代
 * @iter: I/O迭代器，包含I/O操作的状态和信息
 * @dio: 直接I/O结构体，包含直接I/O操作的状态和信息
 * @pos: 当前I/O操作的起始位置
 * @alignment: 对齐要求
 * @op: 块设备操作标志
 * 
 * 该函数负责为直接I/O操作分配和设置一个bio结构体，并处理相关的I/O操作。
 * 它处理了bio的分配、加密上下文设置、扇区设置、优先级设置等，并根据操作类型
 * (写或读)进行相应的处理。最后提交bio进行I/O操作。
 */
static ssize_t iomap_dio_bio_iter_one(struct iomap_iter *iter,
		struct iomap_dio *dio, loff_t pos, unsigned int alignment,
		blk_opf_t op)
{
	unsigned int nr_vecs;    // bio中需要的向量数量
	struct bio *bio;        // bio结构体指针
	ssize_t ret;            // 返回值，表示操作结果或字节数

	// 根据是否需要缓冲，确定bio中需要的向量数量
	if (dio->flags & IOMAP_DIO_BOUNCE)
		nr_vecs = bio_iov_bounce_nr_vecs(dio->submit.iter, op);
	else
		nr_vecs = bio_iov_vecs_to_alloc(dio->submit.iter, BIO_MAX_VECS);

	// 分配bio结构体并设置相关属性
	bio = iomap_dio_alloc_bio(iter, dio, nr_vecs, op);
	fscrypt_set_bio_crypt_ctx(bio, iter->inode, pos, GFP_KERNEL);
	bio->bi_iter.bi_sector = iomap_sector(&iter->iomap, pos);
	bio->bi_write_hint = iter->inode->i_write_hint;
	bio->bi_ioprio = dio->iocb->ki_ioprio;
	bio->bi_private = dio;
	bio->bi_end_io = iomap_dio_bio_end_io;


	if (dio->flags & IOMAP_DIO_BOUNCE)
		ret = bio_iov_iter_bounce(bio, dio->submit.iter,
				iomap_max_bio_size(&iter->iomap), alignment);
	else
		ret = bio_iov_iter_get_pages(bio, dio->submit.iter,
					     alignment - 1);
	if (unlikely(ret))
		goto out_put_bio;    // 如果填充失败，释放bio并返回错误
	ret = bio->bi_iter.bi_size;    // 获取bio的大小

	/*
	 * 原子写bio必须覆盖完整的长度。如果不覆盖，则出错。
	 */
	if ((op & REQ_ATOMIC) && WARN_ON_ONCE(ret != iomap_length(iter))) {
		ret = -EINVAL;
		goto out_bio_release_pages;
	}

	if (iter->iomap.flags & IOMAP_F_INTEGRITY) {
		if (dio->flags & IOMAP_DIO_WRITE)
			fs_bio_integrity_generate(bio);
		else
			fs_bio_integrity_alloc(bio);
	}

	// 根据操作类型进行相应处理
	if (dio->flags & IOMAP_DIO_WRITE)
		task_io_account_write(ret);
	else if ((dio->flags & IOMAP_DIO_USER_BACKED) &&
		 !(dio->flags & IOMAP_DIO_BOUNCE))
		bio_set_pages_dirty(bio);     // 用户支持的读操作，标记页面为脏

	/*
	 * 我们只能轮询单个bio的I/O操作。
	 */
	if (iov_iter_count(dio->submit.iter))
		dio->iocb->ki_flags &= ~IOCB_HIPRI;    // 如果还有更多迭代，禁用高优先级I/O
	iomap_dio_submit_bio(iter, dio, bio, pos);    // 提交bio进行I/O操作
	return ret;

out_bio_release_pages:
	if (dio->flags & IOMAP_DIO_BOUNCE)
		bio_iov_iter_unbounce(bio, true, false);
	else
		bio_release_pages(bio, false);
out_put_bio:
	bio_put(bio);    // 释放bio
	return ret;      // 返回操作结果
}


/**
 * iomap_dio_bio_iter - 处理直接I/O操作的bio迭代
 * @iter: 指向iomap_iter结构的指针，包含I/O映射迭代信息
 * @dio: 指向iomap_dio结构的指针，包含直接I/O操作的状态信息
 * 
 * 该函数负责处理直接I/O操作的bio迭代，根据不同的I/O类型和标志执行相应的操作。
 * 它处理写入和读取操作，处理未写入区域、共享区域和新分配区域的情况，
 * 并确保适当的对齐和零填充。
 */
static int iomap_dio_bio_iter(struct iomap_iter *iter, struct iomap_dio *dio)
{
	const struct iomap *iomap = &iter->iomap;  // 获取当前的I/O映射
	struct inode *inode = iter->inode;        // 获取文件节点
	unsigned int fs_block_size = i_blocksize(inode), pad;  // 文件系统块大小
	const loff_t length = iomap_length(iter);  // 获取映射长度
	loff_t pos = iter->pos;                    // 当前位置
	blk_opf_t bio_opf = REQ_SYNC | REQ_IDLE;   // bio操作标志
	bool need_zeroout = false;                // 是否需要零填充
	u64 copied = 0;                           // 已复制字节数
	size_t orig_count;                        // 原始计数
	unsigned int alignment;                  // 对齐要求
	ssize_t ret = 0;                          // 返回值

	/*
	 * 对于总是分配新块并就地外写的文件系统，每个bio需要与块对齐，
	 * 因为这是分配的单位。
	 */
	if (dio->flags & IOMAP_DIO_FSBLOCK_ALIGNED)
		alignment = fs_block_size;        // 使用文件系统块大小作为对齐
	else
		alignment = bdev_logical_block_size(iomap->bdev);  // 使用设备逻辑块大小

	// 检查位置和长度是否符合对齐要求
	if ((pos | length) & (alignment - 1))
		return -EINVAL;

	// 处理写入操作
	if (dio->flags & IOMAP_DIO_WRITE) {
		bool need_completion_work = true;  // 是否需要完成工作

		switch (iomap->type) {
		case IOMAP_MAPPED:
			/*
			 * 直接映射的I/O本质上不需要在I/O完成时做工作。
			 * 但在下面的某些情况下，这将被重新设置。
			 */
			need_completion_work = false;
			break;
		case IOMAP_UNWRITTEN:
			dio->flags |= IOMAP_DIO_UNWRITTEN;  // 标记为未写入区域
			need_zeroout = true;               // 需要零填充
			break;
		default:
			break;
		}

		// 处理原子bio操作
		if (iomap->flags & IOMAP_F_ATOMIC_BIO) {
			/*
			 * 确保映射覆盖完整的写入长度，
			 * 否则它不会作为单个bio提交，
			 * 这是使用硬件原子操作所必需的。
			 */
			if (length != iter->len)
				return -EINVAL;
			bio_opf |= REQ_ATOMIC;  // 添加原子操作标志
		}

		// 处理共享区域
		if (iomap->flags & IOMAP_F_SHARED) {
			/*
			 * 共享区域的取消共享需要在I/O完成时更新元数据。
			 */
			need_completion_work = true;
			dio->flags |= IOMAP_DIO_COW;  // 标记为写时复制
		}

		// 处理新分配的区域
		if (iomap->flags & IOMAP_F_NEW) {
			/*
			 * 新分配的块可能需要在I/O完成时记录在元数据中。
			 */
			need_completion_work = true;
			need_zeroout = true;  // 需要零填充
		}

		/*
		 * 如果我们需要数据同步语义并且这是一个纯覆盖写入，
		 * 不需要任何元数据更新，则使用FUA写入。
		 * 
		 * 这允许我们在I/O完成时避免缓存刷新。
		 */
		if (dio->flags & IOMAP_DIO_WRITE_THROUGH) {
			if (!need_completion_work &&
			    !(iomap->flags & IOMAP_F_DIRTY) &&
			    (!bdev_write_cache(iomap->bdev) ||
			     bdev_fua(iomap->bdev)))
				bio_opf |= REQ_FUA;  // 添加FUA标志
			else
				dio->flags &= ~IOMAP_DIO_WRITE_THROUGH;
		}

		/*
		 * 我们只能对不需要在完成时附加I/O的纯覆盖写入进行内联完成。
		 * 
		 * 这排除了需要零填充或将未写入或共享区域转换为元数据更新的写入。
		 * 
		 * 扩展i_size的写入也不支持，但这在__iomap_dio_rw()中处理。
		 */
		if (need_completion_work)
			dio->flags |= IOMAP_DIO_COMP_WORK;  // 标记需要完成工作

		bio_opf |= REQ_OP_WRITE;  // 设置写操作
	} else {
		bio_opf |= REQ_OP_READ;   // 设置读操作
	}

	/*
	 * 保存原始计数并将迭代修剪为我们当前正在操作的extent。
	 * 一旦完成，迭代将被重新扩展。
	 */
	orig_count = iov_iter_count(dio->submit.iter);
	iov_iter_truncate(dio->submit.iter, length);

	if (!iov_iter_count(dio->submit.iter))
		goto out;

	/*
	 * 轮询I/O完成的规则遵循我们为内联和延迟完成设置的指导原则。
	 * 如果此IO没有这些选项中的任何一个，则清除轮询标志。
	 */
	if (dio->flags & IOMAP_DIO_COMP_WORK)
		dio->iocb->ki_flags &= ~IOCB_HIPRI;

	// 处理零填充
	if (need_zeroout) {
		/* 从块开始到写入偏移量进行零填充 */
		pad = pos & (fs_block_size - 1);

		ret = iomap_dio_zero(iter, dio, pos - pad, pad);
		if (ret)
			goto out;
	}

	// 主循环，处理bio迭代
	do {
		/*
		 * 如果完成已经发生并报告了错误，现在就放弃，
		 * 不再提交更多的bio。
		 */
		if (unlikely(data_race(dio->error)))
			goto out;

		// 处理单个bio迭代
		ret = iomap_dio_bio_iter_one(iter, dio, pos, alignment, bio_opf);
		if (unlikely(ret < 0)) {
			/*
			 * 我们在IO中途停止。我们必须在这里继续进行子块尾部零填充，
			 * 否则这个短的IO可能会暴露我们尚未写入数据的块的尾部中的陈旧数据。
			 */
			break;
		}
		dio->size += ret;    // 更新已处理大小
		copied += ret;       // 更新已复制字节数
		pos += ret;         // 更新位置
		ret = 0;            // 重置返回值
	} while (iov_iter_count(dio->submit.iter));

	/*
	 * 如果extent类型需要零填充，或者写入超出EOF，我们需要对子块写入的尾部进行零填充。
	 * 如果在后一种情况下我们不零填充块尾部，我们可能会通过mmap读取EOF块暴露陈旧数据。
	 */
	if (need_zeroout ||
	    ((dio->flags & IOMAP_DIO_WRITE) && pos >= i_size_read(inode))) {
		/* 从写入结束到块结束进行零填充 */
		pad = pos & (fs_block_size - 1);
		if (pad)
			ret = iomap_dio_zero(iter, dio, pos,
					     fs_block_size - pad);
	}
out:
	/* 撤销对当前extent的迭代限制 */
	iov_iter_reexpand(dio->submit.iter, orig_count - copied);
	if (copied)
		return iomap_iter_advance(iter, copied);
	return ret;
}


/**
 * iomap_dio_hole_iter - 处理直接IO操作中的空洞区域
 * @iter: 指向iomap迭代器的指针，包含IO映射迭代相关信息
 * @dio: 指向直接IO结构的指针，包含直接IO操作的相关信息
 * 
 * 该函数用于处理直接IO操作中的空洞区域，通过将空洞区域填充为零数据。
 * 它使用iov_iter_zero函数将指定长度的数据填充为零，并更新直接IO操作的大小。
 * 
 * 返回值:
 *     成功时返回迭代器前进的长度
 *     如果遇到错误(如无法填充零数据)，返回-EFAULT
 */
static int iomap_dio_hole_iter(struct iomap_iter *iter, struct iomap_dio *dio)
{
	// 使用iov_iter_zero函数将迭代器指定长度的数据填充为零
	// iomap_length(iter)获取当前迭代区域的长度
	loff_t length = iov_iter_zero(iomap_length(iter), dio->submit.iter);

	// 更新直接IO操作的总大小
	dio->size += length;
	// 如果填充的长度为零，表示出现错误，返回-EFAULT
	if (!length)
		return -EFAULT;
	// 迭代器前进指定的长度，继续处理后续数据
	return iomap_iter_advance(iter, length);
}


static int iomap_dio_inline_iter(struct iomap_iter *iomi, struct iomap_dio *dio)
{
	const struct iomap *iomap = &iomi->iomap;
	struct iov_iter *iter = dio->submit.iter;
	void *inline_data = iomap_inline_data(iomap, iomi->pos);
	loff_t length = iomap_length(iomi);
	loff_t pos = iomi->pos;
	u64 copied;

	if (WARN_ON_ONCE(!inline_data))
		return -EIO;

	if (dio->flags & IOMAP_DIO_WRITE) {
		loff_t size = iomi->inode->i_size;

		if (pos > size)
			memset(iomap_inline_data(iomap, size), 0, pos - size);
		copied = copy_from_iter(inline_data, length, iter);
		if (copied) {
			if (pos + copied > size)
				i_size_write(iomi->inode, pos + copied);
			mark_inode_dirty(iomi->inode);
		}
	} else {
		copied = copy_to_iter(inline_data, length, iter);
	}
	dio->size += copied;
	if (!copied)
		return -EFAULT;
	return iomap_iter_advance(iomi, copied);
}

/**
 * iomap_dio_iter - 处理直接I/O（DIO）的一次迭代
 * @iter: 指向iomap迭代器的指针，包含当前I/O映射的状态和信息
 * @dio: 指向iomap直接I/O对象的指针，包含DIO操作的具体上下文和标志
 *
 * 该函数根据当前迭代器中的映射类型（iomap.type），分发并执行相应的
 * 直接I/O处理逻辑。针对不同的映射类型（如空洞、已映射、未写入等），
 * 调用对应的迭代处理函数。如果在写操作时遇到空洞，或者遇到延迟分配
 * （通常是由于与缓冲写冲突），则会返回I/O错误。
 *
 * 返回值：成功时返回处理的字节数，失败时返回负的错误码（如-EIO）
 */
static int iomap_dio_iter(struct iomap_iter *iter, struct iomap_dio *dio)
{
	/* 根据当前的I/O映射类型进行分支处理 */
	switch (iter->iomap.type) {
	case IOMAP_HOLE:
		/* 如果是写操作遇到了空洞，这是一个异常情况，发出警告并返回错误 */
		if (WARN_ON_ONCE(dio->flags & IOMAP_DIO_WRITE))
			return -EIO;
		/* 读操作遇到空洞，交由空洞迭代函数处理 */
		return iomap_dio_hole_iter(iter, dio);
	case IOMAP_UNWRITTEN:
		/* 如果是读操作遇到未写入的块，将其视为空洞处理 */
		if (!(dio->flags & IOMAP_DIO_WRITE))
			return iomap_dio_hole_iter(iter, dio);
		/* 写操作遇到未写入的块，交由bio迭代函数处理 */
		return iomap_dio_bio_iter(iter, dio);
	case IOMAP_MAPPED:
		/* 已映射的块，直接交由bio迭代函数处理 */
		return iomap_dio_bio_iter(iter, dio);
	case IOMAP_INLINE:
		/* 内联数据，交由内联迭代函数处理 */
		return iomap_dio_inline_iter(iter, dio);
	case IOMAP_DELALLOC:
		/*
		 * DIO 根本没有与 mmap() 访问进行串行化，因此
		 * 如果 page_mkwrite 发生在回写和 DIO 路径中的
		 * iomap_iter() 调用之间，那么它将会看到
		 * page-mkwrite 分配的 DELALLOC 块。
		 */
		/* 遇到延迟分配块，说明与缓冲写发生了冲突，输出限速警告日志 */
		pr_warn_ratelimited("Direct I/O collision with buffered writes! File: %pD4 Comm: %.20s\n",
				    dio->iocb->ki_filp, current->comm);
		/* 返回I/O错误 */
		return -EIO;
	default:
		/* 遇到未知的映射类型，触发一次性警告 */
		WARN_ON_ONCE(1);
		/* 返回I/O错误 */
		return -EIO;
	}
}


/*
 * iomap_dio_rw() always completes O_[D]SYNC writes regardless of whether the IO
 * is being issued as AIO or not.  This allows us to optimise pure data writes
 * to use REQ_FUA rather than requiring generic_write_sync() to issue a
 * REQ_FLUSH post write. This is slightly tricky because a single request here
 * can be mapped into multiple disjoint IOs and only a subset of the IOs issued
 * may be pure data writes. In that case, we still need to do a full data sync
 * completion.
 *
 * When page faults are disabled and @dio_flags includes IOMAP_DIO_PARTIAL,
 * __iomap_dio_rw can return a partial result if it encounters a non-resident
 * page in @iter after preparing a transfer.  In that case, the non-resident
 * pages can be faulted in and the request resumed with @done_before set to the
 * number of bytes previously transferred.  The request will then complete with
 * the correct total number of bytes transferred; this is essential for
 * completing partial requests asynchronously.
 *
 * Returns -ENOTBLK In case of a page invalidation invalidation failure for
 * writes.  The callers needs to fall back to buffered I/O in this case.
 */
/**
 * __iomap_dio_rw - 执行直接I/O读写操作的核心函数
 * @iocb: 内核异步I/O控制块，包含了文件、偏移量等I/O上下文信息
 * @iter: 用户空间数据的I/O向量迭代器
 * @ops: iomap操作函数集，用于文件系统的映射回调
 * @dops: 直接I/O特定操作函数集
 * @dio_flags: 直接I/O的标志位，控制特定行为（如强制等待、覆盖写等）
 * @private: 传递给文件系统的私有数据指针
 * @done_before: 在此次I/O操作之前已经完成的字节数
 *
 * 此函数负责初始化直接I/O请求，根据读写方向设置相应的标志，
 * 处理页缓存无效化，提交I/O请求，并根据同步/异步模式决定
 * 是等待完成还是直接返回排队状态。
 *
 * Return: 成功时返回指向iomap_dio结构体的指针；如果I/O被异步排队，
 * 返回ERR_PTR(-EIOCBQUEUED)；失败时返回相应的ERR_PTR错误码；
 * 如果请求长度为0，返回NULL。
 */
struct iomap_dio *
__iomap_dio_rw(struct kiocb *iocb, struct iov_iter *iter,
		const struct iomap_ops *ops, const struct iomap_dio_ops *dops,
		unsigned int dio_flags, void *private, size_t done_before)
{
	/* 获取当前文件对应的inode */
	struct inode *inode = file_inode(iocb->ki_filp);
	
	/* 初始化iomap迭代器，用于遍历和映射文件区域 */
	struct iomap_iter iomi = {
		.inode		= inode,               /* 关联的inode */
		.pos		= iocb->ki_pos,        /* 当前I/O的起始位置 */
		.len		= iov_iter_count(iter),/* 待处理的数据长度 */
		.flags		= IOMAP_DIRECT,        /* 标记为直接I/O映射 */
		.private	= private,             /* 文件系统私有数据 */
	};
	
	/* 判断是否需要等待I/O完成：同步请求或设置了强制等待标志 */
	bool wait_for_completion =
		is_sync_kiocb(iocb) || (dio_flags & IOMAP_DIO_FORCE_WAIT);
	struct blk_plug plug;       /* 块设备插头，用于合并I/O请求 */
	struct iomap_dio *dio;      /* 直接I/O描述符 */
	loff_t ret = 0;             /* 返回值/错误码 */

	/* 跟踪点：记录直接I/O读写的开始 */
	trace_iomap_dio_rw_begin(iocb, iter, dio_flags, done_before);

	/* 如果请求的数据长度为0，直接返回NULL */
	if (!iomi.len)
		return NULL;

	/* 为直接I/O描述符分配内存 */
	dio = kmalloc_obj(*dio);
	if (!dio)
		return ERR_PTR(-ENOMEM); /* 内存分配失败，返回内存不足错误 */

	/* 初始化直接I/O描述符的各个字段 */
	dio->iocb = iocb;                           /* 关联的异步I/O控制块 */
	atomic_set(&dio->ref, 1);                   /* 引用计数初始化为1 */
	dio->size = 0;                              /* 已处理的数据大小初始化为0 */
	dio->i_size = i_size_read(inode);           /* 读取当前文件的大小 */
	dio->dops = dops;                           /* 关联的直接I/O操作集 */
	dio->error = 0;                             /* 错误码初始化为0 */
	dio->flags = dio_flags & (IOMAP_DIO_FSBLOCK_ALIGNED | IOMAP_DIO_BOUNCE); /* 提取有效的标志位 */
	dio->done_before = done_before;             /* 记录之前已完成的大小 */

	/* 初始化提交相关的子结构体 */
	dio->submit.iter = iter;                    /* 关联用户数据迭代器 */
	dio->submit.waiter = current;               /* 记录当前进程为等待者 */

	/* 如果I/O控制块标记为不等待（NOWAIT），则在iomap迭代器中也设置此标志 */
	if (iocb->ki_flags & IOCB_NOWAIT)
		iomi.flags |= IOMAP_NOWAIT;

	/* 根据读写方向进行不同的处理 */
	if (iov_iter_rw(iter) == READ) {
		/* 读操作处理逻辑 */
		
		/* 如果读取的起始位置已经超过或等于文件大小，无需读取，直接退出 */
		if (iomi.pos >= dio->i_size)
			goto out_free_dio;

		/* 如果迭代器由用户空间内存支持，设置用户支持标志 */
		if (user_backed_iter(iter))
			dio->flags |= IOMAP_DIO_USER_BACKED;

		/* 等待相关页面的写回完成，并准备读取 */
		ret = kiocb_write_and_wait(iocb, iomi.len);
		if (ret)
			goto out_free_dio;
	} else {
		/* 写操作处理逻辑 */
		
		iomi.flags |= IOMAP_WRITE;            /* 标记为写映射 */
		dio->flags |= IOMAP_DIO_WRITE;        /* 标记为直接I/O写 */

		/* 如果指定了仅覆盖写模式 */
		if (dio_flags & IOMAP_DIO_OVERWRITE_ONLY) {
			ret = -EAGAIN; /* 默认设置为重试错误 */
			
			/* 如果写入范围超出文件当前大小，不能覆盖写，退出 */
			if (iomi.pos >= dio->i_size ||
			    iomi.pos + iomi.len > dio->i_size)
				goto out_free_dio;
				
			iomi.flags |= IOMAP_OVERWRITE_ONLY; /* 设置仅覆盖写标志 */
		}

		/* 如果请求了原子写入，设置原子标志 */
		if (iocb->ki_flags & IOCB_ATOMIC)
			iomi.flags |= IOMAP_ATOMIC;

		/* 对于数据同步或完全同步请求，需要同步完成处理 */
		if (iocb_is_dsync(iocb)) {
			dio->flags |= IOMAP_DIO_NEED_SYNC;

		       /**
			* 对于仅数据同步的写入，我们乐观地尝试为此I/O使用
			* WRITE_THROUGH（直写）模式。此标志要求要么通过设备
			* 写缓存进行FUA写入，要么向没有易失性写缓存的设备
			* 进行常规写入。对于前者，任何非FUA写入的发生都将
			* 清除此标志，因此我们在完成之前就知道是否需要刷新
			* 缓存。
			*/
			if (!(iocb->ki_flags & IOCB_SYNC))
				dio->flags |= IOMAP_DIO_WRITE_THROUGH;
		}

		/**
		 * i_size（文件大小）的更新必须在进程上下文中进行。
		 * 如果写入范围超出当前文件大小，则标记需要延迟完成工作。
		 */
		if (iomi.pos + iomi.len > dio->i_size)
			dio->flags |= IOMAP_DIO_COMP_WORK;

		/**
		 * 尝试使我们正在写入范围的缓存页无效化。
		 * 如果无效化失败，让调用者回退到缓冲I/O。
		 */
		ret = kiocb_invalidate_pages(iocb, iomi.len);
		if (ret) {
			if (ret != -EAGAIN) {
				/* 跟踪页缓存无效化失败事件 */
				trace_iomap_dio_invalidate_fail(inode, iomi.pos,
								iomi.len);
				if (iocb->ki_flags & IOCB_ATOMIC) {
					/**
					 * 页无效化失败，这可能是暂时的，
					 * 解锁并查看调用者是否重试。
					 */
					ret = -EAGAIN;
				} else {
					/* 回退到缓冲写 */
					ret = -ENOTBLK;
				}
			}
			goto out_free_dio;
		}
	}

	/* 如果不需要等待完成，且超级块的直接I/O完成工作队列尚未初始化，则初始化它 */
	if (!wait_for_completion && !inode->i_sb->s_dio_done_wq) {
		ret = sb_init_dio_done_wq(inode->i_sb);
		if (ret < 0)
			goto out_free_dio;
	}

	/* 标记inode正在进行的直接I/O操作开始（阻止某些文件系统操作） */
	inode_dio_begin(inode);

	/* 开启块设备插头，以便合并后续的I/O请求 */
	blk_start_plug(&plug);
	
	/* 循环遍历并映射文件区域，提交I/O请求 */
	while ((ret = iomap_iter(&iomi, ops)) > 0) {
		/* 对每个映射区域执行直接I/O迭代处理 */
		iomi.status = iomap_dio_iter(&iomi, dio);

		/**
		 * 我们只能对单个bio的I/O进行轮询。
		 * 如果有多个bio，则清除高优先级（轮询）标志。
		 */
		iocb->ki_flags &= ~IOCB_HIPRI;
	}

	/* 拔出插头，提交已合并的I/O请求给块设备驱动 */
	blk_finish_plug(&plug);

	/**
	 * 我们只报告已读取到i_size（文件大小）位置的数据。
	 * 将迭代器恢复到与该状态对应的位置，因为某些调用者
	 * （如splice代码）依赖于这一点。
	 */
	if (iov_iter_rw(iter) == READ && iomi.pos >= dio->i_size)
		iov_iter_revert(iter, iomi.pos - dio->i_size);

	/* 如果遇到错误，但已经完成了部分I/O，且允许部分完成 */
	if (ret == -EFAULT && dio->size && (dio_flags & IOMAP_DIO_PARTIAL)) {
		/* 如果不是NOWAIT请求，则必须等待已完成部分的结束 */
		if (!(iocb->ki_flags & IOCB_NOWAIT))
			wait_for_completion = true;
		ret = 0; /* 将错误码清零，表示部分成功 */
	}

	/* 遇到回退到缓冲I/O的魔术错误码 */
	if (ret == -ENOTBLK) {
		wait_for_completion = true; /* 需要等待以安全回退 */
		ret = 0;
	}
	
	/* 如果发生其他严重错误，记录到dio结构体中 */
	if (ret < 0)
		iomap_dio_set_error(dio, ret);

	/**
	 * 如果我们发出的所有写入都已经直写到介质，则不需要在
	 * I/O完成时刷新缓存。在这种情况下，清除同步标志。
	 *
	 * 否则，如果需要任何同步工作，则清除内联完成标志，
	 * 因为同步工作需要在进程上下文中执行。
	 */
	if (dio->flags & IOMAP_DIO_WRITE_THROUGH)
		dio->flags &= ~IOMAP_DIO_NEED_SYNC;
	else if (dio->flags & IOMAP_DIO_NEED_SYNC)
		dio->flags |= IOMAP_DIO_COMP_WORK;

	/**
	 * 我们即将放弃额外的提交引用，这可能是对dio的最后
	 * 一个引用。这里有三种不同的进展方式：
	 *
	 * (a) 如果这是最后一个引用，我们将始终自己完成并释放dio。
	 * (b) 如果这不是最后一个引用，并且我们服务于异步iocb，
	 *     我们在递减后绝不能触及dio，I/O完成处理程序将完成
	 *     并释放它。
	 * (c) 如果这不是最后一个引用，但我们服务于同步iocb，
	 *     I/O完成处理程序将在最终引用丢弃时唤醒我们，我们
	 *     将在I/O完成处理程序唤醒我们之后，在这里完成并释放它。
	 */
	dio->wait_for_completion = wait_for_completion;
	
	/* 原子地递减引用计数并测试是否为0（即没有其他引用） */
	if (!atomic_dec_and_test(&dio->ref)) {
		/* 还有其他引用存在（I/O仍在进行中） */
		if (!wait_for_completion) {
			/* 异步模式：不等待完成，记录跟踪点并返回排队状态 */
			trace_iomap_dio_rw_queued(inode, iomi.pos, iomi.len);
			return ERR_PTR(-EIOCBQUEUED);
		}

		/* 同步模式：循环等待I/O完成 */
		for (;;) {
			set_current_state(TASK_UNINTERRUPTIBLE); /* 设置为不可中断睡眠状态 */
			if (!READ_ONCE(dio->submit.waiter))      /* 检查等待者是否已被清除（完成标志） */
				break;

			blk_io_schedule(); /* 调度其他进程运行，等待I/O完成唤醒 */
		}
		__set_current_state(TASK_RUNNING); /* 恢复为运行状态 */
	}

	/* 成功完成，返回dio结构体指针 */
	return dio;

out_free_dio:
	/* 退出清理：释放dio占用的内存 */
	kfree(dio);
	/* 如果有错误码，返回对应的错误指针 */
	if (ret)
		return ERR_PTR(ret);
	/* 没有错误码（如读取位置超出文件大小），返回NULL */
	return NULL;
}

EXPORT_SYMBOL_GPL(__iomap_dio_rw);

/**
 * iomap_dio_rw - 执行直接I/O读写操作
 * @iocb: 指向异步I/O控制块的指针，包含文件偏移量等I/O上下文信息
 * @iter: 指向I/O向量迭代器的指针，描述用户空间的数据缓冲区
 * @ops: 指向iomap操作回调函数集的指针，用于文件系统的映射请求
 * @dops: 指向直接I/O特定操作回调函数集的指针
 * @dio_flags: 直接I/O的标志位，用于控制特定的I/O行为
 * @private: 传递给文件系统的私有数据指针
 * @done_before: 在此次I/O操作之前已经完成的字节数，用于计算总进度
 *
 * 该函数是Linux内核中iomap框架下直接I/O(Direct I/O)读写的入口封装。
 * 它主要负责调用底层执行函数发起I/O操作，并根据执行结果决定是返回错误码
 * 还是等待并完成整个I/O操作。
 *
 * Return: 成功完成时返回读写的字节数，失败时返回负的错误码
 */
ssize_t
iomap_dio_rw(struct kiocb *iocb, struct iov_iter *iter,
		const struct iomap_ops *ops, const struct iomap_dio_ops *dops,
		unsigned int dio_flags, void *private, size_t done_before)
{
	struct iomap_dio *dio;

	/* 调用底层的直接I/O读写核心逻辑函数，返回iomap_dio结构体指针 */
	dio = __iomap_dio_rw(iocb, iter, ops, dops, dio_flags, private,
			     done_before);
			     
	// 检查返回值是否为无效指针或空指针
	if (IS_ERR_OR_NULL(dio))
		return PTR_ERR_OR_ZERO(dio); // 若为错误指针则返回负的错误码，若为NULL则返回0
		
	// 若直接I/O操作已正常提交或完成，则执行完成操作并返回实际读写的字节数
	return iomap_dio_complete(dio);
}

EXPORT_SYMBOL_GPL(iomap_dio_rw);
