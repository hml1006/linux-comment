# arc_settls

## 原理与功能

`arc_settls` 是 ARC（Argonaut RISC Core）架构专用的系统调用，用于设置线程本地存储（TLS）指针。在 ARM64 架构上，此系统调用存在于编号表中，但仅为 ARC 架构提供支持，ARM64 上为 stub 实现（返回 -ENOSYS）。

### 功能说明

- 设置用户态线程本地存储（TLS）寄存器
- ARC 架构编号为 245（`__NR_arc_settls`）
- 定义为 `__NR_arc_settls` = `__NR_arch_specific_syscall + 1`

## 函数原型

```c
// ARC 架构原生实现
SYSCALL_DEFINE1(arc_settls, void *, user_tls_data_ptr)
{
    task_thread_info(current)->thr_ptr = (unsigned int)user_tls_data_ptr;
    return 0;
}
```

### 写的是"内核镜像"，不是硬件寄存器

源码（`arch/arc/kernel/process.c`）逐字如下：

```c
SYSCALL_DEFINE1(arc_settls, void *, user_tls_data_ptr)
{
	task_thread_info(current)->thr_ptr = (unsigned int)user_tls_data_ptr;
	return 0;
}

SYSCALL_DEFINE0(arc_gettls)
{
	return task_thread_info(current)->thr_ptr;
}
```

三个细节：

1. **强制 `(unsigned int)` 转换**：`thr_ptr` 声明为 `unsigned long`，而 ARC 只有 32 位，转换不丢信息，只是明确"这是一个 32 位用户地址"。
2. **没有 `access_ok()`/`copy_from_user()`**：参数是一个"要被记住的数字"，内核不解引用它，所以不需要校验。即便传 `0xdeadbeef`，也只是把垃圾写进 `thread_info`。
3. **写入的是每任务镜像**：`task_thread_info(current)` 取的是当前任务内核栈上的 `thread_info`，因此写入天然是 per-thread 的，不需要锁；`arc_settls` 与 `arc_gettls` 之间也没有任何并发问题（同一任务同一时刻只会在一个 CPU 上执行）。

`thr_ptr` 的消费者只有四处（可用 `grep -rn thr_ptr arch/arc` 复核）：`arc_settls`、`arc_gettls`、`copy_thread()`（clone/fork 时预置子任务）、`arch_ptrace()` 的 `PTRACE_GET_THREAD_AREA`。系统调用返回路径不会把 `thr_ptr` 重新加载进 r25——真正让这个值变成用户态线程指针的，是 `copy_thread()` 在子任务首次被调度前把它写进 `c_callee->r25`。

## 调用链分析

```
arc_settls(user_tls_data_ptr)
  └─ task_thread_info(current)->thr_ptr = (unsigned int)user_tls_data_ptr
       └─ 返回 0 表示成功
```

## 关键数据结构

```c
// arch/arc/include/asm/thread_info.h
struct thread_info {
    unsigned long thr_ptr;    // 线程本地存储指针
    // ... 其他字段
};
```

数据结构关系：

```
task_struct
  └─ thread_info
       └─ thr_ptr  ───→ 用户态 TLS 数据区域
```

### thr_ptr 的写入与读取者

| 位置 | 代码 | 方向 |
|--|--|--|
| `arc_settls()` | `task_thread_info(current)->thr_ptr = (unsigned int)ptr` | 写 |
| `copy_thread()`，`clone_flags & CLONE_SETTLS` | `task_thread_info(p)->thr_ptr = tls` | 写（新任务） |
| `copy_thread()`，无 `CLONE_SETTLS`（fork 等） | `task_thread_info(p)->thr_ptr = task_thread_info(current)->thr_ptr` | 写（继承） |
| `copy_thread()` 末尾 | `c_callee->r25 = task_thread_info(p)->thr_ptr` | 读 → 预置用户 callee 寄存器 |
| `arc_gettls()` | `return task_thread_info(current)->thr_ptr` | 读 |
| `arch_ptrace()` / `PTRACE_GET_THREAD_AREA` | `put_user(task_thread_info(child)->thr_ptr, ...)` | 读 → 暴露给调试器 |

`clone(CLONE_SETTLS)` 是 `arc_settls` 的"批量替代"：创建线程时直接把 TLS 指针交给内核，由 `copy_thread()` 一次性完成"记录 + 预置 r25"，省掉一次系统调用。`copy_thread()` 的注释特别提醒，这里的 `tls` 来自 **C 库 `clone()` 的第 4 个参数**，与系统调用 `clone` 的参数布局并非一一对应：

```c
	if (unlikely(clone_flags & CLONE_SETTLS)) {
		/*
		 * set task's userland tls data ptr from 4th arg
		 * clone C-lib call is difft from clone sys-call
		 */
		task_thread_info(p)->thr_ptr = tls;
	} else {
		/* Normal fork case: set parent's TLS ptr in child */
		task_thread_info(p)->thr_ptr =
		task_thread_info(current)->thr_ptr;
	}
```

