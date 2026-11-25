# securityfs — 安全模块文件系统

## 1. 概述与实现机制

securityfs 为 LSM（Linux Security Module）提供文件系统接口，用于暴露安全策略、属性文件和统计信息。挂载在 `/sys/kernel/security/`。

### 核心特性

- **LSM 接口**：为 SELinux、AppArmor、IMA 等安全模块提供文件系统接口
- **简单 API**：基于 simple_fs 框架，API 与 debugfs 类似
- **策略管理**：加载/卸载安全策略，查看策略状态
- **运行时配置**：用户空间安全管理工具通过 securityfs 配置内核安全模块

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  load_policy /etc/selinux/targeted/policy/policy.31         │
│  cat /sys/kernel/security/selinux/status                    │
│  cat /sys/kernel/security/apparmor/profiles                │
└────────────────────────┬─────────────────────────────────────┘
                         │ VFS 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│               securityfs (security/inode.c)                  │
│  securityfs_create_file() / securityfs_create_dir()          │
│  securityfs_remove()                                        │
│  基于 simple_fs 框架 (类似 debugfs)                          │
└────────────────────────┬─────────────────────────────────────┘
                         │
                         ▼
┌──────────────────────────────────────────────────────────────┐
│               LSM 模块创建各自的文件                          │
│  SELinux:     security/selinux/selinuxfs.c                   │
│  AppArmor:    security/apparmor/apparmorfs.c                 │
│  IMA:         security/integrity/ima/ima_fs.c                │
│  TOMOYO:      security/tomoyo/securityfs_if.c                │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

securityfs 基于 simple_fs，数据结构与 debugfs 类似，由 dentry 和 inode 管理。

```c
// 文件: security/inode.c
// securityfs 使用 simple_fs 框架，无自定义数据结构
// 文件系统类型定义
static struct file_system_type securityfs_fs_type = {
    .name = "securityfs",
    .init_fs_context = securityfs_init_fs_context,
    .kill_sb = securityfs_kill_super,
};

// securityfs 的 super_operations
static const struct super_operations securityfs_super_operations = {
    .statfs = simple_statfs,
    .drop_inode = generic_delete_inode,
    .evict_inode = evict_inode,
};
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/security.h>

// 创建文件
struct dentry *securityfs_create_file(const char *name, umode_t mode,
                                      struct dentry *parent, void *data,
                                      const struct file_operations *fops);

// 创建目录
struct dentry *securityfs_create_dir(const char *name, struct dentry *parent);

// 删除文件/目录
void securityfs_remove(struct dentry *dentry);
```

### 3.2 使用示例

```c
// 示例: LSM 模块创建 securityfs 文件

// SELinux 创建文件
// security/selinux/selinuxfs.c
static struct dentry *selinux_dir;  // /sys/kernel/security/selinux/

static int __init selinux_fs_init(void)
{
    struct dentry *dentry;

    // 创建 selinux 目录
    selinux_dir = securityfs_create_dir("selinux", NULL);
    if (IS_ERR(selinux_dir))
        return PTR_ERR(selinux_dir);

    // 创建 selinux 状态文件
    dentry = securityfs_create_file("status", 0444, selinux_dir,
                                    NULL, &sel_status_ops);
    if (IS_ERR(dentry))
        goto err;

    // 创建策略加载文件
    dentry = securityfs_create_file("load", 0200, selinux_dir,
                                    NULL, &sel_load_ops);
    if (IS_ERR(dentry))
        goto err;

    // 创建 AVC 统计文件
    dentry = securityfs_create_file("avc", 0444, selinux_dir,
                                    NULL, &sel_avc_ops);
    // ... 更多文件

    return 0;
err:
    securityfs_remove(selinux_dir);
    return -ENOMEM;
}

// 读取 status 的回调
static int sel_status_show(struct seq_file *m, void *v)
{
    struct selinux_state *state = &selinux_state;
    int enforcing = selinux_enabled_bool(state);

    seq_printf(m, "enabled: %d\n", enforcing);
    seq_printf(m, "enforcing: %d\n", enforcing);
    seq_printf(m, "checkreqprot: %d\n", selinux_checkreqprot_bool(state));
    return 0;
}

static int sel_status_open(struct inode *inode, struct file *file)
{
    return single_open(file, sel_status_show, NULL);
}

static const struct file_operations sel_status_ops = {
    .open    = sel_status_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};
```

```c
// 示例: IMA 模块创建文件
// security/integrity/ima/ima_fs.c
static struct dentry *ima_dir;

static int __init ima_fs_init(void)
{
    ima_dir = securityfs_create_dir("ima", NULL);
    if (IS_ERR(ima_dir))
        return PTR_ERR(ima_dir);

    // 创建策略文件
    securityfs_create_file("policy", 0644, ima_dir, NULL, &ima_measure_policy_ops);
    // 创建运行时度量列表
    securityfs_create_file("binary_runtime_measurements", 0444, ima_dir,
                           NULL, &ima_measurements_ops);
    // 创建 ASCII 格式度量列表
    securityfs_create_file("ascii_runtime_measurements", 0444, ima_dir,
                           NULL, &ima_ascii_measurements_ops);
    return 0;
}
```

