# ELF 文件格式与内核加载（arm64 视角）

> 源码基线：本树 Linux 7.2（含中文注释），以 arm64 为主。代码片段与行号均取自本树，可用 `sed -n` 复核。
> 相关篇目：execve 系统调用见 [`../syscall/process/execve.md`](../syscall/process/execve.md)，fork+exec 全链路见 [`../sched/fork_exec_analysis.md`](../sched/fork_exec_analysis.md)。

---

## 1. 定位：ELF 在三段流水线中的位置

ELF 不是内核私有格式，而是编译器、链接器、内核 loader、动态链接器、调试器之间的公共契约：编译阶段产出 ET_REL 的 `.o`（含节、符号表、重定位），静态链接把节合并为段并生成程序头表（ET_EXEC 或 ET_DYN），内核 `load_elf_binary()` 只读 Ehdr 与 Phdr 并建立 VMA/栈/auxv，动态链接器 `ld.so` 再读 `PT_DYNAMIC`、`DT_*` 与重定位表完成装载和绑定。内核实际只接受两种 `e_type`（`include/uapi/linux/elf.h:72-78`）：

| 值 | 名称 | 内核态度 |
| --- | --- | --- |
| 1 | `ET_REL` | 拒绝（`fs/binfmt_elf.c:860`），这是 `.o`/`.ko` 的形态，归模块加载器 |
| 2 | `ET_EXEC` | 接受，按 `p_vaddr` 固定映射，`load_bias` 为 0 |
| 3 | `ET_DYN` | 接受，可能是 PIE 程序，也可能是 `ld.so` 自身（静态 PIE） |
| 4 | `ET_CORE` | 不作为可执行文件加载，只出现在 coredump 输出中 |

完整的 exec 调用链（`elf_format` 在 `fs/binfmt_elf.c:96` 定义、`:2142` 注册）：

```
execve(2) → do_execveat_common() → bprm_execve()           fs/exec.c
 → exec_binprm()              fs/exec.c:1709（最多 5 层 binfmt 重写，超出 ELOOP）
 → search_binary_handler()    fs/exec.c:1675（按 formats 链表尝试，-ENOEXEC 则继续）
 → load_elf_binary()          fs/binfmt_elf.c:832
 → begin_new_exec() / setup_arg_pages()
 → elf_load() → elf_map() → vm_mmap()   建立 PT_LOAD 的 VMA
 → create_elf_tables()                  栈 + argv/envp/auxv
 → START_THREAD() → start_thread()      arch/arm64/include/asm/processor.h:339
 → 用户态 PC = ld.so 入口（有 PT_INTERP）或 ELF e_entry
```

正因为 `search_binary_handler()` 允许各 binfmt 依次尝试，`#!` 脚本、`binfmt_misc`、ELF 才能共存于同一套 exec 逻辑。

---

## 2. 文件格式：头、段、节、符号与重定位

### 2.1 Elf64_Ehdr

```c
typedef struct elf64_hdr {
  unsigned char	e_ident[EI_NIDENT];	/* ELF "magic number" */
  Elf64_Half e_type;
  Elf64_Half e_machine;
  Elf64_Word e_version;
  Elf64_Addr e_entry;		/* Entry point virtual address */
  Elf64_Off e_phoff;		/* Program header table file offset */
  Elf64_Off e_shoff;		/* Section header table file offset */
  Elf64_Word e_flags;
  Elf64_Half e_ehsize;
  Elf64_Half e_phentsize;
  Elf64_Half e_phnum;
  Elf64_Half e_shentsize;
  Elf64_Half e_shnum;
  Elf64_Half e_shstrndx;
} Elf64_Ehdr;
```

（`include/uapi/linux/elf.h:234`；64 位下 `sizeof(Elf64_Ehdr) == 64`，恰好放进 256 字节的 `bprm->buf`）

`e_ident[16]` 逐字节含义：`EI_MAG0..3` 为 `0x7f 'E' 'L' 'F'`（`ELFMAG`，`elf.h:362`）；`EI_CLASS` 为 `ELFCLASS64`=2；`EI_DATA` 为 `ELFDATA2LSB`=1（内核自身的 `ELF_DATA` 宏按 `__AARCH64EB__` 选择，但加载路径不显式校验该字节）；`EI_VERSION` 为 `EV_CURRENT`=1；`EI_OSABI` 为 `ELFOSABI_NONE`=0 或 `ELFOSABI_LINUX`=3（内核不校验）；`EI_PAD` 为填充。**内核加载可执行文件时完全不读节头表**，`e_shoff`/`e_shnum`/`e_shstrndx` 只在链接期和 coredump 扩展编号中使用。

### 2.2 Elf64_Phdr：段视图

```c
typedef struct elf64_phdr {
  Elf64_Word p_type;
  Elf64_Word p_flags;
  Elf64_Off p_offset;		/* Segment file offset */
  Elf64_Addr p_vaddr;		/* Segment virtual address */
  Elf64_Addr p_paddr;		/* Segment physical address */
  Elf64_Xword p_filesz;		/* Segment size in file */
  Elf64_Xword p_memsz;		/* Segment size in memory */
  Elf64_Xword p_align;		/* Segment alignment, file & memory */
} Elf64_Phdr;
```

（`include/uapi/linux/elf.h:268`；`p_align` 要求 `p_vaddr ≡ p_offset (mod p_align)`，`p_memsz-p_filesz` 即 BSS）

关键 `p_type`（`elf.h:28-47`）与内核行为：

| 值 | 名称 | 内核行为 |
| --- | --- | --- |
| 1 | `PT_LOAD` | 唯一会被 `elf_map()` 映射的类型 |
| 2 | `PT_DYNAMIC` | 不解析，交给 ld.so |
| 3 | `PT_INTERP` | 读出解释器路径，`open_exec()` 存入 `bprm->interpreter` |
| 4 / 6 | `PT_NOTE` / `PT_PHDR` | note 仅 coredump 写；`AT_PHDR` 由含 phdr 的 PT_LOAD 推算 |
| 7 | `PT_TLS` | **内核不处理**，TLS 块由 ld.so 按此段参数分配 |
| `0x6474e551` | `PT_GNU_STACK` | 决定栈是否可执行（`EXSTACK_*`） |
| `0x6474e552` | `PT_GNU_RELRO` | **内核不处理**（见 4.3） |
| `0x6474e553` | `PT_GNU_PROPERTY` | 解析 `.note.gnu.property`，arm64 用于 BTI |
| `PT_LOPROC+2` | `PT_AARCH64_MEMTAG_MTE` | arm64 专属，仅 coredump 使用 |

