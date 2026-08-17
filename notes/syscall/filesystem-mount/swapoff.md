# swapoff 系统调用分析

## 1. 概述

停用交换分区或交换文件，释放交换空间。

**原型：**

```c
SYSCALL_DEFINE1(swapoff, const char __user *, specialfile)
```

**参数：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `specialfile` | `const char *` | 交换分区/文件的路径 |

**返回值：**

- 成功返回 `0`
- 失败返回负值错误码：
  - `-EPERM` — 缺少 `CAP_SYS_ADMIN` 权限
  - `-ENOENT` — 指定的交换设备不存在
  - `-EBUSY` — 交换空间正在使用中
  - `-ENOMEM` — 内存不足，无法换回页面

## 2. 使用场景

- **系统关闭**: 关机前停用所有交换空间
- **交换空间调整**: 更换或删除交换分区前停用
- **内存回收**: 停用交换，强制将交换出的页面换回内存

## 3. 函数调用栈

```
swapoff(specialfile) (系统调用入口)
└─ ksys_swapoff(specialfile)                          // mm/swapfile.c
   ├─ path = user_path(specialfile)                    // 路径解析
   ├─ swap_info = find_swap_info_by_bdev(bdev)         // 查找 swap_info
   │
   ├─ [同步] ─── try_to_unuse(type, ...)               // 将交换出的页面换回
   │    ├─ for_each_possible_cpu()                     // 遍历所有 CPU
   │    ├─ for_each_online_pgdat()                     // 遍历所有内存节点
   │    │    └─ shrink_all_memory()                    // 回收内存
   │    └─ unuse_pte_range()                           // 逐个恢复 PTE 映射
   │
   ├─ [清理] ─── destroy_swap_extents(p)               // 释放交换区映射
   ├─ [关闭] ─── bdev_fput(swap_file)                  // 关闭块设备
   └─ [释放] ─── free_swap_info(p)                     // 释放 swap_info_struct
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
swapoff("/dev/sda2")
  │
  v
ksys_swapoff()
  │
  ├─ 查找 swap_info_struct
  │
  ├─ try_to_unuse()  ──────────────────────────┐
  │   (将交换出的页面换回内存)                     │
  │    ├─ 遍历所有进程的 VMA                      │
  │    ├─ 找到使用该交换区的 PTE                   │
  │    └─ 分配物理页，从交换区读回数据               │
  │                                              │
  ├─ destroy_swap_extents()  // 清理映射           │
  ├─ bdev_fput()            // 关闭设备            │
  └─ free_swap_info()       // 释放数据结构          │
```

## 6. 使用示例

```c
#include <unistd.h>
#include <sys/swap.h>
#include <stdio.h>

int main(void)
{
    // 停用交换分区
    if (swapoff("/dev/sda2") == -1) {
        perror("swapoff");
        return 1;
    }
    printf("Swap off /dev/sda2\n");
    return 0;
}
```

## 7. 参考

- `mm/swapfile.c` — swapon/swapoff 实现
- `include/linux/swap.h` — swap_info_struct 定义
- [ARM64 系统调用表](../arm64-syscall-table.md#文件系统挂载与结构)