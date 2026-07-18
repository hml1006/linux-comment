# swapon 系统调用分析

## 1. 概述

启用交换分区或交换文件，使系统可以将内存页面换出到指定设备。

**原型：**

```c
SYSCALL_DEFINE2(swapon, const char __user *, specialfile, int, swap_flags)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `specialfile` | `const char *` | 交换分区/文件的路径 |
| `swap_flags` | `int` | 交换标志（`SWAP_FLAG_PREFER` 等） |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-EBUSY` — 设备已被用作交换空间
  - `-EINVAL` — 无效的交换签名
  - `-ENOMEM` — 内存不足

## 2. 使用场景

- **系统启动**: 启用预设的交换分区（`/etc/fstab`）
- **临时扩展内存**: 添加交换文件以应对内存压力
- **休眠支持**: 启用休眠交换分区

## 3. 函数调用栈

```
swapon(specialfile, swap_flags) (系统调用入口)
└─ ksys_swapon(specialfile, swap_flags)               // mm/swapfile.c
   ├─ blkdev_get_by_path(path, FMODE_READ|FMODE_WRITE, ...)  // 打开块设备
   ├─ alloc_swap_info()                                // 分配 swap_info_struct
   │
   ├─ read_swap_header()                               // 读取交换空间头部
   │    └─ 校验魔数 ("SWAPSPACE2" 或 "SWAP-SPACE")
   │
   ├─ setup_swap_extents(p, ...)                       // 设置交换区映射
   │    └─ swapon_swapfile()                           // 交换文件检查
   │
   └─ enable_swap_info(p, prio, swap_map, ...)        // 启用交换空间
        ├─ insert_swap_info(p)                         // 插入 swap_info 数组
        └─ 更新全局 swap 优先级链表
```

## 4. 关键数据结构

```c
// ===== struct swap_info_struct (交换空间信息, include/linux/swap.h) =====
struct swap_info_struct {
    unsigned long flags;                  // SWP_* 标志位
    signed short prio;                    // 交换优先级
    signed char type;                     // 交换类型/索引
    unsigned char *swap_map;              // 页槽位映射数组
    unsigned int cluster_info;            // 集群分配信息
    unsigned int lowest_bit;              // 最低可用位
    unsigned int highest_bit;             // 最高可用位
    unsigned int pages;                   // 总页数
    unsigned int max;                     // 最大槽位索引
    unsigned int inuse_pages;             // 已使用的页数
    struct block_device *bdev;            // 块设备
    struct file *swap_file;               // 交换文件
    struct percpu_cluster __percpu *percpu_cluster; // 每 CPU 集群
};

// ===== struct swap_header (交换空间头部, include/linux/swap.h) =====
// 交换空间头部，位于交换分区/文件的第一个扇区
union swap_header {
    struct {
        char reserved[PAGE_SIZE - 10];    // 保留
        char magic[10];                   // 魔数 "SWAPSPACE2" 或 "SWAP-SPACE"
    } magic;
    struct {
        char bootbits[1024];              // 引导扇区（保留）
        unsigned int version;             // 版本
        unsigned int last_page;           // 最后一页
        unsigned int nr_badpages;         // 坏页数量
        unsigned int padding[125];        // 填充
        unsigned int badpages[1];         // 坏页列表
    } info;
};
```

## 5. 流程图

```
swapon("/dev/sda2", 0)
  │
  v
ksys_swapon()
  │
  ├─ blkdev_get_by_path()  // 打开块设备
  │
  ├─ alloc_swap_info()     // 分配 swap_info_struct
  │
  ├─ read_swap_header()    // 读取交换头部
  │    └─ 校验魔数 "SWAPSPACE2"
  │
  ├─ setup_swap_extents()  // 建立槽位映射
  │
  └─ enable_swap_info()    // 启用交换
       ├─ swap_map = kvzalloc(pages)  // 分配槽位映射数组
       ├─ insert_swap_info()          // 插入全局数组
       └─ 更新优先级链表
```

## 6. 使用示例

```c
#include <unistd.h>
#include <sys/swap.h>
#include <stdio.h>

int main(void)
{
    // 启用交换分区
    if (swapon("/dev/sda2", 0) == -1) {
        perror("swapon");
        return 1;
    }
    printf("Swap on /dev/sda2 enabled\n");
    return 0;
}
```

## 7. 参考

- `mm/swapfile.c` — swapon 实现
- `include/linux/swap.h` — swap_info_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)