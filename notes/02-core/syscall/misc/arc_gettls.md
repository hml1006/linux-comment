# arc_gettls

## 原理与功能

`arc_gettls` 是 ARC（Argonaut RISC Core）架构专用的系统调用，用于获取线程本地存储（TLS）指针。在 ARM64 架构上，此系统调用存在于编号表中，但仅为 ARC 架构提供支持，ARM64 上为 stub 实现（返回 -ENOSYS）。

### 功能说明

- 获取用户态线程本地存储（TLS）寄存器值
- ARC 架构编号为 246（`__NR_arc_gettls`）
- 定义为 `__NR_arc_gettls` = `__NR_arch_specific_syscall + 2`

## 函数原型

```c
// ARC 架构原生实现
SYSCALL_DEFINE0(arc_gettls)
{
    return task_thread_info(current)->thr_ptr;
}
```

### 返回值即指针：源码里的原始注释

`arch/arc/kernel/process.c` 在 `arc_settls` 与 `arc_gettls` 之间留了一段设计说明：

```c
/*
 * We return the user space TLS data ptr as sys-call return code
 * Ideally it should be copy to user.
 * However we can cheat by the fact that some sys-calls do return
 * absurdly high values
 * Since the tls dat aptr is not going to be in range of 0xFFFF_xxxx
 * it won't be considered a sys-call error
 * and it will be loads better than copy-to-user, which is a definite
 * D-TLB Miss
 */
```

这段注释是本系统调用全部"协议"的来源，逐句拆开看：

1. **本应 `copy_to_user()`**：常规做法是用户传一个 `void __user *` 输出参数，内核把 TLS 指针写进去。
2. **但那样必然缺页**：`copy_to_user()` 会访问一个用户页，触发 D-TLB miss（甚至 page fault），对 `__tls_get_addr` 这种高频路径代价太大。
3. **利用"系统调用返回通道"**：直接 `return` 指针值，用户态用 `syscall(SYS_arc_gettls)` 的返回值当指针。
4. **前提是避开错误窗口**：Linux 规定返回值落在 `[-4095, -1]`（即 `0xFFFFF001` ~ `0xFFFFFFFF`）表示 `-errno`。只要 TLS 数据区不在最顶端这 4 KB 内，用户态就不会把指针误判成错误。

严格地说，32 位 ARC 上 `long` 与 `unsigned long` 等宽。而 `arch/arc/include/asm/processor.h` 把用户地址空间限制为 `TASK_SIZE = 0x60000000`（0x0000_0000 ~ 0x5FFF_FFFF），所以 TLS 指针永远小于 `0x80000000`，既不会以负数返回，也不可能落进 `0xFFFFF001` 之后约 4 KB 的错误窗口。注释里"不会落在 0xFFFF_xxxx"是基于这一地址布局的断言，而不是通用保证——换一个用户空间能到 `0xFFFFxxxx` 的架构，这种"借返回通道传指针"的技巧就不成立了。

## 调用链分析

```
arc_gettls()
  └─ task_thread_info(current)->thr_ptr    // 直接从 thread_info 读取 TLS 指针
       └─ 返回 thr_ptr 值作为系统调用返回值
```

### 设计考虑

ARC 架构的设计者注意到，如果通过 `copy_to_user()` 将 TLS 指针写入用户空间缓冲区，会导致一次额外的 D-TLB 缺失（页面错误）。因此采用了一种"取巧"的方式：**直接将 TLS 数据指针作为系统调用返回值返回**。

由于有效的 TLS 数据指针地址范围通常不在 `0xFFFFxxxx`（即 `-4095` 到 `-1`）之内，而内核将返回值在 `-4095` 到 `-1` 范围的视为错误（`ERR_PTR` 范围），因此这种设计不会导致用户态误判为错误。

### 系统调用表如何生成：为什么 arm64 上根本没有这一号

本树 `scripts/syscall.tbl` 里这三行是"带 ABI 标签"的：

```
# Architectures may provide up to 16 syscalls of their own between 244 and 259
244	arc	cacheflush			sys_cacheflush
245	arc	arc_settls			sys_arc_settls
246	arc	arc_gettls			sys_arc_gettls
247	arc	sysfs				sys_sysfs
248	arc	arc_usr_cmpxchg			sys_arc_usr_cmpxchg
```