`p_flags` 到 `PROT_*` 的转换在 `make_prot()`（`fs/binfmt_elf.c:625`）：`PF_R/W/X` 映射为 `PROT_READ/WRITE/EXEC`，再交给 `arch_elf_adjust_prot()` 修正。

### 2.3 节 vs 段

节头表服务于链接器与调试器，段头表服务于加载器，二者并非一一对应：一个 `PT_LOAD` 通常覆盖多个 `SHF_ALLOC` 节。

| 维度 | 段（Program Header） | 节（Section Header） |
| --- | --- | --- |
| 目的 | 运行时映射与权限 | 链接、重定位、调试信息 |
| 内核是否读取 | 是（全部） | 否（coredump 只写一个扩展编号节头） |
| 典型项 | `PT_LOAD`/`PT_INTERP`/`PT_DYNAMIC` | `.text`/`.rodata`/`.symtab`/`.rela.dyn` |
| 权限表达 | `p_flags`（R/W/X） | `sh_flags`（`SHF_ALLOC`/`SHF_EXECINSTR`/`SHF_WRITE`） |
| 文件占用 | `p_filesz`/`p_memsz`（含 BSS） | `sh_size`；`SHT_NOBITS` 不占文件 |

`Elf64_Shdr` 定义在 `include/uapi/linux/elf.h:339`。AArch64 只用 `SHT_RELA`=4（带显式 addend），因为指令重定位无法靠原地读出原值推导 addend；`SHT_REL`=9 不用于 arm64 用户态。

### 2.4 符号表与重定位

`Elf64_Sym`（`include/uapi/linux/elf.h:205`）六个字段：`st_name`（字符串表下标，`st_value` 在 ET_DYN 中是相对基址的偏移）、`st_info`（`ELF_ST_BIND`/`ELF_ST_TYPE` 拆分，`elf.h:144-149`）、`st_other`、`st_shndx`、`st_size`。绑定分 `STB_LOCAL`(0)/`STB_GLOBAL`(1)/`STB_WEAK`(2)，类型分 `STT_NOTYPE`(0)/`STT_OBJECT`(1)/`STT_FUNC`(2)/`STT_TLS`(6)；全局与弱符号参与跨模块解析，弱符号允许被强定义覆盖。

```c
typedef struct elf64_rela {
  Elf64_Addr r_offset;	/* Location at which to apply the action */
  Elf64_Xword r_info;	/* index and type of relocation */
  Elf64_Sxword r_addend;	/* Constant addend used to compute value */
} Elf64_Rela;
```

（`include/uapi/linux/elf.h:190`；`r_info` 高 32 位为符号下标、低 32 位为类型，见 `elf.h:171-172`）

AArch64 用户态最核心的是 `R_AARCH64_RELATIVE`=1027（`arch/arm64/include/asm/elf.h:69`），它不含符号，公式为 `*r_offset = load_bias + r_addend`，正是 PIE 与共享库搬迁的主力；`R_AARCH64_ABS64`=257、`R_AARCH64_CALL26`=283、`R_AARCH64_ADR_PREL_PG_HI21`=275 则多见于 ET_REL（模块、`.o`）阶段。

### 2.5 文件布局

文件从偏移 0 开始是 64 字节 Ehdr；`e_phoff` 指向连续的 Phdr 数组（每项 56 字节），`e_shoff` 指向 Shdr 数组；段内容按各自的 `p_offset`/`p_filesz` 散布在两者之间，`p_memsz` 超出 `p_filesz` 的部分（`.bss`）不占文件空间，由内核映射后用匿名页补零。

---

## 3. 内核加载流程：load_elf_binary

### 3.1 总览

| 顺序 | 动作 | 位置 |
| --- | --- | --- |
| 1-2 | 校验 magic/`e_type`/`e_machine`/可 mmap；读程序头表 | `fs/binfmt_elf.c:856-867`、`:520` |
| 3-4 | 找 `PT_INTERP` 并 `open_exec()`；扫 `PT_GNU_STACK` | `:874-936`、`:939-955` |
| 5-6 | 解析 `PT_GNU_PROPERTY`；`begin_new_exec()` 进入不可回退点 | `:993-1007`、`fs/exec.c:1110` |
| 7 | `SET_PERSONALITY2()`、`setup_arg_pages()` | `:1016-1031` |
| 8 | 逐个 `PT_LOAD` 映射并计算 `load_bias` | `:1042-1243` |
| 9 | 装载解释器，得到真正入口 | `:1253-1284` |
| 10 | `set_binfmt()`、vDSO、`create_elf_tables()` | `:1288-1299` |
| 11-12 | 设置 `mm->start_code/end_code/start_brk`；`START_THREAD()` | `:1301-1342`、`:1379` |

前置校验只有五行，失败一律返回 `-ENOEXEC` 以便继续尝试别的格式：

```c
	if (memcmp(elf_ex->e_ident, ELFMAG, SELFMAG) != 0)
		goto out;

	if (elf_ex->e_type != ET_EXEC && elf_ex->e_type != ET_DYN)
		goto out;
	if (!elf_check_arch(elf_ex))
		goto out;
	if (elf_check_fdpic(elf_ex))
		goto out;
	if (!can_mmap_file(bprm->file))
		goto out;
```

（`fs/binfmt_elf.c:857-867`）

`elf_ex` 指向 `bprm->buf`（`:849`），该缓冲区只有 `BINPRM_BUF_SIZE`=256 字节（`include/uapi/linux/binfmts.h:19`），由 `prepare_binprm()` 从文件头读入（`fs/exec.c:1628`）——64 字节 Ehdr 够用，但程序头表必须单独读，这就是 `load_elf_phdrs()` 存在的原因：

```c
	if (elf_ex->e_phentsize != sizeof(struct elf_phdr))
		goto out;

	size = sizeof(struct elf_phdr) * elf_ex->e_phnum;
	if (size == 0 || size > 65536)
		goto out;

	elf_phdata = kmalloc(size, GFP_KERNEL);
```

（`fs/binfmt_elf.c:531-540`）

三道闸门分工明确：`e_phentsize` 必须等于内核编译期结构体大小（拒绝 ABI 不匹配）；`e_phnum` 为 0 或总大小超过 64 KiB 直接失败，防止超大 `e_phnum` 触发整数溢出与内存耗尽。

### 3.2 映射 PT_LOAD 与 BSS 清零

