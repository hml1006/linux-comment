# vDSO (虚拟动态共享对象) 实现机制

## 1. 概述

vDSO (virtual dynamic shared object) 是 Linux 内核映射到每个用户进程地址空间中的一小段共享库，允许某些系统调用在用户态直接完成，无需陷入内核。主要利用以下原理：

- **时间相关系统调用**：内核将时间数据（时钟源参数、基准时间等）写入共享数据页（vvar），用户态 vDSO 代码直接读取计算
- **随机数 getrandom**：将 ChaCha20 密钥共享给用户态，用户态自行生成随机数
- **getcpu**：通过架构特定指令（如 x86 的 `LSL`、LoongArch 的 `rdtime.d`）直接读取 CPU ID

### 核心优势

- 消除系统调用上下文切换开销（约 50-200ns 节省）
- 每个时钟 tick 内核更新一次 vvar 数据页，用户态无锁读取
- 用户态无需任何权限即可访问

---

## 2. 使用 vDSO 加速的系统调用

| 系统调用 | vDSO 符号 | 回退机制 | 支持架构 |
|----------|-----------|---------|---------|
| `clock_gettime` | `__vdso_clock_gettime` / `__kernel_clock_gettime` | `clock_gettime_fallback()` → `svc #0` | 所有架构 |
| `gettimeofday` | `__vdso_gettimeofday` / `__kernel_gettimeofday` | `gettimeofday_fallback()` → `svc #0` | 所有架构 |
| `time` | `__vdso_time` | 直接读取 `basetime[CLOCK_REALTIME].sec` | x86 |
| `clock_getres` | `__vdso_clock_getres` / `__kernel_clock_getres` | `clock_getres_fallback()` → `svc #0` | 所有架构 |
| `getcpu` | `__vdso_getcpu` | `getcpu` 系统调用 | x86, LoongArch |
| `getrandom` | `__vdso_getrandom` / `__kernel_getrandom` | `getrandom_syscall()` | x86_64, arm64, RISC-V, LoongArch, PowerPC, s390 |
| `rt_sigreturn` | `__kernel_rt_sigreturn` / `__vdso_rt_sigreturn` | — | arm64, MIPS |
| `riscv_hwprobe` | `__vdso_riscv_hwprobe` | `riscv_hwprobe` 系统调用 | RISC-V |

### 2.1 支持的时钟 ID

```c
#define VDSO_BASES  (CLOCK_TAI + 1)   // 共 13 个时钟基
#define VDSO_HRES   (BIT(CLOCK_REALTIME) | BIT(CLOCK_MONOTONIC) | \
                     BIT(CLOCK_BOOTTIME) | BIT(CLOCK_TAI))
#define VDSO_COARSE (BIT(CLOCK_REALTIME_COARSE) | BIT(CLOCK_MONOTONIC_COARSE))
#define VDSO_RAW    (BIT(CLOCK_MONOTONIC_RAW))
#define VDSO_AUX    __GENMASK(CLOCK_AUX_LAST, CLOCK_AUX)
```

---

## 3. 核心数据结构

### 3.1 vDSO 数据页布局

```c
enum vdso_pages {
    VDSO_TIME_PAGE_OFFSET,      // 0: 时间数据页 (vdso_time_data)
    VDSO_TIMENS_PAGE_OFFSET,    // 1: 时间命名空间页
    VDSO_RNG_PAGE_OFFSET,       // 2: RNG 数据页 (vdso_rng_data)
    VDSO_ARCH_PAGES_START,      // 3+: 架构特定数据页
    VDSO_ARCH_PAGES_END = VDSO_ARCH_PAGES_START + VDSO_ARCH_DATA_PAGES - 1,
    VDSO_NR_PAGES               // 总页数
};
```

### 3.2 struct vdso_time_data

