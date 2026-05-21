// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 Red Hat, Inc.
 * Copyright (c) 2016-2021 Christoph Hellwig.
 */
#include <linux/iomap.h>
#include "trace.h"

static inline void iomap_iter_reset_iomap(struct iomap_iter *iter)
{
	if (iter->iomap.flags & IOMAP_F_FOLIO_BATCH) {
		folio_batch_release(iter->fbatch);
		folio_batch_reinit(iter->fbatch);
		iter->iomap.flags &= ~IOMAP_F_FOLIO_BATCH;
	}

	iter->status = 0;
	memset(&iter->iomap, 0, sizeof(iter->iomap));
	memset(&iter->srcmap, 0, sizeof(iter->srcmap));
}

/* Advance the current iterator position and decrement the remaining length */
int iomap_iter_advance(struct iomap_iter *iter, u64 count)
{
	if (WARN_ON_ONCE(count > iomap_length(iter)))
		return -EIO;
	iter->pos += count;
	iter->len -= count;
	return 0;
}

static inline void iomap_iter_done(struct iomap_iter *iter)
{
	WARN_ON_ONCE(iter->iomap.offset > iter->pos);
	WARN_ON_ONCE(iter->iomap.length == 0);
	WARN_ON_ONCE(iter->iomap.offset + iter->iomap.length <= iter->pos);
	WARN_ON_ONCE(iter->iomap.flags & IOMAP_F_STALE);

	iter->iter_start_pos = iter->pos;

	trace_iomap_iter_dstmap(iter->inode, &iter->iomap);
	if (iter->srcmap.type != IOMAP_HOLE)
		trace_iomap_iter_srcmap(iter->inode, &iter->srcmap);
}

/**
 * iomap_iter - iterate over a ranges in a file
 * @iter: iteration structue
 * @ops: iomap ops provided by the file system
 *
 * Iterate over filesystem-provided space mappings for the provided file range.
 *
 * This function handles cleanup of resources acquired for iteration when the
 * filesystem indicates there are no more space mappings, which means that this
 * function must be called in a loop that continues as long it returns a
 * positive value.  If 0 or a negative value is returned, the caller must not
 * return to the loop body.  Within a loop body, there are two ways to break out
 * of the loop body:  leave @iter.status unchanged, or set it to a negative
 * errno.
 */
/**
 * iomap_iter - 执行单次 I/O 映射迭代
 * @iter: 指向 iomap_iter 结构的指针，包含当前迭代的状态和信息
 * @ops: 指向 iomap_ops 结构的指针，包含特定文件系统的映射操作回调函数
 *
 * 该函数负责处理 I/O 映射的一次迭代过程。如果是连续迭代的后续调用，
 * 它会先调用 iomap_end 结束上一次的映射处理，并根据当前状态决定是否
 * 继续进行新的映射。如果需要继续，则调用 iomap_begin 获取新的映射。
 *
 * 返回值：
 *   1  - 映射成功获取，调用者应继续处理该映射
 *   0  - 迭代正常结束（无更多数据或未取得进展）
 *   <0 - 错误码，表示迭代过程中发生错误
 */
int iomap_iter(struct iomap_iter *iter, const struct iomap_ops *ops)
{
	/* 检查上一次映射是否被标记为过期（STALE），需要重新处理 */
	bool stale = iter->iomap.flags & IOMAP_F_STALE;
	/* 记录本次迭代相较于起始位置前进的字节数 */
	ssize_t advanced;
	/* 记录原始请求的总长度，用于传给 iomap_end 回调 */
	u64 olen;
	/* 存储回调函数的返回值或最终状态 */
	int ret;

	/* 跟踪点，用于内核调试和性能分析，记录迭代信息 */
	trace_iomap_iter(iter, ops, _RET_IP_);

	/* 
	 * 如果映射长度为0，说明这是第一次调用或者上一次已经处理完毕，
	 * 直接跳转到 begin 标签处开始获取新的映射 
	 */
	if (!iter->iomap.length)
		goto begin;

	/*
	 * 计算当前迭代向前推进了多少字节，以及原始请求的总字节数，
	 * 这些信息将传递给 ->iomap_end() 回调函数。
	 */
	advanced = iter->pos - iter->iter_start_pos;
	olen = iter->len + advanced;

	/* 如果文件系统提供了 iomap_end 回调，则调用它进行映射结束的清理或收尾工作 */
	if (ops->iomap_end) {
		ret = ops->iomap_end(iter->inode, iter->iter_start_pos,
				iomap_length_trim(iter, iter->iter_start_pos,
						  olen),
				advanced, iter->flags, &iter->iomap);
		/* 
		 * 如果 iomap_end 返回错误，且迭代没有任何进展（未前进一字节），
		 * 则立即将错误返回给调用者
		 */
		if (ret < 0 && !advanced)
			return ret;
	}

	/* 检测并警告旧的返回值语义：如果 status 大于 0，说明使用了过时的编码规范 */
	if (WARN_ON_ONCE(iter->status > 0))
		iter->status = -EIO;

	/*
	 * 使用 iter->len 来决定是否继续进入下一个映射。
	 * 在以下情况下显式终止迭代：
	 * 1. 遇到错误状态 (iter->status < 0)
	 * 2. 当前迭代没有剩余数据 (iter->len == 0)
	 * 3. 当前迭代完全没有进展且映射未标记为过期 (!advanced && !stale)
	 * 除非映射被标记为过期（stale），需要重新处理。
	 */
	if (iter->status < 0)
		ret = iter->status;       /* 发生错误，返回错误状态 */
	else if (iter->len == 0 || (!advanced && !stale))
		ret = 0;                  /* 无剩余数据或无进展，正常结束 */
	else
		ret = 1;                  /* 条件满足，准备继续下一次映射 */

	/* 重置迭代器中的 iomap 状态，为获取新映射做准备 */
	iomap_iter_reset_iomap(iter);

	/* 如果不需要继续迭代（ret <= 0），直接返回结果 */
	if (ret <= 0)
		return ret;

begin:
	/* 调用文件系统提供的 iomap_begin 回调，获取从当前位置开始的映射 ext4_iomap_begin */
	ret = ops->iomap_begin(iter->inode, iter->pos, iter->len, iter->flags,
			       &iter->iomap, &iter->srcmap);
	/* 如果获取映射失败，直接返回错误码 */
	if (ret < 0)
		return ret;
	
	/* 映射成功获取，更新迭代器的内部状态（如已处理的长度等） */
	iomap_iter_done(iter);
	
	/* 返回 1 表示成功获取映射，调用者应继续处理 */
	return 1;
}

