# rpc_pipefs — RPC 管道文件系统

## 1. 概述与实现机制

rpc_pipefs 是 SUNRPC 服务（NFS 客户端）与内核 RPC 层之间的通信管道文件系统，主要用于 NFS 认证和凭据管理。挂载在 `/var/lib/nfs/rpc_pipefs/`（通常）。

### 核心作用

- **NFS 认证通信**：为内核 RPC 层与用户空间守护进程（rpc.gssd、rpc.idmapd）提供双向通信通道
- **凭据管理**：管理 Kerberos 认证凭据、NFS 映射信息
- **缓存管理**：管理认证缓存和映射缓存

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  rpc.gssd (Kerberos 守护进程)                               │
│  rpc.idmapd (ID 映射守护进程)                                │
│  读写 /var/lib/nfs/rpc_pipefs/ 中的管道文件                 │
└────────────────────────┬─────────────────────────────────────┘
                         │ 文件读写
                         ▼
┌──────────────────────────────────────────────────────────────┐
│              rpc_pipefs (net/sunrpc/rpc_pipe.c)             │
│  rpc_mkpipe() → 创建内核/用户空间通信管道                   │
│  rpc_unlink() → 删除管道                                    │
│  rpc_pipe_open() / rpc_pipe_release()                       │
│  rpc_pipe_read() / rpc_pipe_write() → 数据传递              │
│  rpc_get_inode() → 创建 inode                               │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│              SUNRPC 层 (net/sunrpc/)                        │
│  rpc_pipe_generic_upcall() → 向用户空间发送请求             │
│  rpc_pipe_generic_downcall() → 接收用户空间响应             │
│  gssd 认证请求 / 缓存管理                                   │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 rpc_pipe — RPC 管道

```c
// 文件: include/linux/sunrpc/rpc_pipe_fs.h
struct rpc_pipe {
    struct list_head pipe;             // 管道链表
    struct list_head in_upcall;        // 上行请求列表
    struct list_head in_downcall;      // 下行响应列表
    int pipelen;                       // 管道数据长度
    int nreaders;                      // 读端计数
    int nwriters;                      // 写端计数
    int pipid;                         // 管道 ID
    int flags;                         // 标志位
    struct dentry *dentry;             // 对应的 dentry
    struct message *prep_mess;         // 预备消息
    const struct rpc_pipe_msg_ops *msg_ops; // 消息操作
    struct file *file;                 // 对应的文件
    struct work_struct queue_work;     // 队列工作
    wait_queue_head_t waitq;          // 等待队列
    struct mutex lock;                 // 互斥锁
};
```

### 2.2 rpc_inode — RPC inode

```c
// 文件: include/linux/sunrpc/rpc_pipe_fs.h
struct rpc_inode {
    struct inode vfs_inode;            // 嵌入的 VFS inode
    const struct rpc_pipe_ops *ops;    // RPC 管道操作
    struct rpc_pipe *pipe;             // 关联的 RPC 管道
    struct rpc_clnt *rpc_clnt;         // RPC 客户端
    struct rpc_clnt *rpc_clnt_old;     // 旧 RPC 客户端
    unsigned long private;             // 私有数据
    struct dentry *dentry;             // 关联的 dentry
};
```

### 2.3 rpc_pipe_msg_ops — 管道消息操作

```c
// 文件: include/linux/sunrpc/rpc_pipe_fs.h
struct rpc_pipe_msg_ops {
    int (*upcall)(struct file *filp, struct rpc_pipe_msg *msg, void *calldata); // 上行调用
    int (*downcall)(struct file *filp, struct rpc_pipe_msg *msg, void *calldata); // 下行调用
    void (*release)(struct file *filp, struct rpc_pipe_msg *msg, void *calldata); // 释放
};
```

### 2.4 rpc_pipe_ops — 管道操作