```c
struct vdso_time_data {
    struct arch_vdso_time_data  arch_data;           // 架构特定数据
    struct vdso_clock           clock_data[CS_BASES]; // 时钟源数据
    struct vdso_clock           aux_clock_data[MAX_AUX_CLOCKS]; // 辅助时钟
    s32                         tz_minuteswest;      // 时区
    s32                         tz_dsttime;
    u32                         hrtimer_res;         // 高精度定时器分辨率
    u32                         __unused;
} ____cacheline_aligned;
```

### 3.3 struct vdso_clock (每时钟源)

```c
struct vdso_clock {
    u32     seq;            // 序列计数器 (写时奇数, 一致时偶数)
    s32     clock_mode;     // 时钟模式 (NONE/ARCH specific/TIMENS)
    u64     cycle_last;     // 时钟源上次更新时的 cycle 值
    u64     max_cycles;     // 64 位乘法不会溢出的最大 cycles
    u64     mask;           // 时钟源掩码
    u32     mult;           // 时钟源乘数
    u32     shift;          // 时钟源移位值
    union {
        struct vdso_timestamp basetime[VDSO_BASES];  // 基准时间 (系统页)
        struct timens_offset  offset[VDSO_BASES];    // 时间命名空间偏移
    };
};
```

### 3.4 struct vdso_timestamp

```c
struct vdso_timestamp {
    u64 sec;
    u64 nsec;   // 高分辨率时钟左移了 shift 位
};
```

### 3.5 struct vdso_rng_data

```c
struct vdso_rng_data {
    u64 generation;    // RNG 重新播种计数器
    u8  is_ready;      // RNG 是否已初始化
};
```

### 3.6 struct vgetrandom_state (每个线程的状态)

```c
struct vgetrandom_state {
    union {
        struct {
            u8 batch[CHACHA_BLOCK_SIZE * 3 / 2];  // 1.5 个 ChaCha20 块的缓冲输出
            u32 key[CHACHA_KEY_SIZE / sizeof(u32)]; // 当前密钥
        };
        u8 batch_key[CHACHA_BLOCK_SIZE * 2];
    };
    u64 generation;    // 生成密钥时的快照
    u8  pos;           // 批次中的偏移量
    bool in_use;       // 重入保护
};
```

---

## 4. 整体架构与工作流程

### 4.1 系统架构