`elf_map()` 负责文件页映射，`elf_load()` 在其上补齐 BSS。前者有个易忽略的细节：ET_DYN 第一次映射要按 `total_size` 映射整幅镜像，再把多余部分 unmap，否则 ASLR 选出的地址可能与镜像自身重叠：

```c
	if (total_size) {
		total_size = ELF_PAGEALIGN(total_size);
		map_addr = vm_mmap(filep, addr, total_size, prot, type, off);
		if (!BAD_ADDR(map_addr))
			vm_munmap(map_addr+size, total_size-size);
	} else
		map_addr = vm_mmap(filep, addr, size, prot, type, off);
```

（`fs/binfmt_elf.c:394-400`；`total_size` 由 `total_mapping_size()` 按所有 PT_LOAD 的 `[p_vaddr, p_vaddr+p_memsz)` 跨度算出，`:463`）

BSS 分两段处理：文件尾页中 `p_filesz` 之后到页边界的残留必须清掉（否则把文件垃圾当数据，`padzero()` 用 `clear_user()`，`:127`），`p_filesz` 之后新增的整页则用匿名私有映射补零，`vm_brk_flags()` 的第三参数保留 `PROT_EXEC`，让需要可执行 BSS 的架构仍能工作（`:444-459`）。

单个 PT_LOAD 的映射入口是 `elf_load(bprm->file, load_bias + vaddr, elf_ppnt, elf_prot, elf_flags, total_size)`（`:1188`），返回值统一用 `BAD_ADDR()`（`>= TASK_SIZE`，`:105`）检查，错误经 `IS_ERR_VALUE()` 区分指针错误码与普通 `-EINVAL`（`:1190-1194`）。

第一个 PT_LOAD 的 `elf_flags` 按类型分三支：ET_EXEC 用 `MAP_FIXED_NOREPLACE`；ET_DYN 带 PT_INTERP（PIE）先算 `load_bias` 再用 `MAP_FIXED_NOREPLACE`；ET_DYN 无 PT_INTERP（`ld.so` 自身）交给 mmap 区自选地址。后续 PT_LOAD 一律 `MAP_FIXED`，因为整幅区间已被第一次映射占住。

### 3.3 AT_PHDR、brk 与段边界

`AT_PHDR` 不从 `PT_PHDR` 直接取，而是按"程序头表所在的那个 PT_LOAD"换算，这样链接器即便不发 `PT_PHDR` 也能正确：`phdr_addr = e_phoff - p_offset + p_vaddr`（`:1209-1213`），循环结束后再加 `load_bias`（`:1246`）。`elf_brk` 取所有 PT_LOAD 中 `p_vaddr + p_memsz` 的最大值再加 `load_bias`；ASLR 开启时先在 `.bss` 与 brk 之间留一页空隙再随机化（`arch_randomize_brk()`，`:1332-1342`），静态 PIE 则先把 brk 搬到 `ELF_ET_DYN_BASE`（`:1323-1330`）。`start_code/end_code/start_data/end_data` 分别取带 `PF_X` 段的最小/最大 vaddr 与所有段边界，写回 `mm` 供 `/proc/PID/stat`、`/proc/PID/maps` 使用（`:1215-1251`、`:1301-1306`）。

### 3.4 auxv：内核递给用户态的第一手信息

`create_elf_tables()` 在栈上依次铺设 argc、argv 指针数组、envp 指针数组与辅助向量；auxv 先用 `NEW_AUX_ENT` 成对写入 `mm->saved_auxv`，末尾 `memset` 清零到 `AT_NULL`，最后整块拷到栈上（`:293-298`、`:361`）。

```c
	NEW_AUX_ENT(AT_HWCAP, ELF_HWCAP);
	NEW_AUX_ENT(AT_PAGESZ, ELF_EXEC_PAGESIZE);
	NEW_AUX_ENT(AT_CLKTCK, CLOCKS_PER_SEC);
	NEW_AUX_ENT(AT_PHDR, phdr_addr);
	NEW_AUX_ENT(AT_PHENT, sizeof(struct elf_phdr));
	NEW_AUX_ENT(AT_PHNUM, exec->e_phnum);
	NEW_AUX_ENT(AT_BASE, interp_load_addr);
	if (bprm->interp_flags & BINPRM_FLAGS_PRESERVE_ARGV0)
		flags |= AT_FLAGS_PRESERVE_ARGV0;
	NEW_AUX_ENT(AT_FLAGS, flags);
	NEW_AUX_ENT(AT_ENTRY, e_entry);
```

（`fs/binfmt_elf.c:250-260`）

| 键 | 值 | 来源 |
| --- | --- | --- |
| `AT_PHDR`(3) | 主程序程序头表的**虚拟地址** | 上面的 1209-1213 推算 |
| `AT_PHENT`(4)/`AT_PHNUM`(5) | `sizeof(Elf64_Phdr)`=56 / 主程序 `e_phnum` | 编译期常量 / Ehdr |
| `AT_BASE`(7) | 解释器装载基址；静态程序为 0 | `load_elf_interp()` 返回值 |
| `AT_ENTRY`(9) | 主程序 `e_entry + load_bias` | `e_entry` 变量（非解释器入口） |
| `AT_HWCAP`(16) | CPU 特性位图 | `ELF_HWCAP` → `cpu_get_elf_hwcap()` |
| `AT_RANDOM`(25) | 16 字节随机数地址 | `get_random_bytes()`（`:226`） |
| `AT_SECURE`(23)/`AT_EXECFN`(31) | setuid 布尔 / 可执行文件路径地址 | `bprm->secureexec` / `bprm->exec` |
| `AT_SYSINFO_EHDR`(33) | vDSO 基址 | arm64 `ARCH_DLINFO` |

`AT_RANDOM` 是栈上 16 字节的地址，glibc 用它初始化栈保护 canary 与指针混淆，是用户态 ASLR 的关键一环。

### 3.5 栈布局

```
高地址
  ┌──────────────────────────┐
  │ 参数字符串 / 环境字符串    │  mm->arg_start .. arg_end
  ├──────────────────────────┤
  │ auxv：AT_* 成对，AT_NULL 结尾│  AT_RANDOM/AT_PLATFORM 指向的随机数与平台串在附近
  ├──────────────────────────┤
  │ NULL / envp[n-1..0]      │
  │ NULL / argv[argc-1..0]   │
  │ argc                     │  ← sp（mm->start_stack）
  └──────────────────────────┘
低地址（栈向低地址增长）
```

