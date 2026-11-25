# init_module 系统调用分析

## 1. 概述

`init_module` 从用户空间缓冲区加载内核模块。将 ELF 格式的模块二进制数据从用户空间拷贝到内核，进行验证、符号解析、重定位后执行模块初始化函数。

**原型：**

```c
SYSCALL_DEFINE3(init_module, void __user *, umod, unsigned long, len,
                const char __user *, uargs)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `umod` | `void __user *` | 指向用户空间模块数据的指针（ELF 格式） |
| `len` | `unsigned long` | 模块数据长度（字节） |
| `uargs` | `const char __user *` | 模块参数字符串（可选，可为空字符串） |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- `insmod` 命令加载模块
- `modprobe` 命令加载模块及其依赖
- 内核模块开发调试
- 系统启动时加载必要模块

## 3. 函数调用栈

```
SYSCALL_DEFINE3(init_module, umod, len, uargs)           // kernel/module/main.c
  └─ load_module(umod, len, uargs)
       ├─ 权限检查: capable(CAP_SYS_MODULE)
       │    无权限 → 返回 -EPERM
       ├─ copy_module_from_user(umod, len, &info)        // 从用户空间拷贝模块 ELF 数据
       │    失败 → 返回 -EFAULT
       ├─ elf_validity_check(info)                        // 验证 ELF 文件格式
       │    ├─ 检查 ELF 魔数
       │    ├─ 检查架构匹配
       │    ├─ 检查版本魔术字 (vermagic)
       │    └─ 检查模块版本 (modversions)
       ├─ layout_and_allocate(info)                       // 布局计算和内存分配
       │    ├─ module_alloc(info->mod->core_layout.size)  // 分配 core 段内存
       │    ├─ module_alloc(info->mod->init_layout.size)  // 分配 init 段内存
       │    └─ 复制各段数据到分配的内存
       ├─ add_module_usage(info->mod, ...)                // 记录模块依赖关系
       ├─ simplify_symbols(info)                          // 简化符号解析
       │    └─ resolve_symbol_wait(info)                  // 解析外部符号引用
       │         └─ 查找已加载模块的导出符号
       ├─ apply_relocations(info)                         // 应用重定位
       │    └─ apply_relocate_add(sechdrs, strtab, ...)   // 应用 ELF 重定位条目
       ├─ module_enable_ro(info->mod, ...)                // 设置只读保护
       │    ├─ set_memory_ro(core_layout.base, ...)       // core 段设为只读
       │    └─ set_memory_ro(init_layout.base, ...)       // init 段设为只读
       ├─ module_enable_nx(info->mod)                     // 设置 NX（不可执行）保护
       ├─ do_mod_ctors(info->mod)                         // 执行 C++ 全局构造函数
       ├─ do_one_initcall(info->mod->init)                // 执行 module_init() 函数
       │    └─ 调用模块的初始化函数
       ├─ 释放 init 段（module_init 执行完后可释放）
       └─ return 0