```
┌──────────────────────────────────────────────────────────────┐
│                    用户进程地址空间                            │
│                                                              │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │  vDSO 代码页 (只读/可执行)                                │ │
│  │  ┌────────────────────────────────────────────────────┐  │ │
│  │  │ __vdso_clock_gettime()  /  __kernel_clock_gettime()│  │ │
│  │  │ __vdso_gettimeofday()   /  __kernel_gettimeofday() │  │ │
│  │  │ __vdso_time()                                      │  │ │
│  │  │ __vdso_clock_getres()  /  __kernel_clock_getres()  │  │ │
│  │  │ __vdso_getcpu()                                    │  │ │
│  │  │ __vdso_getrandom()    /  __kernel_getrandom()      │  │ │
│  │  │ __kernel_rt_sigreturn()                            │  │ │
│  │  └────────────────────────────────────────────────────┘  │ │
│  └──────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │  vvar 数据页 (只读)                                      │ │
│  │  ┌───────┬────────┬────────┬──────────────────────────┐  │ │
│  │  │ Time  │ Timens │  RNG   │  Arch Data (可选)        │  │ │
│  │  │ Page  │  Page  │  Page  │                          │  │ │
│  │  └───────┴────────┴────────┴──────────────────────────┘  │ │
│  └──────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌──────────────────────────────────────────────────────────┐ │
│  │  vclock 页 (x86 专有: PVCLOCK / HVCLOCK)                │ │
│  └──────────────────────────────────────────────────────────┘ │
│                                       ↑                      │
│                                       │ 缺页时映射            │
│                                       │ (vvar_fault)         │
└───────────────────────────────────────┼──────────────────────┘
                                        │
┌───────────────────────────────────────┼──────────────────────┐
│        内核空间                        │                      │
│                                       │                      │
│  ┌────────────────────────────────────┴────────────────────┐ │
│  │  vDSO 数据存储 (datastore.c)                            │ │
│  │  vdso_time_data_store  (struct vdso_time_data)          │ │
│  │  vdso_rng_data_store   (struct vdso_rng_data)           │ │
│  │  vdso_arch_data_store  (struct vdso_arch_data)          │ │
│  └─────────────────────────────────────────────────────────┘ │
│                           ↑                                   │
│                           │ 每个 tick 更新                     │
│  ┌────────────────────────┴─────────────────────────────────┐ │
│  │  update_vsyscall() (kernel/time/vsyscall.c)              │ │
│  │  1. vdso_write_begin()  → 序列计数器置奇数                │ │
│  │  2. 填充 clock_data 中的 cycle_last, mult, shift, mask   │ │
│  │  3. 填充 basetime (REALTIME, MONOTONIC, BOOTTIME, TAI,  │ │
│  │     REALTIME_COARSE, MONOTONIC_COARSE, MONOTONIC_RAW)    │ │
│  │  4. 设置 clock_mode, hrtimer_res                         │ │
│  │  5. vdso_write_end() → 序列计数器置偶数                   │ │
│  │  6. __arch_sync_vdso_time_data() (dcache flush 等)       │ │
│  └──────────────────────────────────────────────────────────┘ │
│                           ↑                                   │
│  ┌────────────────────────┴─────────────────────────────────┐ │
│  │  timekeeping 子系统 (kernel/time/timekeeping.c)          │ │
│  │  每次 tick 或时间调整时调用 update_vsyscall()             │ │
│  └──────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 clock_gettime 在 vDSO 中的处理流程

```
用户态调用 clock_gettime(CLOCK_MONOTONIC, &ts)
    │
    ▼
libc: 调用 __vdso_clock_gettime() 或 __kernel_clock_gettime()
    │
    ▼
__cvdso_clock_gettime(clock, ts)
    │
    ├─── __cvdso_clock_gettime_common(vd, clock, ts)
    │       │
    │       ├── msk = 1U << clock    // 时钟 ID 转位掩码
    │       │
    │       ├── if (msk & VDSO_HRES)    → vc = &clock_data[CS_HRES_COARSE],
    │       │                               调用 do_hres(vd, vc, clock, ts)
    │       │
    │       ├── if (msk & VDSO_COARSE)  → 调用 do_coarse(vd, vc, clock, ts)
    │       │                               直接读取 basetime，无需读硬件计数器
    │       │
    │       ├── if (msk & VDSO_RAW)     → vc = &clock_data[CS_RAW],
    │       │                               调用 do_hres(vd, vc, clock, ts)
    │       │
    │       ├── if (msk & VDSO_AUX)     → 调用 do_aux(vd, clock, ts)
    │       │
    │       └── else → return false → 回退到 clock_gettime_fallback()
    │
    ▼
do_hres(vd, vc, clock, ts)
    │
    ├── do {
    │       // 1. 序列计数器自旋等待 (seq 为奇数时等待)
    │       while (READ_ONCE(vc->seq) & 1) {
    │           if (clock_mode == VDSO_CLOCKMODE_TIMENS)
    │               return do_hres_timens(vd, vc, clock, ts);
    │           cpu_relax();
    │       }
    │       smp_rmb();  // 确保 seq 读后数据读
    │
    │       // 2. 获取硬件计数器 (如 ARM CNTVCT)
    │       cycles = __arch_get_hw_counter(clock_mode, vd);
    │
    │       // 3. 计算时间: ns = ((cycles - cycle_last) * mult + basetime.nsec) >> shift
    │       delta = (cycles - vc->cycle_last) & vc->mask;
    │       ns = (delta * vc->mult + basetime.nsec) >> vc->shift;
    │       sec = basetime.sec;
    │
    │   } while (vdso_read_retry(vc, seq));  // 检查 seq 是否变化
    │
    └── vdso_set_timespec(ts, sec, ns);  // 将 nsec 归一化
    │
    ▼