栈顶先由 `randomize_stack_top(STACK_TOP)` 随机化（`:1028`），再由 `setup_arg_pages()`（`fs/exec.c:602`）把临时栈从 `STACK_TOP_MAX` 迁到最终位置并扩展：`setup_arg_pages() → arch_align_stack() → PAGE_ALIGN() → relocate_vma_down() → expand_stack_locked()`。argv/envp 字符串总量受 `bprm_stack_limits()` 约束：不超过 `max(_STK_LIM/4*3, ARG_MAX)` 与 `rlim_stack/4` 的较小者，并预留指针数组空间（`fs/exec.c:402-445`）。最后 `ELF_PLAT_INIT` 清 x0（`arch/arm64/include/asm/elf.h:159`，表示无 atexit 处理器），`START_THREAD()` 把 PC 设为入口、PSTATE 设为 `PSR_MODE_EL0t`（`arch/arm64/include/asm/processor.h:339-345`）。

---

## 4. 解释器与动态链接

### 4.1 PT_INTERP 与 load_elf_interp

读到 `PT_INTERP` 后，`open_exec()` 得到的解释器文件暂存 `bprm->interpreter`；但真正进入解释器代码要靠 `load_elf_interp()` 把它映射进同一地址空间。`load_addr` 初值来自主程序的 `load_bias`：若 `no_base && ET_DYN`，先置 `-vaddr` 让第一个段落到 0 附近，再由 `load_addr = map_addr - ELF_PAGESTART(vaddr)` 反推真实基址（`fs/binfmt_elf.c:684-700`）。函数返回 `load_addr` 后，主流程得到 `interp_load_addr` 并把入口改为 `interp_load_addr + interp_elf_ex->e_entry`（`:1263-1264`）。于是三者分工为：**PC 进入 `ld.so` 的 e_entry，`AT_BASE` 告诉 `ld.so` 自己在哪里，`AT_ENTRY`/`AT_PHDR` 告诉它主程序在哪里**；`ld.so` 的 `_dl_start` 正是靠 `AT_PHDR` 找到主程序的 `PT_DYNAMIC`。

### 4.2 为什么必须重定位

静态链接时编译器不知道共享库最终落在哪个地址，于是把"地址未知"的引用留成占位指令/数据加一条重定位记录；装载时 `ld.so` 按 `S + A - P` 或 `B + A` 把真实值填回。相关动态标签（`include/uapi/linux/elf.h:81-124`）：

| 动态标签 | 值 | 用途 |
| --- | --- | --- |
| `DT_NEEDED` | 1 | 依赖的 `.so` 名（字符串表下标） |
| `DT_STRTAB`/`DT_SYMTAB` | 5/6 | `.dynstr` / `.dynsym` 地址 |
| `DT_RELA`/`DT_RELASZ` | 7/8 | `.rela.dyn` 地址与大小 |
| `DT_JMPREL`/`DT_PLTGOT` | 23/3 | `.rela.plt` 与 GOT 基址 |
| `DT_GNU_HASH` | `0x6ffffef5` | 符号查找哈希表 |
| `DT_RELACOUNT` | `0x6ffffff9` | 前置 `R_AARCH64_RELATIVE` 个数，可批量处理 |

`ld.so` 自举顺序：

```
_start → 解析 auxv（AT_PHDR/PHNUM/ENTRY/BASE/HWCAP）
 → 读主程序 PT_DYNAMIC，收集 DT_NEEDED
 → 递归装载依赖 .so（各自 mmap PT_LOAD）
 → 符号解析（DT_SYMTAB/STRTAB/GNU_HASH），按依赖顺序
 → 处理 .rela.dyn（R_AARCH64_RELATIVE 可批量）
 → 处理 .rela.plt 填 GOT（惰性绑定留到首次调用）
 → mprotect(PT_GNU_RELRO 区间, PROT_READ) 固化 GOT 只读部分
 → 调用各库 init / 主程序 _init，跳到 AT_ENTRY
```

### 4.3 RELRO / PLT 与内核边界

调用 `foo@plt` 时，`.plt` 用 `adrp/add` 把控制流交给 `.got.plt[foo]`：首次调用前该槽指向 resolver 桩，绑定后写回真实地址；RELRO 之后 `.got.plt` 前段被 `mprotect` 成只读。

容易误解的一点：**内核完全不处理 `PT_GNU_RELRO`**。全树检索只有 UAPI 定义（`include/uapi/linux/elf.h:42`）、kunit 测试与旧笔记，`fs/` 下没有任何内核代码读取它；RELRO 的 `mprotect` 是 `ld.so` 自己做的。同理 `PT_TLS` 不被内核消费——TLS 块由 `ld.so` 按 `p_filesz/p_memsz/p_align` 分配，再通过 `tpidr_el0` 设置线程指针。

| 职责 | 内核 | ld.so |
| --- | --- | --- |
| 建立 PT_LOAD 的 VMA 与权限、页内 BSS 清零、栈/auxv | 是 | 否 |
| 装依赖库、符号解析、重定位、RELRO、TLS | 否 | 是 |
| BTI 保护位（arm64） | 是（按 GNU property） | 给自身以外的库补设 |

---

## 5. PIE 与 ASLR

### 5.1 load_bias 的三种来源

ET_DYN 第一次映射前必须决定 `load_bias`，代码把"程序"和"装载器"分开处理：

```c
			if (interpreter) {
				/* On ET_DYN with PT_INTERP, we do the ASLR. */
				load_bias = ELF_ET_DYN_BASE;
				if (current->flags & PF_RANDOMIZE)
					load_bias += arch_mmap_rnd();
				/* Adjust alignment as requested. */
				if (alignment)
					load_bias &= ~(alignment - 1);
				elf_flags |= MAP_FIXED_NOREPLACE;
			} else {
				/*
				 * For ET_DYN without PT_INTERP, we rely on
				 * the architectures's (potentially ASLR) mmap
				 * base address (via a load_bias of 0).
				 */
```

（`fs/binfmt_elf.c:1138-1152`）

| 形态 | 判定 | 基址选择 | 典型例子 |
| --- | --- | --- | --- |
| 非 PIE 可执行 | ET_EXEC | 0，按 `p_vaddr` 固定 | 部分静态二进制 |
| PIE 程序 | ET_DYN + PT_INTERP | `ELF_ET_DYN_BASE + arch_mmap_rnd()` | 发行版 `/bin/ls` |
| 静态 PIE / 装载器 | ET_DYN 无 PT_INTERP | 0（交给 mmap 区选地址） | `ld.so`、static-pie busybox |