```

### 3.1 ELF 验证流程

```c
// kernel/module/main.c
static int elf_validity_check(struct load_info *info)
{
    // 1. 检查 ELF 魔数 (ELF Magic)
    // 2. 检查 ELF 类 (32/64 位匹配)
    // 3. 检查字节序
    // 4. 检查 ELF 类型 (ET_REL)
    // 5. 检查机器类型 (arch)
    // 6. 检查 vermagic 字符串匹配
    // 7. 检查 modversions 一致性
    // 8. 检查段表完整性
}
```

## 4. 关键数据结构

### 4.1 模块加载信息

```c
// kernel/module/internal.h
struct load_info {
    const char *name;                // 模块名
    Elf_Ehdr *hdr;                   // ELF 头
    unsigned long len;               // 模块数据长度
    Elf_Shdr *sechdrs;               // 段表
    char *secstrings;                // 段名表
    unsigned int symindex;           // 符号表段索引
    unsigned int strindex;           // 字符串表段索引
    unsigned int modindex;           // .gnu.linkonce.this_module 段索引
    unsigned int versindex;          // __versions 段索引
    unsigned int infoindex;          // .modinfo 段索引
    struct module *mod;              // 解析后的模块结构
    bool sig_ok;                     // 签名验证结果
    // ...
};
```

### 4.2 struct module（内核模块）

```c
// include/linux/module.h
struct module {
    enum module_state state;           // 模块状态
    struct list_head list;             // 全局模块链表
    char name[MODULE_NAME_LEN];        // 模块名称
    struct module *symtab;             // 符号表
    unsigned long num_symtab;          // 符号数量
    int (*init)(void);                 // 初始化函数指针
    void *module_init;                 // init 段地址
    void *module_core;                 // core 段地址
    unsigned int init_size;            // init 段大小
    unsigned int core_size;            // core 段大小
    struct module_layout core_layout;  // core 段布局
    struct module_layout init_layout;  // init 段布局
    atomic_t refcnt;                   // 引用计数
    void (*exit)(void);                // 清理函数指针
    // ...
};
```

## 5. 流程图

```
用户态调用 init_module(umod, len, uargs)
    │
    ▼
┌─────────────────────────────────────┐
│  权限检查: CAP_SYS_MODULE           │
│  无权限 → 返回 -EPERM               │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  copy_module_from_user()            │  ← 拷贝模块数据到内核
│  elf_validity_check()               │  ← 验证 ELF 格式
│  失败 → 返回 -ENOEXEC               │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  layout_and_allocate()              │  ← 内存布局和分配
│  ├─ 计算各段大小和位置              │
│  ├─ module_alloc() 分配内存         │
│  └─ 复制段数据到内存                │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  simplify_symbols()                 │  ← 解析外部符号
│  ├─ 在已加载模块中查找符号          │
│  └─ 更新符号地址                    │
│  apply_relocations()                │  ← 应用重定位
│  └─ 修正代码中的地址引用            │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  module_enable_ro()                 │  ← 设置只读保护
│  module_enable_nx()                 │  ← 设置 NX 保护
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  do_mod_ctors()                     │  ← C++ 构造函数
│  do_one_initcall(mod->init)         │  ← 执行 module_init()
│  → 模块状态设为 LIVE                │
│  → 释放 init 段                     │
└─────────────────────────────────────┘
    │
    ▼
  返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_MODULE` 能力 |
| `-EFAULT` | 地址错误 | 从用户空间拷贝模块数据失败 |
| `-ENOEXEC` | 格式错误 | ELF 格式无效或架构不匹配 |
| `-EINVAL` | 无效参数 | 模块参数或符号表无效 |
| `-ENOMEM` | 内存不足 | 内核分配内存失败 |
| `-EEXIST` | 模块已存在 | 同名模块已加载 |
| `-ENOENT` | 符号未找到 | 模块依赖的外部符号在已加载模块中不存在 |
| `-EBUSY` | 模块忙 | 模块依赖关系导致无法加载 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

int main(void)
{
    const char *modpath = "/lib/modules/6.12.0/kernel/drivers/misc/example.ko";

    // 打开并读取模块文件
    int fd = open(modpath, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct stat st;
    fstat(fd, &st);

    void *mod_data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mod_data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    // 加载模块
    if (syscall(SYS_init_module, mod_data, st.st_size, "") == -1) {
        perror("init_module");
        munmap(mod_data, st.st_size);
        close(fd);
        return 1;
    }

    printf("Module loaded successfully\n");
    munmap(mod_data, st.st_size);
    close(fd);
    return 0;
}
```

## 8. 参考

- 源码: `kernel/module/main.c`（`SYSCALL_DEFINE3(init_module)` 和 `load_module()`）
- 头文件: `include/linux/module.h`, `include/uapi/linux/module.h`
- 内部实现: `kernel/module/internal.h`
- 用户态命令: `insmod`（kmod 包），`modprobe`（kmod 包）
- 相关系统调用: `finit_module()`, `delete_module()`