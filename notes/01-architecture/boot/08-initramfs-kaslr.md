# initramfs、rootfs 与 KASLR

## 1. 概述

initramfs 是内核启动早期挂载的内存根文件系统，在真实根设备就绪前提供
`/init`、驱动和挂载工具；KASLR 则在启动早期随机化内核镜像虚拟地址。
当前 `.config`：

| 配置 | 值 | 说明 |
| --- | --- | --- |
| `CONFIG_BLK_DEV_INITRD` | y | 支持 initramfs/initrd |
| `CONFIG_INITRAMFS_SOURCE` | `""` | 用默认 cpio 列表 |
| `CONFIG_RD_GZIP` | y | 支持 gzip 压缩 |
| `CONFIG_RD_ZSTD` | n | 不支持 zstd |
| `CONFIG_BLK_DEV_RAM` | n | 无 `/dev/ram0` |
| `CONFIG_INITRAMFS_FORCE` | n | 外部 initrd 可覆盖内建 initramfs |
| `CONFIG_RANDOMIZE_BASE` | **n** | **KASLR 未启用** |
| `CONFIG_RELOCATABLE` | y | 可重定位（KASLR 前提） |
| `CONFIG_ARM64_VA_BITS` | 52 | 52 位虚拟地址 |

因此 initramfs 完整可用；KASLR 代码在编译期被裁掉，panic 时打印
`Kernel Offset: disabled`。

## 2. initramfs 的生成

### 2.1 usr/Makefile

```
usr/Makefile
├─ compress-$(CONFIG_INITRAMFS_COMPRESSION_*)  选择 gzip/bzip2/lzma/xz/lzo/lz4/zstd
├─ obj-$(CONFIG_BLK_DEV_INITRD) := initramfs_data.o
├─ ramfs-input := $(CONFIG_INITRAMFS_SOURCE)
├─ 为空 → $(src)/default_cpio_list
├─ 单个 .cpio → 直接使用，不重复压缩
├─ 否则 → gen_initramfs.sh 生成 initramfs_data.cpio
└─ initramfs_inc_data 依赖 cpio-data
```

`INITRAMFS_SOURCE=""` 时默认内容（`usr/default_cpio_list`）只有：

```
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /root 0700 0 0
```

没有 `/init`，因此内核会继续寻找真实根，或按 `init=` 尝试。

### 2.2 gen_initramfs.sh 与 gen_init_cpio

`usr/gen_initramfs.sh` 是构建期脚本：解析 `-l`（依赖列表）、`-o`（输出）、
`-u/-g`（uid/gid 映射）、`-d`（mtime）；`input_file()` 把普通文件加入
`cpio_list`，`.cpio` 直接记录，目录交给 `dir_filelist()` 递归；最后调用
`usr/gen_init_cpio $output $timestamp $cpio_list`。

`gen_init_cpio` 是 hostprog，按 **newc** 格式输出：

```
070701 <ino> <mode> <uid> <gid> <nlink> <mtime> <filesize>
       <devmajor> <devminor> <rdevmajor> <rdevminor> <namesize> <check>
<文件名>\0<padding>
<内容><padding>
...
TRAILER!!!\0<padding>
```

`070701` 无校验和，`070702` 有；`070707`（odc）内核会报
“incorrect cpio method used: use -H newc option”。

### 2.3 打包进镜像

`usr/initramfs_data.S` 用 `.incbin` 把 cpio 放进 `.init.ramfs` 段：

```asm
.section .init.ramfs,"a"
__irf_start:
.incbin "usr/initramfs_inc_data"
__irf_end:
.section .init.ramfs.info,"a"
.globl __initramfs_size
__initramfs_size:
	.quad __irf_end - __irf_start
```

`.init.ramfs` 属于 init 段，`__initramfs_start` / `__initramfs_size` 供
`initramfs.c` 引用；数据位于内核镜像内部，无需 bootloader 额外加载。

## 3. 解包：populate_rootfs

### 3.1 调用时机

```
kernel_init → kernel_init_freeable
├─ do_basic_setup → do_initcalls
│  └─ rootfs_initcall: populate_rootfs()            [init/initramfs.c:783]
├─ ...
└─ prepare_namespace()                              [init/do_mounts.c:457]
```

`populate_rootfs()` 把解包放到 async 域：

