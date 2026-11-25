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

## 4.9 模块产物与 modpost

Rust 模块的构建路径与 C 模块一致：

1. `rustc --crate-type rlib` 编译 `.rs` → `.o`
2. `scripts/Makefile.build` 的 `rust_common_cmd` 负责 `--emit=obj`、依赖文件、`RUST_MODFILE`
3. `modpost` 读取 `modules.order`，为模块生成 `.mod.c`（含 vermagic、license、参数）
4. `scripts/Makefile.modfinal` 链接出 `.ko`

因此 Rust 模块同样受 `CONFIG_MODULES` 门控；当前配置未启用模块，Rust 模块无从加载。

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
