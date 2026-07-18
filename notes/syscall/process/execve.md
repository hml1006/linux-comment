# execve 系统调用分析

## 1. 概述

`execve` 系统调用用于执行一个新程序，用新程序替换当前进程的地址空间、寄存器状态和堆栈。执行成功后，原进程的代码段、数据段、堆和栈完全被新程序取代，但进程的 PID 保持不变。

### 关键特点

- 替换当前进程的地址空间，不创建新进程
- 通过 `load_elf_binary` 加载 ELF 格式可执行文件
- 支持脚本文件（通过 `#!` 行解析器）
- 处理 `close_on_exec` 标志的文件描述符自动关闭
- `execve` 是 `execveat` 的简化版本，固定使用 `AT_FDCWD`

---

## 2. 函数原型

```c
#include <unistd.h>

int execve(const char *pathname, char *const argv[], char *const envp[]);
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `pathname` | 要执行的文件路径 |
| `argv` | 传递给新程序的参数数组 |
| `envp` | 环境变量数组 |

### 内核入口

```c
// fs/exec.c:1924
SYSCALL_DEFINE3(execve,
        const char __user *, filename,
        const char __user *const __user *, argv,
        const char __user *const __user *, envp)
{
    CLASS(filename, name)(filename);
    return do_execveat_common(AT_FDCWD, name,
                              native_arg(argv), native_arg(envp), 0);
}
```

---

## 3. 调用链分析

### 完整调用链

```
execve(pathname, argv, envp)
└─ syscall(__NR_execve, pathname, argv, envp)
   └─ SYSCALL_DEFINE3(execve)                       // fs/exec.c:1924
      └─ do_execveat_common(AT_FDCWD, filename, argv, envp, 0)  // fs/exec.c:1778
         ├─ is_rlimit_overlimit(...) → -EAGAIN       // RLIMIT_NPROC 检查
         ├─ CLASS(bprm, bprm)(fd, filename, flags)   // 分配并初始化 struct linux_binprm
         │  └─ alloc_bprm(fd, filename, flags)        // fs/exec.c:1685
         │     ├─ bprm = kzalloc(sizeof(*bprm), GFP_KERNEL)
         │     ├─ bprm_mm_init(bprm)                  // 初始化新 mm_struct
         │     │  └─ mm_alloc() → 分配 mm_struct
         │     │  └─ __bprm_mm_init(bprm)             // 初始化栈的 VMA
         │     ├─ bprm->file = do_open_execat(fd, filename, flags)  // 打开目标文件
         │     │  └─ do_open_execat(fd, filename, flags)
         │     │     ├─ file_open_root_mnt(...) 或 file_open_name(...)
         │     │     └─ deny_write_access(file)       // 禁止写入（执行中）
         │     └─ bprm->filename = filename->name
         ├─ count(argv, MAX_ARG_STRINGS)              // 统计参数个数
         ├─ count(envp, MAX_ARG_STRINGS)              // 统计环境变量个数
         ├─ bprm_stack_limits(bprm)                   // 检查栈空间限制
         ├─ copy_string_kernel(bprm->filename, bprm)  // 拷贝文件名到栈
         ├─ copy_strings(bprm->envc, envp, bprm)      // 拷贝环境变量到栈
         ├─ copy_strings(bprm->argc, argv, bprm)      // 拷贝参数到栈
         ├─ bprm->exec = bprm->p                      // 记录执行入口
         ├─ exec_binprm(bprm, argv, envp)             // 核心执行路径
         │  └─ search_binary_handler(bprm)             // 查找合适的二进制格式处理器
         │     └─ fmt->load_binary(bprm)               // → load_elf_binary(bprm)
         │        │                                    // fs/binfmt_elf.c
         │        ├─ kernel_read(bprm->file, 0, elf_ex, sizeof(elf_ex))
         │        │  → 读取 ELF 头部
         │        ├─ 检查 ELF magic 数 (ELFMAG)
         │        ├─ elf_phdata = load_elf_phdrs()     // 读取程序头表
         │        ├─ [PT_INTERP] → 加载动态链接器
         │        │  ├─ load_elf_interp(loc->interp_elf_ex, ...)
         │        │  └─ elf_map(bprm->file, ...) → do_mmap
         │        ├─ elf_map(bprm->file, ...) → do_mmap  // 映射 PT_LOAD 段
         │        │  ├─ 每个 PT_LOAD 段调用一次
         │        │  └─ setup_new_exec(bprm)            // 设置新执行环境
         │        ├─ de_thread(me)                      // 单线程化
         │        │  └─ 等待其他线程退出，成为线程组唯一线程
         │        ├─ exec_mmap(bprm->mm)                // 切换地址空间
         │        │  ├─ 释放旧 mm_struct
         │        │  └─ 切换至 bprm 中的新 mm_struct
         │        ├─ exec_fd清理 → close_on_exec       // 关闭设置了 FD_CLOEXEC 的 fd
         │        ├─ finalize_exec(regs)                // 架构特定最终化
         │        │  └─ arch/arm64/kernel/process.c
         │        └─ start_thread(regs, elf_entry, bprm->p)  // 设置新进程的入口点
         │           └─ arch/arm64/include/asm/processor.h
         │              ├─ regs->pc = elf_entry         // 新程序入口
         │              └─ regs->sp = bprm->p           // 新栈指针
         ├─ acct_update_integrals(current)              // 记账更新
         └─ audit_bprm(bprm)                            // 审计日志