返回 ts 给用户
```

### 4.3 getrandom 在 vDSO 中的处理流程

```
用户态调用 getrandom(buffer, len, flags)
    │
    ▼
__vdso_getrandom(buffer, len, flags, opaque_state, opaque_len)
    │
    ▼
__cvdso_getrandom_data(rng_info, buffer, len, flags, state, opaque_len)
    │
    ├── if (!rng_info->is_ready) → 回退到系统调用 (RNG 未初始化)
    │
    ├── if (state->in_use) → 回退到系统调用 (重入保护)
    │
    ├── WRITE_ONCE(state->in_use, true);
    │
    ├── retry_generation:
    │   ├── current_generation = READ_ONCE(rng_info->generation);
    │   │
    │   ├── if (state->generation != current_generation) {
    │   │       // 密钥过期，需要重新播种
    │   │       WRITE_ONCE(state->generation, current_generation);
    │   │       getrandom_syscall(state->key, sizeof(state->key), 0);
    │   │       state->pos = sizeof(state->batch);  // 强制刷新批次
    │   │   }
    │   │
    │   ├── 1. 先消费 state->batch 中的剩余字节
    │   │      memcpy_and_zero_src(buffer, state->batch + state->pos, batch_len);
    │   │
    │   ├── 2. 如果 len 还大，直接生成 ChaCha20 块写入 buffer
    │   │      __arch_chacha20_blocks_nostack(buffer, state->key, counter, nblocks);
    │   │
    │   ├── 3. 重新填充 batch 并擦除密钥 (前向安全性)
    │   │      __arch_chacha20_blocks_nostack(state->batch_key, state->key, counter, 2);
    │   │
    │   └── 4. 验证 generation 未变化 (检测 fork 或内存压力)
    │
    └── WRITE_ONCE(state->in_use, false);
```

---

## 5. 序列计数机制 (Seqlock)

vDSO 使用序列计数器实现无锁并发访问，类似于 RCU：

```c
// 读者侧
u32 vdso_read_begin(const struct vdso_clock *vc) {
    while (unlikely((seq = READ_ONCE(vc->seq)) & 1))
        cpu_relax();              // seq 奇数 → 写者正在更新，自旋等待
    smp_rmb();                    // 确保 seq 读后数据读
    return seq;
}

u32 vdso_read_retry(const struct vdso_clock *vc, u32 start) {
    smp_rmb();                    // 确保数据读后 seq 读
    return READ_ONCE(vc->seq) != start;  // seq 变化 → 重试
}

// 写者侧 (内核)
void vdso_write_begin(struct vdso_time_data *vd) {
    WRITE_ONCE(vc[CS_HRES_COARSE].seq, vc[CS_HRES_COARSE].seq + 1);  // seq 变为奇数
    WRITE_ONCE(vc[CS_RAW].seq, vc[CS_RAW].seq + 1);
    smp_wmb();  // 确保 seq 写后数据写
}

void vdso_write_end(struct vdso_time_data *vd) {
    smp_wmb();  // 确保数据写后 seq 写
    WRITE_ONCE(vc[CS_HRES_COARSE].seq, vc[CS_HRES_COARSE].seq + 1);  // seq 变为偶数
    WRITE_ONCE(vc[CS_RAW].seq, vc[CS_RAW].seq + 1);
}
```

**时序示例**：

```
时间 →
写者:  seq=1(奇数)  写数据...   seq=2(偶数)
读者:  read seq=2   读数据      read seq=2 → OK
读者:  read seq=1   spin wait  read seq=2   读数据    read seq=2 → OK
```

---

## 6. vDSO 初始化与映射流程

### 6.1 构建时生成 vDSO 镜像

```
vDSO 源文件 (.c) → 编译 (特殊标志: -fpic -O2) → .o 文件
    → 链接 (特殊链接器脚本 vdso.lds.S) → .so.dbg
    → objcopy 剥离 → .so
    → vdso2c 工具 → vdso*-image.c (C 数据数组)
    → 编译为内核对象 vdso*-image.o