第 2 列是 ABI 名（`common`、`64`、`time32`、`arc`、`riscv`……）。`scripts/syscalltbl.sh` 用 `grep -E "^[0-9]+[[:space:]]+($abis)"` 过滤，只把指定 ABI 的行写进 `asm/syscall_table_*.h`。各架构在 `Makefile.syscalls` 里声明自己的 ABI 集合，例如 arm64：

```make
syscall_abis_32 +=
syscall_abis_64 += renameat rlimit memfd_secret
syscalltbl = arch/arm64/tools/syscall_%.tbl
```

于是 arm64 实际接受的是 `common|64|renameat|rlimit|memfd_secret`，`arc` 行被整体丢弃，245/246 在 arm64 上是**未分配编号**。这与 `include/uapi/asm-generic/unistd.h` 的约定一致：

```c
/*
 * Architectures may provide up to 16 syscalls of their own
 * starting with this value.
 */
#define __NR_arch_specific_syscall 244
```

所以：

- 用户态在 arm64 上写 `syscall(246, ...)` 时，`arch/arm64/kernel/sys.c` 的 `sys_call_table` 默认项是 `sys_ni_syscall`，返回 `-ENOSYS`；
- `__NR_arc_gettls` 这个宏在 arm64 的用户态头文件里根本不存在，glibc 自然也不会调用它；
- 笔记开头写的"ARM64 上为 stub 实现"应理解为"落到通用 `sys_ni_syscall` 空白项"，而不是某处有一份 ARC 的占位函数。

## 关键数据结构

```c
// arch/arc/include/asm/thread_info.h
struct thread_info {
    unsigned long thr_ptr;    // 线程本地存储指针
    // ... 其他字段
};
```

完整定义（`arch/arc/include/asm/thread_info.h`）只有 6 个字段，文件头注释要求整个结构"fits entirely inside one cache line"，因此 `thr_ptr` 与 `ksp`、`cpu`、`task` 总是同处一行：

```c
struct thread_info {
	unsigned long flags;		/* low level flags */
	unsigned long ksp;		/* kernel mode stack top in __switch_to */
	int preempt_count;		/* 0 => preemptible, <0 => BUG */
	int cpu;			/* current CPU */
	unsigned long thr_ptr;		/* TLS ptr */
	struct task_struct *task;	/* main task structure */
};
```

### thr_ptr 的生命周期

`arc_gettls` 只是"读"，真正让它有意义的是另外三条写入/恢复路径：

| 时机 | 代码 | 行为 |
|--|--|--|
| `arc_settls(ptr)` | `process.c` | 直接写入 `(unsigned int)user_tls_data_ptr` |
| `clone(..., CLONE_SETTLS, ..., tls)` | `copy_thread()` | `task_thread_info(p)->thr_ptr = tls` |
| 普通 `fork()`/`clone()` 无 `CLONE_SETTLS` | `copy_thread()` | 继承父线程：`thr_ptr = task_thread_info(current)->thr_ptr` |
| 首次被调度 | `copy_thread()` | `c_callee->r25 = task_thread_info(p)->thr_ptr`，把值预置到子任务的用户 callee 寄存器保存区 |
| 每次上下文切换 | `ctx_sw_asm.S` | `SAVE_CALLEE_SAVED_KERNEL` / `RESTORE_CALLEE_SAVED_KERNEL` 保存恢复 r25 |
| ptrace | `arch/arc/kernel/ptrace.c` | `PTRACE_GET_THREAD_AREA` 用 `put_user(task_thread_info(child)->thr_ptr, ...)` 读出 |

关键点：ARC 把"用户 TLS 指针"放在 **r25** 里（`copy_thread()` 的注释称其为 "usermode thread pointer #1"），而 `__switch_to` 通过 `SAVE_CALLEE_SAVED_KERNEL` / `RESTORE_CALLEE_SAVED_KERNEL` 保存恢复内核态 callee-saved 寄存器集合，r25 在集合内，于是 TLS 指针随任务切换自动"恢复"。这也是为什么 `thr_ptr` 必须存在 `thread_info` 里：`__switch_to` 需要从 `task_struct` 找回上次保存的 r25 值。

