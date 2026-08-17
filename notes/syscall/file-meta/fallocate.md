# fallocate 系统调用

## 原理与功能

`fallocate` 系统调用对文件执行物理空间分配或释放操作，允许用户直接控制文件占用的磁盘块。与 `truncate` 不同，`fallocate` 可以精确控制文件范围内的空间分配。

### 功能说明

- **预分配空间**：为文件分配物理块，确保后续写入不会失败
- **释放空间**：释放文件指定范围内的物理块（打洞，创建稀疏文件）
- **收缩空间**：截断文件指定范围（`FALLOC_FL_COLLAPSE_RANGE`）
- **零填充**：用零填充指定范围（`FALLOC_FL_ZERO_RANGE`）
- **插入空间**：在文件中间插入零填充空间（`FALLOC_FL_INSERT_RANGE`）

## 使用场景

- 数据库预分配数据文件（如 PostgreSQL、MySQL）
- P2P 下载工具预分配空间（避免碎片）
- 虚拟机磁盘镜像预分配
- 稀疏文件管理（打洞释放空间）
- 视频编辑中的空间插入/删除操作

## API 及使用案例

### 函数原型

```c
#include <fcntl.h>
#include <linux/falloc.h>

int fallocate(int fd, int mode, off_t offset, off_t len);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `fd` | `int` | 已打开的文件描述符 |
| `mode` | `int` | 操作模式（默认 0 为预分配） |
| `offset` | `off_t` | 起始偏移量 |
| `len` | `off_t` | 操作长度 |

### 操作模式

| 模式 | 值 | 说明 |
|------|-----|------|
| `FALLOC_FL_KEEP_SIZE` | 0x01 | 预分配但不改变文件大小 |
| `FALLOC_FL_PUNCH_HOLE` | 0x02 | 打洞释放空间（需与 KEEP_SIZE 合用） |
| `FALLOC_FL_NO_HIDE_STALE` | 0x04 | 不隐藏过时数据 |
| `FALLOC_FL_COLLAPSE_RANGE` | 0x08 | 收缩范围（删除数据） |
| `FALLOC_FL_ZERO_RANGE` | 0x10 | 用零填充范围 |
| `FALLOC_FL_INSERT_RANGE` | 0x20 | 插入空间 |
| `FALLOC_FL_UNSHARE_RANGE` | 0x40 | 取消共享范围 |

### 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <unistd.h>

int main() {
    int fd = open("datafile.bin", O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    // 预分配 1GB 空间（但不改变文件大小）
    // 后续写入时不会因为磁盘空间不足而失败
    if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, 1ULL*1024*1024*1024) == -1) {
        perror("fallocate");
        close(fd);
        return 1;
    }
    printf("已预分配 1GB 空间（文件大小不变）\n");

    // 扩展文件大小并分配空间（不使用 KEEP_SIZE）
    if (fallocate(fd, 0, 0, 2ULL*1024*1024*1024) == -1) {
        perror("fallocate extend");
        close(fd);
        return 1;
    }
    printf("已扩展并分配 2GB 空间\n");

    // 打洞：释放 0~1GB 范围的空间
    if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                  0, 1ULL*1024*1024*1024) == -1) {
        perror("fallocate punch hole");
        close(fd);
        return 1;
    }
    printf("已释放 0~1GB 范围的空间（稀疏文件）\n");

    close(fd);
    return 0;
}
```

## 执行流程

```
用户进程                          内核
    |                               |
    | fallocate(fd, mode,           |
    |   offset, len)                |
    |-----> syscall(#47) ---------->|
    |       __arm64_sys_fallocate() |
    |                               |
    |    +----------------------+   |
    |    | do_fallocate()       |   |
    |    | fs/open.c            |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | fdget(fd)            |   |
    |    | 获取 struct file     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | 检查操作权限:        |   |
    |    | 写模式 + 文件系统    |   |
    |    | 是否支持 fallocate   |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | file->f_op->fallocate()|  |
    |    | 文件系统特定实现     |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | ext4_fallocate()     |   |
    |    | ext4 实现            |   |
    |    +---------+------------+   |
    |              |                |
    |    +---------v------------+   |
    |    | ext4_alloc_file_blocks|  |
    |    | 物理块分配           |   |
    |    +---------+------------+   |
    |              |                |
    |<---- 返回 0 ----------------+ |
    |                               |
```

## 函数调用栈

```
fallocate(fd, mode, offset, len)
  └─ syscall(__NR_fallocate, fd, mode, offset, len)
       └─ __arm64_sys_fallocate()                  // arch/arm64/kernel/syscall.c
            └─ do_fallocate(fd, mode, offset, len)  // fs/open.c
                 ├─ fdget(fd)                      // 获取 struct file
                 ├─ 权限检查
                 └─ file->f_op->fallocate(file, mode, offset, len)
                      └─ ext4_fallocate()           // 以 ext4 为例
                           ├─ ext4_alloc_file_blocks()  // 预分配
                           │    └─ ext4_map_blocks()    // 块映射
                           └─ ext4_truncate()          // 打洞时释放块
```

## 关键数据结构

### 文件操作结构体

```c
// include/linux/fs.h
struct file_operations {
    // ...
    long (*fallocate)(struct file *file, int mode,
                      loff_t offset, loff_t len);
    // ...
};
```

### ext4 文件系统块分配

```c
// fs/ext4/ext4.h
struct ext4_map_blocks {
    ext4_fsblk_t   m_pblk;       // 物理块号
    ext4_lblk_t    m_lblk;       // 逻辑块号
    unsigned int   m_len;        // 块数量
    unsigned int   m_flags;      // 映射标志
    // ...
};
```

## 备注

- ARM64 系统调用号为 #47
- 需要文件以写模式打开
- 不是所有文件系统都支持 `fallocate`（ext4/xfs/btrfs 支持，tmpfs 部分支持）
- `FALLOC_FL_PUNCH_HOLE` 需要与 `FALLOC_FL_KEEP_SIZE` 一起使用
- 预分配与 `truncate` 不同：预分配保证物理空间，`truncate` 只是改变 inode 大小