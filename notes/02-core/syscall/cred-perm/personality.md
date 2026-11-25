# personality 系统调用分析

## 1. 概述

设置或获取进程的执行域（personality）。执行域影响进程的某些行为，如系统调用接口的 ABI 兼容性、信号处理方式、UNAME 版本伪装等。

**原型：**

```c
SYSCALL_DEFINE1(personality, unsigned int, personality)
```

**参数：**
- `personality`：新的执行域值。若为 `0xffffffff`，则仅查询当前值而不修改

**返回值：** 返回修改前的 personality 值（即旧值）

### 1.1 源码全貌

本树 `kernel/exec_domain.c` 只剩这十几行有效代码——`struct exec_domain` 及其 handler/信号映射机制早已被删除，`personality` 现在只是一个 `task_struct` 里的 `unsigned int` 字段：

```c
SYSCALL_DEFINE1(personality, unsigned int, personality)
{
	unsigned int old = current->personality;

	if (personality != 0xffffffff)
		set_personality(personality);

	return old;
}
```

`set_personality()` 在 `include/linux/personality.h` 里就是一个直接赋值宏，没有加锁、没有任何校验：

```c
#define personality(pers)	(pers & PER_MASK)
#define set_personality(pers)	(current->personality = (pers))
```

同一文件还保留着 `/proc/execdomains` 的桩实现，输出恒为 `0-0 Linux [kernel]`，只是为了让老工具不报错。

两个容易忽略的语义：

1. **查询用 `0xffffffff` 而不是 0**：0 是合法的 `PER_LINUX`，不能当"只查询"的哨兵。
2. **返回值永远是旧值**：包括查询场景。因此"personality() 返回 -1"在 32 位 ABI 上不可能出现，顶层字节被刻意留空（`PER_MASK = 0x00ff`，注释明确说"避免与错误返回冲突"）。

## 2. 使用场景

- 兼容旧版 Linux 程序（如 `PER_LINUX32` 模拟 32 位行为）
- 修改 `uname` 输出（`UNAME26` 特性）
- 查询当前进程的执行域

## 3. 函数调用栈

```
personality(personality)                                 // kernel/exec_domain.c
  ├─ old = current->personality
  ├─ [personality != 0xffffffff] → set_personality(personality)
  │    └─ current->personality = personality
  └─ 返回 old
```

### 3.1 arm64 的覆写实现

arm64 并不直接使用 `sys_personality`，而是在 `arch/arm64/kernel/sys.c` 里提供一个包装：

```c
SYSCALL_DEFINE1(arm64_personality, unsigned int, personality)
{
	if (personality(personality) == PER_LINUX32 &&
		!system_supports_32bit_el0())
		return -EINVAL;
	return ksys_personality(personality);
}

#define __arm64_sys_personality		__arm64_sys_arm64_personality
```

要点：

- 判定时先用 `personality()`（即 `& PER_MASK`）剥掉标志位，所以传 `PER_LINUX32 | UNAME26` 同样会被识别为 `PER_LINUX32`。
- 如果 CPU 不支持 32 位 EL0（例如只实现了 AArch64 的核，或启动时关闭了 compat），设置 `PER_LINUX32` 直接返回 `-EINVAL`。这是 `personality(2)` 少见的、真实存在的错误码。
- `ksys_personality()` 定义在 `include/linux/syscalls.h`，逻辑与 `kernel/exec_domain.c` 中的 `SYSCALL_DEFINE1` 完全一致，供各架构包装复用。PowerPC 的用法更极端：`do_ppc64_personality()` 在"当前是 `PER_LINUX32` 且请求 `PER_LINUX`"时自动把请求改写成 `PER_LINUX32`，并在返回旧值时把 `PER_LINUX32` 反向显示成 `PER_LINUX`——纯粹是 64 位用户态对 32 位 ABI 的兼容伪装。
- 编号方面：arm64 原生表 `personality` = 92，32 位 compat 表 = 136。本树 `include/generated/autoconf.h` 未定义 `CONFIG_COMPAT`，因此 compat 路径（以及 `COMPAT_UTS_MACHINE` 的 `armv8b/armv8l` 分支）在当前配置下不编译。

## 4. 关键常量

```c
// include/uapi/linux/personality.h
enum {
    PER_LINUX       = 0x0000,  // Linux 默认
    PER_LINUX_32BIT = 0x0000 | ADDR_LIMIT_32BIT,
    PER_LINUX_FDPIC = 0x0000 | FDPIC_FUNCPTRS,
    PER_SVR4        = 0x0001,  // System V R4
    PER_SVR3        = 0x0002,  // System V R3
    PER_SCOSVR3     = 0x0003,  // SCO UNIX
    PER_OSR5        = 0x0003,  // SCO OpenServer 5
    PER_WYSEV386    = 0x0004,  // Wyse UNIX
    PER_ISCR4       = 0x0005,  // ISC UNIX
    PER_BSD         = 0x0006,  // BSD
    PER_XENIX       = 0x0007,  // Xenix
    PER_MASK        = 0x00ff,
};

/* 额外标志位 */
#define ADDR_LIMIT_32BIT 0x0800000  // 地址空间限制在 32 位
#define UNAME26          0x0020000  // 伪装 uname 版本为 2.6.x
```

