# 4. Rust for Linux：内核中的 Rust 支持

## 4.1 概述

Rust for Linux 让内核可以用 Rust 编写模块和驱动。它不是把整个内核改写为 Rust，而是提供一层 **安全的 `kernel` crate**，把常用的 C 内核 API 包装成符合 Rust 所有权/生命周期模型的接口，驱动代码通过 `module!` 宏注册为普通的内核模块。

设计目标：

- `#![no_std]`：不依赖标准库，只依赖 `core` 与内核提供的 `alloc`
- 与 C 代码共享同一套 Kbuild/modpost/模块加载器
- 所有 `unsafe` 集中在内核抽象层，驱动侧尽量安全
- 编译器/工具链不满足要求时，`make rustavailable` 给出明确诊断

当前源码树是 Linux 7.2，Rust 相关目录已经完整存在；但 `.config` 中 **没有 `CONFIG_RUST`**，因此 Rust 代码不会被编译（见 4.10）。

## 4.2 目录结构

```
rust/
├── Makefile / bindgen_parameters   # 构建规则与 bindgen 参数
├── bindings/                       # C 头文件入口与生成的绑定
├── core.rs / compiler_builtins.rs  # core 与 compiler_builtins 适配
├── exports.c / ffi.rs              # 导出符号与 FFI 类型
├── helpers/                        # 仅导出给 Rust 的 C 辅助函数
├── kernel/                         # 主要安全抽象 crate
├── macros/                         # 过程宏：module!、paste! 等
├── pin-init/                       # pin 初始化支持
├── proc-macro2/ quote/ syn/        # 内置过程宏依赖
├── uapi/                           # UAPI 绑定
└── zerocopy/ zerocopy-derive/      # 零拷贝序列化
```

`rust/kernel/` 是驱动开发者主要接触的目录，包含 `alloc`、`sync`、`device`、`driver`、`platform`、`of`、`acpi`、`fs`、`block`、`i2c`、`usb`、`drm`、`net`、`workqueue`、`irq`、`mm` 等子系统抽象。

`rust/macros/` 中的过程宏包括：`module.rs`（展开 `__init`/`__exit`、modinfo、参数）、`paste.rs`（标识符拼接）、`vtable.rs`（`extern "C"` 虚表）、`fmt.rs`、`kunit.rs`、`export.rs`、`concat_idents.rs`、`for_lt.rs`。

### 4.2.1 crate 依赖图

`rust/Makefile` 的变量定义已经把依赖关系写死，可以还原成下面这张有向图（箭头表示 `--extern`）：

```
core (定制，来自 rust-src)
 ↑
compiler_builtins ──> ffi ──> bindings ──┐
        ↑                    uapi ───────┤
        │                                ├──> kernel
   zerocopy <── zerocopy-derive(proc) ───┤
        ↑                                │
   build_error                           │
        ↑                                │
   pin_init <── pin_init_internal(proc) ─┤
        ↑                                │
     macros (proc, 依赖 proc_macro2/quote/syn)
```

对应 `rust/Makefile` 中的具体规则（第 717-781 行）：

```make
$(obj)/compiler_builtins.o: $(src)/compiler_builtins.rs $(obj)/core.o FORCE
	+$(call if_changed_rule,rustc_library)

$(obj)/ffi.o: $(src)/ffi.rs $(obj)/compiler_builtins.o FORCE
	+$(call if_changed_rule,rustc_library)

$(obj)/bindings.o: $(src)/bindings/lib.rs \
    $(obj)/ffi.o $(obj)/pin_init.o \
    $(obj)/bindings/bindings_generated.rs \
    $(obj)/bindings/bindings_helpers_generated.rs FORCE
	+$(call if_changed_rule,rustc_library)

$(obj)/kernel.o: private rustc_target_flags = --extern ffi --extern pin_init \
    --extern build_error --extern macros --extern bindings --extern uapi \
    --extern zerocopy --extern zerocopy_derive
```

`kernel.o` 是所有上游 Rust 代码的最终链接单元：驱动模块通过 `--extern kernel` 引用它，而不是直接引用 `bindings`。`bindings/lib.rs` 开头也明确写了这条纪律：

```rust
//! This crate may not be directly used. If you need a kernel C API that is
//! not ported or wrapped in the `kernel` crate, then do so first instead of
//! using this crate.
```

### 4.2.2 顶层 crate 的职责

| 源 | crate 名 | 作用 |
| --- | --- | --- |
| `$(RUST_LIB_SRC)/core`（本树无本地 `rust/core.rs`） | `core` | 用 rust-src 重新编译的 `core`，关闭 `no_fp_fmt_parse` |
| `rust/compiler_builtins.rs` | `compiler_builtins` | 只提供内核需要的内建函数，其余定义为 `panic!` |
| `rust/ffi.rs` | `ffi` | C 基础类型别名（`c_char = u8` 等），并做 `size_of` 静态断言 |
| `rust/zerocopy/` | `zerocopy` | 零拷贝序列化（vendored 上游 0.8.54） |
| `rust/pin-init/` | `pin_init` | `pin_init!`、`#[pin_data]`、`PinInit` |
| `rust/bindings/` | `bindings` | `bindgen` 生成的 C 绑定 + `__BINDGEN__` helpers |
| `rust/uapi/` | `uapi` | 单独为 UAPI 头生成的绑定 |
| `rust/kernel/` | `kernel` | 安全抽象层（驱动唯一应依赖的 crate） |
| `rust/build_error.rs` | `build_error` | `build_assert!`/`build_error!` 的运行期兜底 |
| `rust/exports.c` | （C） | 把 Rust 符号以 GPL-only 方式 `EXPORT_SYMBOL` |
| `rust/helpers/` | （C） | 把 `static inline`/宏包装成可调用符号 |

`rust/exports.c` 的注释解释了为什么需要这个“C 侧的 hack”：

```c
/*
 * A hack to export Rust symbols for loadable modules without having to redo
 * the entire `include/linux/export.h` logic in Rust.
 *
 * This requires Rust's new/future `v0` mangling scheme because the default one
 * ("legacy") uses invalid characters for C identifiers (thus we cannot use the
 * `EXPORT_SYMBOL_*` macros).
 *
 * All symbols are exported as GPL-only to guarantee no GPL-only feature is
 * accidentally exposed.
 */
```

即：Rust 的 `v0` mangling 生成合法 C 标识符，才能用 `EXPORT_SYMBOL_GPL()`；同时统一 GPL-only，避免把仅供 GPL 模块使用的抽象暴露给专有模块。

## 4.3 工具链依赖

### 4.3.1 必需组件

根据 `Documentation/rust/quick-start.rst`：

| 组件 | 用途 |
| --- | --- |
| `rustc` | 编译 Rust（要求支持内核使用的 unstable features） |
| `rust-src` | 提供 `core`/`alloc` 源码，用于定制构建 |
| `bindgen`（含 `libclang`） | 从 C 头文件生成 Rust 绑定 |
| `rustfmt` / `clippy` | 格式化与静态检查 |
| LLVM/clang | `bindgen` 依赖 `libclang`；通常配合 `make LLVM=1` |

`rustc` 的 `core` 构建需要 `RUST_LIB_SRC` 指向 rust-src 的 `library` 目录；`rust/Makefile` 默认取：

```make
RUST_LIB_SRC ?= $(rustc_sysroot)/lib/rustlib/src/rust/library
```

### 4.3.2 可用性检查

`scripts/rust_is_available.sh` 检查 `rustc` 版本与 `CONFIG_RUSTC_VERSION`、`bindgen` 版本与 `libclang`、`rust-src` 是否存在、编译器是否支持所需 feature。顶层 `Makefile` 的 `prepare` 阶段只在 `CONFIG_RUST` 为真时执行它（`ifdef CONFIG_RUST` → `rust_is_available.sh` → `$(MAKE) $(build)=rust`）。

用户可直接运行 `make LLVM=1 rustavailable`（顶层 `Makefile:1975`）。

### 4.3.3 版本探测符号

即使 `CONFIG_RUST` 未启用，Kconfig 仍会探测工具链并写入 `.config`。当前可见：`CONFIG_RUSTC_VERSION=109600`、`CONFIG_RUSTC_LLVM_VERSION=220102`、`CONFIG_RUSTC_LLVM_MAJOR_VERSION=22`，以及 `RUSTC_HAS_SPAN_FILE`、`RUSTC_HAS_UNNECESSARY_TRANSMUTES`、`RUSTC_HAS_FILE_WITH_NUL`、`RUSTC_HAS_FILE_AS_C_STR`、`RUSTC_SUPPORTS_ARM64`、`HAVE_RUST=y`。这些只是能力探测，不代表 Rust 已启用。

### 4.3.4 最低版本与探测值

`scripts/min-tool-version.sh` 给出硬性下限：

