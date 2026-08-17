# chroot 系统调用分析

## 1. 概述

改变当前进程的根目录。将进程的根目录（`/`）改为指定目录，进程及其子进程将无法访问该目录之外的任何文件。

**原型：**

```c
SYSCALL_DEFINE1(chroot, const char __user *, filename)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `filename` | `const char *` | 新的根目录路径 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_CHROOT` 权限
  - `-EACCES` — 无执行权限
  - `-ENOENT` — 路径不存在
  - `-ENOTDIR` — 路径不是目录

## 2. 使用场景

- **chroot 监狱**: 隔离进程文件系统访问，用于测试或安全加固
- **系统恢复**: 在 Live CD 环境中 chroot 到损坏的系统
- **构建环境**: 为软件构建创建隔离的文件系统视图
- **容器初始化**: 容器运行时切换根文件系统的基础步骤

## 3. 函数调用栈

```
chroot(filename) (系统调用入口)
└─ ksys_chroot(filename)                              // fs/open.c
   ├─ user_path_at(AT_FDCWD, filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path)
   │                                                    // 路径解析
   ├─ inode_lock(path.dentry->d_inode)                 // 加锁
   ├─ mnt_want_write(path.mnt)                         // 可写检查
   │
   ├─ [权限检查]
   │  ├─ capable(CAP_SYS_CHROOT)                       // 需要 CAP_SYS_CHROOT
   │  └─ inode_permission(inode, MAY_EXEC | MAY_ACCESS) // 执行权限
   │
   ├─ set_fs_root(current->fs, &path)                  // 更新进程根目录
   │
   ├─ mnt_drop_write(path.mnt)                         // 释放写锁
   ├─ inode_unlock(path.dentry->d_inode)               // 解锁
   └─ path_put(&path)                                  // 释放路径引用
```

## 4. 关键数据结构

```c
// ===== struct fs_struct (进程文件系统信息, include/linux/fs_struct.h) =====
struct fs_struct {
    struct path root;              // 进程根目录
    struct path pwd;               // 进程当前工作目录
    struct seqcount rw_seqcount;   // 顺序锁
    int in_exec;                   // 是否正在执行 exec
};

// ===== struct path (路径, include/linux/path.h) =====
struct path {
    struct vfsmount *mnt;          // 挂载点
    struct dentry *dentry;         // 目录项
};
```

## 5. 流程图

```
用户态调用 chroot("/newroot")
  │
  v
ksys_chroot(filename)
  │
  ├─ user_path_at()  // 解析路径 → 获取 dentry+mnt
  │
  ├─ capable(CAP_SYS_CHROOT)  // 权限检查
  │
  ├─ inode_permission(MAY_EXEC)  // 新根目录需可执行
  │
  └─ set_fs_root(current->fs, &path)
       │
       ├─ path_get(&path)       // 增加引用计数
       ├─ path_put(&fs->root)   // 释放旧根引用
       └─ fs->root = path       // 设置新根
```

## 6. 使用示例

```c
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

int main(void)
{
    // 准备 chroot 环境（需要 root 权限）
    // mkdir -p /tmp/jail/{bin,lib,lib64}
    // cp /bin/bash /tmp/jail/bin/
    // cp /lib/x86_64-linux-gnu/l* /tmp/jail/lib/

    if (chroot("/tmp/jail") == -1) {
        perror("chroot");
        return 1;
    }

    // 进入 chroot 环境后，需要切换工作目录
    if (chdir("/") == -1) {
        perror("chdir");
        return 1;
    }

    // 现在只能看到 /tmp/jail 中的文件
    // 执行 /bin/bash 会失败，因为找不到 /bin/bash
    // 需要先复制必要的二进制和库到 /tmp/jail

    printf("chroot to /tmp/jail successful\n");
    return 0;
}
```

## 7. 参考

- `fs/open.c` — chroot 实现
- `include/linux/fs_struct.h` — fs_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)