```c
initramfs_cookie = async_schedule_domain(do_populate_rootfs, NULL,
					 &initramfs_domain);
usermodehelper_enable();
if (!initramfs_async)
	wait_for_initramfs();
```

### 3.2 do_populate_rootfs

`init/initramfs.c:719` 的核心逻辑：

```c
char *err = unpack_to_rootfs(__initramfs_start, __initramfs_size);
if (err)
	panic_show_mem("%s", err);		/* 内建 initramfs 失败 → panic */

if (!initrd_start || IS_ENABLED(CONFIG_INITRAMFS_FORCE))
	goto done;

err = unpack_to_rootfs((char *)initrd_start, initrd_end - initrd_start);
if (err) {
#ifdef CONFIG_BLK_DEV_RAM
	populate_initrd_image(err);		/* 老式 initrd → /initrd.image */
#else
	printk(KERN_EMERG "Initramfs unpacking failed: %s\n", err);
#endif
}
done:
	security_initramfs_populated();
	if (!do_retain_initrd && initrd_start && !kexec_free_initrd())
		free_initrd_mem(initrd_start, initrd_end);
	initrd_start = 0;
	initrd_end = 0;
```

要点：内建 initramfs 失败直接 panic；外部 initrd 失败只告警（当前
`BLK_DEV_RAM=n`）；`CONFIG_INITRAMFS_FORCE=y` 忽略外部 initrd；默认释放
initrd 内存，`retain_initrd`/`keepinitrd` 可保留并通过 `/sys/firmware/initrd`
导出。

### 3.3 unpack_to_rootfs 与状态机

`unpack_to_rootfs()`（519 行）先判断明文 cpio 还是压缩流：

- 以 `0` 开头且 4 字节对齐 → 明文 newc，直接喂状态机；
- 否则 `decompress_method()` 按 magic 选解压器（gzip 需 `CONFIG_RD_GZIP=y`），
  输出经 `flush_buffer()` 进入状态机；
- 结束后状态不是 `Reset` → “junk at the end of compressed archive”。

状态机（224 行）：`Start`（读 110 字节头）→ `Collect`（跨 buffer 拼接）→
`GotHeader`（`do_header()` 解析）→ `SkipIt`/`GotName`/`CopyFile`/
`GotSymlink` → `Reset`（4 字节对齐）。`do_header()` 区分
`070701`/`070702`/`070707`/非法 magic；`parse_header()` 解析 ino、mode、
uid、gid、nlink、mtime、body_len、major、minor、rdev、name_len、hdr_csum。
`do_name()` 遇到 `TRAILER!!!` 结束；普通文件 `filp_open()`，目录
`init_mkdir()`，设备 `init_mknod()`，硬链接由 `maybe_link()`/`find_link()`
复用 inode。

## 4. rootfs 与根文件系统挂载

### 4.1 rootfs

`init/do_mounts.c:512` 的 `init_rootfs()` 决定 rootfs 用 ramfs 还是 tmpfs：
无 `root=` 且无 `rootfstype=` 时置 `is_tmpfs = true`；`root_fs_names` 含
`"tmpfs"` 时也用 tmpfs。`rootfs_fs_type` 的 `init_fs_context` 在
`ramfs_init_fs_context` 与 `shmem_init_fs_context` 之间选择。
`fs/namespace.c` 随后 `init_mount_tree()` 把 rootfs 挂到 `/`，initramfs
解包出的文件都落在它上面。

### 4.2 prepare_namespace

```
prepare_namespace()                          [init/do_mounts.c:457]
├─ root_delay 秒延时
├─ wait_for_device_probe()
├─ md_run_setup()
├─ parse_root_device(saved_root_name)        解析 root=
├─ initrd_load()                             老式 initrd → /dev/ram0
├─ root_wait 时 wait_for_root()
├─ mount_root(saved_root_name)
│  ├─ Root_NFS → mount_nfs_root()
│  ├─ Root_CIFS → mount_cifs_root()
│  ├─ Root_Generic → mount_root_generic()
│  └─ 其他/0 → mount_block_root()
├─ devtmpfs_mount()
├─ init_pivot_root(".", ".")
└─ init_umount(".", MNT_DETACH)
```