两类 ET_DYN 必须分开的原因写在源码注释里：`ld.so` 可能被直接执行（`./ld.so someprog`）再装载程序，若二者共享同一段地址区间就会自相冲突，因此 `ELF_ET_DYN_BASE` 只给"程序"用，装载器走独立的 mmap 区（`fs/binfmt_elf.c:1109-1136`）。arm64 的基址特意取在 4 GiB 以上，把整个 32 位地址空间留给需要 32 位指针的兼容场景：

```c
#ifdef CONFIG_ARM64_FORCE_52BIT
#define ELF_ET_DYN_BASE		(2 * TASK_SIZE_64 / 3)
#else
#define ELF_ET_DYN_BASE		(2 * DEFAULT_MAP_WINDOW_64 / 3)
#endif /* CONFIG_ARM64_FORCE_52BIT */
```

（`arch/arm64/include/asm/elf.h:130-134`）

### 5.2 随机化来源

| 随机化对象 | 内核实现 | arm64 参数 |
| --- | --- | --- |
| PIE / mmap 基址 | `arch_mmap_rnd()`，`mm/util.c:399` | `mmap_rnd_bits`，上下限见 `arch/arm64/Kconfig:296-320` |
| 栈顶 | `randomize_stack_top()`，`mm/util.c:341` | `STACK_RND_MASK`≈1 GiB VA（`arch/arm64/include/asm/elf.h:195`） |
| brk | `arch_randomize_brk()`，`mm/util.c:390`（weak） | 默认 1 GiB 窗口内 `randomize_page` |

`randomize_stack_top()` 只在 `PF_RANDOMIZE` 时取 `get_random_long() & STACK_RND_MASK << PAGE_SHIFT` 并从栈顶扣减（`mm/util.c:341-355`）。`PF_RANDOMIZE` 由 `load_elf_binary()` 设置，条件是 `randomize_va_space` 非零且进程没有 `ADDR_NO_RANDOMIZE` personality（`fs/binfmt_elf.c:1020-1022`）。三级语义对应 sysctl：0 全关；1 只随机化 mmap/栈/vDSO；2 再加上 brk（`snapshot_randomize_va_space > 1` 分支，`:1332`）。

### 5.3 进程地址空间示意

```
 0x0000_0000_0000_0000
   PIE 基址 ≈ 2/3 × DEFAULT_MAP_WINDOW_64 + 随机偏移
        │ .text .rodata .data .bss
        ▼
       brk / heap ── 向上增长（ASLR 时单独随机）
       mmap 区（.so、线程栈）── 向下增长
       [vDSO] ← AT_SYSINFO_EHDR
       [stack] ← randomize_stack_top(STACK_TOP)
 0x0000_ffff_ffff_ffff   DEFAULT_MAP_WINDOW_64（VA_BITS_MIN=48）
```

arm64 常量见 `arch/arm64/include/asm/memory.h:43,55-56` 与 `arch/arm64/include/asm/processor.h:55-56,76-93`：`VA_BITS` 是编译期 `CONFIG_ARM64_VA_BITS`，`TASK_SIZE_64` 取运行期 `vabits_actual`，`DEFAULT_MAP_WINDOW_64` 固定为 `1 << VA_BITS_MIN`（48 位）。所以 52 位 VA 内核下用户态默认仍只能用低 48 位窗口，超出部分要靠 opt-in 的 hint 地址。

---

## 6. arm64 相关：EM_AARCH64、HWCAP、BTI/PAC 与 VA

### 6.1 机器校验与 personality

`elf_check_arch()` 只比 `e_machine`（`arch/arm64/include/asm/elf.h:97`：`((x)->e_machine == EM_AARCH64)`，`EM_AARCH64`=183 见 `include/uapi/linux/elf-em.h:46`）；AArch32 走 `compat_elf_check_arch()`（`elf.h:216-217`），另查 `EF_ARM_EABI_MASK` 等标志。`SET_PERSONALITY` 清 `TIF_32BIT` 与 `READ_IMPLIES_EXEC`（`arch/arm64/include/asm/elf.h:161-165`），平台字符串 `"aarch64"` 见 `elf.h:87-92`，最终出现在 `AT_PLATFORM`。

### 6.2 ELF_HWCAP 与 AT_HWCAP

arm64 把 CPU 特性位图折算成 `AT_HWCAP`：`cpu_get_elf_hwcap()` 返回 `elf_hwcap[0]`（`arch/arm64/kernel/cpufeature.c:3911-3919`），并保证 62/63 位恒为 0；`ELF_HWCAP/2/3` 宏在 `arch/arm64/include/asm/hwcap.h:72-74`，后两者由 `create_elf_tables()` 作为 `AT_HWCAP2/3` 下发。位图由 `arm64_elf_hwcaps[]`（`cpufeature.c:3283`）在 CPU 探测时置位，与本文相关的几位：

| 位 | 宏 | 用户态用途 |
| --- | --- | --- |
| 1 / 9 | `HWCAP_ASIMD` / `HWCAP_FPHP` | NEON / 半精度浮点 |
| 30 / 31 | `HWCAP_PACA` / `HWCAP_PACG` | 地址认证 / 通用认证能力 |
| `ID_AA64PFR1.BT` | `KERNEL_HWCAP_BTI` | BTI 支持（`cpufeature.c:3357`） |
| `ID_AA64PFR1.MTE` | `KERNEL_HWCAP_MTE` | 内存标记 |

（`arch/arm64/include/uapi/asm/hwcap.h:27-57`）`AT_HWCAP` 的意义是：内核已探测过 CPU，用户态不必再解 ID 寄存器，直接按位选实现——glibc 的 ifunc 就以它为输入。

### 6.3 BTI：从 GNU property 到 PROT_BTI

```
PT_GNU_PROPERTY
 → parse_elf_properties() fs/binfmt_elf.c:775 → parse_elf_property() :728
 → arch_parse_elf_property() arch/arm64/include/asm/elf.h:260
 → arch_state.flags |= ARM64_ELF_BTI
 → make_prot() fs/binfmt_elf.c:625 → arch_elf_adjust_prot() process.c:963
 → prot |= PROT_BTI (0x10) → elf_map() → 页表属性
```

解析端只认 `GNU_PROPERTY_AARCH64_FEATURE_1_AND`（`0xc0000000`）里的 BTI 位，且要求硬件真支持：

```c
	if (type == GNU_PROPERTY_AARCH64_FEATURE_1_AND) {
		const u32 *p = data;

		if (datasz != sizeof(*p))
			return -ENOEXEC;

		if (system_supports_bti() &&
		    (*p & GNU_PROPERTY_AARCH64_FEATURE_1_BTI))
			arch->flags |= ARM64_ELF_BTI;
	}
```