```

### exec_binprm 详细流程

```c
// fs/exec.c:1660
static int exec_binprm(struct linux_binprm *bprm,
                       struct user_arg_ptr argv,
                       struct user_arg_ptr envp)
{
    int ret;

    // 对脚本文件，可能需要多次解析（如 #!/bin/sh 再嵌套 #!/usr/bin/perl）
    ret = search_binary_handler(bprm);
    if (ret >= 0) {
        // 成功加载二进制
        audit_bprm(bprm);
        sched_exec();  // 调度器提示
        return ret;
    }

    return ret;
}
```

---

## 4. 关键数据结构

```c
// ========== 二进制参数结构 (include/linux/binfmts.h) ==========

struct linux_binprm {
    struct file *file;                  // 要执行的文件
    struct file *executable;            // 实际执行的文件（脚本可能不同）
    struct mm_struct *mm;               // 新地址空间
    unsigned long p;                    // 栈顶指针（当前参数压栈位置）
    unsigned long argmin;               // 栈底限制
    unsigned int argc;                  // 参数个数
    unsigned int envc;                  // 环境变量个数
    const char *filename;               // 原始文件名
    const char *interp;                 // 解释器名（脚本的 #! 行）
    unsigned long exec;                 // 执行入口地址
    unsigned long loader;               // 加载器地址
    unsigned long flags;                // 标志位
    unsigned long buf[BINPRM_BUF_SIZE]; // 文件头部数据（最多 256 字节）
    struct cred *cred;                  // 新凭证（setuid 等）
    int unsafe;                         // 不安全标志
    // ...
};

// ========== ELF 二进制格式处理程序 ==========

// 注册的二进制格式处理程序链表
static LIST_HEAD(formats);
// 通过 register_binfmt() 注册

struct linux_binfmt {
    struct list_head lh;                // 链表节点
    struct module *module;              // 所属模块
    int (*load_binary)(struct linux_binprm *);  // 加载二进制
    int (*load_shlib)(struct file *);           // 加载共享库
    int (*core_dump)(struct coredump_params *cprm);  // 核心转储
    unsigned long min_coredump;         // 最小 core dump 大小
};

// ========== ELF 头部 (include/uapi/linux/elf.h) ==========

#define ELFMAG "\177ELF"
#define SELFMAG 4

typedef struct elf64_hdr {
    unsigned char e_ident[16];          // ELF 标识（含 magic 数）
    Elf64_Half    e_type;               // 目标文件类型
    Elf64_Half    e_machine;            // 体系结构
    Elf64_Word    e_version;            // 版本
    Elf64_Addr    e_entry;              // 入口点
    Elf64_Off     e_phoff;              // 程序头表偏移
    Elf64_Off     e_shoff;              // 节头表偏移
    Elf64_Word    e_flags;              // 标志
    Elf64_Half    e_ehsize;             // ELF 头部大小
    Elf64_Half    e_phentsize;          // 程序头表项大小
    Elf64_Half    e_phnum;              // 程序头表项数
    Elf64_Half    e_shentsize;          // 节头表项大小
    Elf64_Half    e_shnum;              // 节头表项数
    Elf64_Half    e_shstrndx;           // 节名字符串表索引
} Elf64_Ehdr;

// ========== ELF 程序头 (include/uapi/linux/elf.h) ==========