| 工具 | 最低版本 | 备注 |
| --- | --- | --- |
| `rustc` | 1.85.0 | `SRCARCH=s390` 时为 1.96.0 |
| `bindgen` | 0.71.1 | |
| `llvm`（libclang） | 17.0.1 | `SRCARCH=loongarch` 时为 18.0.0 |

而本树 `.config` 探测到的是 `RUSTC_VERSION=109600`（1.96.0）与 `RUSTC_LLVM_VERSION=220102`（22.1.2），明显高于下限。`rust_is_available.sh` 用 `get_canonical_version()` 把 `x.y.z` 压成整数比较：

```sh
get_canonical_version()
{
	IFS=.
	set -- $1
	echo $((100000 * $1 + 100 * $2 + $3))
}
```

注释里说明了为什么比其它版本脚本多一位：`rustc` 将在 2026 年底达到 1.100.0。

### 4.3.5 rust_is_available.sh 的检查顺序

脚本（`scripts/rust_is_available.sh`）严格按“环境 → 存在性 → 版本 → 源”推进，任一致命错误立即 `exit 1`：

1. 必须由 Kbuild 调用：`RUSTC`、`BINDGEN`、`CC` 任一未设置就报错并提示用 `make rustavailable`；
2. `command -v "$RUSTC"` / `command -v "$BINDGEN"` 检查可执行文件；
3. 解析 `rustc --version`、`bindgen --version`，与 `min-tool-version.sh` 比较；
4. 通过 `bindgen` 处理 `scripts/rust_is_available_bindgen_libclang.h` 获取其实际加载的 `libclang` 版本，低于 LLVM 下限则失败；
5. 若 `bindgen` 的 `libclang` 与 `$CC` 的 clang 版本不一致，只设 `warning=1`（不失败），因为 ABI 可能仍兼容；
6. 检查 `core` 源码存在：

```sh
rustc_sysroot=$("$RUSTC" $KRUSTFLAGS --print sysroot)
rustc_src=${RUST_LIB_SRC:-"$rustc_sysroot/lib/rustlib/src/rust/library"}
rustc_src_core="$rustc_src/core/src/lib.rs"
if [ ! -e "$rustc_src_core" ]; then
	...
	exit 1
fi
```

注意第 6 步同时支持 `RUST_LIB_SRC` 覆盖与 `--sysroot`（通过 `$KRUSTFLAGS` 传入）。脚本末尾用 `trap` 在失败或警告时提示阅读 `Documentation/rust/quick-start.rst`，这就是 `make rustavailable` 输出里那段 `***` 的来源。

`RUSTC_HAS_*` 系列与脚本无关，而是 `init/Kconfig` 第 181-195 行基于版本号的直接比较：

```kconfig
config RUSTC_HAS_SPAN_FILE
	def_bool RUSTC_VERSION >= 108800
```

另有 `$(rustc-option,...)`（`scripts/Kconfig.include` 第 78 行）用于“这个 `-Z`/`-C` 选项当前 rustc 是否接受”的能力探测。两者共同决定 `rust/kernel/lib.rs` 里哪些 `#![feature(...)]` 生效。

## 4.4 构建集成

### 4.4.1 rust/Makefile

`rust/Makefile` 开头：

```make
obj-$(CONFIG_RUST) += core.o compiler_builtins.o ffi.o
always-$(CONFIG_RUST) += exports_core_generated.h

obj-$(CONFIG_RUST) += zerocopy.o
obj-$(CONFIG_RUST) += bindings.o pin_init.o kernel.o
obj-$(CONFIG_RUST) += uapi.o
obj-$(CONFIG_RUST) += exports.o
always-$(CONFIG_RUST) += bindings/bindings_generated.rs ...
```

因此 `CONFIG_RUST` 为空时，`rust/` 目录不会产生任何对象。

### 4.4.2 bindgen 生成绑定

```make
quiet_cmd_bindgen = BINDGEN $@
      cmd_bindgen = \
	$(BINDGEN) $< $(bindgen_target_flags) --rust-target 1.85 \
		--use-core --with-derive-default --ctypes-prefix ffi \
		-o $@ -- $(bindgen_c_flags_final) -DMODULE ...
```

要点：

- 输入是 `bindings/bindings_helper.h`
- 参数来自 `rust/bindgen_parameters`（过滤掉的类型/函数、`--allowlist-*` 等）
- 使用 `--use-core`、`--ctypes-prefix ffi`，与 `rust/ffi.rs` 对齐
- `bindgen_c_flags_final` 复用内核 `c_flags`，过滤掉 `bindgen_skip_c_flags` 中 clang 不认识的 GCC 参数
- `--rust-target 1.85` 是当前生成代码的最低 Rust 版本目标

### 4.4.3 编译 Rust 源文件

`scripts/Makefile.build` 定义 `rust_common_cmd`（第 325 行附近）：

```make
rust_common_cmd = \
	RUST_MODFILE=$(modfile) $(RUSTC_OR_CLIPPY) $(rust_flags) \
	-Zallow-features=$(rust_allowed_features) \
	-Zcrate-attr='feature($(rust_allowed_features))' \
	--crate-type rlib -L $(objtree)/rust/ ...
```

允许的 feature 列表：

```make
rust_allowed_features := arbitrary_self_types,asm_goto,generic_arg_infer,used_with_arg
```

`kernel/lib.rs` 也显式声明了这些 `#![feature(...)]`，并通过：

```rust
#[cfg(not(CONFIG_RUST))]
compile_error!("Missing kernel configuration for conditional compilation");
```

防止在未启用配置时误编译。

### 4.4.4 core 与 compiler_builtins

内核不直接使用宿主 `core`，而是用 `rust/core.rs` 重新编译一份，关闭不需要的功能（如 `no_fp_fmt_parse`），以减小体积并避免浮点格式化依赖。`compiler_builtins.rs` 类似。

### 4.4.5 helper C 函数

`rust/helpers/` 中的 C 文件把内联函数、宏、静态 inline 包装成可链接符号，仅供 Rust 调用；对应导出头 `exports_helpers_generated.h` 由 `scripts/generate_rust_exports` 生态生成。`CONFIG_RUST_INLINE_HELPERS` 可选择以 LLVM bitcode 形式内联。

### 4.4.6 bindgen 的三份输出

`rust/Makefile` 第 504-538 行定义了三条 `bindgen` 规则，产物各不相同：

| 目标 | 输入 | 关键参数 / 后处理 |
| --- | --- | --- |
| `bindings/bindings_generated.rs` | `bindings/bindings_helper.h` | `--rust-target 1.85 --use-core --ctypes-prefix ffi`，然后用 `sed -Ei 's/pub const RUST_CONST_HELPER_.../pub const .../'` 去掉前缀 |
| `uapi/uapi_generated.rs` | `uapi/uapi_helper.h` | 同一套 `bindgen_parameters` |
| `bindings/bindings_helpers_generated.rs` | `helpers/helpers.c` | `--blocklist-type '.*' --allowlist-var '' --allowlist-function 'rust_helper_.*'`，再用 `sed` 把 `pub fn rust_helper_xxx` 改写成带 `#[link_name="rust_helper_xxx"]` 的 `pub fn xxx` |

公共命令行：

```make
quiet_cmd_bindgen = BINDGEN $@
      cmd_bindgen = \
	$(BINDGEN) $< $(bindgen_target_flags) --rust-target 1.85 \
		--use-core --with-derive-default --ctypes-prefix ffi --no-layout-tests \
		--no-debug '.*' --enable-function-attribute-detection \
		-o $@ -- $(bindgen_c_flags_final) -DMODULE \
		$(bindgen_target_cflags) $(bindgen_target_extra)
```

`bindgen_parameters`（73 行）不是随机过滤，而是每一条都有原因，例如：

```
--blocklist-type __kernel_s?size_t      # 手动映射到 isize/usize
--opaque-type xregs_state               # 避免 packed 类型嵌套 repr(align)
--opaque-type spinlock                  # SMP 关闭时是 ZST，会触发 Rust 警告
--newtype-enum lru_status --with-attribute-custom-enum=lru_status='#[cfi_encoding="10lru_status"]'
--blocklist-item VM_READ ...            # 由 const helper 提供，避免重复定义
--with-derive-custom-struct .*=MaybeZeroable
```

`bindgen_c_flags` 复用内核的 `c_flags`，但先过滤掉 clang 不认识的 GCC 参数（`bindgen_skip_c_flags`），再补 `--target=$(BINDGEN_TARGET)`：

```make
BINDGEN_TARGET_arm64	:= aarch64-linux-gnu
...
bindgen_extra_c_flags = -w --target=$(BINDGEN_TARGET)
bindgen_c_flags = $(filter-out $(bindgen_skip_c_flags), $(c_flags)) \
	$(bindgen_extra_c_flags)
bindgen_c_flags_final = $(bindgen_c_flags_lto) -fno-builtin -D__BINDGEN__
```

