# devpts — 伪终端文件系统

## 1. 概述与实现机制

devpts 为伪终端（PTY）提供文件系统接口，管理 `/dev/pts/` 目录下的伪终端从设备文件。每个伪终端从设备对应一个文件（如 `/dev/pts/0`），提供终端模拟功能。

### 核心概念

- **PTY 架构**：每个伪终端对包含一个 master（主设备）和一个 slave（从设备）
- **master 端**（`/dev/ptmx`）：由终端模拟器（如 xterm、gnome-terminal）打开
- **slave 端**（`/dev/pts/N`）：由 shell 或应用程序作为标准终端访问
- **多路复用**：`/dev/ptmx` 是共享主设备，打开时自动分配新的 slave 编号

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  xterm:  open("/dev/ptmx") → master fd (pts0)              │
│  xterm:  grantpt(), unlockpt(), ptsname() → "/dev/pts/0"   │
│  xterm:  fork() + exec() → shell 继承 slave fd             │
│  shell:  读写 /dev/pts/0 作为标准终端                        │
└────────────────────────┬─────────────────────────────────────┘
                         │ 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│              devpts 文件系统 (fs/devpts/inode.c)             │
│  devpts_fill_super() → 初始化超级块                         │
│  devpts_new_index() → 分配新的 PTY 编号                     │
│  devpts_pty_new() → 创建新的 PTY 从设备文件                 │
│  devpts_get_priv() → 获取 PTY 私有数据                      │
│  devpts_kill_index() → 释放 PTY 编号                        │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│          PTY 核心 (drivers/tty/pty.c)                       │
│  pty_open() → 打开 PTY 设备                                │
│  pty_write() → 写入 PTY (master→slave 或 slave→master)     │
│  pty_read() → 读取 PTY (master→slave 或 slave→master)      │
│  pty_close() → 关闭 PTY 设备                                │
│  tty_driver 注册 → 注册 PTY 驱动                           │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 pts_fs_info — devpts 文件系统信息

```c
// 文件: fs/devpts/inode.c
struct pts_fs_info {
    struct ida allocated_ptys;       // PTY 编号分配器
    struct pts_mount_opts mount_opts; // 挂载选项
    struct super_block *sb;          // 关联的超级块
    struct dentry *ptmx_dentry;      // ptmx 的 dentry
};
```

### 2.2 pts_mount_opts — 挂载选项

```c
// 文件: fs/devpts/inode.c
struct pts_mount_opts {
    umode_t mode;                    // 默认权限
    kuid_t uid;                      // 默认用户 ID
    kgid_t gid;                      // 默认组 ID
    int pte_mode;                    // ptmx 模式
    int newinstance;                 // 新实例标志
    int max;                         // 最大 PTY 数
};
```

### 2.3 devpts_private — PTY 私有数据

```c
// 文件: include/linux/devpts.h
struct devpts_private {
    struct tty_struct *tty;          // 关联的 tty 结构
    int pty_number;                   // PTY 编号
};
```

### 2.4 PTY 驱动

```c
// 文件: drivers/tty/pty.c
// master PTY 驱动
static const struct tty_operations pty_ops_bsd = {
    .open = pty_open,
    .close = pty_close,
    .write = pty_write,
    .write_room = pty_write_room,
    .chars_in_buffer = pty_chars_in_buffer,
    .flush_buffer = pty_flush_buffer,
    .ioctl = pty_ioctl,
    .hangup = pty_hangup,
};

// 从设备 PTY 操作
static const struct tty_operations pty_unix98_ops = {
    .open = pty_open,
    .close = pty_close,
    .write = pty_write,
    .write_room = pty_write_room,
    .chars_in_buffer = pty_chars_in_buffer,
    .flush_buffer = pty_flush_buffer,
    .ioctl = pty_ioctl,
    .hangup = pty_hangup,
};
```

---

## 3. API 与使用方法

### 3.1 用户空间 API

```c
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

// 打开伪终端 master
int master_fd = open("/dev/ptmx", O_RDWR);

// 获取 slave 名称
char *slave_name = ptsname(master_fd);  // 得到 "/dev/pts/N"

// 设置权限
grantpt(master_fd);   // 设置 slave 文件权限为 0620

// 解锁 slave
unlockpt(master_fd);  // 解锁 slave，允许打开

// 打开 slave
int slave_fd = open(slave_name, O_RDWR);

// 分配终端
int slave_fd = posix_openpt(O_RDWR);  // 等同于 open("/dev/ptmx", O_RDWR)
```

### 3.2 终端模拟器示例