typedef struct elf64_phdr {
    Elf64_Word    p_type;               // 段类型（PT_LOAD, PT_INTERP 等）
    Elf64_Word    p_flags;              // 段标志（PF_R, PF_W, PF_X）
    Elf64_Off     p_offset;             // 段在文件中的偏移
    Elf64_Addr    p_vaddr;              // 段的虚拟地址
    Elf64_Addr    p_paddr;              // 段的物理地址（未使用）
    Elf64_Xword   p_filesz;             // 段在文件中的大小
    Elf64_Xword   p_memsz;              // 段在内存中的大小
    Elf64_Xword   p_align;              // 段对齐
} Elf64_Phdr;
```

---

## 5. 流程图

```
                    execve(pathname, argv, envp)
                                      |
                            +---------v----------+
                            | SYSCALL_DEFINE3     |
                            | (fs/exec.c)         |
                            +---------+----------+
                                      |
                            +---------v----------+
                            | do_execveat_common  |
                            | (AT_FDCWD, ...)     |
                            +---------+----------+
                                      |
                   +------------------+------------------+
                   |                                     |
            +------v------+                      +------v------+
            | alloc_bprm  |                      | exec_binprm |
            | (fs/exec.c) |                      | (fs/exec.c) |
            +------+------+                      +------+------+
                   |                                     |
        +----------+----------+              +----------v----------+
        | bprm_mm_init(bprm) |              | search_binary_      |
        | (分配新 mm_struct)  |              | handler(bprm)       |
        +----------+----------+              +----------+----------+
        | do_open_execat()   |                         |
        | (打开可执行文件)    |              +----------v----------+
        +----------+----------+              | load_elf_binary     |
        | copy_strings()     |              | (fs/binfmt_elf.c)   |
        | (拷贝参数/环境变量) |              +----------+----------+
        +----------+----------+                         |
                   |                      +-----------+-----------+
                   |                      |                       |
                   |              +-------v-------+       +-------v-------+
                   |              | 读取 ELF 头部  |       | 加载 PT_LOAD 段 |
                   |              | (kernel_read)  |       | (elf_map →    |
                   |              +-------+-------+       |  do_mmap)     |
                   |                      |               +-------+-------+
                   |              +-------v-------+               |
                   |              | PT_INTERP 处理  |       +------v--------+
                   |              | 加载动态链接器  |       | de_thread()   |
                   |              | (load_elf_     |       | 单线程化      |
                   |              |  interp)       |       +------+--------+
                   |              +-------+-------+              |
                   |                      |               +------v--------+
                   |                      |               | exec_mmap()   |
                   |                      |               | 切换地址空间  |
                   |                      |               +------+--------+
                   |                      |                      |
                   |                      |               +------v--------+
                   |                      |               | exec_fd清理    |
                   |                      |               | (close_on_    |
                   |                      |               |  exec)        |
                   |                      |               +------+--------+
                   |                      |                      |
                   |                      |               +------v--------+
                   |                      |               | start_thread  |
                   |                      |               | (设置 pc, sp) |
                   |                      |               +------+--------+
                   |                      |                      |
                   +----------+-----------+----------------------+
                              |
                     +--------v--------+
                     | 返回 0（成功）   |
                     | (不返回，直接    |
                     |  跳转到新程序)   |
                     +-----------------+
```

---

## 6. 错误处理

| 错误码 | 条件 | 触发位置 |
|--------|------|----------|
| `-E2BIG` | 参数或环境变量总大小超过限制 | `copy_strings` |
| `-EACCES` | 文件无执行权限或不包含有效 shebang | `do_open_execat` |
| `-EAGAIN` | 超出 RLIMIT_NPROC 限制 | `do_execveat_common` |
| `-EFAULT` | 参数/环境变量指针指向无效用户空间 | `copy_strings` |
| `-ELIBBAD` | 无效的 ELF 文件格式 | `load_elf_binary` |
| `-ENOENT` | 文件不存在 | `do_open_execat` |
| `-ENOEXEC` | 文件格式不可执行 | `search_binary_handler` |
| `-ENOMEM` | 内存不足 | `bprm_mm_init` 等 |
| `-ETXTBSY` | 文件正在被写入 | `deny_write_access` |

---

## 7. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    char *argv[] = {"/bin/ls", "-l", "/tmp", NULL};
    char *envp[] = {"PATH=/usr/bin", "HOME=/home/user", NULL};

    printf("正在执行 /bin/ls...\n");

    int ret = execve("/bin/ls", argv, envp);
    // 只有失败时才会执行到这里
    perror("execve");
    return 1;
}
```

---

## 8. 与 execveat 对比

| 特性 | execve | execveat |
|------|--------|----------|
| **路径解析** | 总是从当前工作目录开始 | 可通过 fd 指定目录 |
| **fd 参数** | 无 | `dfd` 参数 |
| **flags** | 无 | 支持 `AT_EMPTY_PATH`、`AT_SYMLINK_NOFOLLOW` |
| **实现** | 调用 `do_execveat_common(AT_FDCWD, ...)` | 直接调用 `do_execveat_common(fd, ...)` |
| **使用场景** | 通用程序执行 | 执行 /proc/self/fd/N 中的文件等 |

---

## 9. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#进程控制)
- `fs/exec.c` - 核心实现
- `fs/binfmt_elf.c` - ELF 加载器
- `include/linux/binfmts.h` - struct linux_binprm 定义