本树交叉编译目标就是 arm64，因此 `bindgen` 会以 `--target=aarch64-linux-gnu` 解析头文件；若宿主机缺少对应 clang 目标，会在这里失败。

### 4.4.7 exports_* 的真实生成方式

本树的导出头不是由某个 `scripts/generate_rust_exports` 脚本生成，而是 `rust/Makefile` 第 550-575 行的 `cmd_exports`：

```make
rust_exports = $(NM) -p --defined-only $(1) | awk '$$2~/(T|R|D|B)/ && $$3!~/__(pfx|cfi|odr_asan)/ { printf $(2),$$3 }'

quiet_cmd_exports = EXPORTS $@
      cmd_exports = \
	$(call rust_exports,$<,"EXPORT_SYMBOL_RUST_GPL(%s);\n") > $@
```

即：对 `core.o`、`helpers.o`、`bindings.o`、`kernel.o` 分别跑 `nm`，取出 `T/R/D/B` 类型且不以 `__pfx`、`__cfi`、`__odr_asan` 开头的符号，生成形如：

```c
EXPORT_SYMBOL_RUST_GPL(rust_helper_foo);
```

的头文件，再由 `rust/exports.c` 包含并编译成 `exports.o`。`rust/exports.c` 里的宏定义为：

```c
#define EXPORT_SYMBOL_RUST_GPL(sym) extern int sym; EXPORT_SYMBOL_GPL(sym)
```

为什么连 `helpers` 与 `bindings` 的符号也要导出？Makefile 第 559-567 行的注释说明：Rust 的泛型与内联函数不一定在定义它的 crate 中生成代码，因此调用方 crate 可能引用任意符号，必须全部导出。`CONFIG_RUST_INLINE_HELPERS` 打开时 helpers 变为 bitcode 内联，`exports_helpers_generated.h` 也就不再包含。

### 4.4.8 core 与 compiler_builtins 的细节

需要更正 4.4.4 的表述：本树**没有** `rust/core.rs` 这个文件，`core.o` 直接由 rust-src 的 `$(RUST_LIB_SRC)/core/src/lib.rs` 编译而来（`rust/Makefile` 第 721 行），本地只通过 flags 做定制。`core.o` 的规则（第 717-727 行）有三处特殊处理：

```make
$(obj)/core.o: private skip_clippy = 1
$(obj)/core.o: private skip_flags = $(core-skip_flags)
$(obj)/core.o: private rustc_objcopy = $(foreach sym,$(redirect-intrinsics),--redefine-sym $(sym)=__rust$(sym))
$(obj)/core.o: private rustc_target_flags = $(core-flags)
$(obj)/core.o: $(RUST_LIB_SRC)/core/src/lib.rs \
    $(wildcard $(objtree)/include/config/RUSTC_VERSION_TEXT) FORCE
	+$(call if_changed_rule,rustc_library)
```

1. `core-flags` 只加 `--edition=$(core-edition)` 与 `--cfg no_fp_fmt_parse`（关闭浮点格式化解析，省体积）；
2. `redirect-intrinsics`（第 667-679 行）列出了 `__addsf3`、`__multi3`、`__udivmodti4` 等符号，`objcopy --redefine-sym` 把它们改名成 `__rust__addsf3` 等，避免与内核自己提供的符号冲突；
3. `compiler_builtins.o` 额外用 `objcopy -w -W '__*'` 弱化所有符号（第 730 行），确保不覆盖内核已有实现。

`compiler_builtins.rs` 的策略很直接：

```rust
macro_rules! define_panicking_intrinsics(
    ($reason: tt, { $($ident: ident, )* }) => {
        $(
            #[doc(hidden)]
            #[export_name = concat!("__rust", stringify!($ident))]
            pub extern "C" fn $ident() {
                panic!($reason);
            }
        )*
    }
);

define_panicking_intrinsics!("`f32` should not be used", { ... });
```

即“内核不应该用到浮点内建函数；真用到就 panic”，以便尽早暴露问题，而不是悄悄拖入整套 `compiler-rt`。

`build_error.rs` 则是 `build_assert!` 的运行期兜底：默认构建若 `build_error!` 仍存在于二进制中，链接阶段就失败；`CONFIG_RUST_BUILD_ASSERT_ALLOW`（`lib/Kconfig.debug` 第 3597 行）允许把它降级为运行期 panic。

### 4.4.9 rust-analyzer 集成

`rust/Makefile` 第 652-665 行的 `rust-analyzer` 目标生成 `rust-project.json`：

```make
rust-analyzer:
	$(Q)MAKEFLAGS= $(srctree)/scripts/generate_rust_analyzer.py \
		--cfgs='core=$(core-cfgs)' $(core-edition) \
		--cfgs='zerocopy=$(zerocopy-cfgs)' \
		...
		$(realpath $(srctree)) $(realpath $(objtree)) \
		$(rustc_sysroot) $(RUST_LIB_SRC) $(if $(KBUILD_EXTMOD),$(srcroot)) \
		> rust-project.json
```

`scripts/generate_rust_analyzer.py` 的关键步骤：

1. 读 `include/generated/rustc_cfg`，去掉 `--cfg=` 前缀，得到 `generated_cfg`（第 77-81 行）；
2. 构造 sysroot crate：`core`、`alloc`、`std`、`proc_macro`（`append_sysroot_crate`，`is_workspace_member=False`）；
3. 构造 `rust/` 下的 crate，依赖与 `rust/Makefile` 一致；`bindings`/`uapi`/`kernel` 额外登记 `source.include_dirs`（源树与 objtree），因为要包含生成的 `.rs`；
4. 扫描 `scripts/*.rs`，只收录在 `scripts/Makefile` 里带 `-rust` 标记的 hostprog（`generate_rust_target`、`rustdoc_test_builder`、`rustdoc_test_gen`）；
5. 扫描 `samples/`、`drivers/` 下的 `.rs`，仅当同目录 `Makefile` 或 `Kbuild` 中出现 `<name>.o` 时视为 crate root；
6. 输出 `{"crates": [...], "sysroot": "..."}`，按 key 排序。

一个容易忽略的点：`build_crate()` 会给每个 crate 注入 `RUST_MODFILE=This is only for rust-analyzer`（第 105-108 行），否则 `module!` 宏展开时会因缺少环境变量而失败。

## 4.5 kernel crate

### 4.5.1 lib.rs 结构

`rust/kernel/lib.rs`：

```rust
#![no_std]
#![feature(generic_arg_infer)]
#![feature(arbitrary_self_types)]
#![feature(derive_coerce_pointee)]
#![feature(used_with_arg)]
#![cfg_attr(CONFIG_RUSTC_HAS_FILE_WITH_NUL, feature(file_with_nul))]

#[cfg(not(CONFIG_RUST))]
compile_error!("Missing kernel configuration for conditional compilation");

extern crate self as kernel;
pub use ffi;

pub mod alloc;
pub mod bitfield;
pub mod bug;
pub mod device;
pub mod driver;
pub mod error;
pub mod fs;
pub mod io;
...
```

模块按 Kconfig 条件编译，例如 `#[cfg(CONFIG_AUXILIARY_BUS)] pub mod auxiliary;`、`#[cfg(CONFIG_BLOCK)] pub mod block;`、`#[cfg(CONFIG_I2C = "y")] pub mod i2c;`。这依赖 `include/generated/rustc_cfg` 把 Kconfig 值转成 `--cfg CONFIG_FOO`。

### 4.5.2 Module trait

```rust
pub trait Module: Sized + Sync + Send {
    fn init(module: &'static ThisModule) -> error::Result<Self>;
}
```

对应 C 的 `module_init()`。还有：

```rust
pub trait InPlaceModule: Sync + Send {
    fn init(module: &'static ThisModule) -> impl pin_init::PinInit<Self, error::Error>;
}
```

用于需要在固定地址就地初始化的模块（如包含自引用结构）。

`ThisModule` 包装 `*mut bindings::module`，通过 `ThisModule::from_ptr()` 在宏展开代码中获得 `THIS_MODULE`。

### 4.5.3 ModuleMetadata

```rust
pub trait ModuleMetadata {
    const NAME: &'static CStr;
    const LICENSE: &'static CStr;
    // ...
}
```

`module!` 宏为每个模块生成该 trait 的实现，用于填充 `.modinfo`。

### 4.5.4 ModuleMetadata 与 ThisModule 的实际定义

需要精确化 4.5.3：本树的 `ModuleMetadata` 只有**一个**关联常量，没有 `LICENSE` 等字段（`rust/kernel/lib.rs` 第 184-188 行）：

```rust
/// Metadata attached to a [`Module`] or [`InPlaceModule`].
pub trait ModuleMetadata {
    /// The name of the module as specified in the `module!` macro.
    const NAME: &'static crate::str::CStr;
}
```

