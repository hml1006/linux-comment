# delete_module 系统调用分析

## 1. 概述

`delete_module` 卸载已加载的内核模块。卸载前需要检查模块的引用计数，如果模块正在被使用或设置了 `O_NONBLOCK` 且模块忙，则卸载失败。

**原型：**

```c
SYSCALL_DEFINE2(delete_module, const char __user *, name_user, unsigned int, flags)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| `name_user` | `const char __user *` | 模块名称（用户空间字符串） |
| `flags` | `unsigned int` | 标志位（`O_NONBLOCK` 表示非阻塞模式） |

**返回值：**
- 成功返回 0
- 失败返回负的错误码

## 2. 使用场景

- `rmmod` 命令卸载模块
- `modprobe -r` 递归卸载模块
- 内核模块开发中的热插拔测试
- 系统维护时替换模块

## 3. 函数调用栈

```
SYSCALL_DEFINE2(delete_module, name_user, flags)          // kernel/module/main.c
  ├─ 权限检查: capable(CAP_SYS_MODULE)
  │    无权限 → 返回 -EPERM
  ├─ copy_module_name(name_user, &name)                   // 从用户空间拷贝模块名
  │    失败 → 返回 -EFAULT
  ├─ mod = find_module(name)                               // 查找模块
  │    找不到 → 返回 -ENOENT
  ├─ [flags & O_NONBLOCK] 非阻塞模式
  │    ├─ mod->state != MODULE_STATE_LIVE → 返回 -EINVAL
  │    └─ module_refcount(mod) > 0 → 返回 -EBUSY
  ├─ [else] 阻塞模式
  │    └─ wait_for_zero_refcount(mod)                      // 等待引用计数归零
  │         └─ 可能进入睡眠等待
  ├─ free_module(mod)                                      // 释放模块
  │    ├─ do_one_initcall(mod->exit)                       // 执行 module_exit 回调
  │    ├─ unregister_pernet_operations()                   // 注销 pernet 操作
  │    ├─ remove_module_section(mod)                       // 移除模块段
  │    │    └─ sysfs_remove_bin_file / sysfs_remove_group  // 移除 sysfs 文件
  │    ├─ mod_sysfs_teardown(mod)                          // 清理 sysfs
  │    ├─ module_deallocate(mod)                           // 释放模块内存
  │    └─ free_module_elf(info)                            // 释放 ELF 信息
  └─ return 0
```

### 3.1 free_module 详细流程

```c
// kernel/module/main.c
static void free_module(struct module *mod)
{
    // 1. 执行模块退出函数
    if (mod->exit != NULL)
        do_one_initcall(mod->exit);

    // 2. 等待模块中的所有 kthread 退出
    // 3. 注销 pernet 操作
    unregister_pernet_operations(mod);

    // 4. 释放模块参数
    // 5. 从全局模块链表中移除
    // 6. 移除 sysfs 接口
    // 7. 释放模块文本和数据段
    // 8. 释放模块描述符
    module_deallocate(mod);
}
```

## 4. 关键数据结构

### 4.1 struct module（内核模块）

```c
// include/linux/module.h
struct module {
    enum module_state state;           // 模块状态: LIVE/COMING/GOING
    struct list_head list;             // 全局模块链表
    char name[MODULE_NAME_LEN];        // 模块名称
    int (*init)(void);                 // 模块初始化函数
    void (*exit)(void);                // 模块清理函数
    atomic_t refcnt;                   // 引用计数
    // ... 更多字段
};
```

### 4.2 模块状态

```c
// include/linux/module.h
enum module_state {
    MODULE_STATE_LIVE,     // 模块正常运行，可使用
    MODULE_STATE_COMING,   // 模块正在加载中
    MODULE_STATE_GOING,    // 模块正在卸载中
    MODULE_STATE_UNFORMED, // 模块尚未格式化
};
```

## 5. 流程图

```
用户态调用 delete_module(name, flags)
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
│  copy_from_user() 获取模块名        │
│  find_module() 查找模块             │
│  找不到 → 返回 -ENOENT              │
│  状态无效 → 返回 -EINVAL            │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  检查引用计数                       │
│  ├─ O_NONBLOCK 模式                 │
│  │    refcnt > 0 → 返回 -EBUSY      │
│  └─ 阻塞模式                        │
│       wait_for_zero_refcount()     │
│       (等待直到 refcnt == 0)        │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│  free_module(mod)                   │
│  ├─ do_one_initcall(mod->exit)     │  ← 执行 module_exit()
│  ├─ unregister_pernet_operations() │
│  ├─ 从全局链表移除                  │
│  ├─ sysfs 清理                      │
│  └─ module_deallocate() 释放内存    │
└─────────────────────────────────────┘
    │
    ▼
  返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EPERM` | 权限不足 | 调用者没有 `CAP_SYS_MODULE` 能力 |
| `-EFAULT` | 地址错误 | 从用户空间拷贝模块名失败 |
| `-ENOENT` | 模块不存在 | `find_module()` 找不到指定名称的模块 |
| `-EINVAL` | 无效参数 | 模块状态不是 `LIVE`，或模块名无效 |
| `-EBUSY` | 模块忙 | `O_NONBLOCK` 模式下模块引用计数 > 0 |

## 7. 使用示例

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <fcntl.h>

int main(void)
{
    const char *modname = "example_module";

    // 阻塞模式卸载模块
    if (syscall(SYS_delete_module, modname, 0) == -1) {
        perror("delete_module");
        return 1;
    }

    printf("Module '%s' unloaded successfully\n", modname);
    return 0;
}
```

### 非阻塞模式

```c
// 非阻塞模式：如果模块忙则立即返回 -EBUSY
if (syscall(SYS_delete_module, modname, O_NONBLOCK) == -1) {
    if (errno == EBUSY) {
        printf("Module is busy, try again later\n");
    } else {
        perror("delete_module");
    }
}
```

## 8. 参考

- 源码: `kernel/module/main.c`（`SYSCALL_DEFINE2(delete_module)` 和 `free_module()`）
- 头文件: `include/linux/module.h`
- 用户态命令: `rmmod`（kmod 包），`modprobe -r`（kmod 包）
- 相关系统调用: `init_module()`, `finit_module()`