### 4.1 常量的真实位组合

`include/uapi/linux/personality.h` 里上表中的 `PER_SVR4` 等并不是裸编号，而是"基础编号 | 若干行为标志"。完整标志位如下：

| 标志 | 值 | 含义 |
|--|--|--|
| `UNAME26` | 0x0020000 | `uname()` 版本伪装为 2.6.x |
| `ADDR_NO_RANDOMIZE` | 0x0040000 | 关闭该进程的 VA 随机化 |
| `FDPIC_FUNCPTRS` | 0x0080000 | 函数指针指向描述符（FDPIC） |
| `MMAP_PAGE_ZERO` | 0x0100000 | 映射 0 页（SVR4 兼容） |
| `ADDR_COMPAT_LAYOUT` | 0x0200000 | 使用旧式 mmap 布局 |
| `READ_IMPLIES_EXEC` | 0x0400000 | 可读即意味着可执行（无 NX） |
| `ADDR_LIMIT_32BIT` | 0x0800000 | 地址空间限制在 32 位 |
| `SHORT_INODE` | 0x1000000 | 旧式短 inode 编号 |
| `WHOLE_SECONDS` | 0x2000000 | 时间戳只到秒 |
| `STICKY_TIMEOUTS` | 0x4000000 | 修改时间戳后不清除 setuid 位 |
| `ADDR_LIMIT_3GB` | 0x8000000 | 3G/1G 地址划分 |

于是真正的取值是这些标志的组合：

| personality | 实际值 | 组成 |
|--|--|--|
| `PER_LINUX` | 0x0000000 | 无标志 |
| `PER_LINUX32` | 0x0000008 | 仅基础编号 8 |
| `PER_LINUX32_3GB` | 0x8000008 | `ADDR_LIMIT_3GB` |
| `PER_LINUX_32BIT` | 0x0800000 | `ADDR_LIMIT_32BIT` |
| `PER_LINUX_FDPIC` | 0x0080000 | `FDPIC_FUNCPTRS` |
| `PER_SVR4` | 0x4100001 | `STICKY_TIMEOUTS \| MMAP_PAGE_ZERO` |
| `PER_SVR3` | 0x5000002 | `STICKY_TIMEOUTS \| SHORT_INODE` |
| `PER_SCOSVR3` | 0x7000003 | `STICKY_TIMEOUTS \| WHOLE_SECONDS \| SHORT_INODE` |
| `PER_OSR5` | 0x6000003 | `STICKY_TIMEOUTS \| WHOLE_SECONDS` |
| `PER_BSD` | 0x0000006 | 无标志 |
| `PER_SUNOS` | 0x4000006 | `STICKY_TIMEOUTS` |
| `PER_XENIX` | 0x5000007 | `STICKY_TIMEOUTS \| SHORT_INODE` |

`personality(2)` 本身**不校验**这些组合是否合理，用户可以随手传 `0x7fffffff`；真正使用这些位的是 mmap 布局、`uname()`、`execve()` 的 `bprm->per_clear` 等路径。因此"设置 personality"本质上是"给后续系统调用留一个提示"，而不是切换内核里的执行域 handler。

### 4.2 安全标志在 setuid exec 时被清除

头文件里有一个容易被忽略的宏：

```c
/*
 * Security-relevant compatibility flags that must be
 * cleared upon setuid or setgid exec:
 */
#define PER_CLEAR_ON_SETID (READ_IMPLIES_EXEC  | \
			    ADDR_NO_RANDOMIZE  | \
			    ADDR_COMPAT_LAYOUT | \
			    MMAP_PAGE_ZERO)
```

`fs/exec.c` 的 `bprm_fill_uid()` 在处理 setuid/setgid 程序时把它并入 `bprm->per_clear`：

```c
if (mode & S_ISUID) {
	bprm->per_clear |= PER_CLEAR_ON_SETID;
	bprm->cred->euid = vfsuid_into_kuid(vfsuid);
}
```

LSM 侧（`security/commoncap.c`、SELinux、Smack、AppArmor）也会在 `bprm_creds_from_file` 阶段设置同样的掩码。`execve()` 在 `begin_new_exec()` 里执行 `me->personality &= ~bprm->per_clear;` 把这些位清掉。原因是这四个标志都会削弱缓解措施（关 ASLR、允许可读即执行、映射 0 页、旧 mmap 布局），如果让它们跨越"提权 exec"继续生效，就等于让低权限进程给 setuid 程序预设了不安全的地址空间行为。

## 5. 使用示例