license/author/description 等不进 trait，而是由 `module!` 直接写成 `.modinfo` 字符串（见 4.6.1）。

`ThisModule` 的完整实现（第 190-214 行）：

```rust
pub struct ThisModule(*mut bindings::module);

// SAFETY: `THIS_MODULE` may be used from all threads within a module.
unsafe impl Sync for ThisModule {}

impl ThisModule {
    pub const unsafe fn from_ptr(ptr: *mut bindings::module) -> ThisModule {
        ThisModule(ptr)
    }

    pub const fn as_ptr(&self) -> *mut bindings::module {
        self.0
    }
}
```

除 `from_ptr()` 与 `as_ptr()` 外，`ThisModule` 没有其它构造或解引用入口，因此驱动代码无法凭空伪造 `THIS_MODULE`；`from_ptr()` 是 `unsafe` 且 `const`，只能由宏在静态初始化时调用。

`Module` 与 `InPlaceModule` 之间有一个 blanket impl（第 169-182 行）：

```rust
impl<T: Module> InPlaceModule for T {
    fn init(module: &'static ThisModule) -> impl pin_init::PinInit<Self, error::Error> {
        let initer = move |slot: *mut Self| {
            let m = <Self as Module>::init(module)?;
            unsafe { slot.write(m) };
            Ok(())
        };
        unsafe { pin_init::pin_init_from_closure(initer) }
    }
}
```

即“实现了 `Module` 就自动获得就地初始化版本”，`module!` 宏统一按 `InPlaceModule::init()` 调用，避免为两种 trait 写两套展开。

### 4.5.5 kernel crate 的模块规模与 cfg 映射

`rust/kernel/lib.rs` 第 43-138 行声明了约 90 个 `pub mod`，覆盖：

| 类别 | 模块示例 |
| --- | --- |
| 内核基础设施 | `alloc`、`sync`、`list`、`rbtree`、`maple_tree`、`xarray`、`workqueue`、`task`、`time`、`jump_label`、`tracepoint` |
| 设备模型 | `device`、`driver`、`devres`、`faux`、`platform`、`pci`、`usb`、`i2c`、`auxiliary`、`of`、`acpi`、`io`、`ioctl`、`dma`、`scatterlist` |
| 文件系统 | `fs`、`debugfs`、`configfs`、`seq_file`、`miscdevice` |
| 其他 | `block`、`drm`、`gpu`、`net`、`mm`、`regulator`、`clk`、`pwm`、`cpufreq`、`opp`、`soc`、`irq`、`iommu`、`security`、`cred` |

其中不少模块是条件编译的，例如：

```rust
#[cfg(CONFIG_AUXILIARY_BUS)]
pub mod auxiliary;
#[cfg(CONFIG_BLOCK)]
pub mod block;
#[cfg(CONFIG_DRM = "y")]
pub mod drm;
#[cfg(CONFIG_I2C = "y")]
pub mod i2c;
#[cfg(CONFIG_KUNIT)]
pub mod kunit;
#[cfg(CONFIG_NET)]
pub mod net;
#[cfg(CONFIG_PCI)]
pub mod pci;
#[cfg(CONFIG_USB = "y")]
pub mod usb;
```

注意两种写法：`#[cfg(CONFIG_FOO)]` 在 FOO 为 `y` 或 `m` 时为真；`#[cfg(CONFIG_FOO = "y")]` 只在 `y` 时为真。后者用于“模块化时抽象不可用”的场景（例如 `i2c`/`usb`/`drm` 的 Rust 抽象要求对应的 C 代码内置）。

这些 cfg 来自 `include/generated/rustc_cfg`，`conf_write_autoconf()` 会为每个可写符号输出两行：

```
--cfg=CONFIG_I2C
--cfg=CONFIG_I2C="y"
```

本树该文件 2297 行，正好是“符号数 × 2”量级。`rust_flags` 在 `scripts/Makefile.lib` 第 175 行把它整体 `@` 进来：

```make
rust_flags = $(_rust_flags) $(modkern_rustflags) @$(objtree)/include/generated/rustc_cfg
```

## 4.6 module! 宏

`rust/macros/module.rs` 解析：

```rust
module! {
    type: MyModule,
    name: "my_module",
    author: "…",
    description: "…",
    license: "GPL",
    params: {
        count: u32 { default: 1, description: "…" },
    },
}
```

展开结果大致包括：

1. `struct MyModule` 的 `Module` 实现入口
2. `extern "C" fn __init() -> c_int`，内部调用 `MyModule::init()`
3. `extern "C" fn __exit()`，调用 `Drop` 或显式退出逻辑
4. `#[link_section = ".modinfo"]` 的静态字符串：`license=`、`author=`、`description=`、`vermagic=` 等
5. `module_param_*` 对应的 `__param` 条目与 sysfs 属性
6. `ModuleMetadata` 实现

`__init`/`__exit` 的可见性、`MODULE_LICENSE` 的 GPL 兼容性都在宏里生成；`module!` 同时把 `THIS_MODULE` 传给 `init()`。

### 4.6.1 展开产物逐项核对

`ModuleInfo` 的解析是**有序**的（`parse_ordered_fields!`，第 381-412 行），字段顺序固定为：

```rust
type as type_ [required],
name [required],
authors,
description,
license [required],
alias,
firmware,
imports_ns,
params,
```

也就是说 `license` 必须写在 `name` 之后、`description` 之后，不能随意调换。`type`、`name`、`license` 三项必填。

展开时（第 428-490 行）：

1. `ident = name.replace('-', "_")`：Rust 标识符不允许连字符，`name: "my-mod"` 会变成 `my_mod`，但 `.modinfo` 里仍是原字符串；
2. `ModInfoBuilder` 依次写 `author`（可多个）、`description`、`license`、`alias`、`firmware`、`import_ns`；
3. `file` 只在**内置**时写（`emit_only_builtin("file", &file, false)`），值来自编译期环境变量 `RUST_MODFILE`；这正是 1.5.6 里 `modfile` 的用途，缺失时宏直接 `expect()` panic；
4. 参数额外写 `parmtype=<name>:<type>` 与 `parm=<name>:<desc>`，供 `modinfo` 显示。

需要注意：Rust 侧**不生成** `vermagic=`。vermagic 由 `modpost` 在第二阶段统一写入 `.modinfo`（见 [../11-build/02-module-loader.md](../11-build/02-module-loader.md) 2.6），所以 4.6 列表里的第 4 项应理解为“由 modpost 补上”，而不是宏生成。

模块入口的展开（第 496-612 行）分两种形态：

**可加载模块（`cfg(MODULE)`）**

```rust
#[cfg(MODULE)]
#[no_mangle]
#[link_section = ".init.text"]
pub unsafe extern "C" fn init_module() -> ::kernel::ffi::c_int {
    unsafe { __init() }
}

#[cfg(MODULE)]
#[used(compiler)]
#[link_section = ".init.data"]
static __UNIQUE_ID___addressable_init_module: unsafe extern "C" fn() -> i32 =
    init_module;
```

- 函数名就是 C 侧的 `init_module`，因此模块加载器无需任何特殊约定；
- `__UNIQUE_ID___addressable_init_module` 放在 `.init.data`，保证链接器不会因为“没有直接引用”而丢弃入口；
- 同时生成 `cleanup_module` 到 `.exit.text`，以及对应的 `__UNIQUE_ID___addressable_cleanup_module`；
- `#[cfg(MODULE)] #[used(compiler)] static __IS_RUST_MODULE: () = ();` 是“这是 Rust 模块”的标记，供后续工具识别。

**内置（`not(MODULE)`）**

```rust
#[cfg(not(MODULE))]
#[no_mangle]
pub extern "C" fn #ident_init() -> ::kernel::ffi::c_int {
    unsafe { __init() }
}
```

内置时不能用 `init_module` 这个名字（全内核只能有一个），因此函数名是 `__<ident>_init`，并把函数指针放进 `#initcall_section`；在 `CONFIG_HAVE_ARCH_PREL32_RELOCATIONS` 架构上改用 `global_asm!` 生成相对偏移的 initcall 记录（第 582-591 行）。

两种形态最终都调用同一个 `__init()`：

```rust
unsafe fn __init() -> ::kernel::ffi::c_int {
    let initer = <super::super::LocalModule as ::kernel::InPlaceModule>::init(
        &super::super::THIS_MODULE
    );
    match unsafe { initer.__pinned_init(__MOD.as_mut_ptr()) } {
        Ok(m) => 0,
        ...
```

`__MOD` 是 `static mut MaybeUninit<LocalModule>`，因此模块状态固定驻留、不移动，满足 `Pin` 语义；`Drop` 在 `__exit()` 中执行。

### 4.6.2 参数的生成与限制

每个参数生成两个静态项（`rust/macros/module.rs` 第 120-172 行）：

