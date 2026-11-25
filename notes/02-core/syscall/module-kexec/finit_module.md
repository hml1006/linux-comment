# finit_module 系统调用分析

## 1. 概述

`finit_module` 通过文件描述符加载内核模块。与 `init_module` 不同，它接收一个已打开的文件描述符而非用户空间缓冲区，使得在加载前可以验证文件来源。

**原型：**

```c
SYSCALL_DEFINE3(finit_module, int, fd, const char __user *, uargs, int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `fd` | `int` | 指向内核模块文件（`.ko`）的文件描述符 |
| `uargs` | `const char __user *` | 模块参数字符串（可选，可为空字符串） |
| `flags` | `int` | 标志位（`MODULE_INIT_IGNORE_MODVERSIONS`、`MODULE_INIT_IGNORE_VERMAGIC` 等） |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- `modprobe` 和 `insmod` 命令加载模块
- 通过文件描述符安全地加载模块（可验证来源）
- 从内存 fd 或 memfd 加载模块
- 模块开发中的测试

## 3. 函数调用栈

```
SYSCALL_DEFINE3(finit_module, fd, uargs, flags)          // kernel/module/main.c
  ├─ 权限检查: capable(CAP_SYS_MODULE)
  │    无权限 → 返回 -EPERM
  ├─ 检查 flags 有效性
  │    无效 → 返回 -EINVAL
  ├─ CLASS(fd, f)(fd)                                     // 通过 fd 获取 struct fd
  ├─ fd_empty(f) → 返回 -EBADF
  ├─ 检查文件是否可读
  ├─ load_module(&info, uargs, flags)                     // 加载模块核心函数
  │    ├─ copy_module_from_fd(fd, &info)                  // 从 fd 读取 ELF 数据
  │    ├─ elf_validity_check(info)                        // 验证 ELF 格式
  │    ├─ layout_and_allocate(info)                       // 布局和分配内存
  │    ├─ add_module_usage(info->mod, ...)                // 记录模块依赖
  │    ├─ simplify_symbols(info)                          // 符号解析
  │    ├─ apply_relocations(info)                         // 应用重定位
  │    ├─ module_enable_ro(info->mod, ...)                // 设置只读保护
  │    ├─ do_mod_ctors(info->mod)                         // 执行 C++ 构造函数
  │    ├─ do_one_initcall(info->mod->init)                // 执行 module_init()
  │    └─ 返回 0
  └─ return 0
```

### 3.1 load_module 详细流程

与 `init_module` 共用 `load_module` 核心，唯一区别是数据来源：
- `finit_module`：通过 `copy_module_from_fd()` 从文件描述符读取
- `init_module`：通过 `copy_module_from_user()` 从用户空间缓冲区读取

## 4. 关键数据结构

### 4.1 finit_module 标志位

```c
// include/uapi/linux/module.h
#define MODULE_INIT_IGNORE_MODVERSIONS  1  // 忽略模块版本检测
#define MODULE_INIT_IGNORE_VERMAGIC     2  // 忽略版本魔术字检测
```

### 4.2 模块加载信息结构

```c
// kernel/module/internal.h
struct load_info {
    const char *name;                // 模块名
    char *secstrings;                // 段名
    unsigned int symoffs, stroffs;   // 符号表和字符串表偏移
    struct _ddebug *debug;           // 动态调试信息
    unsigned int num_debug;
    bool sig_ok;                     // 签名验证结果
    struct module *mod;              // 解析后的模块结构
    // ... 更多字段
};
```

## 5. 流程图

```
用户态调用 finit_module(fd, uargs, flags)
    │
    ▼
┌─────────────────────────────────────┐
│  权限检查                           │
│  capable(CAP_SYS_MODULE)            │
│  无权限 → 返回 -EPERM               │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  检查 fd 和 flags 有效性            │
│  fd 无效 → 返回 -EBADF              │
│  flags 无效 → 返回 -EINVAL          │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_module_from_fd(fd, &info)    │  ← 从 fd 读取模块数据
│  elf_validity_check(info)          │  ← 验证 ELF 格式
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  layout_and_allocate(info)         │  ← 分配内存
│  simplify_symbols(info)            │  ← 解析符号
│  apply_relocations(info)           │  ← 重定位
│  module_enable_ro(info->mod, ...)  │  ← 设置内存保护
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  do_mod_ctors(info->mod)           │  ← C++ 构造函数
│  do_one_initcall(info->mod->init)  │  ← 执行 module_init()
│  return 0                          │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_MODULE` 能力 |
| `-EBADF` | 文件描述符无效 | `fd` 不是有效的文件描述符 |
| `-EINVAL` | 无效参数 | `flags` 包含无效值，或 ELF 格式无效 |
| `-ENOEXEC` | 格式错误 | 模块 ELF 格式无效 |
| `-ENOMEM` | 内存不足 | 内核分配内存失败 |
| `-EEXIST` | 模块已存在 | 同名模块已加载 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>

int main(void)
{
    // 打开内核模块文件
    int fd = open("/lib/modules/6.12.0/kernel/drivers/misc/example.ko",
                   O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    // 通过 fd 加载模块
    if (syscall(SYS_finit_module, fd, "", 0) == -1) {
        perror("finit_module");
        close(fd);
        return 1;
    }

    printf("Module loaded successfully via fd\n");
    close(fd);
    return 0;
}
```

## 8. 参考

- 源码: `kernel/module/main.c`（`SYSCALL_DEFINE3(finit_module)`）
- 头文件: `include/uapi/linux/module.h`
- 用户态命令: `insmod`（kmod 包），`modprobe`（kmod 包）
- 相关系统调用: `init_module()`, `delete_module()`