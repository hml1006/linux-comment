# configfs — 配置对象文件系统

## 1. 概述与实现机制

configfs 是一个基于对象的配置文件系统，用户空间通过 `mkdir/rmdir` 创建/删除内核对象，通过写入属性文件来配置对象。与 sysfs 不同，sysfs 暴露已有对象，**configfs 创建新对象**。

### 核心概念

- **用户空间 `mkdir`** → 内核创建配置对象（如 iSCSI target）
- **用户空间写入属性文件** → 修改对象配置
- **用户空间 `rmdir`** → 内核销毁对象

### 与 sysfs 的区别

| 特性 | sysfs | configfs |
|------|-------|----------|
| 对象生命周期 | 内核驱动控制 | 用户空间通过 `mkdir/rmdir` 控制 |
| 创建方式 | 内核创建 | 用户 `mkdir` 触发内核创建 |
| 典型用途 | 暴露设备、驱动信息 | 配置 iSCSI target、NFS 导出等 |
| 挂载点 | `/sys/` | `/sys/kernel/config/` |

### 实现架构

```
┌──────────────────────────────────────────────────────────────┐
│                     用户空间                                  │
│  mkdir /sys/kernel/config/target/iscsi/iqn.2023-.../tpgt_1  │
│  echo "0" > .../tpgt_1/attrib/...                             │
│  rmdir .../tpgt_1                                              │
└────────────────────────┬─────────────────────────────────────┘
                         │ VFS 系统调用
                         ▼
┌──────────────────────────────────────────────────────────────┐
│                configfs 层 (fs/configfs/)                    │
│  configfs_mkdir() → config_item/mkdir 回调                   │
│  configfs_rmdir() → config_item/rmdir 回调                   │
│  configfs_create_file() → 属性文件创建                       │
│  config_item / config_group 对象模型                         │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 config_item — 配置项

```c
// 文件: include/linux/configfs.h
struct config_item {
    char                    *ci_name;       // 项名称
    char                    ci_namebuf[CONFIGFS_ITEM_NAME_LEN]; // 名称缓冲区
    struct kref             ci_kref;        // 引用计数
    struct list_head        ci_entry;       // 链表节点
    struct config_item      *ci_parent;     // 父项
    struct config_group     *ci_group;      // 所属组
    const struct config_item_type *ci_type; // 类型定义
    struct dentry           *ci_dentry;     // 对应的 dentry
    bool                    ci_is_visible;  // 是否可见
};
```

### 2.2 config_group — 配置组

```c
// 文件: include/linux/configfs.h
struct config_group {
    struct config_item      cg_item;        // 继承的 config_item
    struct list_head        cg_children;    // 子项列表
    struct configfs_subsystem *cg_subsys;   // 所属子系统
    struct list_head        default_groups; // 默认子组
    struct list_head        group_entry;    // 组链表节点
};
```

### 2.3 config_item_type — 配置项类型

```c
// 文件: include/linux/configfs.h
struct config_item_type {
    struct module                           *ct_owner;     // 所属模块
    struct configfs_item_operations         *ct_item_ops;  // 项操作
    struct configfs_group_operations        *ct_group_ops; // 组操作
    struct configfs_attribute               **ct_attrs;    // 属性数组
    const struct config_item_type           *ct_type;      // 类型
};
```

### 2.4 configfs_item_operations — 项操作

```c
// 文件: include/linux/configfs.h
struct configfs_item_operations {
    void (*release)(struct config_item *);                    // 释放
    int  (*allow_drop)(struct config_item *);                 // 允许删除
    void (*drop_item)(struct config_item *);                  // 删除项
};
```

### 2.5 configfs_group_operations — 组操作

```c
// 文件: include/linux/configfs.h
struct configfs_group_operations {
    struct config_item *(*make_item)(struct config_group *group, const char *name); // 创建项
    struct config_group *(*make_group)(struct config_group *group, const char *name); // 创建组
    int (*pre_commit)(struct config_item *item);              // 提交前
    void (*commit_item)(struct config_item *item);            // 提交项
    int (*drop_item)(struct config_item *);                   // 删除项
};
```

### 2.6 configfs_subsystem — 子系统

```c
// 文件: include/linux/configfs.h
struct configfs_subsystem {
    struct config_group     su_group;       // 子系统组
    struct mutex            su_mutex;       // 子系统互斥锁
};
```

---

## 3. API 与使用方法

### 3.1 核心 API

```c
#include <linux/configfs.h>

