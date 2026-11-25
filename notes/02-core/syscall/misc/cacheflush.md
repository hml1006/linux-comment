# cacheflush

## 原理与功能

`cacheflush` 是一个缓存刷写系统调用，主要用于 ARC、CSKY、NIOS2、SH、PARISC 等架构。这些架构的缓存一致性通常不由硬件维护，需要在特定场景下由软件显式刷写缓存。

在 ARM64 架构上，`cacheflush` 存在但实现为 stub（`__do_cacheflush` 实际执行缓存刷新操作），编号为 244。ARM64 上缓存一致性由硬件维护，但为了兼容 JIT 编译器等动态代码生成场景，仍提供了有限的缓存刷新能力。

### 功能说明

- 刷新指定地址范围的缓存
- 确保数据缓存（D-cache）写回主存
- 确保指令缓存（I-cache）无效化（用于 JIT 编译器）
- 确保数据修改对指令执行可见

## 函数原型

```c
#include <sys/syscall.h>

// 通用原型
int cacheflush(unsigned long addr, unsigned long scope, unsigned long flags);

// 不同架构略有差异
// CSKY: int cacheflush(void __user *addr, unsigned long bytes, int cache);
// PARISC: int cacheflush(unsigned long addr, unsigned long bytes, unsigned int cache);
// MIPS: int cacheflush(unsigned long addr, unsigned long bytes, unsigned int cache);
// SH: asmlinkage int sys_cacheflush(unsigned long addr, unsigned long len, int op);
```

### 参数说明

| 参数 | 类型 | 描述 |
|--|--|--|
| `addr` | `unsigned long` | 要刷新的起始地址 |
| `scope`/`bytes`/`len` | `unsigned long` | 要刷新的范围（长度） |
| `flags`/`cache`/`op` | `unsigned int` | 缓存操作类型 |

### 常见标志位

```c
// PARISC 架构 (arch/parisc/include/uapi/asm/cachectl.h)
#define ICACHE  (1<<0)  // 刷新指令缓存
#define DCACHE  (1<<1)  // 写回并刷新数据缓存
#define BCACHE  (ICACHE|DCACHE)  // 刷新两个缓存

// SH 架构 (arch/sh/include/asm/cachectl.h)
#define CACHEFLUSH_D_INVAL   0x01  // 数据缓存无效化
#define CACHEFLUSH_D_WB      0x02  // 数据缓存写回
#define CACHEFLUSH_D_PURGE   0x03  // 数据缓存清除
#define CACHEFLUSH_I         0x04  // 指令缓存无效化
```

## 各架构实现

### ARM64 实现

```c
// arch/arm64/kernel/syscall.c
SYSCALL_DEFINE3(cacheflush, unsigned long, addr, unsigned long, scope,
                unsigned long, flags)
{
    unsigned long end = addr + scope;
    
    // 仅支持刷新用户空间地址范围
    if (!access_ok((void __user *)addr, scope))
        return -EFAULT;
    
    return __do_cacheflush(addr, end, flags);
}

static int __do_cacheflush(unsigned long start, unsigned long end, int flags)
{
    int ret;
    
    // 检查 flags 是否合法
    if (flags & ~(ICACHE | DCACHE))
        return -EINVAL;
    
    // 写回数据缓存
    if (flags & DCACHE) {
        ret = flush_cache_user_range(start, end);
        if (ret)
            return ret;
    }
    
    // 无效化指令缓存
    if (flags & ICACHE) {
        ret = flush_icache_user_range(start, end);
        if (ret)
            return ret;
    }
    
    return 0;
}
```

### CSKY 实现

```c
// arch/csky/mm/syscache.c
SYSCALL_DEFINE3(cacheflush, void __user *, addr, unsigned long, bytes, int, cache)
{
    switch (cache) {
    case BCACHE:
    case DCACHE:
        dcache_wb_range((unsigned long)addr, (unsigned long)addr + bytes);
        if (cache != BCACHE)
            break;
        fallthrough;
    case ICACHE:
        flush_icache_mm_range(current->mm,
                (unsigned long)addr, (unsigned long)addr + bytes);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}
```

