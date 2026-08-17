# bpf 系统调用分析

## 1. 概述

BPF（Berkeley Packet Filter，现泛指 extended BPF）系统调用是 Linux 内核 BPF 子系统的入口。它提供了一系列命令来管理 BPF 程序、BPF 映射（map）、BTF 类型信息以及 BPF 链接（link）。

**原型：**

```c
SYSCALL_DEFINE3(bpf, int, cmd, union bpf_attr __user *, uattr, unsigned int, size)
```

**参数：**
- `cmd`：BPF 命令，指定要执行的操作
- `uattr`：指向 `union bpf_attr` 的指针，包含命令特定参数
- `size`：`uattr` 结构体的大小（用于向前兼容）

## 2. 主要命令分类

### 映射操作
| 命令 | 功能 |
|------|------|
| `BPF_MAP_CREATE` | 创建 BPF 映射 |
| `BPF_MAP_LOOKUP_ELEM` | 查找映射元素 |
| `BPF_MAP_UPDATE_ELEM` | 更新映射元素 |
| `BPF_MAP_DELETE_ELEM` | 删除映射元素 |
| `BPF_MAP_GET_NEXT_KEY` | 获取下一个键 |
| `BPF_MAP_FREEZE` | 冻结映射 |

### 程序操作
| 命令 | 功能 |
|------|------|
| `BPF_PROG_LOAD` | 加载 BPF 程序 |
| `BPF_PROG_GET_FD_BY_ID` | 通过 ID 获取程序 fd |
| `BPF_PROG_QUERY` | 查询目标 fd 上的 BPF 程序 |

### BTF 操作
| 命令 | 功能 |
|------|------|
| `BPF_BTF_LOAD` | 加载 BTF 类型信息 |
| `BPF_BTF_GET_FD_BY_ID` | 通过 ID 获取 BTF fd |

### 链接操作
| 命令 | 功能 |
|------|------|
| `BPF_LINK_CREATE` | 创建 BPF 链接 |
| `BPF_LINK_UPDATE` | 更新 BPF 链接 |
| `BPF_LINK_GET_FD_BY_ID` | 通过 ID 获取链接 fd |
| `BPF_LINK_DETACH` | 分离 BPF 链接 |

### 其他
| 命令 | 功能 |
|------|------|
| `BPF_RAW_TRACEPOINT_OPEN` | 打开原始 tracepoint |
| `BPF_TASK_FD_QUERY` | 查询任务 fd 信息 |
| `BPF_MAP_LOOKUP_BATCH` | 批量查找映射元素 |
| `BPF_MAP_UPDATE_BATCH` | 批量更新映射元素 |
| `BPF_MAP_DELETE_BATCH` | 批量删除映射元素 |
| `BPF_ITER_CREATE` | 创建 BPF 迭代器 |
| `BPF_PROG_BIND_MAP` | 绑定映射到程序 |

## 3. 函数调用栈

```
bpf(cmd, uattr, size)                                    // kernel/bpf/syscall.c
  └─ __sys_bpf(cmd, uattr, size)
       ├─ switch (cmd):
       │    ├─ BPF_MAP_CREATE → map_create(&attr)
       │    │    └─ 分配 bpf_map 结构，调用 map_ops->map_alloc
       │    ├─ BPF_PROG_LOAD → bpf_prog_load(&attr)
       │    │    └─ bpf_check() → BPF 验证器                  // kernel/bpf/verifier.c
       │    │    └─ bpf_prog_select_runtime() → JIT 编译
       │    ├─ BPF_MAP_LOOKUP_ELEM → map_lookup_elem(&attr)
       │    ├─ BPF_MAP_UPDATE_ELEM → map_update_elem(&attr)
       │    ├─ BPF_BTF_LOAD → bpf_btf_load(&attr)
       │    ├─ BPF_LINK_CREATE → link_create(&attr)
       │    └─ ... 其他命令
       └─ 返回 fd 或 0（取决于命令）
```

## 4. 关键数据结构

### 4.1 struct bpf_map（BPF 映射）

```c
// include/linux/bpf.h
struct bpf_map {
    const struct bpf_map_ops *ops;       // 映射操作函数集
    struct bpf_map *inner_map_meta;      // 内部映射元数据
    enum bpf_map_type map_type;          // 映射类型
    u32 key_size;                        // 键大小
    u32 value_size;                      // 值大小
    u32 max_entries;                     // 最大条目数
    u32 map_flags;                       // 映射标志
    u32 id;                              // 映射 ID
    int numa_node;                       // NUMA 节点
    struct btf *btf;                     // BTF 类型信息
    char name[BPF_OBJ_NAME_LEN];         // 映射名称
    atomic64_t refcnt;                   // 引用计数
    atomic64_t usercnt;                  // 用户计数
};
```