// 初始化 config_item
void config_item_init(struct config_item *item,
                      const struct config_item_type *type);
void config_item_init_type_name(struct config_item *item,
                                const char *name,
                                const struct config_item_type *type);

// 引用计数
struct config_item *config_item_get(struct config_item *);
void config_item_put(struct config_item *);

// 链接/解除链接
void link_obj(struct config_item *parent, struct config_item *child);
void unlink_obj(struct config_item *item);

// 组的操作
void config_group_init(struct config_group *group);
void config_group_init_type_name(struct config_group *group,
                                 const char *name,
                                 const struct config_item_type *type);
int configfs_register_subsystem(struct configfs_subsystem *subsys);
void configfs_unregister_subsystem(struct configfs_subsystem *subsys);

// 默认组
int configfs_register_default_group(struct config_group *parent,
                                    const char *name,
                                    const struct config_item_type *item_type);
void configfs_unregister_default_group(struct config_group *parent,
                                       const char *name);
```

### 3.2 使用示例

```c
// 示例: 创建一个简单的 configfs 子系统 (模拟 iSCSI target)

// 1. 定义 item 结构
struct my_target {
    struct config_item item;    // 嵌入的 config_item
    int target_id;
    char name[32];
};

// 2. 定义属性
struct my_target_attribute {
    struct configfs_attribute attr;
    ssize_t (*show)(struct my_target *, char *);
    ssize_t (*store)(struct my_target *, const char *, size_t);
};

// 3. 实现属性 show/store
static ssize_t my_target_id_show(struct my_target *t, char *buf)
{
    return sysfs_emit(buf, "%d\n", t->target_id);
}

static ssize_t my_target_id_store(struct my_target *t,
                                  const char *buf, size_t count)
{
    int ret = kstrtoint(buf, 0, &t->target_id);
    if (ret < 0)
        return ret;
    return count;
}

// 4. 定义属性数组
#define MY_TARGET_ATTR(_name, _mode, _show, _store) \
    struct my_target_attribute my_target_attr_##_name = \
        __CONFIGFS_ATTR(_name, _mode, _show, _store)

static MY_TARGET_ATTR(target_id, 0644, my_target_id_show, my_target_id_store);

static struct configfs_attribute *my_target_attrs[] = {
    &my_target_attr_target_id.attr,
    NULL,
};

// 5. 实现 make_item 回调
static struct config_item *my_make_item(struct config_group *group,
                                        const char *name)
{
    struct my_target *t;

    t = kzalloc(sizeof(*t), GFP_KERNEL);
    if (!t)
        return ERR_PTR(-ENOMEM);

    strncpy(t->name, name, sizeof(t->name) - 1);
    config_item_init_type_name(&t->item, name, &my_item_type);

    return &t->item;
}

// 6. 定义类型
static struct configfs_item_operations my_item_ops = {
    .release = my_release,
};

static struct configfs_group_operations my_group_ops = {
    .make_item = my_make_item,
};

static const struct config_item_type my_item_type = {
    .ct_item_ops  = &my_item_ops,
    .ct_attrs     = my_target_attrs,
    .ct_owner     = THIS_MODULE,
};

// 7. 注册子系统
static struct configfs_subsystem my_subsys = {
    .su_group = {
        .cg_item = {
            .ci_namebuf = "my_subsys",
            .ci_type = &my_subsys_type,
        },
    },
};

static const struct config_item_type my_subsys_type = {
    .ct_group_ops = &my_group_ops,
    .ct_owner     = THIS_MODULE,
};

static int __init my_init(void)
{
    config_group_init(&my_subsys.su_group);
    mutex_init(&my_subsys.su_mutex);
    return configfs_register_subsystem(&my_subsys);
}
```

```bash
# 用户空间使用
mount -t configfs none /sys/kernel/config
cd /sys/kernel/config/my_subsys
mkdir my_target_1                    # 触发 my_make_item()
echo 42 > my_target_1/target_id      # 写入属性
cat my_target_1/target_id            # 读取属性
# 42
rmdir my_target_1                    # 销毁对象
```

---

## 4. 函数调用栈

### 4.1 创建对象 (mkdir)

```
mkdir /sys/kernel/config/my_subsys/my_target
  ↓ sys_mkdir() → vfs_mkdir() → configfs_mkdir()
    → configfs_mkdir(dir, dentry, mode)
      → configfs_create_dir(item, dentry, ...)    // 创建目录项
        → create_default_group(item, ...)          // 创建默认组
      → config_group = to_config_group(parent_item)
      → type = config_group->cg_item.ci_type
      → type->ct_group_ops->make_item(config_group, name)  // 调用回调
        → 分配并初始化 my_target
        → config_item_init_type_name(&t->item, name, &my_item_type)
        → link_obj(parent_item, &t->item)           // 链接到父项
      → configfs_create_dir(item, ...)              // 创建目录
        → configfs_new_dirent()                     // 创建新的 dirent
        → configfs_create_link()                    // 创建符号链接
      → config_item->ci_dentry = dentry             // 关联 dentry
