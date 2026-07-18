# sysfs 系统调用

## 概述

> **注意：`sysfs` 系统调用是一个历史遗留接口，仅在部分架构（如 ARC）上有独立的系统调用编号。** 在 ARM64 架构上，`sysfs` 没有独立的系统调用编号，且该接口在现代 Linux 中已被废弃。

`sysfs` 系统调用用于获取已注册文件系统类型的相关信息，包括按名称查询索引、按索引查询名称、以及获取文件系统总数。其功能已完全被 `/proc/filesystems` 虚拟文件系统和 `sysfs` 文件系统（挂载于 `/sys`）替代。

---

## 函数原型

```c
#include <unistd.h>
#include <sys/syscall.h>

long syscall(SYS_sysfs, int option, unsigned long arg1, unsigned long arg2);
```

或使用库函数封装：

```c
#include <sys/syscall.h>

int sysfs(int option, ...);
```

### 参数说明

| 参数 | 类型 | 描述 |
|------|------|------|
| `option` | `int` | 操作选项（1、2、3，见下方说明） |
| `arg1` | `unsigned long` | 取决于 option（名称字符串或索引值） |
| `arg2` | `unsigned long` | 取决于 option（缓冲区指针） |

### 操作选项

| option | 功能 | arg1 | arg2 | 返回值 |
|--------|------|------|------|--------|
| 1 | 根据文件系统名称查询索引 | `const char __user *` 名称 | 未使用 | 文件系统索引，失败返回 `-EINVAL` |
| 2 | 根据索引获取文件系统名称 | 索引值 | `char __user *` 缓冲区 | 0 成功，失败返回负值 |
| 3 | 获取已注册文件系统数量 | 未使用 | 未使用 | 文件系统数量 |

---

## 详细调用链分析

### 内核入口

```c
// fs/filesystems.c
SYSCALL_DEFINE3(sysfs, int, option, unsigned long, arg1, unsigned long, arg2)
{
    int retval = -EINVAL;

    switch (option) {
        case 1:
            retval = fs_index((const char __user *) arg1);
            break;
        case 2:
            retval = fs_name(arg1, (char __user *) arg2);
            break;
        case 3:
            retval = fs_maxindex();
            break;
    }
    return retval;
}
```

### 内部辅助函数

#### fs_index() — 名称转索引

```c
// fs/filesystems.c
static int fs_index(const char __user *name)
{
    struct file_system_type *tmp;
    int err, index;

    /* 从用户空间拷贝文件系统名称 */
    name = getname(name);
    if (IS_ERR(name))
        return PTR_ERR(name);

    err = -EINVAL;
    read_lock(&file_systems_lock);
    /* 遍历已注册文件系统链表，查找匹配项 */
    for (tmp = file_systems, index = 0; tmp; tmp = tmp->next, index++) {
        if (strcmp(tmp->name, name) == 0) {
            err = index;
            break;
        }
    }
    read_unlock(&file_systems_lock);
    putname(name);
    return err;
}
```

#### fs_name() — 索引转名称

```c
// fs/filesystems.c
static int fs_name(unsigned int index, char __user *buf)
{
    struct file_system_type *tmp;
    int len, res = -EINVAL;

    read_lock(&file_systems_lock);
    /* 遍历链表到指定索引位置 */
    for (tmp = file_systems; tmp; tmp = tmp->next, index--) {
        if (index == 0) {
            if (try_module_get(tmp->owner))
                res = 0;
            break;
        }
    }
    read_unlock(&file_systems_lock);
    if (res)
        return res;

    /* 拷贝名称到用户空间 */
    len = strlen(tmp->name) + 1;
    res = copy_to_user(buf, tmp->name, len) ? -EFAULT : 0;
    put_filesystem(tmp);
    return res;
}
```

#### fs_maxindex() — 获取文件系统数量

```c
// fs/filesystems.c
static int fs_maxindex(void)
{
    struct file_system_type *tmp;
    int index;

    read_lock(&file_systems_lock);
    for (tmp = file_systems, index = 0; tmp; tmp = tmp->next, index++)
        ;
    read_unlock(&file_systems_lock);
    return index;
}
```

### 完整调用链