内核从不 dereference `thr_ptr`：`arc_gettls` 只返回值，`ptrace` 只做 `put_user`。因此用户传入任意非法指针也不会让内核出错——它只是一个被搬运的整数。

## 流程图

```
用户态调用 arc_gettls()
  │
  ▼
系统调用入口 (trap)
  │
  ▼
SYSCALL_DEFINE0(arc_gettls)     // arch/arc/kernel/process.c
  │
  ▼
task_thread_info(current)->thr_ptr  // 读取 thr_ptr 字段
  │
  ▼
返回 thr_ptr 值
  │
  ▼
用户态获得 TLS 指针
```

## 使用场景

- 线程本地存储（TLS）管理
- 用户态线程库（如 pthreads）实现
- 编译器 `__thread` 关键字支持

## 相关系统调用

| 系统调用 | 编号 | 功能 |
|--|--|--|
| `arc_settls` | 245 | 设置 TLS 指针 |
| `arc_gettls` | 246 | 获取 TLS 指针 |
| `arc_usr_cmpxchg` | 248 | 用户态 CAS 操作 |

## 注意事项

- ARM64 上此系统调用仅为 ARC 架构兼容性保留
- ARM64 通过 `mrs x0, tpidr_el0` 指令直接读取 TLS 寄存器
- glibc 在 ARC 架构上使用此系统调用来实现 TLS 访问

### 语义边界与"错误码"

| 项目 | 说明 |
|--|--|
| 参数 | 无（`SYSCALL_DEFINE0`），因此不存在 `EFAULT` |
| 返回值 | 当前线程 `thr_ptr`；**不是** errno 编码 |
| ARC 上的错误码 | 无。语义上恒成功 |
| 其它架构 | 编号未分配 → `sys_ni_syscall` → `-ENOSYS` |
| 调用时机 | 任何时候；不睡眠、不加锁、不访问用户内存 |
| 与 `arc_settls` 的配对 | `settls` 写、`gettls` 读，二者共用 `thread_info.thr_ptr` |

因为 `arc_gettls` 不产生 `errno`，用户态判断"失败"的唯一方式反而是"返回值落在 `0xFFFFF001`~`0xFFFFFFFF`"——而按 ARC 的 `TASK_SIZE`，这种值不可能出现，所以可以无条件把它当指针用。ARC 版 glibc 因此可以用 `syscall(__NR_arc_gettls)` 取回线程指针，而不必像常规系统调用那样先检查 `errno`。

### 与 ARM64 的实现对照

arm64 没有对应的系统调用，用户 TLS 直接落在系统寄存器 `tpidr_el0`：

```c
/* arch/arm64/kernel/process.c，copy_thread() / __switch_to() */
*task_user_tls(p) = read_sysreg(tpidr_el0);

write_sysreg(*task_user_tls(next), tpidr_el0);
```

| 维度 | ARC | ARM64 |
|--|--|--|
| 用户读写方式 | 系统调用 `arc_gettls`/`arc_settls` | `mrs`/`msr tpidr_el0` 指令 |
| 内核保存位置 | `thread_info.thr_ptr` | `task_struct.thread.uw.tp_value`（`task_user_tls()`） |
| 切换恢复 | 作为 callee-saved r25 随栈帧恢复 | `__switch_to()` 显式 `write_sysreg` |
| 首次设置 | `clone(CLONE_SETTLS)` / `copy_thread()` | 同左，`p->thread.uw.tp_value = tls` |
| 读取开销 | 一次 trap（数百周期量级） | 一条指令量级 |
| 32/64 位 | 仅 32 位 ARC | 64 位与 compat 共用 `tpidr_el0` |

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/arc/kernel/process.c](/home/louis/code/linux/arch/arc/kernel/process.c) | arc_gettls 实现 |
| [arch/arc/include/asm/syscalls.h](/home/louis/code/linux/arch/arc/include/asm/syscalls.h) | 系统调用声明 |
| [tools/arch/arc/include/uapi/asm/unistd.h](/home/louis/code/linux/tools/arch/arc/include/uapi/asm/unistd.h) | 系统调用编号定义 |