### 4.2 struct bpf_prog（BPF 程序）

```c
// include/linux/filter.h
struct bpf_prog {
    u16 pages;                    // 分配的内存页数
    u16 jited:1;                  // 是否已 JIT 编译
    u16 gpl_compatible:1;        // 是否兼容 GPL
    u32 len;                      // 指令数
    enum bpf_prog_type type;      // 程序类型
    struct bpf_prog_aux *aux;     // 辅助信息
    struct sock_fprog_kern *orig_prog; // 原始 BPF 程序
    unsigned int (*bpf_func)(const void *ctx, const struct bpf_insn *insn);
    union {
        struct sock_filter insns[0];  // 经典 BPF 指令
        struct bpf_insn insnsi[0];    // 扩展 BPF 指令
    };
};
```

## 5. 流程图

```
用户态: bpf(BPF_PROG_LOAD, &attr, sizeof(attr))
    │
    v
┌─────────────────────────────────────┐
│ bpf_prog_load(&attr)                │
│ 1. 拷贝 BPF 指令到内核空间          │
│ 2. 检查程序类型和权限               │
│ 3. 分配 bpf_prog 结构               │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ bpf_check() → BPF 验证器            │
│ 1. 控制流分析 (CFG)                 │
│ 2. 生命周期检查                     │
│ 3. 类型安全验证                     │
│ 4. 辅助函数调用检查                 │
│ 5. 映射访问检查                     │
│ 失败 → 返回 -EINVAL                 │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ bpf_prog_select_runtime()           │
│ 1. 尝试 JIT 编译                   │
│ 2. 回退到解释器                    │
│ 3. 设置 bpf_func 函数指针          │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ 创建匿名 fd → 返回 fd 给用户空间   │
│ 用户可通过 fd 管理程序生命周期     │
└─────────────────────────────────────┘
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | 无效的命令 / 属性无效 / 验证失败 |
| `-EPERM` | 权限不足 | 需要 CAP_BPF 或 CAP_SYS_ADMIN |
| `-ENOMEM` | 内存不足 | 分配失败 |
| `-EFAULT` | 内存错误 | uattr 指针不可访问 |
| `-E2BIG` | 程序过大 | BPF 指令数超过上限 |
| `-EACCES` | 拒绝访问 | 验证器检测到不安全操作 |

## 7. 使用示例

```c
#include <linux/bpf.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 简单的 BPF 程序：始终返回 0 */
struct bpf_insn prog[] = {
    BPF_MOV64_IMM(BPF_REG_0, 0),  // r0 = 0
    BPF_EXIT_INSN(),              // return r0
};

int main(void)
{
    union bpf_attr attr = {
        .prog_type = BPF_PROG_TYPE_SOCKET_FILTER,
        .insns = (__u64)prog,
        .insn_cnt = sizeof(prog) / sizeof(prog[0]),
        .license = (__u64)"GPL",
    };

    int fd = syscall(__NR_bpf, BPF_PROG_LOAD, &attr, sizeof(attr));
    if (fd < 0) {
        perror("BPF_PROG_LOAD");
        return 1;
    }

    printf("BPF program loaded, fd: %d\n", fd);

    /* 创建 BPF 映射 */
    union bpf_attr map_attr = {
        .map_type = BPF_MAP_TYPE_ARRAY,
        .key_size = 4,
        .value_size = 8,
        .max_entries = 1024,
    };

    int map_fd = syscall(__NR_bpf, BPF_MAP_CREATE, &map_attr,
                         sizeof(map_attr));
    if (map_fd < 0) {
        perror("BPF_MAP_CREATE");
        close(fd);
        return 1;
    }

    printf("BPF map created, fd: %d\n", map_fd);

    close(map_fd);
    close(fd);
    return 0;
}
```

## 8. 参考

- 源码位置：`kernel/bpf/syscall.c`
- 验证器：`kernel/bpf/verifier.c`
- BPF 核心：`kernel/bpf/core.c`
- 头文件：`include/uapi/linux/bpf.h`, `include/linux/bpf.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#BPF 与追踪)