（`arch/arm64/include/asm/elf.h:268-277`）

生效端有一段值得记住的非对称逻辑：动态链接时解释器负责给"除自己以外"的所有可执行映射加 `PROT_BTI`，所以内核只对 `is_interp == has_interp` 的情形下手：

```c
	if (is_interp != has_interp)
		return prot;

	if (!(state->flags & ARM64_ELF_BTI))
		return prot;

	if (prot & PROT_EXEC)
		prot |= PROT_BTI;

	return prot;
```

（`arch/arm64/kernel/process.c:971-981`，前置注释说明解释器负责其余映射）

带 PT_INTERP 的程序，内核只给主程序自己的 `PF_X` 段加 BTI，`ld.so` 再用同样的 property 判定给依赖库加；静态二进制没有解释器，内核一次给所有 X 段加上。`PROT_BTI` 定义在 `arch/arm64/include/uapi/asm/mman.h:7`（`0x10`），`mprotect` 的支持掩码在 `arch/arm64/include/asm/mman.h:62`。BTI 只保护间接跳转目标，所以入口与 PLT 桩必须落在 `bti c`/`bti j` 指令上（指令本身 4 字节对齐），这由链接器在 `-mbranch-protection=bti` 下完成，内核不重写指令。

### 6.4 PAC：内核只报告、不介入

PAC 常与 BTI 并提，但内核侧处理完全不同：`GNU_PROPERTY_AARCH64_FEATURE_1_PAC`（`1U << 1`）只出现在 `arch/arm64/include/asm/assembler.h:784` 的汇编宏里，内核的 `arch_parse_elf_property()` **只读 BTI 位**；签名/验签指令（`PACIASP`/`AUTIASP`、`PACIA`/`AUTIA`）由编译器按 `-mbranch-protection=pac-ret` 插入，用户态运行不需要内核建立页属性。内核暴露的是能力位 `HWCAP_PACA`/`HWCAP_PACG`（`uapi/asm/hwcap.h:56-57`，由 `cpufeature.c:3360-3361` 决定），以及在 ptrace/coredump 中保存的 `NT_ARM_PAC_MASK`(0x406)、`NT_ARM_PACA_KEYS`(0x407)、`NT_ARM_PACG_KEYS`(0x408)（`include/uapi/linux/elf.h:510-515`）。一句话：BTI 是"页属性问题"，内核参与；PAC 是"指令与密钥问题"，内核只做能力上报与上下文保存。

### 6.5 vDSO 与 AT_SYSINFO_EHDR

vDSO 在 `load_elf_binary()` 末尾通过 `ARCH_SETUP_ADDITIONAL_PAGES` 安装（`fs/binfmt_elf.c:1290-1294`），`arch_setup_additional_pages()`（`arch/arm64/kernel/vdso.c:333`）用 `get_unmapped_area()` 找空位并建立特殊映射（`vdso.c:99-133`），随后 `ARCH_DLINFO` 把地址写进 auxv：

```c
#define ARCH_DLINFO							\
do {									\
	NEW_AUX_ENT(AT_SYSINFO_EHDR,					\
		    (elf_addr_t)current->mm->context.vdso);		\
									\
	/*								\
	 * Should always be nonzero unless there's a kernel bug.	\
	 * If we haven't determined a sensible value to give to		\
	 * userspace, omit the entry:					\
	 */								\
	if (likely(signal_minsigstksz))					\
		NEW_AUX_ENT(AT_MINSIGSTKSZ, signal_minsigstksz);	\
	else								\
		NEW_AUX_ENT(AT_IGNORE, 0);				\
} while (0)
```

（`arch/arm64/include/asm/elf.h:168-182`，`AT_MINSIGSTKSZ` 是信号传递所需的最小栈）

arm64 的 `AT_VECTOR_SIZE_ARCH` 因此为 2（`arch/arm64/include/uapi/asm/auxvec.h:24`），`mm->saved_auxv[AT_VECTOR_SIZE]` 的大小在 `include/linux/mm_types.h:1293` 由 `AT_VECTOR_SIZE_BASE`(24，`include/linux/auxvec.h:7`) 加 arch 项算出。vDSO 本身也是 ELF 映像（`vdso.c:74` 校验 `\177ELF`），glibc 用 `AT_SYSINFO_EHDR` 定位并解析符号，从而绕过 syscall 直接读时钟。

---

## 7. coredump：ET_CORE 视角

### 7.1 输出结构

`elf_core_dump()`（`fs/binfmt_elf.c:1993`）是两遍扫描：先算偏移写头，再回填数据。布局固定为"Ehdr → Phdr[] → notes → 页对齐 → VMA 数据"：

```
┌──────────────────────────────┐
│ Elf64_Ehdr (e_type = ET_CORE)│
├──────────────────────────────┤
│ Phdr[0] PT_NOTE              │ → notes 区
│ Phdr[1..n] PT_LOAD (每个 VMA) │ → 内存快照
│ [+ arm64 MTE 附加 Phdr]      │   p_type = PT_AARCH64_MEMTAG_MTE
├──────────────────────────────┤
│ notes（每线程 PRSTATUS + 全局）│
├── 页对齐 ────────────────────┤
│ VMA 数据（按地址或大小排序）   │
```

头部填写只有十几行，`e_type` 直接定为 `ET_CORE`：`memcpy(elf->e_ident, ELFMAG, SELFMAG)` 后依次填 `EI_CLASS/EI_DATA/EI_VERSION/EI_OSABI`（取自 `ELF_CLASS/ELF_DATA/ELF_OSABI`，arm64 定义见 `arch/arm64/include/asm/elf.h:74-80`），再写 `e_type=ET_CORE`、`e_machine`、`e_phoff=sizeof(struct elfhdr)`、`e_phentsize`、`e_phnum`（`fs/binfmt_elf.c:1442-1455`）。段数可能超过 16 位，于是走扩展编号：`e_phnum = PN_XNUM`(0xffff)，真实数量写进 `Shdr[0].sh_info`（`:2014-2017` 与 `fill_extnum_info()`，`:1970`）——这也是内核唯一写节头的场合。

### 7.2 notes 内容