```
sysfs(option, arg1, arg2)                      // 用户空间调用
  │
  └─ SYSCALL_DEFINE3(sysfs, option, arg1, arg2) // fs/filesystems.c
       │
       └─ switch(option):
            │
            ├─ option 1: fs_index(name)
            │    ├─ getname(name)              // 从用户空间拷贝名称
            │    ├─ read_lock(&file_systems_lock)
            │    ├─ 遍历 file_systems 链表
            │    ├─ strcmp(tmp->name, name)
            │    └─ 返回匹配的索引值
            │
            ├─ option 2: fs_name(index, buf)
            │    ├─ read_lock(&file_systems_lock)
            │    ├─ 遍历到第 index 个文件系统
            │    ├─ try_module_get(tmp->owner)  // 增加模块引用计数
            │    ├─ read_unlock
            │    ├─ copy_to_user(buf, name, len)
            │    └─ put_filesystem(tmp)         // 减少模块引用计数
            │
            └─ option 3: fs_maxindex()
                 ├─ read_lock(&file_systems_lock)
                 ├─ 遍历链表计数
                 ├─ read_unlock
                 └─ 返回文件系统总数
```

---

## 关键数据结构

### file_system_type — 文件系统类型

```c
// include/linux/fs.h
struct file_system_type {
    const char *name;                          /* 文件系统名称 */
    int fs_flags;                              /* 文件系统标志 */
    struct dentry *(*mount)(struct file_system_type *, int,
                            const char *, void *);  /* 挂载回调 */
    void (*kill_sb)(struct super_block *);     /* 卸载回调 */
    struct module *owner;                      /* 所属模块 */
    struct file_system_type *next;             /* 链表下一项 */
    struct hlist_head fs_supers;               /* 已挂载超级块列表 */
    ...
};
```

### 全局文件系统链表

```c
// fs/super.c
extern struct file_system_type *file_systems;
```

该链表是内核中所有已注册文件系统类型的全局链表，通过 `read_lock(&file_systems_lock)` 保护。

---

## 执行流程（ASCII 流程图）

```
                    ┌───────────────┐
                    │  sysfs()      │
                    │  (userspace)  │
                    └───────┬───────┘
                            │ syscall
                            ▼
                    ┌───────────────┐
                    │ sys_sysfs()   │
                    │ fs/filesystems │
                    │    .c         │
                    └───────┬───────┘
                            │
                    ┌───────┴──────────────────────────┐
                    │  switch(option):                  │
                    │                                   │
                    │  option 1 ──► fs_index(name)      │
                    │       │                           │
                    │       ├─ getname()                │
                    │       ├─ 遍历 file_systems 链表   │
                    │       └─ 返回名称匹配的索引       │
                    │                                   │
                    │  option 2 ──► fs_name(index,buf)  │
                    │       │                           │
                    │       ├─ 遍历到第 index 项        │
                    │       ├─ try_module_get()         │
                    │       └─ copy_to_user()           │
                    │                                   │
                    │  option 3 ──► fs_maxindex()       │
                    │       │                           │
                    │       └─ 遍历链表返回总数         │
                    │                                   │
                    │  default ──► return -EINVAL       │
                    └───────────────────────────────────┘
```

---

## 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | `option` 不是 1、2、3，或未找到匹配的文件系统名称 |
| `-EFAULT` | 用户空间地址错误 | `name` 或 `buf` 指向不可访问的用户空间地址 |
| `-ENOSYS` | 功能未实现 | ARM64 等架构上无此系统调用编号 |

---

## 使用示例

### 1. 查询文件系统名称对应的索引

```c
#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* 查询 ext4 文件系统的索引 */
    long ret = syscall(SYS_sysfs, 1, "ext4", 0);
    if (ret < 0) {
        perror("sysfs");
        return 1;
    }
    printf("ext4 index: %ld\n", ret);
    return 0;
}
```

### 2. 当前替代方案：读取 /proc/filesystems

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    FILE *fp = fopen("/proc/filesystems", "r");
    if (!fp) {
        perror("fopen");
        return 1;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    int index = 0;

    while ((read = getline(&line, &len, fp)) != -1) {
        /* 跳过 nodev 行，或保留均可 */
        printf("%d: %s", index++, line);
    }

    free(line);
    fclose(fp);
    return 0;
}
```

---

## 源码位置

| 文件 | 说明 |
|------|------|
| `fs/filesystems.c` | `SYSCALL_DEFINE3(sysfs)` 实现，含 `fs_index()`、`fs_name()`、`fs_maxindex()` |
| `include/linux/fs.h` | `struct file_system_type` 定义 |
| `include/linux/syscalls.h` | `sys_sysfs` 声明 |