### PARISC 实现

```c
// arch/parisc/kernel/cache.c
SYSCALL_DEFINE3(cacheflush, unsigned long, addr, unsigned long, bytes,
        unsigned int, cache)
{
    unsigned long start, end;
    int error = 0;

    if (bytes == 0)
        return 0;
    if (!access_ok((void __user *)addr, bytes))
        return -EFAULT;

    end = addr + bytes;

    // 数据缓存：使用 fdc 指令逐行刷新
    if (cache & DCACHE) {
        start = addr;
        // 使用 fdc 指令循环刷新
        while (start < end) {
            fdc(start);
            start += dcache_stride;
        }
        sync();
    }

    // 指令缓存：使用 fic 指令逐行刷新
    if (cache & ICACHE && error == 0) {
        start = addr;
        // 使用 fic 指令循环刷新
        while (start < end) {
            fic(start);
            start += icache_stride;
        }
        sync();
    }

    return error;
}
```

### SH 实现

```c
// arch/sh/kernel/sys_sh.c
asmlinkage int sys_cacheflush(unsigned long addr, unsigned long len, int op)
{
    struct vm_area_struct *vma;

    if ((op <= 0) || (op > (CACHEFLUSH_D_PURGE|CACHEFLUSH_I)))
        return -EINVAL;

    if (addr + len < addr)
        return -EFAULT;

    // 验证地址范围属于当前进程
    mmap_read_lock(current->mm);
    vma = find_vma(current->mm, addr);
    if (vma == NULL || addr < vma->vm_start || addr + len > vma->vm_end) {
        mmap_read_unlock(current->mm);
        return -EFAULT;
    }

    // 执行数据缓存操作
    switch (op & CACHEFLUSH_D_PURGE) {
    case CACHEFLUSH_D_INVAL:
        __flush_invalidate_region((void *)addr, len);
        break;
    case CACHEFLUSH_D_WB:
        __flush_wback_region((void *)addr, len);
        break;
    case CACHEFLUSH_D_PURGE:
        __flush_purge_region((void *)addr, len);
        break;
    }

    // 如果需要，刷新指令缓存
    if (op & CACHEFLUSH_I)
        flush_icache_range(addr, addr + len);

    mmap_read_unlock(current->mm);
    return 0;
}
```

## 调用链分析（ARM64）

```
cacheflush(addr, scope, flags)
  │
  └─ __arm64_sys_cacheflush(addr, scope, flags)      // arch/arm64/kernel/syscall.c
       │
       ├─ access_ok() 验证用户空间地址
       │
       └─ __do_cacheflush(start, end, flags)
            │
            ├─ 检查 flags 合法性
            │
            ├─ 如果 flags & DCACHE:
            │    └─ flush_cache_user_range(start, end)
            │         └─ __flush_cache_user_range(start, end)
            │              ├─ dcache_clean_pou(start, end)  // 数据缓存写回
            │              │    └─ dc cvac / dc civac       // 缓存操作指令
            │              └─ dsb ish                       // 数据同步屏障
            │
            └─ 如果 flags & ICACHE:
                 └─ flush_icache_user_range(start, end)
                      └─ __flush_icache_user_range(start, end)
                           ├─ icache_inval_pou(start, end)  // 指令缓存无效
                           │    └─ ic ivau                  // 缓存操作指令
                           └─ dsb ish                       // 数据同步屏障
                                └─ isb                       // 指令同步屏障
```

## 关键数据结构

```c
// ARM64 缓存刷新函数声明
// arch/arm64/include/asm/cacheflush.h
void flush_cache_user_range(unsigned long start, unsigned long end);
void __flush_cache_user_range(unsigned long start, unsigned long end);
void flush_icache_user_range(unsigned long start, unsigned long end);

// 相关缓存操作指令语义
// dc cvac:   Data Cache Clean by Virtual Address to Point of Unification
// dc civac:  Data Cache Clean and Invalidate by Virtual Address to PoU
// ic ivau:   Instruction Cache Invalidate by Virtual Address to PoU
// dsb ish:   Data Synchronization Barrier (Inner Shareable)
// isb:       Instruction Synchronization Barrier
```

