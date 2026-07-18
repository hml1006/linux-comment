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

## 调用链分析

```
arc_gettls()
  └─ task_thread_info(current)->thr_ptr    // 直接从 thread_info 读取 TLS 指针
       └─ 返回 thr_ptr 值作为系统调用返回值
```

### 设计考虑

ARC 架构的设计者注意到，如果通过 `copy_to_user()` 将 TLS 指针写入用户空间缓冲区，会导致一次额外的 D-TLB 缺失（页面错误）。因此采用了一种"取巧"的方式：**直接将 TLS 数据指针作为系统调用返回值返回**。

由于有效的 TLS 数据指针地址范围通常不在 `0xFFFFxxxx`（即 `-4095` 到 `-1`）之内，而内核将返回值在 `-4095` 到 `-1` 范围的视为错误（`ERR_PTR` 范围），因此这种设计不会导致用户态误判为错误。

## 关键数据结构

```c
// arch/arc/include/asm/thread_info.h
struct thread_info {
    unsigned long thr_ptr;    // 线程本地存储指针
    // ... 其他字段
};
```

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

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/arc/kernel/process.c](/home/louis/code/linux/arch/arc/kernel/process.c) | arc_gettls 实现 |
| [arch/arc/include/asm/syscalls.h](/home/louis/code/linux/arch/arc/include/asm/syscalls.h) | 系统调用声明 |
| [tools/arch/arc/include/uapi/asm/unistd.h](/home/louis/code/linux/tools/arch/arc/include/uapi/asm/unistd.h) | 系统调用编号定义 |