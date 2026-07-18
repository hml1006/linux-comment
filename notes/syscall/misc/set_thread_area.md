# set_thread_area

## 原理与功能

`set_thread_area` 是 x86 和 CSKY 架构提供系统调用，用于设置线程本地存储（TLS）段描述符。不同架构的实现差异较大：

- **x86 (32/64位)**：修改 GDT（全局描述符表）中的 TLS 段描述符，用于设置 FS/GS 段寄存器指向的 TLS 区域
- **CSKY 架构**：架构专用系统调用，用于设置 TLS 指针
- **ARM64 架构**：此系统调用仅存在于编号表中，为其他架构兼容性保留，ARM64 上为 stub 实现

### 功能说明

- x86：设置用户态 TLS 段描述符（GDT 中的 `gdt_page.tls_array`）
- CSKY：设置线程本地存储指针
- ARM64：stub 实现（返回 -ENOSYS）

## 函数原型

### x86 架构

```c
// x86 架构
SYSCALL_DEFINE1(set_thread_area, struct user_desc __user *, u_info);

// 或 UML 子架构
SYSCALL_DEFINE1(set_thread_area, struct user_desc __user *, user_desc);
```

### CSKY 架构

```c
// CSKY 架构
long sys_set_thread_area(unsigned long addr);
```

## x86 实现详解

### 完整实现

```c
// arch/x86/kernel/tls.c
SYSCALL_DEFINE1(set_thread_area, struct user_desc __user *, u_info)
{
    return do_set_thread_area(current, -1, u_info, 1);
}

int do_set_thread_area(struct task_struct *p, int idx,
                       struct user_desc __user *u_info, int add_tls)
{
    struct user_desc info;
    // 从用户空间复制描述符信息
    if (copy_from_user(&info, u_info, sizeof(info)))
        return -EFAULT;

    // 如果 entry_number 为 -1，自动分配空闲槽位
    if (idx == -1)
        idx = info.entry_number;
    
    // 如果 entry_number 为 -1，查找空闲槽位
    if (idx == -1) {
        idx = get_free_idx();
        if (idx < 0)
            return -ESRCH;
    }

    // 检查索引是否在有效范围内
    if (idx < GDT_ENTRY_TLS_MIN || idx > GDT_ENTRY_TLS_MAX)
        return -EINVAL;

    // 设置 TLS 描述符
    set_tls_desc(p, idx, &info, 1);

    // 将分配的槽位号写回用户空间
    if (put_user(idx, &u_info->entry_number))
        return -EFAULT;
    
    return 0;
}
```

### 关键数据结构

```c
// arch/x86/include/asm/desc.h
struct desc_struct {
    union {
        struct {
            unsigned int a;
            unsigned int b;
        };
        struct {
            u16 limit0;
            u16 base0;
            unsigned base1: 8, type: 4, s: 1, dpl: 2, p: 1;
            unsigned limit: 4, avl: 1, l: 1, d: 1, g: 1, base2: 8;
        };
    };
} __attribute__((packed));

// 用户空间传递的描述符信息
// include/uapi/asm-generic/ioctls.h (定义在 asm/ldt.h)
struct user_desc {
    unsigned int  entry_number;     // GDT 中的槽位号
    unsigned int  base_addr;        // 段基地址
    unsigned int  limit;            // 段限长
    unsigned int  seg_32bit:1;      // 32 位段标志
    unsigned int  contents:2;       // 段类型内容
    unsigned int  read_exec_only:1; // 只读/只执行
    unsigned int  limit_in_pages:1; // 限长单位（4KB/字节）
    unsigned int  seg_not_present:1;// 段不存在
    unsigned int  useable:1;        // 可用位
    unsigned int  lm:1;            // 64 位模式标志（仅 x86_64）
};

// GDT 中 TLS 区域定义
#define GDT_ENTRY_TLS_MIN    6
#define GDT_ENTRY_TLS_MAX    8
#define GDT_ENTRY_TLS_ENTRIES 3

// 每个线程的 TLS 数组
struct thread_struct {
    struct desc_struct tls_array[GDT_ENTRY_TLS_ENTRIES];
    // ...
};
```