`initrd_load()`（`init/do_mounts_initrd.c:48`）仅处理传统 initrd：
`create_dev("/dev/ram", Root_RAM0)` 后 `rd_load_image()`，失败时打印
“using deprecated initrd support, will be removed in January 2027; use
initramfs instead ...”，最后 `init_unlink("/initrd.image")`。当前
`CONFIG_BLK_DEV_RAM=n`，应使用 initramfs。

### 4.3 执行 /init

`kernel_init()` 在 `prepare_namespace()` 后依次尝试：
`ramdisk_execute_command`（`/init`）→ `init=`（`execute_command`）→
`CONFIG_DEFAULT_INIT` → `/sbin/init`、`/etc/init`、`/bin/init`、`/bin/sh`；
全部失败则 `panic("No working init found.")`。initramfs 里的 `/init` 通常
负责加载模块、挂载真实根并 `switch_root`。

## 5. KASLR 原理

### 5.1 熵来源

1. bootloader 在 `/chosen/kaslr-seed` 放随机 u64；
2. 没有 seed 时尝试 ARM64 `RNDR`（`__early_cpu_has_rndr()` +
   `__arm64_rndr()`）；
3. UEFI 引导时 stub 用 `EFI_RNG_PROTOCOL` 取种子，并可能随机化物理加载位置。

`arch/arm64/Kconfig:2361` 的 `RANDOMIZE_BASE` 明确说明熵由 bootloader
通过 `/chosen/kaslr-seed` 提供。

### 5.2 偏移计算

`arch/arm64/kernel/pi/kaslr_early.c`（MMU 开启前的 position-independent
代码，不能用全局变量/绝对地址）：

```c
static u64 __init get_kaslr_seed(void *fdt, int node)
{
	prop = fdt_getprop_w(fdt, node, "kaslr-seed", &len);
	if (!prop || len != sizeof(u64))
		return 0;
	ret = fdt64_to_cpu(*prop);
	*prop = 0;			/* 消费后擦除 */
	return ret;
}

u64 __init kaslr_early_init(void *fdt, int chosen)
{
	if (kaslr_disabled_cmdline())
		return 0;
	seed = get_kaslr_seed(fdt, chosen);
	if (!seed) {
		if (!__early_cpu_has_rndr() ||
		    !__arm64_rndr((unsigned long *)&seed))
			return 0;
	}
	range = (VMALLOC_END - KIMAGE_VADDR) / 2;
	return range / 2 + (((__uint128_t)range * seed) >> 64);
}
```

内核被放在 VMALLOC 区域“中间一半”，避开低/高四分之一，减少与 vmalloc、
模块区域碰撞；128 位乘法把 seed 均匀映射到 `range`。

### 5.3 映射与重定位

`arch/arm64/kernel/pi/map_kernel.c:241`：

```c
u64 va_base, pa_base = (u64)&_text;
u64 kaslr_offset = pa_base % MIN_KIMG_ALIGN;
...
if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
	u64 kaslr_seed = kaslr_early_init(fdt_mapped, chosen);

	if (kaslr_seed && kaslr_requires_kpti())
		arm64_use_ng_mappings = ng_mappings_allowed();

	kaslr_offset |= kaslr_seed & ~(MIN_KIMG_ALIGN - 1);
}
va_base = KIMAGE_VADDR + kaslr_offset;
map_kernel(kaslr_offset, va_base - pa_base, root_level);
```

关键：

- **低 21 位（`MIN_KIMG_ALIGN = SZ_2M`）来自物理地址**
  （`pa_base % MIN_KIMG_ALIGN`），保证虚拟与物理 2MB 对齐一致，早期页表
  可继续用 2MB block；
- 高位来自 seed，两者按位或得到最终偏移；
- `map_kernel()` 内部 `relocate_kernel(kaslr_offset)` 修正所有
  `R_AARCH64_RELATIVE` 重定位项（依赖 `CONFIG_RELOCATABLE=y`）；
- 启用 KPTI 相关条件时，KASLR 还会影响 `arm64_use_ng_mappings`。

### 5.4 运行期判定

`arch/arm64/include/asm/memory.h:249` 的 `kaslr_offset()` 就是
`(u64)&_text - KIMAGE_VADDR`。`kaslr_init()`（`arch/arm64/kernel/kaslr.c:15`，
仅 `RANDOMIZE_BASE=y` 时编译）：