```c
// 伪终端使用示例 (终端模拟器核心逻辑)
int pty_fork(int *master_fd, char *slave_name, struct termios *slave_termios)
{
    int mfd, sfd;
    pid_t pid;

    // 1. 打开 master
    mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (mfd < 0)
        return -1;

    // 2. 获取 slave 名称
    if (grantpt(mfd) || unlockpt(mfd))
        goto err;

    if (ptsname_r(mfd, slave_name, 256))
        goto err;

    // 3. fork
    pid = fork();
    if (pid == -1)
        goto err;

    if (pid == 0) {
        // 子进程 (shell)
        // 关闭 master
        close(mfd);

        // 创建新的会话
        setsid();

        // 打开 slave
        sfd = open(slave_name, O_RDWR);
        if (sfd < 0)
            exit(1);

        // 设置终端属性
        if (slave_termios)
            tcsetattr(sfd, TCSANOW, slave_termios);

        // 复制 slave fd 到 stdin/stdout/stderr
        dup2(sfd, 0);
        dup2(sfd, 1);
        dup2(sfd, 2);

        if (sfd > 2)
            close(sfd);

        // 执行 shell
        execl("/bin/bash", "bash", NULL);
        exit(1);
    }

    // 父进程 (终端模拟器)
    *master_fd = mfd;
    return pid;

err:
    close(mfd);
    return -1;
}
```

### 3.3 内核内部 API

```c
#include <linux/devpts.h>

// 分配 PTY 编号
int devpts_new_index(struct pts_fs_info *fsi);

// 释放 PTY 编号
void devpts_kill_index(struct pts_fs_info *fsi, int idx);

// 创建 PTY 从设备文件
int devpts_pty_new(struct pts_fs_info *fsi, struct tty_struct *tty);

// 获取 PTY 私有数据
struct devpts_private *devpts_get_priv(struct dentry *dentry);

// 删除 PTY 从设备文件
void devpts_pty_kill(struct tty_struct *tty);
```

---

## 4. 函数调用栈

### 4.1 打开 PTY 从设备

```
open("/dev/pts/0", O_RDWR)
  → tty_open()                                    // drivers/tty/tty_io.c
    → tty_open_by_driver(device, inode, filp)     // 查找 tty 驱动
      → devpts_get_priv(inode)                     // 获取 PTY 私有数据
      → priv->tty                                  // 拿到关联的 tty_struct
      → tty = tty_init_dev(driver, index)           // 初始化 tty 设备
        → tty_alloc_file(filp)                     // 分配 tty 文件
        → tty_add_file(tty, filp)                  // 关联 tty 和文件
        → tty->ops->open(tty, filp)                // 调用 pty_open
          → pty_open(tty, filp)                    // drivers/tty/pty.c
            → tty->link->count++                   // 增加 master 计数
            → 建立 master/slave 连接
      → tty_driver_lookup_tty(driver, inode, index) // 查找/创建 tty
```

### 4.2 创建 PTY (pty_unix98_install)

```
open("/dev/ptmx", O_RDWR)
  → ptmx_open(inode, filp)                         // drivers/tty/pty.c
    → devpts_new_index(fsi)                        // 分配 PTY 编号
      → ida_alloc_range(&fsi->allocated_ptys, 0, fsi->mount_opts.max, ...)
    → tty = alloc_tty_struct(driver, index)        // 分配 tty 结构
    → tty->driver_data = NULL
    → tty_add_file(tty, filp)                      // 关联 tty 和文件
    → tty->ops->open(tty, filp)                    // 打开 master
      → pty_open(tty, filp)
    → ptm_driver_install(tty, driver, index)        // 安装 master
    → devpts_pty_new(fsi, tty)                     // 创建 slave 设备文件
      → dentry = lookup_one_len(name, ...)          // 查找 /dev/pts/N
      → inode = new_inode(sb)                      // 创建 inode
      → inode->i_private = priv = kzalloc(...)     // 分配私有数据
      → priv->tty = tty                            // 关联 tty
      → priv->pty_number = index                   // 设置 PTY 编号
      → d_instantiate(dentry, inode)               // 实例化
    → tty_driver_install_tty(driver, tty)          // 注册 tty
```

### 4.3 数据流 (键盘输入 → shell)

```
用户键盘输入 → 终端模拟器 (xterm)
    │
    ├── xterm 读取键盘输入
    ├── xterm write(master_fd, data, len)          # 写入 master 端
    │     → tty_write()
    │       → pty_write(tty, buf, count)
    │         → tty_insert_flip_char(tty->link, c, TTY_NORMAL)
    │         → tty_flip_buffer_push(tty->link)     # 推送到 slave 的 flip buffer
    │
    ├── shell 在 slave 端读取
    │     → read(slave_fd, buf, len)
    │       → tty_read()
    │         → n_tty_read()                        # N_TTY 线路规程
    │           → 从 flip buffer 读取数据
    │           → 回显处理 (ECHO 等)
    │           → copy_to_user(buf, data, len)
    │
    └── shell 处理输入 (显示字符、执行命令等)
```