```

### 6.2 内核初始化

```
start_kernel()
    → timekeeping_init()
        → update_vsyscall()  // 首次填充 vvar 数据页

arch_initcall (vdso_init)  [arm64]
    → __vdso_init()
        → 验证 ELF 魔数
        → 计算 vDSO 代码页数
        → 分配并填充页面列表
```

### 6.3 进程创建时映射

```
exec() 系统调用
    → load_elf_binary()  (fs/binfmt_elf.c)
        → arch_setup_additional_pages()  [每个架构实现]
            │
            ├── arm64: __setup_additional_pages()
            │   ├── get_unmapped_area()  → 分配虚拟地址
            │   ├── vdso_install_vvar_mapping()  → 映射 vvar 数据页 [vvar]
            │   │   └── _install_special_mapping()  → 创建 VMA
            │   └── _install_special_mapping()  → 映射 vDSO 代码页 [vdso]
            │
            ├── x86: map_vdso()
            │   ├── get_unmapped_area()
            │   ├── _install_special_mapping()  → [vdso] 代码页
            │   ├── vdso_install_vvar_mapping() → [vvar] 数据页
            │   └── _install_special_mapping()  → [vvar_vclock] 时钟页
            │
            └── 设置 current->mm->context.vdso = vdso_base
            → 设置 AT_SYSINFO_EHDR 辅助向量
```

### 6.4 缺页处理 (vvar_fault)

```c
static vm_fault_t vvar_fault(const struct vm_special_mapping *sm,
                             struct vm_area_struct *vma, struct vm_fault *vmf)
{
    switch (vmf->pgoff) {
    case VDSO_TIME_PAGE_OFFSET:
        pfn = __phys_to_pfn(__pa_symbol(vdso_k_time_data));
        if (timens_page) {
            // 时间命名空间 → 同时映射命名空间页
            vmf_insert_pfn(vma, addr + VDSO_TIMENS_PAGE_OFFSET * PAGE_SIZE, pfn);
            pfn = page_to_pfn(timens_page);
        }
        break;
    case VDSO_TIMENS_PAGE_OFFSET:
        pfn = __phys_to_pfn(__pa_symbol(vdso_k_time_data));
        break;
    case VDSO_RNG_PAGE_OFFSET:
        pfn = __phys_to_pfn(__pa_symbol(vdso_k_rng_data));
        break;
    case VDSO_ARCH_PAGES_START ... VDSO_ARCH_PAGES_END:
        pfn = __phys_to_pfn(__pa_symbol(vdso_k_arch_data)) + offset;
        break;
    }
    return vmf_insert_pfn(vma, vmf->address, pfn);
}
```

---

## 7. 时间数据更新流程 (update_vsyscall)

```
每个 tick 或时间调整时:
    timekeeping_advance() / do_settimeofday64()
        → update_vsyscall(tk)
            │
            ├── vdso_write_begin(vdata)
            │   ├── clock_data[CS_HRES_COARSE].seq++  (变为奇数)
            │   └── clock_data[CS_RAW].seq++           (变为奇数)
            │
            ├── 设置 clock_mode
            ├── 填充 CLOCK_REALTIME basetime
            ├── 填充 CLOCK_REALTIME_COARSE basetime
            ├── 填充 CLOCK_MONOTONIC_COARSE basetime
            │
            ├── if (clock_mode != VDSO_CLOCKMODE_NONE)
            │       update_vdso_time_data(vdata, tk)
            │           ├── 填充 clock_data[CS_HRES_COARSE]:
            │           │     cycle_last, mask, mult, shift
            │           │     basetime[CLOCK_MONOTONIC]
            │           │     basetime[CLOCK_BOOTTIME]
            │           │     basetime[CLOCK_TAI]
            │           └── 填充 clock_data[CS_RAW]:
            │                 cycle_last, mask, mult, shift
            │                 basetime[CLOCK_MONOTONIC_RAW]
            │
            ├── __arch_update_vdso_clock()  (架构特定调整)
            │
            └── vdso_write_end(vdata)
                ├── clock_data[CS_RAW].seq++           (变为偶数)
                └── clock_data[CS_HRES_COARSE].seq++  (变为偶数)
                → __arch_sync_vdso_time_data(vdata)
                    (ARM: dcache flush, x86: TLB flush 等)