| 名称 | `n_type` | 写入者 | 内容 |
| --- | --- | --- | --- |
| `CORE`/`PRSTATUS` | 1 | `fill_prstatus()`（`:1486`） | 每线程信号、pid/ppid、累计时间、`pr_reg` 寄存器集 |
| `CORE`/`PRPSINFO` | 3 | `fill_psinfo()`（`:1520`） | comm、状态、uid/gid、`pr_psargs`（最多 80 字节） |
| `CORE`/`AUXV` | 6 | `fill_auxv_note()`（`:1565`） | 崩溃时的完整 auxv |
| `CORE`/`SIGINFO` | `0x53494749` | `fill_siginfo_note()`（`:1575`） | `siginfo` 转用户态布局 |
| `CORE`/`FILE` | `0x46494c45` | `fill_files_note()`（`:1593`） | VMA→文件映射表（count/page_size/start/end/file_ofs/名） |
| `LINUX`/`NT_ARM_*` | 0x400+ | `fill_thread_core_info()`（`:1720`） | SVE/SME/MTE/PAC 等 regset |

`fill_thread_core_info()` 的注释说明关键约定：`NT_PRSTATUS` 是特例，寄存器数据塞进 note 内容里的 `pr_reg` 字段而非整个 note 内容；其余 regset 各自生成独立 note，名字由 `regset->core_note_name` 给出（`fs/binfmt_elf.c:1726-1779`）。arm64 定义了 `CORE_DUMP_USE_REGSET`（`arch/arm64/include/asm/elf.h:122`），因此走 regset 分支。

### 7.3 core_pattern 与管道

默认输出文件名是 `core`（`fs/coredump.c:85`）。`core_pattern` 首字符决定去向：`|` 走管道、`@` 走 socket、其余当路径。管道模式通过 usermode helper 启动用户程序，把 core 写到其 stdin：

```c
	sub_info = call_usermodehelper_setup(helper_argv[0], helper_argv, NULL,
					     GFP_KERNEL, umh_coredump_setup,
					     NULL, cprm);
	if (!sub_info)
		return false;

	if (call_usermodehelper_exec(sub_info, UMH_WAIT_EXEC)) {
		coredump_report_failure("|%s pipe failed", cn->corename);
		return false;
	}
```

（`fs/coredump.c:1017-1026`；`core_pipe_limit` 防止 core 处理程序自己崩溃形成递归，`:1002-1006`）

```
do_coredump()            fs/coredump.c:1092
 → coredump_parse()     core_pattern 占位符（%p/%e/%s…）展开
 → dump_vma_snapshot()  mmap_write_lock 下快照 VMA（:1720）
 → vma_dump_size()      按 MMF_DUMP_* 过滤，不 dump 的段 size=0
 → coredump_write() → binfmt->core_dump() == elf_core_dump()
 → write_note_info() → dump_emit() → dump_user_range() 逐页写入
 → arm64: elf_core_write_extra_phdrs()/extra_data() 写 MTE 标签
```

### 7.4 arm64 的 MTE 附加段

开启 MTE 的进程，coredump 除 VMA 数据外还要保存每页 tag。arm64 用 `PT_AARCH64_MEMTAG_MTE`（`PT_LOPROC+2`，`include/uapi/linux/elf.h:47`）作为独立 `PT_LOAD` 描述 tag 数据区，实现在 `arch/arm64/kernel/elfcore.c:79-114`。这就是 `elf_core_dump()` 中到处是 `elf_core_extra_phdrs()`/`elf_core_extra_data_size()` 钩子的原因：让体系结构在不改主流程的前提下追加段。

---

## 8. 模块 ELF 与用户态 ELF 的差异

`.ko` 是 **ET_REL**，与用户态 ELF 共享同一套格式定义，但加载路径完全不同：

| 维度 | 用户态 ELF | 内核模块 .ko |
| --- | --- | --- |
| `e_type` | `ET_EXEC`/`ET_DYN` | `ET_REL`（`kernel/module/main.c:1908` 强制） |
| 主要视图 | 程序头（段） | 节头（每个 `SHF_ALLOC` 节被搬运） |
| 地址与符号 | 内核 mmap 定址；ld.so 运行时绑定 | `move_module()` 定址；`simplify_symbols()` 加载期绑定内核导出符号 |
| 重定位 | ld.so 处理 `R_AARCH64_RELATIVE` 等 | 内核 `apply_relocate_add()` 逐条处理 |
| 校验 | magic + `e_machine` + 可 mmap | magic + ET_REL + `elf_check_arch` + 节头完整性 |

```c
	if (memcmp(info->hdr->e_ident, ELFMAG, SELFMAG) != 0) {
		pr_err("Invalid ELF header magic: != %s\n", ELFMAG);
		return -ENOEXEC;
	}
	if (info->hdr->e_type != ET_REL) {
		pr_err("Invalid ELF header type: %u != %u\n",
		       info->hdr->e_type, ET_REL);
		return -ENOEXEC;
	}
```

（`kernel/module/main.c:1904-1912`）

重定位分派只看节类型，另有一条 livepatch 专用旁路：

```c
		if (info->sechdrs[i].sh_flags & SHF_RELA_LIVEPATCH)
			err = klp_apply_section_relocs(mod, info->sechdrs,
						       info->secstrings,
						       info->strtab,
						       info->index.sym, i,
						       NULL);
		else if (info->sechdrs[i].sh_type == SHT_REL)
			err = apply_relocate(info->sechdrs, info->strtab,
					     info->index.sym, i, mod);
		else if (info->sechdrs[i].sh_type == SHT_RELA)
			err = apply_relocate_add(info->sechdrs, info->strtab,
						 info->index.sym, i, mod);
```

（`kernel/module/main.c:1631-1642`）

AArch64 的 `apply_relocate_add()`（`arch/arm64/kernel/module.c:231`）与用户态公式一致：`loc` 即 ELF 规范中的 P（`sh_info` 指向目标节基址加 `r_offset`），`sym` 由 `ELF64_R_SYM(r_info)` 索引，`val = sym->st_value + r_addend` 即 `S + A`，随后 `switch (ELF64_R_TYPE(...))` 分派 `R_AARCH64_ABS64`/`MOVW_*`/`ADR_PREL_PG_HI21`/`CALL26` 等（`:246-255`）。

差异的**根因**可以一句话概括：模块由内核自己"链接并搬迁"，所以重定位必须由内核执行；用户态程序由 `ld.so` 在虚拟地址空间中搬迁，内核只负责建立映射。模块侧完整流程、节布局与 livepatch 重定位见 [`../../11-build/02-module-loader.md`](../../11-build/02-module-loader.md) 与 [`../../11-build/03-livepatch-kexec.md`](../../11-build/03-livepatch-kexec.md)。

---

## 9. 工具与调试

