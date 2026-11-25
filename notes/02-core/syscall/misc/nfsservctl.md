# nfsservctl 系统调用分析

> 所属分类：其他杂项（已废弃） | [ARM64 系统调用表](../arm64-syscall-table.md#30-其他杂项)

## 1. 概述

`nfsservctl` 是早期 knfsd（内核 NFS 服务器）的控制接口，用于启动服务线程、增删 NFS 客户端、导出/取消导出文件系统、按路径获取文件句柄（供 mountd 使用）。它把命令号映射到 nfsd 文件系统中的特殊文件，再通过读写这些文件完成配置。

**当前状态：该调用在 ARM64 上映射到 `sys_ni_syscall`，永远返回 `-ENOSYS`。**

- 64 位：`include/uapi/asm-generic/unistd.h:126` 定义 `__NR_nfsservctl 42`，`:127` 绑定 `sys_ni_syscall`；
- 32 位兼容：`arch/arm64/tools/syscall_32.tbl:183` 为 `169 common nfsservctl sys_ni_syscall`；
- 原实现 `fs/nfsctl.c` 已于 2011-06-21 由提交 `49b28684fdba "nfsd: Remove deprecated nfsctl system call and related code."` 删除，同时移除 `fs/nfsd/export.c` 中的旧 nfsctl 代码、`fs/compat.c` 的 compat 版本以及 `include/linux/nfsd/syscall.h`。

当前 7.2 源码仍保留 nfsd 文件系统实现（`fs/nfsd/nfsctl.c`），但不再通过系统调用访问；现代用户态通过挂载在 `/proc/fs/nfsd` 的 nfsd 文件系统、`rpc.nfsd` 与 `exportfs` 配置。另外，本树 `.config` 未启用 `CONFIG_NFSD`（内核 NFS 服务器未编译）。

## 2. 函数原型与参数

历史用户空间原型：

```c
#include <linux/nfsd/syscall.h>

int nfsservctl(int cmd, struct nfsctl_arg *arg, void *res);
```

历史内核入口（已删除的 `fs/nfsctl.c`）：

```c
SYSCALL_DEFINE3(nfsservctl, int, cmd, struct nfsctl_arg __user *, arg,
                void __user *, res)
```

| 参数 | 说明 |
|------|------|
| `cmd` | `NFSCTL_SVC`(0)、`ADDCLIENT`(1)、`DELCLIENT`(2)、`EXPORT`(3)、`UNEXPORT`(4)、`GETFD`(7)、`GETFS`(8) |
| `arg` | 指向 `struct nfsctl_arg`，首字段 `ca_version` 必须等于 `NFSCTL_VERSION`(0x0201) |
| `res` | 输出缓冲区，仅 `GETFD`/`GETFS` 使用；可为 `NULL` |

当前系统调用表中的入口：

```c
/* include/uapi/asm-generic/unistd.h */
#define __NR_nfsservctl 42
__SYSCALL(__NR_nfsservctl, sys_ni_syscall)
```

## 3. 内核实现路径

当前（7.2，ARM64）：

```
__arm64_sys_nfsservctl(cmd, arg, res)
  └─ sys_ni_syscall()
       └─ return -ENOSYS;
```

历史实现（`fs/nfsctl.c`，已删除）：

```
__arm64_sys_nfsservctl(cmd, arg, res)
  ├─ copy_from_user(&version, &arg->ca_version, sizeof(int))
  │    version != NFSCTL_VERSION → -EINVAL
  ├─ cmd 越界或 map[cmd].name 为空 → -EINVAL
  ├─ do_open(map[cmd].name, map[cmd].rsize ? O_RDWR : O_WRONLY)
  │    ├─ do_kern_mount("nfsd", 0, "nfsd", NULL)
  │    ├─ file_open_root(mnt->mnt_root, mnt, name, flags)
  │    │    /* 打开 .svc/.add/.del/.export/.unexport/.getfd/.getfs */
  │    └─ mntput(mnt)
  ├─ file->f_op->write(file, &arg->u, map[cmd].wsize, &file->f_pos)
  ├─ 若 map[cmd].rsize != 0:
  │    file->f_op->read(file, res, map[cmd].rsize, &file->f_pos)
  ├─ fput(file)
  └─ 成功返回 0，失败返回负错误码
```

命令到文件的映射表（原 `map[]`）：

| cmd | 值 | nfsd 文件名 | 写入大小 | 读取大小 |
|-----|----|-------------|----------|----------|
| `NFSCTL_SVC` | 0 | `.svc` | `sizeof(struct nfsctl_svc)` | 0 |
| `NFSCTL_ADDCLIENT` | 1 | `.add` | `sizeof(struct nfsctl_client)` | 0 |
| `NFSCTL_DELCLIENT` | 2 | `.del` | `sizeof(struct nfsctl_client)` | 0 |
| `NFSCTL_EXPORT` | 3 | `.export` | `sizeof(struct nfsctl_export)` | 0 |
| `NFSCTL_UNEXPORT` | 4 | `.unexport` | `sizeof(struct nfsctl_export)` | 0 |
| `NFSCTL_GETFD` | 7 | `.getfd` | `sizeof(struct nfsctl_fdparm)` | `NFS_FHSIZE` |
| `NFSCTL_GETFS` | 8 | `.getfs` | `sizeof(struct nfsctl_fsparm)` | `sizeof(struct knfsd_fh)` |

兼容 32 位 ABI 的代码位于已删除的 `fs/compat.c`，负责转换指针宽度、`__kernel_old_dev_t`、`__kernel_ino_t` 等字段。

## 4. 核心数据结构

历史结构（均已随文件删除）：

```c
// include/linux/nfsd/syscall.h
#define NFSCTL_VERSION  0x0201

struct nfsctl_svc {
    unsigned short svc_port;
    int            svc_nthreads;
};

struct nfsctl_client {
    char           cl_ident[NFSCLNT_IDMAX + 1];
    int            cl_naddr;
    struct in_addr cl_addrlist[NFSCLNT_ADDRMAX];
    int            cl_fhkeytype;
    int            cl_fhkeylen;
    unsigned char  cl_fhkey[NFSCLNT_KEYMAX];
};

struct nfsctl_export {
    char             ex_client[NFSCLNT_IDMAX + 1];
    char             ex_path[NFS_MAXPATHLEN + 1];
    __kernel_old_dev_t ex_dev;
    __kernel_ino_t   ex_ino;
    int              ex_flags;
    __kernel_uid_t   ex_anon_uid;
    __kernel_gid_t   ex_anon_gid;
};

struct nfsctl_fdparm { struct sockaddr gd_addr; char gd_path[NFS_MAXPATHLEN + 1]; int gd_version; };
struct nfsctl_fsparm { struct sockaddr gd_addr; char gd_path[NFS_MAXPATHLEN + 1]; int gd_maxlen; };
```

`struct nfsctl_arg` 首字段是 `int ca_version`，其后是上述结构的 union `u`。`struct knfsd_fh` 是内核 NFS 文件句柄表示，`GETFS` 返回可变长度句柄；`GETFD` 返回固定长度 `NFS_FHSIZE` 的旧式句柄。

现代对应实现位于 `fs/nfsd/nfsctl.c`：注册 `nfsd_fs_type`，在 `/proc/fs/nfsd` 下提供 `threads`、`versions`、`portlist`、`exports`、`filehandle` 等文件，各自有 write/read 处理函数，取代了按命令号硬编码的 `map[]`。

## 5. 关键流程

```
【历史】rpc.nfsd / exportfs / mountd
        ├─ 构造 nfsctl_arg，设置 ca_version = NFSCTL_VERSION
        ├─ nfsservctl(NFSCTL_SVC, &arg, NULL)      → 启动服务线程
        ├─ nfsservctl(NFSCTL_EXPORT, &arg, NULL)   → 导出文件系统
        └─ nfsservctl(NFSCTL_GETFD, &arg, fh)      → mountd 取文件句柄
        │
        ▼
sys_nfsservctl
  ├─ 版本与命令号校验
  ├─ 挂载 nfsd 文件系统并打开映射文件
  ├─ 写 arg->u（内核文件操作执行实际动作）
  ├─ 必要时读回响应（GETFD/GETFS）
  └─ 返回 0

【当前】
nfsservctl(...) → sys_ni_syscall() → -ENOSYS
```

现代等价操作：

```sh
mount -t nfsd nfsd /proc/fs/nfsd
echo 8 > /proc/fs/nfsd/threads          # 对应 NFSCTL_SVC 的线程数
exportfs -o rw,sync,no_subtree_check 192.168.1.0/24:/srv/nfs
exportfs -u 192.168.1.0/24:/srv/nfs
cat /proc/fs/nfsd/exports
```

## 6. 返回值与错误码

当前实现：

| 返回值 | 说明 |
|--------|------|
| `-ENOSYS` | 恒定返回，功能不存在 |

历史错误码：

| 错误码 | 触发条件 |
|--------|----------|
| `-EFAULT` | 读取 `arg->ca_version` 或写入参数/读回结果失败 |
| `-EINVAL` | `ca_version != NFSCTL_VERSION`；`cmd` 越界或未实现 |
| 其他负值 | `do_kern_mount("nfsd", ...)` / `file_open_root()` 的 `PTR_ERR()`，或 nfsd 文件回调错误 |

## 7. 使用示例

当前内核上直接调用必然失败：

```c
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    /* 历史命令号 NFSCTL_SVC = 0，参数在当前内核不会被解释 */
    long ret = syscall(__NR_nfsservctl, 0, NULL, NULL);
    if (ret < 0)
        printf("nfsservctl: %s (errno=%d)\n", strerror(errno), errno);
    return 0;
}
```

在 7.2 ARM64 上输出 `Function not implemented`（`ENOSYS`）。若需要以编程方式管理 nfsd，使用 `nfs-utils` 工具或直接读写 `/proc/fs/nfsd/*`；注意本树 `.config` 未启用 `CONFIG_NFSD`，未启用时上述路径不存在。

## 8. 涉及文件

| 路径 | 作用 |
|------|------|
| `include/uapi/asm-generic/unistd.h` | `__NR_nfsservctl 42`，绑定 `sys_ni_syscall`（126-127 行） |
| `arch/arm64/include/generated/uapi/asm/unistd_64.h` | 64 位编号 42 |
| `arch/arm64/tools/syscall_32.tbl` | 32 位兼容编号 169 → `sys_ni_syscall` |
| `kernel/sys_ni.c` | `sys_ni_syscall()` 返回 `-ENOSYS` |
| 历史文件（已删除）`fs/nfsctl.c` | `SYSCALL_DEFINE3(nfsservctl)`、`do_open()`、`map[]` |
| `fs/nfsd/nfsctl.c` | 当前 nfsd 文件系统实现（`/proc/fs/nfsd`），需 `CONFIG_NFSD` |