```c
// 文件: include/linux/sunrpc/rpc_pipe_fs.h
struct rpc_pipe_ops {
    ssize_t (*upcall)(struct file *, struct rpc_pipe_msg *, void *calldata); // 上行
    ssize_t (*downcall)(struct file *, struct rpc_pipe_msg *, void *calldata); // 下行
    int (*open_pipe)(struct inode *);     // 打开管道
    int (*release_pipe)(struct inode *);  // 释放管道
    int (*destroy_msg)(struct rpc_pipe_msg *); // 销毁消息
};
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/sunrpc/rpc_pipe_fs.h>

// 创建/删除管道
struct dentry *rpc_mkpipe(const char *name, struct dentry *parent,
                          void *private, const struct rpc_pipe_ops *ops,
                          int flags);
int rpc_unlink(struct dentry *dentry);

// 发送消息
int rpc_pipe_generic_upcall(struct file *filp, struct rpc_pipe_msg *msg,
                            void *calldata);
ssize_t rpc_pipe_generic_downcall(struct file *filp,
                                  struct rpc_pipe_msg *msg, void *calldata);
```

### 3.2 使用示例

```c
// NFS 客户端创建 rpc_pipefs 管道示例
// net/sunrpc/auth_gss/gss_krb5_mech.c

// 定义管道操作
static ssize_t gssp_upcall(struct file *filp, struct rpc_pipe_msg *msg,
                           void *calldata)
{
    // 内核向用户空间 gssd 发送 Kerberos 认证请求
    // 将 msg 数据写入管道，用户空间 gssd 读取
    return 0;
}

static ssize_t gssp_downcall(struct file *filp, struct rpc_pipe_msg *msg,
                             void *calldata)
{
    // 用户空间 gssd 写回认证凭据，内核读取
    // 解析 Kerberos ticket 并缓存
    return 0;
}

static const struct rpc_pipe_ops gssp_pipe_ops = {
    .upcall       = gssp_upcall,
    .downcall     = gssp_downcall,
    .open_pipe    = gssp_open_pipe,
    .release_pipe = gssp_release_pipe,
};

// 创建管道
static int gssp_register(struct dentry *parent)
{
    struct dentry *dentry;

    dentry = rpc_mkpipe("gssd", parent, NULL, &gssp_pipe_ops, 0);
    if (IS_ERR(dentry))
        return PTR_ERR(dentry);

    return 0;
}
```

```bash
# 用户空间操作
# 查看 rpc_pipefs 挂载点
mount | grep rpc_pipefs
# sunrpc on /var/lib/nfs/rpc_pipefs type rpc_pipefs (rw,relatime)

# 查看目录结构
ls -la /var/lib/nfs/rpc_pipefs/
# drwxr-xr-x  5 root root 0 ... .
# drwxr-xr-x 10 root root 0 ...
# drwxr-xr-x  2 root root 0 ... cache/
# drwxr-xr-x  2 root root 0 ... gssd/
# drwxr-xr-x  2 root root 0 ... nfs/
# drwxr-xr-x  2 root root 0 ... nfsd/
```

---

## 4. 函数调用栈

### 4.1 rpc_pipefs 初始化

```
start_kernel()
  → do_initcalls()
    → init_sunrpc()                            // net/sunrpc/sunrpc_syms.c
      → rpc_pipefs_init()                      // net/sunrpc/rpc_pipe.c
        → register_filesystem(&rpc_pipe_fs_type) // 注册 rpc_pipefs
        → kern_mount(&rpc_pipe_fs_type)          // 内核挂载
```

### 4.2 创建管道

```
rpc_mkpipe("gssd", parent, private, &ops, flags)
  → rpc_mkpipe_dentry(parent, name, private, ops)  // 创建 dentry
    → rpc_create_client_dir(parent, name)           // 创建客户端目录
    → __rpc_lookup_create_exclusive()               // 查找/创建 dentry
    → rpc_get_inode(sb, mode)                       // 创建 inode
      → new_inode(sb)
      → inode->i_private = private
      → inode->i_fop = &rpc_pipe_fops               // 设置管道文件操作
      → inode->i_pipe = pipe                        // 关联管道
    → d_instantiate(dentry, inode)                  // 实例化
```

### 4.3 认证请求流程 (上行)

```
NFS 客户端需要 Kerberos 认证
  → gssd_upcall()                                // 发起上行调用
    → rpc_pipe_generic_upcall(filp, msg, calldata) // 通用上行
      → 将 msg 加入 pipe->in_upcall 列表
      → wake_up(&pipe->waitq)                    // 唤醒等待队列
      → 用户空间 rpc.gssd 进程被唤醒
        → rpc_pipe_read()                        // 读取管道数据
          → 从 in_upcall 列表取出 msg
          → copy_to_user(msg->data, ...)          // 拷贝到用户空间
          → rpc.gssd 处理认证请求
          → 获取 Kerberos TGT
```

