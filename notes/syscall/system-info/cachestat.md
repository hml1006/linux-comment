# cachestat 系统调用分析

## 1. 概述

`cachestat` 查询文件页缓存（page cache）的统计信息，包括缓存页数量、脏页数量、回写中页数量、已回收页数量等。用于监控文件缓存效率和内存压力。

**原型：**

```c
SYSCALL_DEFINE4(cachestat, unsigned int, fd,
                struct cachestat_range __user *, cstat_range,
                struct cachestat __user *, cstat, unsigned int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `fd` | `unsigned int` | 要查询的文件描述符 |
| `cstat_range` | `struct cachestat_range __user *` | 查询范围（偏移和长度） |
| `cstat` | `struct cachestat __user *` | 输出结构体，接收页缓存统计结果 |
| `flags` | `unsigned int` | 保留标志位，当前必须为 0 |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- 文件缓存分析工具
- 内存回收策略优化
- 数据库等应用监控缓存命中率
- 内核调优和性能分析

## 3. 函数调用栈

```
SYSCALL_DEFINE4(cachestat, fd, cstat_range, cstat, flags)  // mm/filemap.c
  ├─ CLASS(fd, f)(fd)                                        // 通过 fd 获取 struct fd
  ├─ fd_empty(f) → 返回 -EBADF
  ├─ copy_from_user(&csr, cstat_range, sizeof(csr))          // 拷贝用户态范围参数
  │    拷贝失败 → 返回 -EFAULT
  ├─ is_file_hugepages(fd_file(f)) → 返回 -EOPNOTSUPP        // 不支持 hugetlbfs
  ├─ can_do_cachestat(fd_file(f)) → 返回 -EPERM              // 检查文件是否可缓存
  ├─ flags != 0 → 返回 -EINVAL
  ├─ first_index = csr.off >> PAGE_SHIFT                     // 计算起始页索引
  ├─ last_index = csr.len == 0 ? ULONG_MAX : ...             // 计算结束页索引
  ├─ memset(&cs, 0, sizeof(cs))
  ├─ mapping = fd_file(f)->f_mapping                         // 获取地址空间映射
  ├─ filemap_cachestat(mapping, first_index, last_index, &cs) // 核心统计函数
  └─ copy_to_user(cstat, &cs, sizeof(cs))                    // 拷贝结果到用户空间
      拷贝失败 → 返回 -EFAULT
```

## 4. 关键数据结构

### 4.1 struct cachestat_range（查询范围）

```c
// include/uapi/linux/mman.h
struct cachestat_range {
    __u64 off;   // 起始偏移量（字节）
    __u64 len;   // 长度（字节），0 表示查询到文件末尾
};
```

### 4.2 struct cachestat（页缓存统计结果）

```c
// include/uapi/linux/mman.h
struct cachestat {
    __u64 nr_cache;             // 在页缓存中的页数
    __u64 nr_dirty;             // 脏页数（需要写回磁盘）
    __u64 nr_writeback;         // 正在回写中的页数
    __u64 nr_evicted;           // 已被回收的页数
    __u64 nr_recently_evicted;  // 最近被回收的页数
};
```

### 4.3 filemap_cachestat 核心实现

```c
// mm/filemap.c
void filemap_cachestat(struct address_space *mapping,
                       pgoff_t first_index, pgoff_t last_index,
                       struct cachestat *cs)
{
    // 遍历指定页范围内的 page cache
    // 使用 xarray（radix tree）查找页
    // 对每一页：
    //   - 如果存在且未回收 → nr_cache++
    //   - 如果存在且脏 → nr_dirty++
    //   - 如果正在回写 → nr_writeback++
    //   - 如果被回收（shadow entry）→ nr_evicted++
    //   - 如果最近被回收 → nr_recently_evicted++
}
```

## 5. 流程图

```
用户态调用 cachestat(fd, range, cstat, flags)
    │
    ▼
┌─────────────────────────────────────┐
│  检查 fd 有效性                     │
│  fd_empty → 返回 -EBADF             │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_from_user(range) 拷贝范围参数 │
│  失败 → 返回 -EFAULT                │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  检查文件类型                       │
│  hugetlbfs → 返回 -EOPNOTSUPP       │
│  不可缓存 → 返回 -EPERM             │
│  flags != 0 → 返回 -EINVAL          │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  计算页索引范围                     │
│  first = off >> PAGE_SHIFT         │
│  last = (off + len - 1) >> SHIFT   │
│  (len=0 时 last = ULONG_MAX)       │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  filemap_cachestat(mapping, ...)   │
│  ├─ 遍历 xarray                     │
│  ├─ 统计各页状态                    │
│  └─ 填充 cachestat 结果             │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_to_user(cstat) 返回结果       │
│  失败 → 返回 -EFAULT                │
│  成功 → 返回 0                      │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EBADF` | 文件描述符无效 | 传入的 `fd` 未打开 |
| `-EFAULT` | 地址错误 | `cstat_range` 或 `cstat` 指针无效 |
| `-EOPNOTSUPP` | 不支持的操作 | 文件是 hugetlbfs 文件系统上的文件 |
| `-EPERM` | 权限不足 | 文件不支持缓存统计（如某些特殊文件系统） |
| `-EINVAL` | 无效参数 | `flags` 非零 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <linux/mman.h>

int main(void)
{
    int fd = open("/tmp/testfile", O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // 写入一些数据以创建页缓存
    char buf[4096] = {0};
    write(fd, buf, sizeof(buf));

    struct cachestat_range range = { .off = 0, .len = 4096 };
    struct cachestat cs;

    if (syscall(SYS_cachestat, fd, &range, &cs, 0) == 0) {
        printf("nr_cache:             %lu\n", cs.nr_cache);
        printf("nr_dirty:             %lu\n", cs.nr_dirty);
        printf("nr_writeback:         %lu\n", cs.nr_writeback);
        printf("nr_evicted:           %lu\n", cs.nr_evicted);
        printf("nr_recently_evicted:  %lu\n", cs.nr_recently_evicted);
    } else {
        perror("cachestat");
    }

    close(fd);
    unlink("/tmp/testfile");
    return 0;
}
```

## 8. 参考

- 源码: `mm/filemap.c`（`SYSCALL_DEFINE4(cachestat)` 和 `filemap_cachestat()`）
- 头文件: `include/uapi/linux/mman.h`
- 测试用例: `tools/testing/selftests/cachestat/test_cachestat.c`
- 配置选项: `CONFIG_CACHESTAT_SYSCALL`