```

### 4.2 删除对象 (rmdir)

```
rmdir /sys/kernel/config/my_subsys/my_target
  ↓ sys_rmdir() → vfs_rmdir() → configfs_rmdir()
    → configfs_rmdir(inode, dentry)
      → item = to_item(dentry)                     // 获取 config_item
      → type = item->ci_type
      → type->ct_item_ops->drop_item(item)         // 调用 drop 回调
        → 释放资源
      → configfs_detach_item(item)                  // 分离项
        → configfs_detach_group()                   // 分离组
          → configfs_detach_attrs()                 // 分离属性
      → config_item_put(item)                       // 释放引用
```

### 4.3 属性读取

```
cat /sys/kernel/config/.../target_id
  ↓ sys_read() → vfs_read() → configfs_read_file()
    → fill_read_buffer(file, buffer, count)
      → config_item = file->f_path.dentry->d_fsdata  // 获取 config_item
      → attr = to_attr(dentry->d_parent->d_fsdata)   // 获取属性
      → attr->type->ct_item_ops->show_attribute(item, attr, buf)
        → container_of(attr, struct my_target_attribute, attr)
        → my_target_id_show(my_target, buf)          // 调用 show 回调
    → simple_read_from_buffer(user_buf, ...)          // 拷贝到用户空间
```

---

## 5. 流程图

### 5.1 configfs 对象模型

```
用户空间: mkdir "my_target"    用户空间: echo "42" > target_id
         │                              │
         ▼                              ▼
内核: configfs_mkdir()       内核: configfs_read/write()
         │                              │
         ├── 创建 kernfs 目录           ├── 找到 config_item
         ├── 调用 make_item()           ├── 找到 configfs_attribute
         │     └── 分配对象              └── 调用 show/store 回调
         ├── 创建默认属性文件
         └── 链接到父项
```

### 5.2 configfs 目录结构

```
/sys/kernel/config/
│
├── target/                          # iSCSI target 子系统
│   └── iscsi/                       # 子组
│       └── iqn.2023-01.com.example:target/
│           ├── tpgt_1/              # make_group 创建
│           │   ├── luns/
│           │   │   └── lun_0/
│           │   │       └── ...
│           │   ├── attrib/
│           │   │   └── authentication
│           │   └── param/
│   │           └── ...
│   └── ...
│
├── my_subsys/                       # 自定义子系统示例
│   └── my_target_1/                 # mkdir → make_item 创建
│       ├── target_id                # 属性文件
│       └── ...                      # 其他属性文件
│
└── ...                              # 其他子系统
```

---

## 6. 使用场景

| 场景 | 描述 | 示例 |
|------|------|------|
| **iSCSI Target** | 创建/配置 iSCSI 存储目标 | 通过 mkdir 创建 target、LUN、ACL |
| **NFS 导出** | 配置 NFS 导出条目 | 动态添加/删除 NFS 导出 |
| **USB Gadget** | 配置 USB 设备功能 | 创建 USB 串口、存储、网络功能 |
| **RDMA 配置** | 配置 InfiniBand/RDMA 设备 | 创建/删除 RDMA 资源 |
| **OcFS2 集群** | Oracle 集群文件系统配置 | 管理集群节点和心跳 |
| **DLM 配置** | 分布式锁管理器配置 | 管理锁空间和锁资源 |

---

## 7. 关键源码文件

| 文件 | 内容 |
|------|------|
| `fs/configfs/configfs.h` | 内部宏和函数声明 |
| `fs/configfs/inode.c` | inode 操作实现 |
| `fs/configfs/file.c` | 属性文件操作（read/write） |
| `fs/configfs/dir.c` | 目录操作（mkdir, rmdir, lookup） |
| `fs/configfs/symlink.c` | 符号链接支持 |
| `fs/configfs/mount.c` | 文件系统类型和挂载 |
| `include/linux/configfs.h` | 核心数据结构定义和 API |