---

## 4. 函数调用栈

### 4.1 securityfs 初始化

```
start_kernel()
  → security_init()
    → populate_securityfs()                     // security/inode.c
      → securityfs_create_dir("selinux", NULL)   // 创建 selinux 目录
      → securityfs_create_dir("apparmor", NULL)  // 创建 apparmor 目录
      → securityfs_create_dir("ima", NULL)       // 创建 ima 目录
      → ...
```

### 4.2 securityfs 创建文件

```
securityfs_create_file("status", 0444, parent, NULL, &ops)
  → securityfs_create_dentry(name, mode, parent, data, fops, NULL) // 核心
    → simple_pin_fs(&securityfs_fs_type, &mount, &mount_count)  // 固定文件系统
    → lookup_one_len(name, parent)              // 查找 dentry
    → securityfs_fill_super() 或 已有超级块
    → inode = securityfs_get_inode(sb, mode, ...)  // 创建 inode
      → new_inode(sb)
      → inode->i_mode = mode
      → inode->i_fop = fops
      → inode->i_private = data
    → d_make_persistent(dentry, inode)          // 关联 dentry 和 inode
    → d_instantiate(dentry, inode)              // 实例化
    → dput(dentry)
```

### 4.3 读取 securityfs 文件

```
cat /sys/kernel/security/selinux/status
  ↓ sys_read() → vfs_read() → file->f_op->read()
    → seq_read()                               // 使用 seq_file 接口
      → sel_status_open() → single_open()      // 设置 seq_file
      → sel_status_show()                      // 生成内容
        → seq_printf(m, "enabled: %d\n", state->enabled)
        → seq_printf(m, "enforcing: %d\n", state->enforcing)
    → copy_to_user(buf, seq_buf, count)        // 拷贝到用户空间
```

---

## 5. 流程图

### 5.1 securityfs 目录结构

```
/sys/kernel/security/
│
├── selinux/                        # SELinux 安全模块
│   ├── access                      # 访问向量缓存
│   ├── avc                         # AVC 统计信息
│   ├── booleans/                   # 布尔策略开关
│   │   ├── allow_ptrace
│   │   ├── secure_mode_insmod
│   │   └── ...
│   ├── checkreqprot                # 检查请求保护位
│   ├── commit_pending_bools        # 提交待定布尔值
│   ├── context                     # 当前安全上下文
│   ├── disable                     # 禁用 SELinux
│   ├── enforce                     # 强制模式 (enforcing/permissive)
│   ├── load                        # 加载安全策略
│   ├── policy                      # 当前策略文件
│   ├── policyvers                  # 策略版本
│   ├── relabel                     # 重新标记
│   ├── status                      # 状态信息
│   └── validatetrans              # 验证转换
│
├── apparmor/                       # AppArmor 安全模块
│   ├── .access                     # 访问控制
│   ├── features                    # 支持的特性
│   ├── profiles/                   # 安全配置文件
│   ├── raw_release                 # 原始释放
│   ├── raw_remove                  # 原始移除
│   ├── raw_data                    # 原始数据
│   ├── replicas                    # 副本数
│   └── tasks/                      # 进程与 profile 映射
│
├── ima/                            # IMA (完整性度量架构)
│   ├── binary_runtime_measurements # 运行时度量列表 (二进制)
│   ├── ascii_runtime_measurements  # 运行时度量列表 (ASCII)
│   ├── policy                      # IMA 策略
│   ├── violations                  # 违规计数
│   └── ...
│
├── evm/                            # EVM (扩展验证模块)
│   └── ...
│
└── tomoyo/                         # TOMOYO Linux
    └── ...
```

---

## 6. 使用场景

| 场景 | 模块 | 描述 |
|------|------|------|
| **安全策略管理** | SELinux/AppArmor | 加载/卸载安全策略、查看策略状态 |
| **强制模式切换** | SELinux | 在 enforcing/permissive 模式间切换 |
| **访问向量缓存** | SELinux/AVC | 查看 AVC 统计，调试权限拒绝 |
| **完整性度量** | IMA | 查看文件完整性度量列表 |
| **策略布尔值** | SELinux | 运行时调整布尔策略开关 |
| **安全配置** | AppArmor | 加载/移除安全配置文件 |
| **内核安全状态** | 所有 LSM | 查看安全模块的启用/状态信息 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `security/inode.c` | securityfs 核心实现（创建文件/目录、删除） |
| `security/selinux/selinuxfs.c` | SELinux 的 securityfs 文件实现 |
| `security/apparmor/apparmorfs.c` | AppArmor 的 securityfs 文件实现 |
| `security/integrity/ima/ima_fs.c` | IMA 的 securityfs 文件实现 |
| `security/tomoyo/securityfs_if.c` | TOMOYO 的 securityfs 接口实现 |
| `include/linux/security.h` | securityfs API 声明 |