```c
if (kaslr_disabled_cmdline()) {
	pr_info("KASLR disabled on command line\n");
	return;
}
if (kaslr_offset() < MIN_KIMG_ALIGN) {
	pr_warn("KASLR disabled due to lack of seed\n");
	return;
}
pr_info("KASLR enabled\n");
__kaslr_is_enabled = true;
```

偏移小于 2MB 说明没有 seed（只剩物理对齐的低 21 位）。`nokaslr` 的实际
处理在早期 cpufeature 代码中，`kaslr.c` 的 `parse_nokaslr()` 只占位。

## 6. KASLR 关闭时的行为

当前 `CONFIG_RANDOMIZE_BASE=n`：

1. `arch/arm64/kernel/Makefile` 的 `obj-$(CONFIG_RANDOMIZE_BASE) += kaslr.o`
   不成立，`kaslr.c` 不编译；
2. `arch/arm64/kernel/pi/Makefile` 的
   `obj-$(CONFIG_RANDOMIZE_BASE) += kaslr_early.pi.o` 不成立，
   `kaslr_early_init()` 不存在；
3. `early_map_kernel()` 中 `if (IS_ENABLED(CONFIG_RANDOMIZE_BASE))` 被编译器
   消除，`kaslr_offset` 只剩 `pa_base % MIN_KIMG_ALIGN`；
4. `include/asm/memory.h` 提供空 `kaslr_init()` 与
   `kaslr_enabled() { return false; }`；
5. `setup_arch()` 仍调用 `kaslr_init()`，但为空操作；
6. panic 时 `dump_kernel_offset()`（`setup.c:413`）打印
   `Kernel Offset: disabled`；
7. `vmcoreinfo` 的 `KERNELOFFSET`（`vmcore_info.c:35`）为固定值/0；
8. EFI stub 的 `handle_kernel_image()` 仍调用
   `efi_kaslr_relocate_kernel(..., efi_kaslr_get_phys_seed())`，物理重定位
   是否发生取决于 stub 侧对 `CONFIG_RANDOMIZE_BASE`/`efi_nokaslr` 的判断
   （以当前源码为准）；虚拟偏移确定为 0。

调试意义：QEMU/gdb 可直接按 `KIMAGE_VADDR` 下断点，无需 `nokaslr`；
若将来启用 KASLR，则需 `nokaslr` 或借助 `/proc/kallsyms`、`KERNELOFFSET`
修正符号；QEMU `-kernel` 直启不经过 UEFI stub，无物理重定位与 EFI 运行时。

## 7. 关键文件清单

| 文件 | 作用 |
| --- | --- |
| `usr/Makefile` | initramfs 构建、压缩方式、默认 cpio 列表 |
| `usr/gen_initramfs.sh` | 目录/文件 → cpio 列表 |
| `usr/gen_init_cpio.c` | newc cpio 生成器（hostprog） |
| `usr/initramfs_data.S` | `.init.ramfs` 段、`__initramfs_start/size` |
| `init/initramfs.c` | `populate_rootfs()`、`unpack_to_rootfs()`、cpio 状态机 |
| `init/do_mounts.c` | `prepare_namespace()`、`mount_root()`、`init_rootfs()` |
| `init/do_mounts_initrd.c` | `initrd_load()`、`rd_load_image()`（传统 initrd） |
| `init/main.c` | `kernel_init_freeable()`、`run_init_process()` |
| `fs/namespace.c` | `init_mount_tree()`，rootfs 挂到 `/` |
| `arch/arm64/kernel/kaslr.c` | `kaslr_init()`（需 `RANDOMIZE_BASE`） |
| `arch/arm64/kernel/pi/kaslr_early.c` | 早期 seed 读取与偏移计算 |
| `arch/arm64/kernel/pi/map_kernel.c` | `early_map_kernel()`、`map_kernel()` |
| `arch/arm64/include/asm/memory.h` | `kaslr_offset()`、`kaslr_enabled()` |
| `arch/arm64/include/asm/boot.h` | `MIN_KIMG_ALIGN = SZ_2M` |
| `arch/arm64/Kconfig` | `RANDOMIZE_BASE`、`RANDOMIZE_MODULE_REGION_FULL` |
| `arch/arm64/kernel/setup.c` | `kaslr_init()` 调用、`dump_kernel_offset()` |
| `drivers/firmware/efi/libstub/arm64-stub.c` | `handle_kernel_image()` 物理重定位入口 |