```

---

## 8. 时间命名空间支持

时间命名空间 (Time Namespace) 允许容器内进程看到不同的时间偏移。

```
进程加入时间命名空间:
    timens_commit() / timens_on_fork()
        → timens_setup_vdso_clock_data()
            → 设置命名空间 vvar 页的 clock_mode = VDSO_CLOCKMODE_TIMENS
            → 填充 offset[] 数组 (monotonic/boottime 偏移)
        → vdso_join_timens()
            → 遍历所有 VMA，找到 [vvar] 映射
            → zap_vma_pages() 清除页表缓存
            → 下次访问时，vvar_fault 重新映射命名空间页面

vDSO 中的处理:
    do_hres():
        while (READ_ONCE(vc->seq) & 1) {
            if (clock_mode == VDSO_CLOCKMODE_TIMENS)
                return do_hres_timens(vd, vc, clk, ts);
                    // 1. 读取真正的系统 vvar 页 (VDSO_TIMENS_PAGE_OFFSET)
                    // 2. 获取主机时间
                    // 3. 加上命名空间偏移量
                    // 4. 返回调整后的时间
        }
```

---

## 9. 各架构具体实现

### 9.1 arm64 (AArch64)

| 组件 | 文件路径 |
|------|---------|
| 内核侧映射 | [arch/arm64/kernel/vdso.c](file:///home/louis/code/linux/arch/arm64/kernel/vdso.c) |
| 用户侧 vDSO | [arch/arm64/kernel/vdso/vgettimeofday.c](file:///home/louis/code/linux/arch/arm64/kernel/vdso/vgettimeofday.c) |
| getrandom | [arch/arm64/kernel/vdso/vgetrandom.c](file:///home/louis/code/linux/arch/arm64/kernel/vdso/vgetrandom.c) |
| ChaCha20 汇编 | [arch/arm64/kernel/vdso/vgetrandom-chacha.S](file:///home/louis/code/linux/arch/arm64/kernel/vdso/vgetrandom-chacha.S) |
| 信号返回 | [arch/arm64/kernel/vdso/sigreturn.S](file:///home/louis/code/linux/arch/arm64/kernel/vdso/sigreturn.S) |
| 链接脚本 | [arch/arm64/kernel/vdso/vdso.lds.S](file:///home/louis/code/linux/arch/arm64/kernel/vdso/vdso.lds.S) |
| 硬件计数器 | [arch/arm64/include/asm/vdso/gettimeofday.h](file:///home/louis/code/linux/arch/arm64/include/asm/vdso/gettimeofday.h) |
| 32位兼容 | [arch/arm64/kernel/vdso32/](file:///home/louis/code/linux/arch/arm64/kernel/vdso32/) |

**arm64 特点**：
- 使用 `__kernel_*` 命名约定 (而非 `__vdso_*`)
- `__arch_get_hw_counter()` 读取 `CNTVCT` (ARM 通用定时器的虚拟计数器)
- `__arch_sync_vdso_time_data()` 需要 dcache 刷新 (硬件缓存一致性)
- 支持 BTI (Branch Target Identification) 防护
- 导出的符号：
  ```
  LINUX_2.6.39 {
      global: __kernel_rt_sigreturn;
              __kernel_gettimeofday;
              __kernel_clock_gettime;
              __kernel_clock_getres;
              __kernel_getrandom;
      local: *;
  };
  ```

### 9.2 x86_64

| 组件 | 文件路径 |
|------|---------|
| 内核侧 VMA 映射 | [arch/x86/entry/vdso/vma.c](file:///home/louis/code/linux/arch/x86/entry/vdso/vma.c) |
| 用户侧 vDSO | [arch/x86/entry/vdso/common/vclock_gettime.c](file:///home/louis/code/linux/arch/x86/entry/vdso/common/vclock_gettime.c) |
| getcpu | [arch/x86/entry/vdso/common/vgetcpu.c](file:///home/louis/code/linux/arch/x86/entry/vdso/common/vgetcpu.c) |
| getrandom | [arch/x86/entry/vdso/vdso64/vgetrandom.c](file:///home/louis/code/linux/arch/x86/entry/vdso/vdso64/vgetrandom.c) |
| 镜像生成工具 | [arch/x86/tools/vdso2c.c](file:///home/louis/code/linux/arch/x86/tools/vdso2c.c) |
| 异常处理 | [arch/x86/entry/vdso/extable.c](file:///home/louis/code/linux/arch/x86/entry/vdso/extable.c) |

**x86 特点**：
- 使用 `struct vdso_image` 描述镜像 (vdso2c 工具生成)
- 支持 TSC (时间戳计数器) 直接读取
- 支持 PVCLOCK / HVCLOCK (虚拟化时钟)
- 支持 `__vdso_getcpu` 通过 `LSL` 指令读取 CPU ID
- 导出的符号：
  ```
  LINUX_2.6 {
      global: clock_gettime; __vdso_clock_gettime;
              gettimeofday; __vdso_gettimeofday;
              getcpu; __vdso_getcpu;
              time; __vdso_time;
              clock_getres; __vdso_clock_getres;
              getrandom; __vdso_getrandom;
      local: *;
  };
  ```

---

## 10. 关键文件汇总

### 10.1 通用 (架构无关)

| 文件 | 作用 |
|------|------|
| [include/vdso/datapage.h](file:///home/louis/code/linux/include/vdso/datapage.h) | 核心数据结构定义 (`vdso_time_data`, `vdso_clock`, `vdso_rng_data`) |
| [include/vdso/helpers.h](file:///home/louis/code/linux/include/vdso/helpers.h) | 序列计数器读写操作 (seqcount) |
| [include/vdso/gettime.h](file:///home/louis/code/linux/include/vdso/gettime.h) | `__cvdso_*` 时间函数声明 |
| [include/vdso/getrandom.h](file:///home/louis/code/linux/include/vdso/getrandom.h) | `vgetrandom_state` 结构体定义 |
| [include/vdso/vsyscall.h](file:///home/louis/code/linux/include/vdso/vsyscall.h) | `vdso_update_begin/end()` 声明 |
| [include/vdso/clocksource.h](file:///home/louis/code/linux/include/vdso/clocksource.h) | `enum vdso_clock_mode` |
| [include/linux/vdso_datastore.h](file:///home/louis/code/linux/include/linux/vdso_datastore.h) | 内核侧接口声明 |
| [lib/vdso/datastore.c](file:///home/louis/code/linux/lib/vdso/datastore.c) | vvar 数据页存储、`vvar_fault` 缺页处理、`vdso_join_timens` |
| [lib/vdso/gettimeofday.c](file:///home/louis/code/linux/lib/vdso/gettimeofday.c) | 通用 vDSO 时间函数实现 (`do_hres`, `do_coarse`, `do_aux`) |
| [lib/vdso/getrandom.c](file:///home/louis/code/linux/lib/vdso/getrandom.c) | 通用 vDSO getrandom 实现 (ChaCha20 快速密钥擦除) |
| [kernel/time/vsyscall.c](file:///home/louis/code/linux/kernel/time/vsyscall.c) | `update_vsyscall()` 时间数据更新核心逻辑 |
| [kernel/time/namespace.c](file:///home/louis/code/linux/kernel/time/namespace.c) | 时间命名空间完整实现 |

### 10.2 arm64 架构

| 文件 | 作用 |
|------|------|
| [arch/arm64/kernel/vdso.c](file:///home/louis/code/linux/arch/arm64/kernel/vdso.c) | arm64 vDSO 初始化与映射 (vdso_init, __setup_additional_pages) |
| [arch/arm64/kernel/vdso/vgettimeofday.c](file:///home/louis/code/linux/arch/arm64/kernel/vdso/vgettimeofday.c) | arm64 `__kernel_clock_gettime/gettimeofday/clock_getres` |
| [arch/arm64/kernel/vdso/vgetrandom.c](file:///home/louis/code/linux/arch/arm64/kernel/vdso/vgetrandom.c) | arm64 `__kernel_getrandom` |
| [arch/arm64/kernel/vdso/vdso.lds.S](file:///home/louis/code/linux/arch/arm64/kernel/vdso/vdso.lds.S) | arm64 链接脚本与符号导出 |
| [arch/arm64/include/asm/vdso/gettimeofday.h](file:///home/louis/code/linux/arch/arm64/include/asm/vdso/gettimeofday.h) | arm64 `__arch_get_hw_counter()` 读取 CNTVCT |

### 10.3 x86 架构

| 文件 | 作用 |
|------|------|
| [arch/x86/entry/vdso/vma.c](file:///home/louis/code/linux/arch/x86/entry/vdso/vma.c) | x86 vDSO VMA 映射 (map_vdso, vdso_fault, vdso_mremap) |
| [arch/x86/tools/vdso2c.c](file:///home/louis/code/linux/arch/x86/tools/vdso2c.c) | vDSO ELF 转 C 数组工具 |
| [arch/x86/entry/vdso/extable.c](file:///home/louis/code/linux/arch/x86/entry/vdso/extable.c) | vDSO 异常修复 |

---

## 11. 构建系统

### 11.1 Kconfig 选项

```kconfig
config HAVE_GENERIC_VDSO      # 架构选择标志
config GENERIC_GETTIMEOFDAY   # 通用时间函数 vDSO
config GENERIC_VDSO_OVERFLOW_PROTECT  # 64位乘法溢出保护
config VDSO_GETRANDOM         # vDSO getrandom 支持
```

### 11.2 编译标志

```
VDSO_LDFLAGS = -shared --hash-style=both --build-id=sha1 \
               --no-undefined -Bsymbolic -z noexecstack

CFLAGS 移除: -D__KERNEL__ -mcmodel=kernel
CFLAGS 添加: -fpic -O2
```

### 11.3 构建流程

```
1. 编译 vDSO 源文件 (.c → .o) 使用特殊编译标志
2. 链接为 .so.dbg 使用特殊链接脚本 (vdso.lds.S)
3. objcopy 剥离为 .so
4. vdso2c 工具转换 .so 为 C 数组 (vdso*-image.c)
5. 编译为内核对象 (vdso*-image.o)
6. 链接进内核镜像
```

---

## 12. 测试

vDSO 测试套件位于 `tools/testing/selftests/vDSO/`：

| 测试文件 | 用途 |
|---------|------|
| vdso_test_gettimeofday.c | 测试 gettimeofday 和 clock_gettime |
| vdso_test_getcpu.c | 测试 getcpu |
| vdso_test_getrandom.c | 测试 getrandom |
| vdso_test_correctness.c | 正确性测试 |
| vdso_test_chacha.c | ChaCha20 实现测试 |
| vdso_test_abi.c | ABI 测试 |
| parse_vdso.c / parse_vdso.h | 参考 vDSO 解析器 |