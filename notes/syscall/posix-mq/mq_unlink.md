# mq_unlink 系统调用分析

## 1. 概述

`mq_unlink` 用于删除一个 POSIX 消息队列。队列的名称被立即删除，但队列本身在所有打开该队列的描述符关闭后才被真正销毁。

**原型：**

```c
SYSCALL_DEFINE1(mq_unlink, const char __user *, u_name)
```

## 2. 参数说明

| 参数 | 说明 |
|------|------|
| `u_name` | 消息队列名称（如 `/my_queue`） |

## 3. 函数调用链

```
mq_unlink (系统调用入口)
  │
  └─ do_mq_unlink(u_name)
       │
       ├─ ipc_ns = current->nsproxy->ipc_ns
       ├─ mnt = ipc_ns->mq_mnt                          // mqueue 文件系统挂载
       │
       ├─ CLASS(filename, name)(u_name)                   // 拷贝文件名
       │
       ├─ audit_inode_parent_hidden(name, mnt->mnt_root)
       ├─ mnt_want_write(mnt)                             // 检查写权限
       │
       ├─ dentry = start_removing_noperm(mnt->mnt_root, &QSTR(name->name))
       │    └─ 查找 dentry
       │
       ├─ inode = d_inode(dentry)
       ├─ ihold(inode)                                    // 增加 inode 引用
       │
       ├─ err = vfs_unlink(&nop_mnt_idmap, d_inode(mnt->mnt_root), dentry, NULL)
       │    └─ 调用 mqueue 文件系统的 unlink 操作
       │         └─ mqueue_unlink(inode, dentry)
       │              └─ 删除 dentry 引用
       │
       ├─ end_removing(dentry)
       ├─ iput(inode)                                     // 释放 inode 引用
       │    └─ 如果这是最后一个引用:
       │         └─ mqueue_inode_ops.evict_inode → 释放队列
       │
       ├─ mnt_drop_write(mnt)
       └─ 返回 0
```

## 4. 延迟销毁机制

```
mq_unlink("/my_queue")
  │
  ├── 名称从目录中删除（后续 mq_open 找不到）
  │
  ├── inode 的引用计数减 1
  │
  ├── 如果还有其他进程持有打开的描述符:
  │    └── inode 保持存活，队列继续可用
  │
  └── 当最后一个描述符关闭时:
       └── fput → release → mqueue_close → iput
            └── inode 引用计数归零 → evict_inode → 释放队列内存
```

## 5. 流程图

```
用户态调用 mq_unlink(name)
  │
  v
do_mq_unlink(name)
  │
  ├── 获取 mqueue 文件系统挂载
  │
  ├── 查找 dentry (start_removing_noperm)
  │
  ├── vfs_unlink() 删除目录项
  │    └── 名称立即可见地消失
  │
  ├── iput(inode) 释放引用
  │    └── [最后一个引用] → 释放 mqueue_inode_info
  │
  └── 返回 0
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `EINVAL` | 无效参数 | 名称格式错误（不以 `/` 开头） |
| `ENOENT` | 队列不存在 | 指定的队列名称不存在 |
| `EACCES` | 权限不足 | 队列所在目录无写权限 |
| `EFAULT` | 地址错误 | name 指针不可访问 |

## 7. 使用示例

```c
#include <mqueue.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    struct mq_attr attr = {
        .mq_maxmsg  = 10,
        .mq_msgsize = 256,
    };

    // 创建队列
    mqd_t mq = mq_open("/temp_queue", O_CREAT | O_RDWR, 0644, &attr);
    if (mq == (mqd_t)-1) {
        perror("mq_open");
        exit(1);
    }

    printf("Queue created, press enter to unlink...\n");
    getchar();

    // 删除队列（队列名称立即可见性消失）
    if (mq_unlink("/temp_queue") == -1) {
        perror("mq_unlink");
        exit(1);
    }
    printf("Queue unlinked, descriptor %d still valid\n", mq);

    // 在关闭描述符之前，队列仍然可以正常使用
    const char *msg = "Last message";
    mq_timedsend(mq, msg, strlen(msg) + 1, 1, NULL);

    // 关闭描述符后队列真正释放
    mq_close(mq);
    printf("Queue fully destroyed after close\n");

    // 验证队列已删除
    mqd_t mq2 = mq_open("/temp_queue", O_RDONLY);
    if (mq2 == (mqd_t)-1) {
        printf("Confirmed: queue no longer exists\n");
    }

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#posix-消息队列)
- 源码位置：`ipc/mqueue.c`
- 用户态头文件：`mqueue.h`