```rust
pub(crate) static #param_name:
    ::kernel::module_param::ModuleParamAccess<#param_type> =
        ::kernel::module_param::ModuleParamAccess::new(#param_default);

const _: () = {
    #[link_section = "__param"]
    #[used(compiler)]
    static #static_name: ::kernel::module_param::KernelParam =
        ::kernel::module_param::KernelParam::new(
            ::kernel::bindings::kernel_param {
                name: ...,
                mod_: ...,
                ops: core::ptr::from_ref(&#ops),
                perm: 0, // Will not appear in sysfs
                level: -1,
                flags: 0,
                ...
            }
        );
};
```

三个关键限制：

| 项 | 值 | 后果 |
| --- | --- | --- |
| `perm` | `0` | 参数**不出现在** `/sys/module/<name>/parameters/`，只能加载时设置 |
| `level` | `-1` | 不参与 `module_param` 的 `level` 机制（不写进 `__param` 的“级别”分组） |
| `ModuleParam` trait | `Sized + Copy` | 参数类型必须 `Copy`，因为析构可能发生在模块卸载之后，导致 use-after-free |

`name` 的写法也随形态变化：可加载模块用裸名 `#param_name_cstr`，内置模块用 `"<module>.<param>"`（`#param_name_cstr_with_module`），与 C 侧 `MODULE_PARAM_PREFIX` 的行为一致。

`perm: 0` 这条最容易踩坑：C 的 `module_param(foo, int, 0644)` 能在 sysfs 读写，而 Rust 目前的 `module!` 参数是“加载时只读”的。若需要运行期可调，必须等 Rust 抽象补齐（`module_param.rs` 注释里明确写了 “Currently, we only support read-only parameters that are not readable from `sysfs`”）。

## 4.7 alloc：内核分配器抽象

`rust/kernel/alloc.rs` 与 `rust/kernel/alloc/`：

| 类型 | 说明 |
| --- | --- |
| `Kmalloc` | 基于 `kmalloc`，物理连续 |
| `Vmalloc` | 基于 `vmalloc`，虚拟连续 |
| `KVmalloc` | 先尝试 kmalloc，失败回退 vmalloc |
| `Box<T, A>` | 对应 `kbox.rs` |
| `Vec<T, A>` | 对应 `kvec.rs` |
| `KBox` / `KVec` | 常用别名 |

`allocator.rs` 为三种分配器实现 Rust 的 `Allocator` trait：

```rust
unsafe impl Allocator for Kmalloc { ... }
unsafe impl Allocator for Vmalloc { ... }
unsafe impl Allocator for KVmalloc { ... }
```

分配标志通过 `pub mod flags` 暴露：

```rust
pub const GFP_KERNEL: Flags = Flags(bindings::GFP_KERNEL);
pub const GFP_ATOMIC: Flags = Flags(bindings::GFP_ATOMIC);
pub const __GFP_ZERO: Flags = Flags(bindings::__GFP_ZERO);
```

`NumaNode` 类型校验节点号范围，`NumaNode::NO_NODE` 对应 `NUMA_NO_NODE`。

`pin_init` crate 提供 `pin_init!`、`#[pin_data]` 等，用于在稳定地址上初始化自引用结构（如包含 `list_head` 或等待队列的对象）。

### 4.7.1 Allocator 是内核自定义 trait

需要澄清 4.7 的措辞：这里的 `Allocator` **不是** `core::alloc::Allocator`，而是内核自己定义的 `unsafe trait`（`rust/kernel/alloc.rs` 第 163 行）：

```rust
pub unsafe trait Allocator {
    /// The minimum alignment satisfied by all allocations from this allocator.
    const MIN_ALIGN: usize;

    fn alloc(layout: Layout, flags: Flags, nid: NumaNode) -> Result<NonNull<[u8]>, AllocError> {
        unsafe { Self::realloc(None, layout, Layout::new::<()>(), flags, nid) }
    }

    unsafe fn realloc(
        ptr: Option<NonNull<u8>>,
        layout: Layout,
        old_layout: Layout,
        flags: Flags,
        nid: NumaNode,
    ) -> Result<NonNull<[u8]>, AllocError>;

    unsafe fn free(ptr: NonNull<u8>, layout: Layout) {
        // 默认实现：realloc 成零大小
        let _ = unsafe { Self::realloc(Some(ptr), Layout::new::<()>(), layout, Flags(0), NumaNode::NO_NODE) };
    }
}
```

`alloc()` 与 `free()` 都有默认实现，实现者只需提供 `realloc()` 与 `MIN_ALIGN`——这大大降低了移植成本。它比 `core` 的版本多出 `flags` 与 `nid` 两个参数，正好对应 `GFP_*` 与 `NUMA_NO_NODE`；因此三个分配器的实现都只是把参数转给 C 的 `krealloc_node_align` / `vrealloc_node_align` / `kvrealloc_node_align`：

```rust
unsafe impl Allocator for Kmalloc {
    const MIN_ALIGN: usize = ARCH_KMALLOC_MINALIGN;

    unsafe fn realloc(...) -> Result<NonNull<[u8]>, AllocError> {
        let layout = Kmalloc::aligned_layout(layout);
        unsafe { ReallocFunc::KREALLOC.call(ptr, layout, old_layout, flags, nid) }
    }
}
```

`Kmalloc::aligned_layout()` 会把请求对齐抬到 `ARCH_KMALLOC_MINALIGN`，因为 slab 分配器不能保证更小的对齐。

`Flags` 是一个 `u32` newtype，实现了 `BitOr`/`BitAnd`/`Not`，因此可以写 `GFP_KERNEL | __GFP_ZERO`。`flags` 模块（第 76-115 行）暴露了常用常量：

| 常量 | 语义 |
| --- | --- |
| `GFP_KERNEL` | 常规可睡眠分配 |
| `GFP_ATOMIC` | 不可睡眠，低水位线 |
| `GFP_NOWAIT` | 不等待直接回收 |
| `GFP_KERNEL_ACCOUNT` | 计入 kmemcg |
| `__GFP_ZERO` | 清零 |
| `__GFP_HIGHMEM` | 允许高端内存（不能用于 kmalloc） |
| `__GFP_NOWARN` | 抑制失败报告 |

`NumaNode`（第 117-119 行）包装 `i32`，`NumaNode::NO_NODE` 对应 `NUMA_NO_NODE`；它的构造函数会校验负值只允许 `NO_NODE`。

### 4.7.2 各文件规模与分工

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| `rust/kernel/alloc.rs` | 273 | `Allocator` trait、`Flags`、`NumaNode`、模块声明与再导出 |
| `rust/kernel/alloc/allocator.rs` | 323 | `Kmalloc`/`Vmalloc`/`KVmalloc` 的 trait 实现、`ReallocFunc` |
| `rust/kernel/alloc/kbox.rs` | 758 | `Box<T, A>`、`KBox`/`VBox`/`KVBox` |
| `rust/kernel/alloc/kvec.rs` | 1661 | `Vec<T, A>`、`KVec`/`VVec`/`KVVec`、`IntoIter` |
| `rust/kernel/alloc/layout.rs` | 121 | `Layout` 辅助与对齐计算 |

`kvec.rs` 的 1661 行说明“内核版 `Vec`”并不只是别名：它要复刻 `alloc::vec::Vec` 的 API（`push`、`reserve`、`into_iter`、切片转换等），同时使用 `Flags`/`NumaNode` 并处理 `GFP_KERNEL` 下可能睡眠的语义。

## 4.8 platform driver 示例

`samples/rust/rust_driver_platform.rs` 演示一个可匹配 ACPI 与 OF 的 platform driver：

```rust
module! {
    type: SampleDriver,
    name: "rust_driver_platform",
    license: "GPL",
}

kernel::acpi_device_table!(ACPI_TABLE, MODULE_ACPI_TABLE,
    [(acpi::DeviceId::new(c"LNUXBEEF"), Info(0))]);
static OF_TABLE: of::IdTable<Info> = ...;

impl platform::Driver for SampleDriver {
    type IdInfo = Info;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);
    fn probe<'bound>(pdev: &'bound platform::Device<Core>,
                     _id_info: Option<&'bound Self::IdInfo>) -> Result<Pin<KBox<Self>>> {
        ...
    }
}
```

`rust/kernel/platform.rs` 中的 `Adapter<T>` 实现 `driver::DriverLayout` 与 `driver::RegistrationOps`：`register()` 调用 `bindings::__platform_driver_register()`，`unregister()` 调用 `platform_driver_unregister()`；`probe_callback()` 是 `extern "C"`，把 `*mut bindings::platform_device` 转成安全的 `&platform::Device`；`probe` 返回的 `Pin<KBox<T>>` 通过 `set_drvdata()` 绑定到设备。

