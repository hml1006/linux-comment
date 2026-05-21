// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 Red Hat, Inc.
 * Copyright (c) 2016-2025 Christoph Hellwig.
 */
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
		bio_set_polled(bio, iocb);
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

ssize_t iomap_dio_complete(struct iomap_dio *dio)
{
	const struct iomap_dio_ops *dops = dio->dops;
	struct kiocb *iocb = dio->iocb;
	loff_t offset = iocb->ki_pos;
	ssize_t ret = dio->error;

	if (dops && dops->end_io)
		ret = dops->end_io(iocb, dio->size, ret, dio->flags);
	if (should_report_dio_fserror(dio))
		fserror_report_io(file_inode(iocb->ki_filp),
				  iomap_dio_err_type(dio), offset, dio->size,
				  dio->error, GFP_NOFS);

	if (likely(!ret)) {
		ret = dio->size;
		/* check for short read */
		if (offset + ret > dio->i_size &&
		    !(dio->flags & IOMAP_DIO_WRITE))
			ret = dio->i_size - offset;
	}

	/*
	 * Try again to invalidate clean pages which might have been cached by
	 * non-direct readahead, or faulted in by get_user_pages() if the source
	 * of the write was an mmap'ed region of the file we're writing.  Either
	 * one is a pretty crazy thing to do, so we don't support it 100%.  If
	 * this invalidation fails, tough, the write still worked...
	 *
	 * And this page cache invalidation has to be after ->end_io(), as some
	 * filesystems convert unwritten extents to real allocations in
	 * ->end_io() when necessary, otherwise a racing buffer read would cache
	 * zeros from unwritten extents.
	 */
	if (!dio->error && dio->size && (dio->flags & IOMAP_DIO_WRITE) &&
	    !(dio->flags & IOMAP_DIO_NO_INVALIDATE))
		kiocb_invalidate_post_direct_write(iocb, dio->size);

	inode_dio_end(file_inode(iocb->ki_filp));

	if (ret > 0) {
		iocb->ki_pos += ret;

		/*
		 * If this is a DSYNC write, make sure we push it to stable
		 * storage now that we've written data.
		 */
		if (dio->flags & IOMAP_DIO_NEED_SYNC)
			ret = generic_write_sync(iocb, ret);
		if (ret > 0)
			ret += dio->done_before;
	}
	trace_iomap_dio_complete(iocb, dio->error, ret);
	kfree(dio);
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

static void __iomap_dio_bio_end_io(struct bio *bio, bool inline_completion)
{
	struct iomap_dio *dio = bio->bi_private;

	if (dio->flags & IOMAP_DIO_BOUNCE) {
		bio_iov_iter_unbounce(bio, !!dio->error,
				dio->flags & IOMAP_DIO_USER_BACKED);
		bio_put(bio);
	} else if (dio->flags & IOMAP_DIO_USER_BACKED) {
		bio_check_pages_dirty(bio);
	} else {
		bio_release_pages(bio, false);
		bio_put(bio);
	}

	/* Do not touch bio below, we just gave up our reference. */

	if (atomic_dec_and_test(&dio->ref)) {
		/*
		 * Avoid another context switch for the completion when already
		 * called from the ioend completion workqueue.
		 */
		if (inline_completion)
			dio->flags &= ~IOMAP_DIO_COMP_WORK;
		iomap_dio_done(dio);
	}
}

void iomap_dio_bio_end_io(struct bio *bio)
{
	struct iomap_dio *dio = bio->bi_private;

	if (bio->bi_status)
		iomap_dio_set_error(dio, blk_status_to_errno(bio->bi_status));
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
	fscrypt_set_bio_crypt_ctx(bio, inode, pos >> inode->i_blkbits,
				  GFP_KERNEL);
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

static ssize_t iomap_dio_bio_iter_one(struct iomap_iter *iter,
		struct iomap_dio *dio, loff_t pos, unsigned int alignment,
		blk_opf_t op)
{
	unsigned int nr_vecs;
	struct bio *bio;
	ssize_t ret;

	if (dio->flags & IOMAP_DIO_BOUNCE)
		nr_vecs = bio_iov_bounce_nr_vecs(dio->submit.iter, op);
	else
		nr_vecs = bio_iov_vecs_to_alloc(dio->submit.iter, BIO_MAX_VECS);

	bio = iomap_dio_alloc_bio(iter, dio, nr_vecs, op);
	fscrypt_set_bio_crypt_ctx(bio, iter->inode,
			pos >> iter->inode->i_blkbits, GFP_KERNEL);
	bio->bi_iter.bi_sector = iomap_sector(&iter->iomap, pos);
	bio->bi_write_hint = iter->inode->i_write_hint;
	bio->bi_ioprio = dio->iocb->ki_ioprio;
	bio->bi_private = dio;
	bio->bi_end_io = iomap_dio_bio_end_io;

	if (dio->flags & IOMAP_DIO_BOUNCE)
		ret = bio_iov_iter_bounce(bio, dio->submit.iter);
	else
		ret = bio_iov_iter_get_pages(bio, dio->submit.iter,
					     alignment - 1);
	if (unlikely(ret))
		goto out_put_bio;
	ret = bio->bi_iter.bi_size;

	/*
	 * An atomic write bio must cover the complete length.  If it doesn't,
	 * error out.
	 */
	if ((op & REQ_ATOMIC) && WARN_ON_ONCE(ret != iomap_length(iter))) {
		ret = -EINVAL;
		goto out_put_bio;
	}

	if (dio->flags & IOMAP_DIO_WRITE)
		task_io_account_write(ret);
	else if ((dio->flags & IOMAP_DIO_USER_BACKED) &&
		 !(dio->flags & IOMAP_DIO_BOUNCE))
		bio_set_pages_dirty(bio);

	/*
	 * We can only poll for single bio I/Os.
	 */
	if (iov_iter_count(dio->submit.iter))
		dio->iocb->ki_flags &= ~IOCB_HIPRI;
	iomap_dio_submit_bio(iter, dio, bio, pos);
	return ret;

out_put_bio:
	bio_put(bio);
	return ret;
}

static int iomap_dio_bio_iter(struct iomap_iter *iter, struct iomap_dio *dio)
{
	const struct iomap *iomap = &iter->iomap;
	struct inode *inode = iter->inode;
	unsigned int fs_block_size = i_blocksize(inode), pad;
	const loff_t length = iomap_length(iter);
	loff_t pos = iter->pos;
	blk_opf_t bio_opf = REQ_SYNC | REQ_IDLE;
	bool need_zeroout = false;
	u64 copied = 0;
	size_t orig_count;
	unsigned int alignment;
	ssize_t ret = 0;

	/*
	 * File systems that write out of place and always allocate new blocks
	 * need each bio to be block aligned as that's the unit of allocation.
	 */
	if (dio->flags & IOMAP_DIO_FSBLOCK_ALIGNED)
		alignment = fs_block_size;
	else
		alignment = bdev_logical_block_size(iomap->bdev);

	if ((pos | length) & (alignment - 1))
		return -EINVAL;

	if (dio->flags & IOMAP_DIO_WRITE) {
		bool need_completion_work = true;

		switch (iomap->type) {
		case IOMAP_MAPPED:
			/*
			 * Directly mapped I/O does not inherently need to do
			 * work at I/O completion time.  But there are various
			 * cases below where this will get set again.
			 */
			need_completion_work = false;
			break;
		case IOMAP_UNWRITTEN:
			dio->flags |= IOMAP_DIO_UNWRITTEN;
			need_zeroout = true;
			break;
		default:
			break;
		}

		if (iomap->flags & IOMAP_F_ATOMIC_BIO) {
			/*
			 * Ensure that the mapping covers the full write
			 * length, otherwise it won't be submitted as a single
			 * bio, which is required to use hardware atomics.
			 */
			if (length != iter->len)
				return -EINVAL;
			bio_opf |= REQ_ATOMIC;
		}

		if (iomap->flags & IOMAP_F_SHARED) {
			/*
			 * Unsharing of needs to update metadata at I/O
			 * completion time.
			 */
			need_completion_work = true;
			dio->flags |= IOMAP_DIO_COW;
		}

		if (iomap->flags & IOMAP_F_NEW) {
			/*
			 * Newly allocated blocks might need recording in
			 * metadata at I/O completion time.
			 */
			need_completion_work = true;
			need_zeroout = true;
		}

		/*
		 * Use a FUA write if we need datasync semantics and this is a
		 * pure overwrite that doesn't require any metadata updates.
		 *
		 * This allows us to avoid cache flushes on I/O completion.
		 */
		if (dio->flags & IOMAP_DIO_WRITE_THROUGH) {
			if (!need_completion_work &&
			    !(iomap->flags & IOMAP_F_DIRTY) &&
			    (!bdev_write_cache(iomap->bdev) ||
			     bdev_fua(iomap->bdev)))
				bio_opf |= REQ_FUA;
			else
				dio->flags &= ~IOMAP_DIO_WRITE_THROUGH;
		}

		/*
		 * We can only do inline completion for pure overwrites that
		 * don't require additional I/O at completion time.
		 *
		 * This rules out writes that need zeroing or metdata updates to
		 * convert unwritten or shared extents.
		 *
		 * Writes that extend i_size are also not supported, but this is
		 * handled in __iomap_dio_rw().
		 */
		if (need_completion_work)
			dio->flags |= IOMAP_DIO_COMP_WORK;

		bio_opf |= REQ_OP_WRITE;
	} else {
		bio_opf |= REQ_OP_READ;
	}

	/*
	 * Save the original count and trim the iter to just the extent we
	 * are operating on right now.  The iter will be re-expanded once
	 * we are done.
	 */
	orig_count = iov_iter_count(dio->submit.iter);
	iov_iter_truncate(dio->submit.iter, length);

	if (!iov_iter_count(dio->submit.iter))
		goto out;

	/*
	 * The rules for polled IO completions follow the guidelines as the
	 * ones we set for inline and deferred completions. If none of those
	 * are available for this IO, clear the polled flag.
	 */
	if (dio->flags & IOMAP_DIO_COMP_WORK)
		dio->iocb->ki_flags &= ~IOCB_HIPRI;

	if (need_zeroout) {
		/* zero out from the start of the block to the write offset */
		pad = pos & (fs_block_size - 1);

		ret = iomap_dio_zero(iter, dio, pos - pad, pad);
		if (ret)
			goto out;
	}

	do {
		/*
		 * If completions already occurred and reported errors, give up now and
		 * don't bother submitting more bios.
		 */
		if (unlikely(data_race(dio->error)))
			goto out;

		ret = iomap_dio_bio_iter_one(iter, dio, pos, alignment, bio_opf);
		if (unlikely(ret < 0)) {
			/*
			 * We have to stop part way through an IO. We must fall
			 * through to the sub-block tail zeroing here, otherwise
			 * this short IO may expose stale data in the tail of
			 * the block we haven't written data to.
			 */
			break;
		}
		dio->size += ret;
		copied += ret;
		pos += ret;
		ret = 0;
	} while (iov_iter_count(dio->submit.iter));

	/*
	 * We need to zeroout the tail of a sub-block write if the extent type
	 * requires zeroing or the write extends beyond EOF. If we don't zero
	 * the block tail in the latter case, we can expose stale data via mmap
	 * reads of the EOF block.
	 */
	if (need_zeroout ||
	    ((dio->flags & IOMAP_DIO_WRITE) && pos >= i_size_read(inode))) {
		/* zero out from the end of the write to the end of the block */
		pad = pos & (fs_block_size - 1);
		if (pad)
			ret = iomap_dio_zero(iter, dio, pos,
					     fs_block_size - pad);
	}
out:
	/* Undo iter limitation to current extent */
	iov_iter_reexpand(dio->submit.iter, orig_count - copied);
	if (copied)
		return iomap_iter_advance(iter, copied);
	return ret;
}

static int iomap_dio_hole_iter(struct iomap_iter *iter, struct iomap_dio *dio)
{
	loff_t length = iov_iter_zero(iomap_length(iter), dio->submit.iter);

	dio->size += length;
	if (!length)
		return -EFAULT;
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

	if (WARN_ON_ONCE(!iomap_inline_data_valid(iomap)))
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