---

## 5. 流程图

### 5.1 PTY 创建流程

```
终端模拟器 (xterm, gnome-terminal, tmux, sshd)
    │
    ├── open("/dev/ptmx", O_RDWR)
    │     → ptmx_open()
    │       → devpts_new_index()         # 分配 PTY 编号 (如 0)
    │       → alloc_tty_struct()         # 分配 tty 结构
    │       → pty_open()                 # 打开 master
    │       → devpts_pty_new()           # 创建 /dev/pts/0
    │       → 返回 master_fd
    │
    ├── grantpt(master_fd)               # 设置权限
    ├── unlockpt(master_fd)              # 解锁 slave
    │
    ├── fork()
    │     │
    │     ├── 子进程:
    │     │     ├── close(master_fd)
    │     │     ├── setsid()             # 创建新会话
    │     │     ├── open("/dev/pts/0")   # 打开 slave
    │     │     ├── dup2(slave_fd, 0/1/2) # 绑定到 stdin/stdout/stderr
    │     │     └── execve("/bin/bash")  # 执行 shell
    │     │
    │     └── 父进程 (终端模拟器):
    │           └── 使用 master_fd 与 shell 通信
    │
    └── 通信循环:
          xterm ←→ master_fd ←→ pty 驱动 ←→ slave_fd ←→ shell
```

### 5.2 PTY 数据流

```
┌─────────────────────────────────────────────────────────────────────┐
│                      PTY 通信模型                                    │
│                                                                     │
│  用户输入                                                 终端输出  │
│     │                                                       ▲       │
│     ▼                                                       │       │
│  ┌─────────────────┐     ┌──────────────┐     ┌─────────────────┐  │
│  │  xterm (用户态)  │     │  PTY 驱动     │     │  bash (用户态)   │  │
│  │                  │     │  (内核态)      │     │                  │  │
│  │  master_fd       │────▶│  pty_write()  │────▶│  stdin (slave)   │  │
│  │  write(data)     │     │  → flip       │     │  read()          │  │
│  │                  │     │  → n_tty      │     │                  │  │
│  │  master_fd       │◀────│  pty_read()   │◀────│  stdout (slave)  │  │
│  │  read(data)      │     │  ← flip       │     │  write()         │  │
│  └─────────────────┘     └──────────────┘     └─────────────────┘  │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ 线路规程 (n_tty):                                            │    │
│  │  - 输入处理: 规范模式 (行缓冲、信号生成)                     │    │
│  │  - 输出处理: 换行转换、制表符扩展                           │    │
│  │  - 回显处理: 将输入回显到输出                               │    │
│  └─────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.3 devpts 目录结构

```
/dev/pts/                          # devpts 挂载点
│
├── 0                              # PTY 编号 0 (第一个伪终端)
├── 1                              # PTY 编号 1
├── 2                              # PTY 编号 2
├── ...
├── ptmx                           # ptmx 设备节点 (多路复用主设备)
│
└── (每个文件对应一个 PTY 从设备)
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **终端模拟器** | xterm、gnome-terminal 等终端模拟器 | 每个终端窗口分配一个 PTY |
| **SSH 服务器** | 远程登录会话管理 | sshd 为每个 SSH 连接创建 PTY |
| **tmux/screen** | 终端复用器 | 多路复用终端会话 |
| **串口模拟** | 终端模拟为串口设备 | 串口通信软件 |
| **容器/虚拟化** | 容器内的终端访问 | `docker exec -it` 分配 PTY |
| **expect/脚本** | 自动化交互式程序 | `expect` 自动控制 PTY 输入输出 |
| **telnet 服务器** | 远程登录 | telnetd 创建 PTY 服务登录会话 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `fs/devpts/inode.c` | devpts 文件系统核心（超级块、挂载、PTY 编号管理） |
| `drivers/tty/pty.c` | PTY 驱动实现（master/slave 打开、读写、关闭） |
| `drivers/tty/tty_io.c` | TTY 核心（tty_open、tty_init_dev 等） |
| `drivers/tty/n_tty.c` | N_TTY 线路规程（规范模式、行缓冲、信号生成） |
| `include/linux/devpts.h` | devpts 头文件（私用数据和 API 声明） |
| `include/linux/tty.h` | TTY 核心数据结构定义 |