其它示例：`rust_driver_pci.rs`、`rust_driver_usb.rs`、`rust_driver_i2c.rs`、`rust_driver_auxiliary.rs`、`rust_driver_faux.rs`、`rust_minimal.rs`、`rust_misc_device.rs`、`rust_debugfs.rs`、`rust_configfs.rs`、`rust_dma.rs`、`rust_soc.rs`、`rust_print_main.rs`。

### 4.8.1 与真实示例的差异

4.8 的代码片段是示意性的，与本树 `samples/rust/rust_driver_platform.rs` 有多处不同，实际写法是：

```rust
struct SampleDriver {
    pdev: ARef<platform::Device>,
}

struct Info(u32);

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <SampleDriver as platform::Driver>::IdInfo,
    [(of::DeviceId::new(c"test,rust-device"), Info(42))]
);

kernel::acpi_device_table!(
    ACPI_TABLE,
    MODULE_ACPI_TABLE,
    <SampleDriver as platform::Driver>::IdInfo,
    [(acpi::DeviceId::new(c"LNUXBEEF"), Info(0))]
);

impl platform::Driver for SampleDriver {
    type IdInfo = Info;
    type Data<'bound> = Self;
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);
    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = Some(&ACPI_TABLE);

    fn probe<'bound>(
        pdev: &'bound platform::Device<Core<'_>>,
        info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self, Error> + 'bound {
        let dev = pdev.as_ref();
        dev_dbg!(dev, "Probe Rust Platform driver sample.\n");
        if let Some(info) = info {
            dev_info!(dev, "Probed with info: '{}'.\n", info.0);
        }
        Ok(Self { pdev: pdev.into() })
    }
}
```

差异点：

| 4.8 旧片段 | 本树实际 |
| --- | --- |
| `module! { ... }` | `kernel::module_platform_driver! { type, name, authors, description, license }`（第 188-194 行） |
| `probe` 返回 `Result<Pin<KBox<Self>>>` | 返回 `impl PinInit<Self, Error> + 'bound` |
| 设备参数 `&platform::Device<Core>` | `&platform::Device<Core<'_>>` |
| 无 `Data` 关联类型 | 必须声明 `type Data<'bound>: Send + 'bound` |
| 用 `Drop` 释放 | `Drop` 仍可用，但另有 `unbind()` 可在仍有 `&Device<Core>` 时做优雅拆除 |

`platform::Driver` 的定义（`rust/kernel/platform.rs` 第 209-249 行）：

```rust
pub trait Driver {
    type IdInfo: 'static;
    type Data<'bound>: Send + 'bound;

    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = None;
    const ACPI_ID_TABLE: Option<acpi::IdTable<Self::IdInfo>> = None;

    fn probe<'bound>(
        dev: &'bound Device<device::Core<'_>>,
        id_info: Option<&'bound Self::IdInfo>,
    ) -> impl PinInit<Self::Data<'bound>, Error> + 'bound;

    fn unbind<'bound>(dev: &'bound Device<device::Core<'_>>, this: Pin<&Self::Data<'bound>>) {
        let _ = (dev, this);
    }
}
```

`Data` 用 GAT（generic associated type）带生命周期，是为了让驱动私有数据可以借用设备；`probe` 返回 `PinInit` 而不是已构造值，配合 `module_platform_driver!` 的 `Pin<KBox<T>>` 分配，保证自引用结构不会被移动。

`Adapter<T>` 的三段式实现：

```rust
pub struct Adapter<T: Driver>(T);

unsafe impl<T: Driver> driver::DriverLayout for Adapter<T> { ... }

unsafe impl<T: Driver> driver::RegistrationOps for Adapter<T> {
    unsafe fn register(...) -> Result {
        (*pdrv.get()).probe = Some(Self::probe_callback);
        ...
        unsafe { bindings::__platform_driver_register(pdrv.get(), module.0, name.as_char_ptr()) }
    }
    unsafe fn unregister(pdrv: &Opaque<Self::DriverType>) {
        unsafe { bindings::platform_driver_unregister(pdrv.get()) };
    }
}

impl<T: Driver> driver::Adapter for Adapter<T> {
    type IdInfo = T::IdInfo;
    fn of_id_table() -> Option<of::IdTable<Self::IdInfo>> { T::OF_ID_TABLE }
    fn acpi_id_table() -> Option<acpi::IdTable<Self::IdInfo>> { T::ACPI_ID_TABLE }
}
```

`probe_callback()` 是 `extern "C"`，把 `*mut bindings::platform_device` 转成 `&platform::Device`，调用 `T::probe()` 得到 `PinInit`，`__pinned_init` 到 `KBox` 后再 `set_drvdata()` 绑定到设备。这套“`DriverLayout` + `RegistrationOps` + `Adapter`”三层抽象也被 PCI/USB/I2C/Auxiliary 复用，差别只在 `DriverType` 与 `register` 函数。

### 4.8.2 本树 samples/rust 的实际清单

本树 `samples/rust/` 下共 15 个 `.rs` 文件（不含 `hostprogs/`）：

```
rust_configfs.rs          rust_debugfs.rs           rust_debugfs_scoped.rs
rust_dma.rs               rust_driver_auxiliary.rs  rust_driver_faux.rs
rust_driver_i2c.rs        rust_driver_pci.rs        rust_driver_platform.rs
rust_driver_usb.rs        rust_i2c_client.rs        rust_minimal.rs
rust_misc_device.rs       rust_print_main.rs        rust_soc.rs
```

对应 `samples/rust/Makefile` 的 `obj-$(CONFIG_SAMPLE_RUST_*)`，每个都有独立 Kconfig 开关（`samples/rust/Kconfig`）：

| 示例 | 依赖 |
| --- | --- |
| `SAMPLE_RUST_CONFIGFS` | `CONFIGFS_FS` |
| `SAMPLE_RUST_DEBUGFS` / `_SCOPED` | `DEBUG_FS` |
| `SAMPLE_RUST_DRIVER_I2C` / `I2C_CLIENT` | `I2C=y` |
| `SAMPLE_RUST_DRIVER_PCI` | `PCI` |
| `SAMPLE_RUST_DRIVER_USB` | `USB=y` |
| `SAMPLE_RUST_DRIVER_AUXILIARY` | `PCI` |
| `SAMPLE_RUST_PRINT` | 无（`rust_print-y := rust_print_main.o rust_print_events.o`） |

注意**没有** `rust_chrdev.rs`：字符设备示例在本树中由 `rust_misc_device.rs`（miscdevice）与 `rust_debugfs.rs` 承担，`chrdev` 抽象不在 `rust/kernel/` 中。若在别的文档看到 `samples/rust/rust_chrdev.rs`，那是旧版本或下游补丁。

另外 `rust_print` 是复合对象（`rust_print-y`），依赖 `rust_print_events.c` 里的 `TRACE_EVENT`；`samples/rust/Makefile` 为此加了 `ccflags-y += -I$(src)`。

## 4.9 模块产物与 modpost

Rust 模块的构建路径与 C 模块一致：

1. `rustc --crate-type rlib` 编译 `.rs` → `.o`
2. `scripts/Makefile.build` 的 `rust_common_cmd` 负责 `--emit=obj`、依赖文件、`RUST_MODFILE`
3. `modpost` 读取 `modules.order`，为模块生成 `.mod.c`（含 vermagic、license、参数）
4. `scripts/Makefile.modfinal` 链接出 `.ko`

因此 Rust 模块同样受 `CONFIG_MODULES` 门控；当前配置未启用模块，Rust 模块无从加载。

### 4.9.1 rustc 的调用参数

把 `scripts/Makefile.build` 第 325-359 行的 `rust_common_cmd` 展开，一次 Rust 编译实际是：

```make
OBJTREE=$(abspath $(objtree)) \
RUST_MODFILE=$(modfile) $(RUSTC_OR_CLIPPY) $(rust_flags) \
	-Zallow-features=$(rust_allowed_features) \
	-Zcrate-attr=no_std \
	-Zcrate-attr='feature($(rust_allowed_features))' \
	-Zunstable-options --extern pin_init --extern kernel \
	--extern zerocopy --extern zerocopy_derive \
	--crate-type rlib -L $(objtree)/rust/ \
	--sysroot=/dev/null \
	--out-dir $(dir $@) --emit=dep-info=$(depfile)
```

几个参数值得解释：

| 参数 | 作用 |
| --- | --- |
| `OBJTREE=` | `bindings/lib.rs` 用 `env!("OBJTREE")` 拼 `include!` 路径 |
| `RUST_MODFILE=$(modfile)` | `module!` 宏写 `file=` modinfo 时读取（见 4.6.1） |
| `-Zcrate-attr=no_std` | 不需每个源文件写 `#![no_std]` |
| `--sysroot=/dev/null` | 禁用 sysroot 查找，强制使用 `-L` 指定的自建 `core`/`compiler_builtins` |
| `-L $(objtree)/rust/` | 指向本次构建产出的 `.rlib` |
| `--emit=dep-info=$(depfile)` | 生成 `.d`，供 `cmd_and_fixdep` 写 `.cmd` |