```c
#include <sys/personality.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    unsigned int old;

    /* 查询当前 personality */
    old = personality(0xffffffff);
    printf("Current personality: 0x%08x\n", old);

    /* 启用 UNAME26 兼容模式 */
    if (!(old & UNAME26)) {
        old = personality(old | UNAME26);
        printf("Enabled UNAME26 compatibility\n");
    }

    struct utsname buf;
    uname(&buf);
    printf("Kernel release: %s\n", buf.release);

    /* 恢复原始值 */
    personality(old);

    return 0;
}
```

### 5.1 UNAME26 的真实效果

`UNAME26` 只影响 `uname()` 的 `release` 字段，实现藏在 `kernel/sys.c` 的 `override_release()`：

```c
static int override_release(char __user *release, size_t len)
{
	int ret = 0;

	if (current->personality & UNAME26) {
		const char *rest = UTS_RELEASE;
		char buf[65] = { 0 };
		int ndots = 0;
		unsigned v;
		size_t copy;

		while (*rest) {
			if (*rest == '.' && ++ndots >= 3)
				break;
			if (!isdigit(*rest) && *rest != '.')
				break;
			rest++;
		}
		v = LINUX_VERSION_PATCHLEVEL + 60;
		copy = clamp_t(size_t, len, 1, sizeof(buf));
		copy = scnprintf(buf, copy, "2.6.%u%s", v, rest);
		...
	}
}
```

效果取决于 `LINUX_VERSION_PATCHLEVEL`：输出恒为 `2.6.<PATCHLEVEL+60><剩余后缀>`。本树是 7.2，`utsname()->release` 为 `7.2.0`，于是后缀是空的、结果 `2.6.62`；源码注释里"3.x 映射为 2.6.40+x"是早期 2.6/3.0 时代的说法，实际公式只有 `+60` 一处。目的不变——让那些"见到 3.0 就崩溃"的老程序继续能跑。注意它只替换 `release`，`version`/`machine` 等字段不动，而且要求调用 `uname()` 时 `current->personality` 仍带着 `UNAME26`（`newuname()` 在 `copy_to_user()` 之后才调用 `override_release()`）。

### 5.2 PER_LINUX32 影响的两个地方

`PER_LINUX32` 不是一个"假执行域"，而是两个具体行为：

1. **`uname().machine` 覆写**：

```c
#ifdef COMPAT_UTS_MACHINE
#define override_architecture(name) \
	(personality(current->personality) == PER_LINUX32 && \
	 copy_to_user(name->machine, COMPAT_UTS_MACHINE, \
		      sizeof(COMPAT_UTS_MACHINE)))
#else
#define override_architecture(name)	0
#endif
```

arm64 的 `COMPAT_UTS_MACHINE` 是 `"armv8l"`（大端为 `"armv8b"`），因此 64 位内核上的 32 位用户态 `uname()` 会得到 `armv8l`，glibc 据此选择 32 位路径。

2. **`/proc/cpuinfo` 的输出**：`arch/arm64/kernel/cpuinfo.c` 的 `c_show()` 里

```c
bool compat = personality(current->personality) == PER_LINUX32;
...
if (compat)
	seq_printf(m, "model name\t: ARMv8 Processor rev %d (%s)\n",
		   MIDR_REVISION(midr), COMPAT_ELF_PLATFORM);
```

这是给 32 位用户态程序看的"补丁"，因为老工具链会解析 `model name` 判断平台。

### 5.3 错误码小结

| 场景 | 返回值 |
|--|--|
| 任意合法/非法 personality，`personality != 0xffffffff` | 旧值（无错误） |
| 查询（传 `0xffffffff`） | 当前值 |
| arm64 且请求 `PER_LINUX32`、CPU 不支持 32 位 EL0 | `-EINVAL` |
| 32 位 compat ABI | 旧值被截断为 `unsigned int`；`PER_MASK` 保证不会与 `-errno` 混淆 |

`personality(2)` 没有任何权限检查：任何进程都可以修改自己的执行域。这也是为什么安全敏感的位必须在 setuid exec 时清除，而不能依赖"只有特权进程才能设置"。

### 5.4 相关系统调用与文档

| 对象 | 关系 | 笔记 |
|--|--|--|
| `uname` | 读取 `UNAME26`/`PER_LINUX32` 影响的两个字段 | [uname.md](../system-info/uname.md) |
| `execve` | `begin_new_exec()` 清除 `per_clear` 位 | [execve.md](../process/execve.md) |
| `mmap` | 受 `ADDR_LIMIT_32BIT`/`ADDR_COMPAT_LAYOUT`/`MMAP_PAGE_ZERO` 影响 | [mmap.md](../memory/mmap.md) |
| `setarch(8)` | 用户态对 `personality(2)` 的封装（`setarch -R` 即 `ADDR_NO_RANDOMIZE`） | — |

## 6. 参考

- 源码位置：`kernel/exec_domain.c`
- 头文件：`include/uapi/linux/personality.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#10-进程凭证与权限)