### 调用链

```
set_thread_area(u_info)
  │
  └─ do_set_thread_area(current, idx=-1, u_info, add_tls=1)
       │
       ├─ copy_from_user(&info, u_info)  // 读取用户态描述符
       │
       ├─ idx = get_free_idx()           // 自动分配空闲槽位 (GDT 6/7/8)
       │    └─ 遍历 tls_array，查找 p=0 的槽位
       │
       ├─ set_tls_desc(p, idx, &info, 1) // 设置 GDT 描述符
       │    └─ 写入 per-CPU 的 gdt_page
       │
       └─ put_user(idx, &u_info->entry_number)  // 返回分配到的槽位号
```

### 流程图

```
用户态调用 set_thread_area(&desc)
  │
  ▼
copy_from_user(info)  ──失败──→ 返回 -EFAULT
  │
 成功
  │
  ▼
entry_number == -1? ──是──→ get_free_idx()
  │                              │
 否 (指定槽位)                   ▼
  │                          ┌─ 找到空闲? ──否──→ 返回 -ESRCH
  │                          │
  │                          ▼
  │                          使用空闲槽位
  │                          │
  └──────────────────────────┘
  │
  ▼
idx 范围检查 (GDT_ENTRY_TLS_MIN ~ GDT_ENTRY_TLS_MAX)
  │
 非法 ──→ 返回 -EINVAL
  │
 合法
  │
  ▼
set_tls_desc()  ─── 写入 GDT 表
  │
  ▼
put_user(entry_number)  ─── 将槽位号返回用户态
  │
  ▼
返回 0 (成功)
```

## CSKY 架构实现

```c
// CSKY 架构的实现
// arch/csky/kernel/syscall.c
long sys_set_thread_area(unsigned long addr)
{
    /* 设置 TLS 指针 */
    task_thread_info(current)->tp_value = addr;
    return 0;
}
```

## 使用场景

- **线程创建**：pthread_create 时为新线程设置 TLS
- **动态链接器**：设置线程私有存储区
- **set_thread_area vs arch_prctl**：
  - 32 位：使用 `set_thread_area` 设置 GDT 段
  - 64 位：使用 `arch_prctl(ARCH_SET_FS, addr)` 设置 FS 基地址

## 相关系统调用

| 系统调用 | 架构 | 功能 |
|--|--|--|
| `set_thread_area` | x86, CSKY | 设置 TLS 段描述符 |
| `get_thread_area` | x86 | 获取 TLS 段描述符 |
| `arch_prctl` | x86_64 | 设置 FS/GS 基地址 |

## 注意事项

- ARM64 上此系统调用仅为 CSKY 架构兼容性保留
- ARM64 通过 `msr tpidr_el0, x0` 指令管理 TLS，无需系统调用
- x86 上最多支持 3 个 TLS 段（GDT 槽位 6、7、8）
- 如果 `entry_number` 为 -1，内核自动分配，并通过 `put_user` 返回

## 源码位置

| 文件 | 说明 |
|--|--|
| [arch/x86/kernel/tls.c](/home/louis/code/linux/arch/x86/kernel/tls.c) | x86 set_thread_area 实现 |
| [arch/x86/kernel/tls.h](/home/louis/code/linux/arch/x86/kernel/tls.h) | x86 TLS 内部声明 |
| [arch/arc/include/asm/syscalls.h](/home/louis/code/linux/arch/arc/include/asm/syscalls.h) | ARC 架构声明 |
| [arch/csky/include/asm/syscalls.h](/home/louis/code/linux/arch/csky/include/asm/syscalls.h) | CSKY 架构声明 |