`--sysroot=/dev/null` 是最容易被误解的一项：它不是“没有 sysroot”，而是让 rustc 不去默认路径找 `core`，从而必须由 `-L $(objtree)/rust/` 提供自建版本。`cmd_rustc_library`（`rust/Makefile` 第 633-650 行）对所有 Rust 库（含 `core.o` 自身）都传这个参数，`core.o` 之所以仍能编译，是因为它的源文件由绝对路径 `$(RUST_LIB_SRC)/core/src/lib.rs` 直接指定，且 `-L` 指向本次构建目录。

`-Zallow-features` 的列表（第 320 行）是外部代码（`samples/`、`drivers/`）可用的 unstable feature 白名单：

```make
rust_allowed_features := arbitrary_self_types,asm_goto,generic_arg_infer,used_with_arg
```

注释还给出了稳定时间表：`asm_goto` 1.87.0 稳定、`generic_arg_infer` 1.89.0 稳定、`arbitrary_self_types` 预计稳定、`used_with_arg` 待定。

### 4.9.2 内置 Rust 对象与 vmlinux

`CONFIG_RUST=y` 时，`rust/` 目录的产物并不都是模块：

- `core.o`、`compiler_builtins.o`、`ffi.o`、`zerocopy.o`、`pin_init.o`、`bindings.o`、`uapi.o`、`kernel.o`、`build_error.o`、`exports.o` 通过 `obj-$(CONFIG_RUST)` 进入 `built-in.a`，最终链入 `vmlinux`；
- `samples/rust/*.rs` 若配置为 `=m`，才走模块两阶段构建；配置为 `=y` 则同样内置。

因此“启用 Rust”即使不启用模块，也能让 `kernel` crate 存在并编译，只是没有 Rust 驱动可加载。这与 C 侧“核心设施内置、驱动可选模块”的模式一致。

### 4.9.3 modpost 对 Rust 模块没有特殊分支

`scripts/mod/modpost.c` 里搜不到任何 `rust` 相关逻辑（仅有历史版权声明里的 “Rusty Russell”）。Rust 模块与 C 模块在 modpost 眼里完全相同：

- `.modinfo` 里的 `license`/`author`/`description` 由 `module!` 宏生成；
- `vermagic` 由 modpost 按 `VERMAGIC_STRING` 统一写入；
- 导出符号通过 `rust/exports.c` 的 `EXPORT_SYMBOL_RUST_GPL()` 进入 `__ksymtab`，与 C 的 `EXPORT_SYMBOL` 同一套节；
- 符号 CRC 由 `gendwarfksyms`（`CONFIG_GENDWARFKSYMS`）或 `genksyms` 计算，Rust 对象同样适用（`rust/Makefile` 的 `rule_rustc_library` 末尾有 `$(call cmd,gendwarfksyms)`）。

唯一“Rust 专属”的痕迹是 `module!` 生成的 `__IS_RUST_MODULE` 静态标记（第 534-536 行），它是给未来工具（如 `klp-build`、调试脚本）识别用的，当前 modpost 并不消费它。

## 4.10 当前 .config 现状

| 项 | 当前状态 | 后果 |
| --- | --- | --- |
| `CONFIG_HAVE_RUST` | `y` | 架构支持 Rust |
| `CONFIG_RUSTC_VERSION` | `109600` | 探测到 rustc 1.96.0 能力 |
| `CONFIG_RUSTC_LLVM_VERSION` | `220102` | LLVM 22.1.2 |
| `CONFIG_RUST` | **不存在** | `rust/` 不参与构建，`rustavailable` 只做检查 |
| `CONFIG_MODULES` | 未启用 | 即使启用 Rust，也无法生成可加载的 `.ko` |

需要区分：

- **源码存在**：`rust/`、`samples/rust/`、`Documentation/rust/`、`scripts/rust_is_available.sh` 均完整。
- **当前配置未启用**：`.config` 中检索不到 `CONFIG_RUST`，`prepare` 不会进入 `$(MAKE) $(build)=rust`，`include/generated/rustc_cfg` 虽然存在，但只反映已探测的 cfg。
- **依赖未满足**：`CONFIG_RUST` 的 Kconfig 还依赖 `HAVE_RUST`、`RUST_IS_AVAILABLE`、`!RANDSTRUCT`、`!GCC_PLUGIN_RANDSTRUCT`、`!DEBUG_INFO_BTF || (PAHOLE_HAS_LANG_EXCLUDE && !LTO)` 等（`init/Kconfig`）。当前配置虽满足大部分，但显式未选择该符号。

要在本树启用 Rust，需要：

```sh
make LLVM=1 rustavailable
make LLVM=1 menuconfig      # General setup -> Rust support
# 同时打开 CONFIG_MODULES 才能构建 Rust 模块
```

## 4.11 关键文件清单

- `/home/louis/code/linux/rust/Makefile`：Rust 目标、bindgen、crate 列表
- `/home/louis/code/linux/rust/kernel/lib.rs`：`kernel` crate 根、`Module` trait、模块列表
- `/home/louis/code/linux/rust/kernel/platform.rs`：platform driver 适配器
- `/home/louis/code/linux/rust/kernel/driver.rs`：通用 driver 注册抽象
- `/home/louis/code/linux/rust/kernel/device.rs`：设备模型绑定
- `/home/louis/code/linux/rust/kernel/alloc.rs`、`rust/kernel/alloc/`：`Kmalloc`/`Vmalloc`/`KVmalloc`
- `/home/louis/code/linux/rust/macros/module.rs`：`module!` 宏展开
- `/home/louis/code/linux/rust/bindings/bindings_helper.h`：bindgen 输入
- `/home/louis/code/linux/rust/bindgen_parameters`：bindgen 过滤参数
- `/home/louis/code/linux/rust/helpers/`：仅导出给 Rust 的 C 辅助函数
- `/home/louis/code/linux/rust/pin-init/`：pin 初始化
- `/home/louis/code/linux/scripts/Makefile.build`：Rust 编译规则与允许 feature
- `/home/louis/code/linux/scripts/rust_is_available.sh`：工具链检查
- `/home/louis/code/linux/samples/rust/rust_driver_platform.rs`：platform driver 示例
- `/home/louis/code/linux/samples/rust/rust_minimal.rs`：最小模块示例
- `/home/louis/code/linux/Documentation/rust/quick-start.rst`：安装与构建步骤
- `/home/louis/code/linux/Documentation/rust/general-information.rst`：总体设计

## 4.12 配置复核：为什么本树选不上 CONFIG_RUST

4.10 的表述基本正确，但可以给出更硬的证据。`init/Kconfig` 第 2210-2224 行的依赖是：

```kconfig
config RUST
	bool "Rust support"
	depends on HAVE_RUST
	depends on RUST_IS_AVAILABLE
	select EXTENDED_MODVERSIONS if MODVERSIONS
	depends on !MODVERSIONS || GENDWARFKSYMS
	depends on !GCC_PLUGIN_RANDSTRUCT
	depends on !RANDSTRUCT
	depends on !DEBUG_INFO_BTF || (PAHOLE_HAS_LANG_EXCLUDE && !LTO)
	depends on !CFI || HAVE_CFI_ICALL_NORMALIZE_INTEGERS_RUSTC
	depends on !KASAN || CC_IS_CLANG
	depends on !KASAN_SW_TAGS || RUSTC_VERSION >= 109600
```

逐项对照本树 `.config` 与实测环境：

| 依赖 | 本树状态 | 是否满足 |
| --- | --- | --- |
| `HAVE_RUST` | `=y`（第 690 行） | 是 |
| `RUST_IS_AVAILABLE` | **未出现在 `.config` / `autoconf.h` 中** | **否** |
| `!MODVERSIONS \|\| GENDWARFKSYMS` | `MODVERSIONS` 未设置 | 是 |
| `!GCC_PLUGIN_RANDSTRUCT` | 未设置 | 是 |
| `!RANDSTRUCT` | `CONFIG_RANDSTRUCT_NONE=y`（第 3387 行） | 是 |
| `!DEBUG_INFO_BTF \|\| ...` | `DEBUG_INFO_BTF` 未设置（第 3531 行） | 是 |
| `!CFI \|\| ...` | `CFI` 未设置 | 是 |
| `!KASAN \|\| CC_IS_CLANG` | `KASAN` 未设置（第 3613 行） | 是 |
| `!KASAN_SW_TAGS \|\| RUSTC_VERSION >= 109600` | 未设置 KASAN_SW_TAGS | 是 |

关键就是第二行。`RUST_IS_AVAILABLE` 是隐藏符号：

```kconfig
config RUST_IS_AVAILABLE
	def_bool $(success,$(srctree)/scripts/rust_is_available.sh)
```