### 4.4 认证响应流程 (下行)

```
rpc.gssd 写回认证凭据
  → write(fd, kerberos_ticket, len)              // 用户空间写入
    → rpc_pipe_write()                           // 内核接收
      → prep_mess = pipe->prep_mess              // 获取预备消息
      → copy_from_user(prep_mess->data, ...)     // 拷贝数据
      → pipe->ops->downcall(filp, prep_mess, calldata) // 下行回调
        → gssp_downcall()                        // 解析 Kerberos ticket
          → 缓存凭据到内核
          → 唤醒等待的 NFS 挂载进程
```

---

## 5. 流程图

### 5.1 NFS Kerberos 认证流程

```
NFS 客户端挂载远程文件系统 (需要 Kerberos 认证)
    │
    ▼
内核 RPC 层
    │
    ├── 检测到需要 Kerberos 认证
    │
    ├── rpc_pipe_generic_upcall()          # 发送上行请求
    │     │
    │     ├── 创建 upcall 消息
    │     ├── 加入 pipe->in_upcall 列表
    │     └── 唤醒等待队列
    │
    ├── 用户空间 rpc.gssd 读取管道
    │     │
    │     ├── rpc_pipe_read()               # 读取上行请求
    │     ├── 解析请求 (目标服务器、服务主体)
    │     ├── 获取 Kerberos TGT (keytab 或 kinit)
    │     └── 写入响应到管道
    │
    ├── rpc_pipe_generic_downcall()         # 接收下行响应
    │     │
    │     ├── 解析 Kerberos ticket
    │     ├── 缓存凭据到 gssd_cred
    │     └── 唤醒等待的 NFS 挂载进程
    │
    └── NFS 挂载继续
          │
          ▼
    NFS 文件系统挂载完成
```

### 5.2 rpc_pipefs 目录结构

```
/var/lib/nfs/rpc_pipefs/
│
├── cache/                           # 缓存管理
│   ├── auth.unix.gid/               # UNIX GID 映射缓存
│   ├── auth.unix.ip/                # UNIX IP 映射缓存
│   └── ...
│
├── gssd/                            # Kerberos 认证管道
│   ├── clntXX                        # NFS 客户端认证管道
│   │   └── (通过 open/read/write 与 rpc.gssd 通信)
│   └── info                          # 认证信息
│
├── nfs/                             # NFS 客户端信息
│   └── <server>/                     # 每个 NFS 服务器对应的目录
│       ├── rpc                      # RPC 管道
│       └── ...                      # 凭据缓存等
│
├── nfsd/                            # NFS 服务端信息
│   └── ...                          # 通过 rpc_pipefs 暴露 NFS 服务器状态
│
└── portremap/                       # 端口映射
    └── ...
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **NFS Kerberos 认证** | 内核与 rpc.gssd 通信获取 Kerberos 凭据 | `mount -t nfs -o sec=krb5 server:/export /mnt` |
| **NFS ID 映射** | 用户名/UID 与 NFS 服务器的映射 | `rpc.idmapd` 守护进程 |
| **NFS 缓存管理** | 管理认证缓存和映射缓存 | auth.unix.gid, auth.unix.ip 缓存 |
| **NFS 服务器状态** | 暴露 NFS 服务器运行时信息 | nfsd 目录下的管道文件 |
| **SUNRPC 通用通信** | 内核 RPC 层与用户空间的通用通信通道 | 通用 upcall/downcall 机制 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `net/sunrpc/rpc_pipe.c` | rpc_pipefs 核心实现（创建管道、读写操作） |
| `net/sunrpc/auth_gss/gss_krb5_mech.c` | Kerberos 认证机制的 gssd 管道通信 |
| `net/sunrpc/auth_gss/auth_gss.c` | GSS API 认证实现 |
| `net/sunrpc/netns.c` | SUNRPC 网络命名空间初始化 |
| `include/linux/sunrpc/rpc_pipe_fs.h` | 核心数据结构定义和 API 声明 |