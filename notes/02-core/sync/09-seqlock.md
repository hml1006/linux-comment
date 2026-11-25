# 9. 顺序锁 (seqlock)

## 9.1 概述

Seqlock (顺序锁) 是一种读写锁优化，读者无锁（仅检查序列号），写者互斥。适用于写者优先级高、读操作可以容忍重试的场景。

**核心思想：**
- 读者轻量：仅读取序列号，不会阻塞写者
- 写者优先：写者永远不会被读者阻塞
- 读者重试：如果发现写操作正在进行，读者重试

## 9.2 关键数据结构

### 9.2.1 seqcount_t

定义在 [include/linux/seqlock.h](file:///home/louis/code/linux/include/linux/seqlock.h)：

```c
// include/linux/seqlock.h
struct seqcount {
    unsigned sequence;           // 序列号计数器
    /*
     * 序列号含义:
     *   偶数 = 无写者, 可以安全读取
     *   奇数 = 有写者正在写入, 读者需重试
     *   每次写者开始操作: sequence++
     *   每次写者结束操作: sequence++ (恢复为偶数)
     */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
    struct lockdep_map dep_map;
#endif
};
```

### 9.2.2 seqlock_t

```c
// include/linux/seqlock.h
typedef struct {
    struct seqcount seqcount;    // 序列号
    spinlock_t lock;             // 写者互斥锁
} seqlock_t;
```

## 9.3 核心 API

### 9.3.1 读者端

```c
// 1. 基本读者 API
unsigned read_seqcount_begin(const seqcount_t *s);
// 返回序列号, 如果为奇数则重试

int read_seqcount_retry(const seqcount_t *s, unsigned start);
// 检查是否重试: 如果 start != 当前序列号, 返回 1

// 2. 读者使用模式
do {
    seq = read_seqcount_begin(&seq);   // 读取序列号开始
    // ... 读取共享数据 ...
} while (read_seqcount_retry(&seq, seq));  // 检查是否需重试

// 3. 带 irq 保护的读者
unsigned read_seqcount_begin_irqsave(const seqcount_t *s, unsigned long flags);
int read_seqcount_retry_irqrestore(const seqcount_t *s, unsigned start, unsigned long flags);
```

### 9.3.2 写者端

```c
// 1. 基本写者 API
void write_seqcount_begin(seqcount_t *s);
// sequence++; (变为奇数, 标记写操作开始)

void write_seqcount_end(seqcount_t *s);
// sequence++; (恢复为偶数, 标记写操作结束)

// 2. 使用模式 (无锁)
write_seqcount_begin(&seq);
// ... 修改共享数据 ...
write_seqcount_end(&seq);

// 3. seqlock_t 版本 (带 spinlock 保护)
void write_seqlock(seqlock_t *sl);
// spin_lock(&sl->lock);
// write_seqcount_begin(&sl->seqcount);

void write_sequnlock(seqlock_t *sl);
// write_seqcount_end(&sl->seqcount);
// spin_unlock(&sl->lock);

// 4. 带 irq 保护的变体
void write_seqlock_irq(seqlock_t *sl);
void write_seqlock_irqsave(seqlock_t *sl, unsigned long flags);
void write_seqlock_bh(seqlock_t *sl);
```

## 9.4 工作原理

### 9.4.1 读者/写者交互

```
时间线:
写者:     ──wwww──────wwww──────wwww──→
序列号:   0    1    2    2    3    4    4
          ↑    ↑         ↑         ↑
          even odd       even      even

读者 A:   ──RRRRRR──────RRRR──→
          ↑               ↑
          seq=0(偶数)     seq=2(偶数)
          数据一致        数据一致

读者 B:   ──────RRRRRRRR──────→
                ↑       ↑
                seq=1   seq=1 != 2
                (奇数)   重试!
```

### 9.4.2 序列号变化

```
写者操作:
  write_seqcount_begin():
    sequence++ (0 → 1, 奇数, 标记写操作)
    smp_mb();       // 内存屏障: 确保之前的序列号对其他 CPU 可见
    // ... 修改数据 ...
    smp_mb();       // 内存屏障: 确保数据修改对其他 CPU 可见
  write_seqcount_end():
    sequence++ (1 → 2, 偶数, 标记写操作结束)

读者操作:
  do {
    seq = READ_ONCE(sequence);  // 读取序列号
    if (seq & 1) {              // 奇数 → 正在写入
      cpu_relax();              // 等待
      continue;
    }
    smp_rmb();                  // 读内存屏障
    // ... 读取数据 ...
    smp_rmb();                  // 读内存屏障
  } while (READ_ONCE(sequence) != seq);  // 序列号变化 → 重试
```

## 9.5 调用栈

### 9.5.1 读者调用链

```
do {
    seq = read_seqcount_begin(&seqcount);
    │
    └─ READ_ONCE(seqcount->sequence)
       │
       ├─ 奇数 → 写者正在写入, cpu_relax() 后重试
       │
       └─ 偶数 → 返回序列号
                │
                └─ smp_rmb()  → 读内存屏障
                            │
                            └─ 读取共享数据
                            │
                            └─ smp_rmb()  → 读内存屏障
} while (read_seqcount_retry(&seqcount, seq));
    │
    └─ READ_ONCE(seqcount->sequence) != seq
       │
       ├─ 相等 → 数据一致, 读取成功
       │
       └─ 不等 → 数据被修改, 重试
```

### 9.5.2 写者调用链

```
write_seqlock(&seqlock):
  │
  └─ spin_lock(&seqlock->lock)       → 写者互斥
        │
        └─ write_seqcount_begin(&seqlock->seqcount)
              │
              └─ seqcount->sequence++  → 奇数, 标记写入
                    │
                    └─ smp_wmb()       → 写内存屏障

... 修改共享数据 ...

write_sequnlock(&seqlock):
  │
  └─ write_seqcount_end(&seqlock->seqcount)
        │
        └─ smp_wmb()                   → 写内存屏障
              │
              └─ seqcount->sequence++  → 偶数, 写入完成
        │
        └─ spin_unlock(&seqlock->lock) → 释放写者锁
```

## 9.6 使用场景

| 场景 | 说明 |
|------|------|
| 时间/时钟 (jiffies, ktime) | 读者频繁, 写者偶尔 (NTP 调整) |
| 系统统计 (statistics) | CPU 统计, 网络统计 |
| 用户/组映射 | 偶尔修改, 频繁读取 |
| 虚拟内存区域计数 | mmap 操作统计 |

## 9.7 使用示例

```c
// 示例: 保护系统时间戳
struct timestamp {
    u64 seconds;
    u64 nanoseconds;
    seqcount_t seq;
};

// 读者
u64 read_timestamp(struct timestamp *ts)
{
    u64 sec, nsec;
    unsigned seq;

    do {
        seq = read_seqcount_begin(&ts->seq);
        sec = ts->seconds;
        nsec = ts->nanoseconds;
    } while (read_seqcount_retry(&ts->seq, seq));

    return sec * NSEC_PER_SEC + nsec;
}

// 写者
void update_timestamp(struct timestamp *ts, u64 new_sec, u64 new_nsec)
{
    write_seqcount_begin(&ts->seq);
    ts->seconds = new_sec;
    ts->nanoseconds = new_nsec;
    write_seqcount_end(&ts->seq);
}
```

## 9.8 使用注意事项

```c
// 1. 读者可能多次重试
// 写者频繁时, 读者可能多次重试, 影响性能
// 不适合写者频繁的场景

// 2. 读者不能阻塞写者
// 但写者之间需要互斥 (通过 spinlock)

// 3. 不适合保护大量数据
// 读者重试开销随数据量增加

// 4. 避免在读者中执行昂贵操作
// 重试可能导致多次执行
```

## 9.9 关键文件

| 文件 | 说明 |
|------|------|
| [include/linux/seqlock.h](file:///home/louis/code/linux/include/linux/seqlock.h) | seqlock API 及数据结构 |