## 流程图（ARM64）

```
用户态调用 cacheflush(addr, len, flags)
  │
  ▼
access_ok 检查 ──失败──→ 返回 -EFAULT
  │
 成功
  │
  ▼
__do_cacheflush(start, end, flags)
  │
  ├─ flags 检查 ──非法──→ 返回 -EINVAL
  │
  ├─ flags & DCACHE?
  │    │
  │    ├─ 是 → flush_cache_user_range(start, end)
  │    │         │
  │    │         └─ __flush_cache_user_range(start, end)
  │    │              ├─ 循环: dc cvac (数据缓存写回)
  │    │              └─ dsb ish (同步屏障)
  │    │
  │    └─ 否 → 跳过
  │
  ├─ flags & ICACHE?
  │    │
  │    ├─ 是 → flush_icache_user_range(start, end)
  │    │         │
  │    │         └─ __flush_icache_user_range(start, end)
  │    │              ├─ 循环: ic ivau (指令缓存无效)
  │    │              ├─ dsb ish (同步屏障)
  │    │              └─ isb (指令同步屏障)
  │    │
  │    └─ 否 → 跳过
  │
  └─ 返回 0
```

## 使用场景

- **JIT 编译器**：动态生成机器码后，需要刷新 dcache（写回）和 icache（无效化），确保执行新代码
- **动态代码修改**：自修改代码（self-modifying code）场景
- **二进制翻译**：将一种指令集翻译为另一种时
- **调试器**：在断点指令注入后刷新缓存
- **跨架构兼容代码**：需要显式处理缓存一致性的场景

## 使用示例

```c
#include <sys/syscall.h>
#include <unistd.h>

#define ICACHE  (1<<0)
#define DCACHE  (1<<1)

// JIT 编译后刷新缓存
void jit_flush_cache(void *code, size_t size)
{
    unsigned long addr = (unsigned long)code;
    unsigned long len = size;
    
    // 先写回数据缓存，再无效化指令缓存
    syscall(__NR_cacheflush, addr, len, DCACHE | ICACHE);
}

// 简单的代码补丁
void patch_code(void *addr, unsigned int new_insn)
{
    // 写入新指令
    *(unsigned int *)addr = new_insn;
    
    // 刷新缓存确保一致性
    syscall(__NR_cacheflush, addr, sizeof(unsigned int), DCACHE | ICACHE);
}
```

## 注意事项

- ARM64 编号为 244，但与 ARC、CSKY、NIOS2 等架构共享编号
- ARM64 上缓存一致性通常由硬件维护，用户态很少需要显式调用
- 主要用于 JIT 编译器和动态代码修改场景
- 不同架构的接口参数含义和返回值可能不同
- SH 架构要求 `CAP_SYS_ADMIN` 权限才能刷新整个缓存

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/arm64/kernel/syscall.c](/home/louis/code/linux/arch/arm64/kernel/syscall.c) | ARM64 cacheflush 实现 |
| [arch/arm64/include/asm/cacheflush.h](/home/louis/code/linux/arch/arm64/include/asm/cacheflush.h) | ARM64 缓存刷新声明 |
| [arch/csky/mm/syscache.c](/home/louis/code/linux/arch/csky/mm/syscache.c) | CSKY cacheflush 实现 |
| [arch/parisc/kernel/cache.c](/home/louis/code/linux/arch/parisc/kernel/cache.c) | PARISC cacheflush 实现 |
| [arch/sh/kernel/sys_sh.c](/home/louis/code/linux/arch/sh/kernel/sys_sh.c) | SH cacheflush 实现 |
| [arch/parisc/include/uapi/asm/cachectl.h](/home/louis/code/linux/arch/parisc/include/uapi/asm/cachectl.h) | 缓存标志位定义 |