它没有 prompt，值为 `n` 时不会被写进 `.config`（所以既看不到 `=y`，也看不到 `# ... is not set`）。实际运行脚本可以确认原因：

```sh
$ RUSTC=rustc BINDGEN=bindgen CC=cc sh scripts/rust_is_available.sh
***
*** Rust bindings generator 'bindgen' could not be found.
***
```

也就是说：本机有 `rustc`（1.99.1）与 `clang`（21.1.8），但**没有 `bindgen`**，脚本在第 2 步就失败，于是 `RUST_IS_AVAILABLE=n`，`CONFIG_RUST` 在 menuconfig 里根本不可见。这与 4.10 的结论一致，但根因更具体：不是“未选择”，而是“工具链不满足导致不可选”。

另外 `.config` 中的版本探测值（`RUSTC_VERSION=109600`、`RUSTC_LLVM_VERSION=220102`）与本机现状（rustc 1.99.1 / LLVM 23.1.1、clang 21.1.8）不一致，说明 `.config` 是由另一套工具链生成的。若要启用 Rust，必须先重新生成配置，而不是直接改 `.config`。

## 4.13 启用 Rust 的完整前置条件

按依赖顺序，需要依次满足：

```sh
# 1. 安装 bindgen（>= 0.71.1）与 libclang（>= 17.0.1）
#    Debian/Ubuntu: apt install bindgen clang libclang-dev
#    Fedora:        dnf install bindgen-cli clang
# 2. rust-src 组件
rustup component add rust-src
# 3. 确认工具链
make LLVM=1 rustavailable
# 4. 选择符号
make LLVM=1 menuconfig     # General setup -> Rust support
# 5. 若需 Rust 模块
#    General setup -> Enable loadable module support
make LLVM=1 -j$(nproc)
```

几个容易卡住的点：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `Rust bindings generator 'bindgen' could not be found` | 未安装 bindgen | 安装 `bindgen`，注意版本 ≥ 0.71.1 |
| `libclang ... is too old` | `bindgen` 加载到的 libclang < 17.0.1 | 安装新版 LLVM，或设 `LIBCLANG_PATH` |
| `Source code for the 'core' standard library could not be found` | 缺 `rust-src` | `rustup component add rust-src`，或设 `RUST_LIB_SRC` |
| `error: the option 'Z' is only accepted on the nightly compiler` 类错误 | 用了 stable rustc 但内核要求特定 feature | 内核支持的最低版本已包含所需 feature；若仍报错，检查 `RUSTC` 是否被环境覆盖 |
| `Missing kernel configuration for conditional compilation` | `kernel` crate 在无 `CONFIG_RUST` 下被编译 | 必须先打开 `CONFIG_RUST`（这是 `lib.rs` 的 `compile_error!` 防线） |
| `RUSTC_CLANG_LLVM_COMPATIBLE=n` 导致 `RUST_INLINE_HELPERS` 不可选 | clang 与 rustc 的 LLVM 版本不一致 | 本机 rustc LLVM 23.1.1 vs clang 21.1.8，正属此列；要么对齐版本，要么保持该选项关闭 |

最后一条尤其适用于本树：`.config` 探测到 LLVM 22.1.2，而本机 clang 是 21.1.8、rustc 内置 LLVM 是 23.1.1。`CONFIG_RUST_INLINE_HELPERS` 的 Kconfig 明确要求 `RUSTC_CLANG_LLVM_COMPATIBLE`，因此即使装上 bindgen，也只能走“helper C 文件单独编译”的非内联路径。

## 4.14 测试与文档

Rust 代码有三类测试（`Documentation/rust/testing.rst`）：

| 类型 | 入口 | 说明 |
| --- | --- | --- |
| KUnit 测试 | `CONFIG_RUST_KUNIT_TESTS` + 各 `RUST_*_KUNIT_TEST` | `rust/kernel/Kconfig.test` 定义，覆盖 allocator/KVec/bitmap/str/atomics/bitfield |
| doctest | `CONFIG_RUST_KERNEL_DOCTESTS`（依赖 `KUNIT=y`） | 把 `kernel` crate 文档示例转成 KUnit |
| Kselftest | `tools/testing/selftests/` | 用户态侧 |

相关目标：

```sh
make LLVM=1 rustdoc        # 生成 Rust 文档（target: rustdoc）
make LLVM=1 rusttest       # 运行 doctest/KUnit 包装
make LLVM=1 CLIPPY=1       # clippy 静态检查（会改变代码生成，勿用于生产构建）
make LLVM=1 rustfmt        # 格式化（跳过 vendored crate 与 generated 文件）
```

本树 `.config` 中 `CONFIG_KUNIT` 未设置（第 3719 行），因此 `RUST_KUNIT_TESTS` 与 `RUST_KERNEL_DOCTESTS` 都不可选；这也意味着即使启用 Rust，也要先打开 KUnit 才能跑这些测试。

`Documentation/rust/` 目录内容：

| 文件 | 内容 |
| --- | --- |
| `quick-start.rst` | 各发行版安装步骤、`RUST_LIB_SRC`、libclang、rustfmt/clippy |
| `general-information.rst` | `no_std`、rustdoc、clippy、abstractions vs bindings |
| `coding-guidelines.rst` | 命名、注释、`unsafe` 使用规范 |
| `arch-support.rst` | 各架构支持矩阵 |
| `testing.rst` | KUnit/doctest/kselftest |
| `index.rst` | 目录入口 |

`general-information.rst` 里“Abstractions vs. bindings”一节是理解 `kernel` crate 设计的关键：bindings 只是 C 声明的 Rust 翻译，abstractions 才是把 `struct mutex` 包装成安全 `Mutex` 的那一层；文档明确要求“缺什么抽象就补抽象，而不是在驱动里直接用 bindings”。

## 4.15 关键文件清单（补充）

- `/home/louis/code/linux/init/Kconfig`（第 63-95、2210-2235 行）：`RUSTC_VERSION`、`RUST_IS_AVAILABLE`、`RUST`
- `/home/louis/code/linux/lib/Kconfig.debug`（第 3597-3643 行）：`RUST_BUILD_ASSERT_ALLOW`、`RUST_KERNEL_DOCTESTS`、`RUST_INLINE_HELPERS`
- `/home/louis/code/linux/rust/kernel/Kconfig.test`：Rust KUnit 测试开关
- `/home/louis/code/linux/rust/kernel/alloc.rs`：`Allocator` trait、`Flags`、`NumaNode`
- `/home/louis/code/linux/rust/kernel/alloc/allocator.rs`：三种分配器实现
- `/home/louis/code/linux/rust/kernel/module_param.rs`：`ModuleParam`、`KernelParam`、`ModuleParamAccess`
- `/home/louis/code/linux/rust/kernel/prelude.rs`：常用导入（`module`、`pin_init`、`vtable` 等宏）
- `/home/louis/code/linux/rust/kernel/platform.rs`：`Driver` trait 与 `Adapter<T>`
- `/home/louis/code/linux/rust/kernel/driver.rs`：`DriverLayout`、`RegistrationOps`、`Adapter`
- `/home/louis/code/linux/rust/kernel/device.rs`、`rust/kernel/device/property.rs`：设备模型与 fwnode 属性读取
- `/home/louis/code/linux/rust/macros/lib.rs`：过程宏入口（`module`、`vtable`、`fmt` 等）
- `/home/louis/code/linux/rust/build_error.rs`：`build_assert!` 兜底
- `/home/louis/code/linux/rust/compiler_builtins.rs`：`panic` 内建函数
- `/home/louis/code/linux/scripts/generate_rust_target.rs`：自定义 target 规格（仅 x86 使用）
- `/home/louis/code/linux/scripts/rust_is_available.sh`：工具链检查
- `/home/louis/code/linux/scripts/min-tool-version.sh`：最低版本表

## 4.16 与相邻章节的衔接

- Rust 的 Kbuild 接入（`rust_common_cmd`、`if_changed`、`.cmd`）与 C 代码完全同构，细节见 [../11-build/01-kbuild-kconfig.md](../11-build/01-kbuild-kconfig.md) 的 1.6.6 与 1.11。
- Rust 模块最终仍是标准 `.ko`，其加载、vermagic、符号解析见 [../11-build/02-module-loader.md](../11-build/02-module-loader.md)；`module!` 生成的 `init_module`/`cleanup_module` 正是那里描述的入口名。
- Rust 抽象所包装的子系统（platform、PCI、USB、I2C、debugfs、configfs）在 `../03-drivers/` 与 `../05-filesystems/` 下有对应 C 侧章节；阅读时注意 Rust 侧的 `Driver`/`Adapter` 只是 C 注册接口的一层薄封装。
- livepatch 与 `klp-build` 的现状见 [../11-build/03-livepatch-kexec.md](../11-build/03-livepatch-kexec.md) 的 3.15；Rust 侧的 `__IS_RUST_MODULE` 标记未来可能被这类工具消费。
