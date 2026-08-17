# arc_usr_cmpxchg

## 原理与功能

`arc_usr_cmpxchg` 是 ARC（Argonaut RISC Core）架构专用的系统调用，用于用户态比较并交换（CAS）操作。此系统调用专为缺乏 `LLOCK`/`SCOND`（Load-Linked/Store-Conditional）指令的旧版 ARC 处理器设计。

在 ARM64 架构上，此系统调用存在于编号表中，但仅为 ARC 架构提供支持，ARM64 上为 stub 实现。

### 功能说明

- 实现用户态原子比较并交换（CAS）操作
- 对于缺乏 LLOCK/SCOND 指令的旧 ARC 核心提供原子操作能力
- ARC 架构编号为 248（`__NR_arc_usr_cmpxchg`）
- 定义为 `__NR_arc_usr_cmpxchg` = `__NR_arch_specific_syscall + 4`

## 函数原型

```c
SYSCALL_DEFINE3(arc_usr_cmpxchg, int __user *, uaddr, int, expected, int, new);
```

| 参数 | 类型 | 描述 |
|--|--|--|
| `uaddr` | `int __user *` | 用户空间地址，指向要操作的内存单元 |
| `expected` | `int` | 期望的旧值 |
| `new` | `int` | 要设置的新值 |

### 返回值

- 成功时返回 `uaddr` 处的旧值（无论是否交换成功）
- 失败时返回负的错误码

### STATUS_Z 标志

ARC 架构通过 `STATUS_Z`（零标志位）向用户态指示操作是否成功：
- 如果 `*uaddr == expected`，则交换成功，设置 `STATUS_Z`
- 如果 `*uaddr != expected`，则交换失败，清除 `STATUS_Z`

## 完整实现

```c
// arch/arc/kernel/process.c
SYSCALL_DEFINE3(arc_usr_cmpxchg, int __user *, uaddr, int, expected, int, new)
{
    struct pt_regs *regs = current_pt_regs();
    u32 uval;
    int ret;

    /*
     * This is only for old cores lacking LLOCK/SCOND, which by definition
     * can't possibly be SMP. Thus doesn't need to be SMP safe.
     */
    WARN_ON_ONCE(IS_ENABLED(CONFIG_SMP));

    /* Z indicates to userspace if operation succeeded */
    regs->status32 &= ~STATUS_Z_MASK;

    ret = access_ok(uaddr, sizeof(*uaddr));
    if (!ret)
         goto fail;

again:
    preempt_disable();

    ret = __get_user(uval, uaddr);
    if (ret)
         goto fault;

    if (uval != expected)
         goto out;

    ret = __put_user(new, uaddr);
    if (ret)
         goto fault;

    regs->status32 |= STATUS_Z_MASK;

out:
    preempt_enable();
    return uval;

fault:
    preempt_enable();

    if (unlikely(ret != -EFAULT))
         goto fail;

    mmap_read_lock(current->mm);
    ret = fixup_user_fault(current->mm, (unsigned long) uaddr,
                           FAULT_FLAG_WRITE, NULL);
    mmap_read_unlock(current->mm);

    if (!ret)
         goto again;

fail:
    return ret;
}
```

## 调用链分析

```
arc_usr_cmpxchg(uaddr, expected, new)
  │
  ├─ 1. 检查：WARN_ON_ONCE(CONFIG_SMP)  // 此系统调用仅用于 UP 系统
  ├─ 2. 清除 STATUS_Z 标志位
  ├─ 3. 检查 access_ok(uaddr)
  ├─ 4. 关闭抢占 (preempt_disable)
  ├─ 5. __get_user(uval, uaddr)       // 读取用户空间值
  ├─ 6. 比较 uval 与 expected
  │    ├─ 不相等 → 跳转到 out（不执行交换）
  │    └─ 相等 → __put_user(new, uaddr)  // 写入新值
  ├─ 7. 设置 STATUS_Z 标志（如果交换成功）
  ├─ 8. 开启抢占 (preempt_enable)
  └─ 9. 返回 uval
```

### 页面错误处理

如果 `__get_user` 或 `__put_user` 返回 `-EFAULT`，内核会尝试修复页面错误：

1. 调用 `fixup_user_fault()` 处理缺页异常
2. 如果修复成功，重新执行 CAS 操作（`goto again`）
3. 这允许在发生页面错误时自动处理，而不是直接返回错误

## 关键数据结构

```c
// arch/arc/include/asm/ptrace.h
struct pt_regs {
    // ...
    long status32;        // ARC 状态寄存器，包含 STATUS_Z 标志位
    // ...
};

#define STATUS_Z_MASK     (1 << 1)   // 零标志位
```

## 流程图

```
开始
  │
  ▼
检查是否 SMP (WARN_ON)
  │
  ▼
清除 STATUS_Z 标志
  │
  ▼
access_ok 检查 ──失败──→ 返回 -EFAULT
  │
  ▼ (成功)
preempt_disable()
  │
  ▼
__get_user(uval, uaddr) ──失败──→ 页面错误处理
  │                                    │
  ▼ (成功)                             ▼
uval == expected? ──否──→ out      fixup_user_fault()
  │                                  │
 是 (是)                             ▼
  │                              修复成功? ──是──→ 重试 (again)
  ▼                               │
__put_user(new, uaddr) ──失败──→ 否
  │                               │
  ▼ (成功)                        ▼
设置 STATUS_Z 标志              返回 -EFAULT
  │
  ▼
out: preempt_enable()
  │
  ▼
返回 uval (旧值)
```

## 使用场景

- 用户态无锁数据结构（如无锁队列、链表）
- 互斥锁（mutex）和自旋锁的用户态实现
- 原子引用计数
- 用于缺乏 LLOCK/SCOND 指令的旧 ARC 处理器

## 注意事项

- ARM64 上此系统调用仅为 ARC 架构兼容性保留
- ARM64 用户态通过 `LDXR`/`STXR` 指令实现原子操作，无需系统调用
- 此系统调用只能在非 SMP（单处理器）系统上使用，SMP 系统上会触发 WARN_ON
- 通过关闭抢占来保证原子性（在单处理器上，关闭抢占即可防止并发访问）

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/arc/kernel/process.c](/home/louis/code/linux/arch/arc/kernel/process.c) | arc_usr_cmpxchg 实现 |
| [arch/arc/include/asm/syscalls.h](/home/louis/code/linux/arch/arc/include/asm/syscalls.h) | 系统调用声明 |
| [tools/arch/arc/include/uapi/asm/unistd.h](/home/louis/code/linux/tools/arch/arc/include/uapi/asm/unistd.h) | 系统调用编号定义 |