| 目的 | 命令 |
| --- | --- |
| 看类型与架构 / ELF 头 | `file ./a.out`、`readelf -h ./a.out` |
| 看程序头（段、权限、对齐） | `readelf -lW ./a.out` |
| 看节头 / 动态段 | `readelf -SW ./a.out`、`readelf -dW ./a.out` |
| 看重定位 / GNU property | `readelf -rW ./a.out`、`readelf -nW ./a.out` |
| 反汇编并标注重定位 | `objdump -dr ./a.out` |
| 观察 auxv / 段映射 | `LD_SHOW_AUXV=1 ./a.out`、`cat /proc/self/maps` |

常见加载失败原因：

| 现象/errno | 内核返回点 | 说明 |
| --- | --- | --- |
| `ENOEXEC` | `fs/binfmt_elf.c:857` / `:862` | magic 不是 `\177ELF`、`e_type` 不是 ET_EXEC/ET_DYN，或 `e_machine` 不匹配（如把 x86-64 二进制拷到 arm64） |
| `ELIBBAD` | `fs/binfmt_elf.c:959` | 解释器不是 ELF 或架构不符（`PT_INTERP` 指向错误文件） |
| `ENOENT` | `fs/binfmt_elf.c:907` | `PT_INTERP` 路径在当前 mount namespace 不存在（容器常见） |
| `EACCES` | `fs/exec.c` 的 `open_exec` | 缺执行权限、noexec 挂载，或 `can_mmap_file()` 失败 |
| `E2BIG` | `fs/exec.c:440` | argv+envp 超过 `bprm_stack_limits()` |
| `ENOMEM`/`EINVAL` | `fs/binfmt_elf.c:1226-1231` | `p_memsz` 越界或 `p_filesz > p_memsz`，`set_brk` 无法完成 |
| 日志 `Uhuuh, elf segment at ...` | `fs/binfmt_elf.c:402-405` | 首个 PT_LOAD 用 `MAP_FIXED_NOREPLACE` 撞上已有映射 |
| 段错误在 ld.so 内 | 用户态 | `AT_PHDR`/程序头被破坏，或解释器与主程序 ABI 不匹配 |

`readelf -l` 中若 `INTERP` 段路径不存在会导致 `ENOENT`；若 `GNU_STACK` 显示 `RWE`，`setup_arg_pages()` 会打印 `process '...' started with executable stack`（`fs/exec.c:682-685`）并放宽栈权限。

---

## 10. 关键文件清单

| 文件 | 关键内容 |
| --- | --- |
| `include/uapi/linux/elf.h` | `Elf64_Ehdr`(234)、`Elf64_Phdr`(268)、`Elf64_Shdr`(339)、`Elf64_Rela`(190)、`Elf64_Sym`(205)、`PT_*`(28)、`ET_*`(72)、`DT_*`(81)、note 类型(389-531)；`include/linux/elfcore.h` 的 `elf_prstatus`/`elf_prpsinfo` 布局 |
| `include/linux/elf.h` / `include/uapi/linux/auxvec.h` | `elfhdr`/`elf_phdr` 别名、`START_THREAD`、`arch_elf_adjust_prot` 默认实现、`AT_VECTOR_SIZE_BASE`=24；`AT_*` 键值（`AT_PHDR`=3 … `AT_EXECFN`=31，`AT_MINSIGSTKSZ`=51） |
| `fs/binfmt_elf.c` | `load_elf_binary`(832)、`load_elf_phdrs`(520)、`elf_map`(371)、`elf_load`(415)、`total_mapping_size`(463)、`maximum_alignment`(491)、`load_elf_interp`(645)、`create_elf_tables`(164)、`elf_core_dump`(1993) |
| `fs/exec.c` | `prepare_binprm`(1628)、`bprm_stack_limits`(402)、`setup_arg_pages`(602)、`begin_new_exec`(1110)、`search_binary_handler`(1675)、`exec_binprm`(1709) |
| `fs/binfmt_script.c` / `fs/coredump.c` | `#!` 解析与 `bprm->interpreter` 交接(34、132-137)；`core_pattern`(85)、`coredump_pipe`(974)、`do_coredump`(1092) |
| `arch/arm64/include/asm/elf.h` | `elf_check_arch`(97)、`ELF_ET_DYN_BASE`(130)、`SET_PERSONALITY`(161)、`ARCH_DLINFO`(168)、`STACK_RND_MASK`(191)、`arch_parse_elf_property`(260) |
| `arch/arm64/kernel/process.c` / `vdso.c` / `elfcore.c` | `arch_elf_adjust_prot`(963)；`arch_setup_additional_pages`(333)；MTE 附加段(79-114) |
| `arch/arm64/kernel/cpufeature.c` / `module.c` / `mm/util.c` | `cpu_get_elf_hwcap`(3911)、`arm64_elf_hwcaps`(3283)；`apply_relocate_add`(231)；`randomize_stack_top`(341)、`arch_randomize_brk`(390)、`arch_mmap_rnd`(399) |
| `kernel/module/main.c` | `elf_validity_ehdr`(1898)、`apply_relocations`(1608) |

---

## 11. 延伸阅读

- [`../sched/fork_exec_analysis.md`](../sched/fork_exec_analysis.md)：fork+exec 全链路，含 `de_thread()`、creds 与栈迁移。
- [`../syscall/process/execve.md`](../syscall/process/execve.md)：execve 系统调用入口与参数处理。
- [`../../01-architecture/armv8a/9.ABI.md`](../../01-architecture/armv8a/9.ABI.md)：AArch64 过程调用标准，理解 PLT、参数寄存器与栈帧的必要背景。
- [`../../11-build/02-module-loader.md`](../../11-build/02-module-loader.md)：ET_REL 模块的节布局、`layout_sections()` 与 `move_module()`。
- [`../../11-build/03-livepatch-kexec.md`](../../11-build/03-livepatch-kexec.md)：`SHF_RELA_LIVEPATCH` 与 livepatch 重定位。

外部规范与手册：System V gABI 与 *ELF for the Arm 64-bit Architecture (AAELF64)*（`R_AARCH64_*` 公式、`PT_GNU_PROPERTY`/BTI 约定）；`man 5 elf`、`man 2 execve`、`man 5 core`；glibc `elf/rtld.c` 的 `_dl_start`。阅读建议：先记牢 Ehdr/Phdr/Rela 三个结构体，再对照 `readelf -l`/`-r` 的输出把字段与真实二进制对上，最后用 `strace` 与 `/proc/PID/maps` 观察一次 `execve`，把第 3 节的步骤与内核建立的分段对齐——这样 ELF 就从"文档里的字节表"变成了"内核里的一串 VMA 操作"。