`arc_settls` 则用于"线程已存在、只想更新内核记录值"的场合；具体用户态是否使用它取决于 libc 实现，内核对两种方式一视同仁。

## 流程图

```
用户态调用 arc_settls(ptr)
  │
  ▼
系统调用入口 (trap)
  │
  ▼
SYSCALL_DEFINE1(arc_settls, void *, user_tls_data_ptr)  // arch/arc/kernel/process.c
  │
  ▼
task_thread_info(current)->thr_ptr = (unsigned int)ptr  // 保存 TLS 指针
  │
  ▼
返回 0 (成功)
```

## 设计特点

- 实现极其简洁：仅将用户态传入的 TLS 指针保存到 `thread_info.thr_ptr` 字段
- 不需要任何权限检查，任何用户态进程都可以设置自己的 TLS 指针
- 该指针在进程上下文切换时由内核自动保存和恢复

### 没有校验为什么也安全

| 潜在风险 | 为什么不存在 |
|--|--|
| 用户传内核地址 | 内核既不解引用也不写回用户空间，只把它当作 32 位整数保存 |
| 用户传未映射地址 | 同上；真正使用该地址的是用户态自己的 TLS 访问，缺页由用户态的正常 fault 路径处理 |
| 越权修改别的线程 | `task_thread_info(current)` 只能拿到当前任务；改别人需要 ptrace 权限 |
| 并发竞争 | 同一任务的 `thread_info` 只被自己写；跨 CPU 迁移时 `thread_info` 跟着任务走 |
| 信息泄露 | 写入方向是"用户 → 内核"，且该值可被同进程的 `arc_gettls` 读回，没有新信息 |

唯一"不设防"的后果是：用户可以把 `thr_ptr` 设成任意值，然后通过 `clone()` 让子线程带着这个值启动（前提是自己也用 `CLONE_SETTLS` 或依赖继承）。这等价于用户态自伤，不构成内核安全问题。它也说明为什么 `arc_settls` 不需要 `CAP_SYS_ADMIN` 之类的检查——这类"只影响调用者自身"的系统调用在内核里普遍不做能力检查。

## 使用场景

- 线程创建时初始化 TLS 区域
- 用户态线程库（如 pthreads）实现
- 动态链接器（ld-linux）设置线程私有存储

## 相关系统调用

| 系统调用 | 编号 | 功能 |
|--|--|--|
| `arc_settls` | 245 | 设置 TLS 指针 |
| `arc_gettls` | 246 | 获取 TLS 指针 |
| `arc_usr_cmpxchg` | 248 | 用户态 CAS 操作 |

## 与 ARM64 对比

| 特性 | ARC | ARM64 |
|--|--|--|
| 实现方式 | 系统调用 | `msr tpidr_el0, x0` 指令 |
| 编号 | 245 | stub |
| 存储位置 | `thread_info.thr_ptr` | `tpidr_el0` 系统寄存器 |
| 性能 | 较高（系统调用开销） | 极低（单条指令） |

## 注意事项

- ARM64 上此系统调用仅为 ARC 架构兼容性保留
- ARM64 通过 `msr tpidr_el0, x0` 指令直接设置 TLS 寄存器

### 编号归因与错误码

`arc_settls` 的编号来自 `scripts/syscall.tbl`：

```
# Architectures may provide up to 16 syscalls of their own between 244 and 259
245	arc	arc_settls			sys_arc_settls
246	arc	arc_gettls			sys_arc_gettls
248	arc	arc_usr_cmpxchg			sys_arc_usr_cmpxchg
```

第 2 列 `arc` 是 ABI 标签，`scripts/syscalltbl.sh` 按 `--abis` 过滤生成各架构的 `asm/syscall_table_*.h`。arm64 在 `arch/arm64/kernel/Makefile.syscalls` 中只声明 `syscall_abis_64 += renameat rlimit memfd_secret`（以及 `common`、`64`），因此这三行全部被丢弃：245/246/248 在 arm64 上是未分配编号，落进 `sys_call_table` 的 `__arm64_sys_ni_syscall` 默认项。这正是"ARM64 上为 stub"的准确含义——不是有专门的占位函数，而是表项留空。

| 项目 | 说明 |
|--|--|
| 参数 | 一个 `void *`（用户态 TLS 数据指针），不做校验 |
| 返回值 | 恒为 0 |
| 错误码 | 无（ARC 上）；其它架构 `-ENOSYS` |
| 对标 arm64 | `msr tpidr_el0, x0`，无系统调用 |
| 联动 | [arc_gettls.md](arc_gettls.md)、[arc_usr_cmpxchg.md](arc_usr_cmpxchg.md) |

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/arc/kernel/process.c](/home/louis/code/linux/arch/arc/kernel/process.c) | arc_settls 实现 |
| [arch/arc/include/asm/syscalls.h](/home/louis/code/linux/arch/arc/include/asm/syscalls.h) | 系统调用声明 |
| [tools/arch/arc/include/uapi/asm/unistd.h](/home/louis/code/linux/tools/arch/arc/include/uapi/asm/unistd.h) | 系统调用编号定义 |