# R_AARCH64_RELATIVE 类型的 .rela.dyn 重定位项

## 目录

1. [概述](#概述)
2. [ELF 重定位基础](#elf-重定位基础)
3. [内核构建过程](#内核构建过程)
4. [链接脚本布局](#链接脚本布局)
5. [启动时重定位流程](#启动时重定位流程)
6. [relocate_kernel 实现](#relocate_kernel-实现)
7. [RELR 压缩格式](#relr-压缩格式)
8. [relacheck 安全检查](#relacheck-安全检查)
9. [init 段生命周期](#init-段生命周期)
10. [与用户态动态链接对比](#与用户态动态链接对比)
11. [关键设计点](#关键设计点)

---

## 概述

R_AARCH64_RELATIVE 是 AArch64 ELF 格式中的一种重定位类型，语义为：

```
*(r_offset + delta) = r_addend + delta
```

即：**将目标地址处的值设置为加载基址加上一个固定偏移**。这种重定位不需要符号查找，动态链接器（或内核自举代码）只需知道加载地址（delta）即可完成修正。

Linux ARM64 内核在启用 `CONFIG_RELOCATABLE=y`（即 KASLR 支持）时，自身被构建为一个 **PIE（Position Independent Executable）**。内核镜像中包含的所有绝对地址引用（如全局指针的初始化值、函数指针表等）都以 `.rela.dyn` 的形式记录，在启动时由内核自身完成重定位处理。

---

## ELF 重定位基础

### Elf64_Rela 结构体

```c
typedef struct {
    Elf64_Addr r_offset;   /* 需要修正的位置（相对于 section 或虚拟地址） */
    Elf64_Xword r_info;    /* 高 32 位：符号索引，低 32 位：重定位类型 */
    Elf64_Sxword r_addend; /* 固定加数（addend） */
} Elf64_Rela;
```

### R_AARCH64_RELATIVE 的计算方式

```
信息字段：r_info = (符号索引 << 32) | R_AARCH64_RELATIVE(1027)
  - 符号索引 = 0（不需要符号查找）
  - 重定位类型 = 1027（R_AARCH64_RELATIVE）

计算：*(r_offset + delta) = r_addend + delta
  - delta = 实际加载地址 - 链接时假设的地址
  - 对于内核：delta = KASLR 偏移量
```

### 触发场景

编译为位置无关代码（`-fpie`）时，编译器遇到以下情况会生成绝对地址引用：

```c
// 全局指针初始化 —— 最常见的触发场景
int global_var = 42;
int *p = &global_var;  // p 的初始值在编译时无法确定

// 函数指针表
void (*fn_table[])(void) = { func_a, func_b, func_c };

// 结构体中的函数指针
struct file_operations fops = {
    .read = my_read,
    .write = my_write,
};
```

---

## 内核构建过程

### 编译选项

`arch/arm64/kernel/pi/Makefile` 中的关键编译选项：

```makefile
KBUILD_CFLAGS := ... -fpie ... -ffreestanding -D__DISABLE_EXPORTS
```

`-fpie` 使编译器生成位置无关代码，所有绝对地址引用都通过 GOT（Global Offset Table）或直接生成重定位项。

### 两步构建流程

```
源代码 (.c)
    │
    ▼
 编译: gcc -fpie -c → relocate.o          (普通 ELF 目标文件)
    │
    ▼
 objcopy: --prefix-symbols=__pi_         → relocate.pi.o
          --remove-section=.note.gnu.property
    │
    ▼
 relacheck: 扫描 .rela.dyn 中的 R_AARCH64_ABS64
             → 若存在且不在 .rodata.prel64 段中，报错终止
             → 若在 .rodata.prel64 段中，转换为 R_AARCH64_PREL64
    │
    ▼
 链接: 所有 .pi.o 文件 → vmlinux           (PIE 可执行文件)
```

关键点：`objcopy --prefix-symbols=__pi_` 将所有符号加上 `__pi_` 前缀。因此 `relocate.c` 中声明的 `extern rela_start[]` 在最终链接时对应的是链接脚本中的 `__pi_rela_start`。

---

## 链接脚本布局

`arch/arm64/kernel/vmlinux.lds.S` 中定义：

```ld
.rela.dyn : ALIGN(8) {
    __pi_rela_start = .;
    *(.rela .rela*)
    __pi_rela_end = .;
}

.relr.dyn : ALIGN(8) {
    __pi_relr_start = .;
    *(.relr.dyn)
    __pi_relr_end = .;
}
```

这两个段位于 **init 段** 内（`__init_begin` ~ `__init_end` 之间），在初始化完成后可以被释放。

### 内存布局示意

```
低地址                         高地址
├──────────────┬──────────────┬──────────────┤
│  .init.text   │ .rela.dyn    │ .relr.dyn    │  ← init 段（可释放）
│  (init 函数)  │ (重定位表)   │ (压缩重定位) │
├──────────────┴──────────────┴──────────────┤
│          __pi_rela_start    __pi_relr_end   │
│                             __pi_relr_start │
│          __pi_rela_end                      │
```

---

## 启动时重定位流程

### 整体调用链

```
head.S: __primary_switch                          [head.S]
    │
    │  MMU 已开启（identity mapping）
    │  x20 = boot_status, x21 = FDT 地址
    ▼
bl __pi_early_map_kernel                          [pi/map_kernel.c]
    │
    ▼
early_map_kernel(boot_status, fdt)
    │
    ├─ map_fdt(fdt)              —— 映射设备树
    ├─ memset(__bss_start, ...)  —— 清零 BSS 段 + 初始页表
    ├─ init_feature_override()   —— 解析 cmdline 特性覆盖
    ├─ kaslr_early_init()        —— 生成 KASLR 随机偏移
    │
    ▼
map_kernel(kaslr_offset, va_offset, root_level)
    │
    │ 第一次遍历（twopass）：
    │ 将所有段映射为 RW（可写）
    │ 切换 ttbr1 到 init_pg_dir
    │
    ├─ [if CONFIG_RELOCATABLE]
    │   relocate_kernel(kaslr_offset)    ← 这里处理 .rela.dyn
    │
    ├─ [if SCS] scs_patch()     —— 动态 SCS 补丁
    │
    │ 取消映射 text 段（避免 TLB 冲突）
    │ 重新映射 text 段为 ROX
    │
    ├─ memcpy(swapper_pg_dir, init_pg_dir)  —— 复制页表到最终位置
    └─ idmap_cpu_replace_ttbr1(swapper_pg_dir) —— 切换到最终内核页表
```

### 调用时机

`relocate_kernel` 的调用时机非常关键：

1. **在第一次映射完成后**、**第二次映射（text 段 readonly）之前**执行
2. 此时内核的 text 段和 data 段都是可写的（RW）
3. 重定位修正完成后，才将 text 段改为 RX（只读可执行）

这样做是因为重定位需要写入数据段中的绝对地址位置，而 text 段在第一次映射时也是 RW 以便于 SCS 补丁。

---

## relocate_kernel 实现

`arch/arm64/kernel/pi/relocate.c` 的完整实现：

```c
extern const Elf64_Rela rela_start[], rela_end[];
extern const u64 relr_start[], relr_end[];

void __init relocate_kernel(u64 offset)
{
    u64 *place = NULL;

    // === 阶段一：处理 RELA 格式重定位 ===
    for (const Elf64_Rela *rela = rela_start; rela < rela_end; rela++) {
        if (ELF64_R_TYPE(rela->r_info) != R_AARCH64_RELATIVE)
            continue;   // 只处理 R_AARCH64_RELATIVE，其他类型跳过
        *(u64 *)(rela->r_offset + offset) = rela->r_addend + offset;
    }

    // 如果未启用 RELR 或 offset 为 0，直接返回
    if (!IS_ENABLED(CONFIG_RELR) || !offset)
        return;

    // === 阶段二：处理 RELR 压缩格式重定位 ===
    for (const u64 *relr = relr_start; relr < relr_end; relr++) {
        if ((*relr & 1) == 0) {
            // 地址项（最低位为 0）：表示一个基地址
            place = (u64 *)(*relr + offset);
            *place++ += offset;
        } else {
            // 位图项（最低位为 1）：表示后续 63 个字的 bitmap
            for (u64 *p = place, r = *relr >> 1; r; p++, r >>= 1)
                if (r & 1)
                    *p += offset;
            place += 63;
        }
    }
}
```

### 关键参数说明

| 参数 | 含义 | 计算方式 |
|------|------|----------|
| `offset` | 实际加载地址与链接地址的差值 | `pa_base % MIN_KIMG_ALIGN` + KASLR seed 的高位部分 |
| `rela_start` | `.rela.dyn` 段起始（由链接脚本定义） | 实际符号为 `__pi_rela_start` |
| `rela_end` | `.rela.dyn` 段结束 | 实际符号为 `__pi_rela_end` |
| `relr_start` | `.relr.dyn` 段起始 | 实际符号为 `__pi_relr_start` |
| `relr_end` | `.relr.dyn` 段结束 | 实际符号为 `__pi_relr_end` |

### offset 的计算

`offset` 的来源是 `early_map_kernel` 中的 `kaslr_offset`：

```c
asmlinkage void __init early_map_kernel(u64 boot_status, phys_addr_t fdt)
{
    u64 kaslr_offset = pa_base % MIN_KIMG_ALIGN;  // 物理地址低位决定

    if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
        u64 kaslr_seed = kaslr_early_init(fdt_mapped, chosen);
        kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);  // 高位来自随机种子
    }
    // ...
    map_kernel(kaslr_offset, va_base - pa_base, root_level);
}
```

- 低位（`pa_base % MIN_KIMG_ALIGN`）：由物理加载地址决定，保证 2MiB block descriptor 对齐
- 高位（`kaslr_seed & ~(MIN_KIMG_ALIGN - 1)`）：来自 FDT 中 `/chosen` 节点的 `kaslr-seed` 属性

---

## RELR 压缩格式

RELR（Relative Relocation）是一种压缩存储相对重定位项的格式，大幅减小 `.rela.dyn` 段的大小。

### 编码格式

```
地址流: [ AAAAAAAA  BBBBBBB1  BBBBBBB1  ...  AAAAAAAA  BBBBBB1  ... ]
            ↑          ↑          ↑                    ↑         ↑
         地址项     位图项     位图项               地址项    位图项
```

| 项类型 | 最低位 | 含义 |
|--------|--------|------|
| 地址项 | 0 | 表示一个基地址，编码 1 个重定位 |
| 位图项 | 1 | 编码最多 63 个重定位，bit[n] 表示基地址 + n 个字的位置是否需要重定位 |

### 解码过程

```
地址项: place = *relr + offset;  *place++ += offset;
           → 基地址处的字需要加上 offset

位图项: for (bit 0..62)  if (bit[i] == 1)  place[i] += offset;
           → 后续每个字按 bitmap 指示决定是否加 offset
           → 处理完后 place += 63 跳过这一组
```

### 压缩效果

RELR 与 ELF RELA 格式相比：

| 格式 | 每个条目大小 | 典型场景 |
|------|-------------|----------|
| Elf64_Rela | 24 字节（r_offset + r_info + r_addend） | 任意类型重定位 |
| RELR 地址项 | 8 字节 | 每隔 63 个字出现一次 |
| RELR 位图项 | 8 字节（编码 63 个字） | 连续密集的重定位 |

由于内核数据段中的绝对地址引用通常非常密集（大量函数指针表、全局指针等），RELR 的压缩率通常可达 10:1 以上。

---

## relacheck 安全检查

`relacheck` 是一个主机端工具，在 `objcopy` 之后、链接之前运行，对 `.pi.o` 文件进行安全检查。

### 检查内容

```
relacheck 扫描所有 SHT_RELA 类型的 section，检查：
  1. 该 section 操作的目标段是否是 data 段（SHF_ALLOC 且非 SHF_EXECINSTR）
  2. 若目标段名包含 ".rodata.prel64"：
       → 将 R_AARCH64_ABS64 转换为 R_AARCH64_PREL64（合法化）
  3. 若目标段是普通 data 段且存在 R_AARCH64_ABS64：
       → 报错 "Unexpected absolute relocations" 并删除目标文件
```

### 为什么需要这个检查

在重定位处理（`relocate_kernel`）执行之前运行的代码中，**不允许存在 R_AARCH64_ABS64 类型的绝对地址引用**。因为：

1. 此时内核尚未完成重定位，绝对地址引用指向的是链接时的地址
2. 如果代码在重定位前就尝试读取这些绝对地址，会得到错误的值
3. 所有 `.pi.o` 中的代码都在重定位之前运行（`early_map_kernel` 本身也属于此范畴）

### prel64 机制

对于不可避免需要在重定位前访问的绝对地址，使用 `.init.rodata.prel64` 段：

```c
// pi.h 中定义
#define __prel64_initconst    __section(".init.rodata.prel64")
#define PREL64(type, name)    union { type *name; prel64_t name ## _prel; }
#define prel64_pointer(__d)   (typeof(__d))prel64_to_pointer(&__d##_prel)

typedef volatile signed long prel64_t;

static inline void *prel64_to_pointer(const prel64_t *offset)
{
    if (!*offset)
        return NULL;
    return (void *)offset + *offset;  // 相对地址计算，无需重定位
}
```

prel64 使用**相对偏移**（地址差）而非绝对地址，因此不需要重定位修正。`relacheck` 将 `.rodata.prel64` 段中的 R_AARCH64_ABS64 转换为 R_AARCH64_PREL64（place-relative 64-bit）。

---

## init 段生命周期

### 段归属

`.rela.dyn` 和 `.relr.dyn` 位于内核的 init 段中，在链接脚本中由 `__init_begin` / `__init_end` 界定。

### 释放时机

```
start_kernel()
    │
    ├─ ... 各种初始化 ...
    │
    └─ kernel_init()
           │
           ├─ do_basic_setup()
           │     └─ do_initcalls()
           │
           ├─ free_initmem()    ← 释放 init 段（包括 .rela.dyn）
           │     └─ free_initmem_default()
           │           └─ free_reserved_area()
           │                 └─ __free_page() 逐页释放
           │
           └─ run_init_process()  ← 启动用户态 init 进程
```

在 `free_initmem()` 之后，`.rela.dyn` 段占用的物理内存被回收，重定位数据不再需要。

---

## 与用户态动态链接对比

| 特性 | 内核自举重定位 | 用户态动态链接 |
|------|---------------|---------------|
| **处理者** | `relocate_kernel()` | `ld-linux.so` (ld.so) |
| **输入段** | `.rela.dyn` + `.relr.dyn` | `.rela.dyn` + `.rela.plt` |
| **处理时机** | MMU 开启后、start_kernel 之前 | 进程加载时，在 main() 之前 |
| **加载地址** | 由 bootloader 决定 + KASLR 随机 | 由内核的 `load_elf_binary` 决定 |
| **符号解析** | 不需要（所有符号在同一个镜像内） | 需要查找共享库符号 |
| **R_AARCH64_RELATIVE** | 仅需要此类型 | 也需要此类型 |
| **R_AARCH64_ABS64/ GLOB_DAT** | 不出现（relacheck 校验） | 用于跨共享库的符号引用 |
| **段生命周期** | init 段，完成后释放 | 整个进程生命周期 |
| **压缩格式** | RELR 支持 | 部分 ld.so 支持 RELR |

### 共同点

两者对 R_AARCH64_RELATIVE 的处理逻辑完全相同：

```
*(addr + delta) = val + delta
```

唯一的区别是 `delta` 的来源：内核中 `delta = kaslr_offset`，用户态中 `delta = load_base`（ELF 加载基址）。

---

## 关键设计点

1. **PIE 内核**：ARM64 内核构建为 PIE 可执行文件，使得 KASLR 成为可能，代价是需要在启动时自举重定位

2. **两阶段映射**：`map_kernel` 的两阶段设计（先 RW 后 ROX）确保重定位可以在 text 段可写时完成，最终保证 text 段只读

3. **RELR 压缩**：利用数据段中绝对地址引用的密集性，用位图压缩重定位表，大幅减少 init 段大小

4. **relacheck 安全校验**：强制保证重定位前执行的代码中没有绝对地址引用，避免使用未修正的地址

5. **prel64 相对引用**：对于确实需要在重定位前访问的地址，使用 place-relative 64 位偏移量，避免绝对地址依赖

6. **init 段可释放**：重定位表位于 init 段，在 `free_initmem()` 时被释放，不占用运行时内存

7. **`__pi_` 符号前缀**：通过 objcopy 的 `--prefix-symbols=__pi_` 将 PI 代码的符号统一加前缀，避免与普通内核符号冲突

8. **KASLR offset 的构成**：低位由物理加载地址决定（保证 2MiB 对齐），高位来自 FDT 随机种子，兼顾了页表映射约束和随机性