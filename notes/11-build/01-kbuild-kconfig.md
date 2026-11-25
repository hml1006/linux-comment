# 1. Kbuild 与 Kconfig：内核配置与构建体系

## 1.1 概述

内核构建分为两个相对独立但衔接紧密的层次：

1. **配置层（Kconfig）**：用 Kconfig 语言描述数千个配置符号及其依赖关系，生成 `.config`、`include/config/auto.conf`、`include/generated/autoconf.h`。
2. **构建层（Kbuild）**：由顶层 `Makefile`、`scripts/Makefile.build`、`scripts/Makefile.lib`、`scripts/Makefile.modpost` 等组成，按目录递归编译、归档、链接出 `vmlinux` 与模块。

当前源码树版本标识来自顶层 `Makefile` 第 2-6 行：

```make
VERSION = 7
PATCHLEVEL = 2
SUBLEVEL = 0
EXTRAVERSION =
NAME = Baby Opossum Posse
```

Kbuild 使用 GNU Make（顶层要求 Make >= 4.0，见 `Makefile` 开头的 `.FEATURES` 检查），核心思想是 **递归 make + 每个目录只写自己的产物**。顶层 `Makefile` 第 28-40 行的注释明确指出：子 Makefile 只应修改自己目录内的文件，跨目录依赖通过 `built-in.a` 与子 make 调用来串接。

## 1.2 Kconfig 语言

Kconfig 的语法由 `scripts/kconfig/lexer.l` 与 `scripts/kconfig/parser.y` 定义。词法层识别的关键字包括（`scripts/kconfig/lexer.l` 第 104-127 行附近）：

```
choice  config  default  endchoice  endif  help
if  imply  menuconfig  range  select  source
```

### 1.2.1 条目类型

| 条目 | 作用 |
| --- | --- |
| `config` | 定义一个配置符号，可在 `.config` 中出现 |
| `menuconfig` | 与 `config` 相同，但允许整块子菜单被禁用 |
| `choice` / `endchoice` | 互斥选择组，只有一个成员可取 `y` |
| `menu` / `endmenu` | 纯组织性菜单，不含符号 |
| `if` / `endif` | 条件包含一段配置 |
| `source` | 引入另一个 Kconfig 文件 |
| `comment` | 仅显示注释文本 |

### 1.2.2 属性

每个 `config` 可带一组属性：

```kconfig
config AUDIT
	bool "Auditing support"
	depends on NET
	help
	  Enable auditing infrastructure ...
```

（实际内容见 `init/Kconfig` 第 539 行起的 `config AUDIT`。）

常用属性：

- 类型：`bool`、`tristate`、`string`、`int`、`hex`
- `prompt`：显式给出提示文本，等价于类型后的字符串
- `default`：默认值，可带条件
- `depends on`：硬依赖，不满足则符号不可见/不可选
- `select`：反向依赖，选中本符号时强制打开目标符号（不处理目标自身的依赖）
- `imply`：弱反向依赖，目标仍可被用户关闭
- `range`：数值范围
- `help`：帮助文本

`tristate` 在 `.config` 中表现为 `y`、`m`、`n` 三态；`bool` 只有 `y`/`n`。

`depends on A && B` 表示符号只在 A、B 都为真时可见；`select C` 不检查 C 的依赖，是 Kconfig 常见问题来源（见 `Documentation/kbuild/kconfig.select-break`）。

## 1.3 scripts/kconfig 的组成

`scripts/kconfig/` 是宿主侧工具，不被编入内核：

| 文件 | 职责 |
| --- | --- |
| `lexer.l` / `lexer.lex.c` | 词法分析，生成 token |
| `parser.y` / `parser.tab.c` | 语法分析，构建 menu/symbol 树 |
| `menu.c` | 菜单树遍历与可见性计算 |
| `symbol.c` | 符号、依赖、取值传播 |
| `expr.c` | 表达式求值与依赖求解 |
| `confdata.c` | `.config` 读写、`auto.conf`/`autoconf.h` 生成 |
| `util.c` | 通用工具函数 |
| `conf.c` | 命令行前端（`--oldconfig`、`--syncconfig` 等） |
| `mconf.c` + `lxdialog/` | `menuconfig` 的 ncurses 前端 |
| `nconf.c`、`gconf.c` | `nconfig`、`gconfig` 前端 |
| `merge_config.sh` | 合并多个配置片段（`make merge_config`） |
| `streamline_config.pl` | `localmodconfig`/`localyesconfig` 的裁剪脚本 |

### 1.3.1 前端程序

`scripts/kconfig/Makefile` 把 `config/menuconfig/nconfig/gconfig/xconfig` 分别映射到 `conf/mconf/nconf/gconf/qconf`。顶层 `Makefile` 第 762-766 行把所有 `*config` 目标转发给 Kconfig 子系统：

```make
config: outputmakefile scripts_basic FORCE
	$(Q)$(MAKE) $(build)=scripts/kconfig $@

%config: outputmakefile scripts_basic FORCE
	$(Q)$(MAKE) $(build)=scripts/kconfig $@
```

常见目标：

- `make config` / `menuconfig` / `nconfig` / `xconfig`
- `make oldconfig`：以现有 `.config` 为基础，只询问新符号
- `make olddefconfig`：新符号取默认值，不交互
- `make defconfig`：使用 `arch/$(SRCARCH)/configs/$(KBUILD_DEFCONFIG)`
- `make savedefconfig`：输出精简的 `defconfig`
- `make allyesconfig` / `allnoconfig` / `randconfig`
- `make localmodconfig` / `localyesconfig`：依据当前已加载模块裁剪（`streamline_config.pl`）

### 1.3.2 从 .config 到构建输入

`confdata.c` 中 `conf_write_autoconf()`（第 1177 行起）依次写出：

1. `include/generated/autoconf.h`：C 预处理器宏（`#define CONFIG_FOO 1`）
2. `include/config/auto.conf`：Make 变量（`CONFIG_FOO=y`）
3. `include/generated/rustc_cfg`：Rust 侧 `--cfg` 输入
4. `include/config/tristate.conf` 等中间文件

注意 `confdata.c` 第 1209-1213 行的注释：`auto.conf` 必须最后写，因为 Kbuild 把 `auto.conf` 当作配置成功的标志和依赖时间戳。

## 1.4 顶层 Makefile 的主要目标

顶层 `Makefile` 的目标大致分为：

| 目标 | 说明 |
| --- | --- |
| `__all` | 默认入口，第 23 行定义 |
| `all` | 第 836 行 `all: vmlinux`；若配置了模块还会追加 modules |
| `modules` | 第 1708 行，依赖 `vmlinux` 与 `modules_prepare` |
| `config` / `%config` | 转发给 `scripts/kconfig` |
| `prepare` / `prepare0` / `archprepare` | 生成头文件、脚本、`include/generated/*` |
| `scripts` | 构建 `scripts/` 下的宿主工具 |
| `vmlinux` | 第 1369 行，依赖 `vmlinux.o`、链接脚本、`modpost` |
| `clean` / `mrproper` / `distclean` | 清理级别递增 |
| `headers_install` / `headers` | 导出 UAPI 头 |
| `dtbs` / `dtbs_install` | 设备树构建与安装 |
| `rustavailable` | 第 1975 行，检查 Rust 工具链可用性 |
| `help` | 打印所有可用目标 |

`prepare` 链的关键片段（第 1400-1416 行）：

```make
archprepare: outputmakefile archheaders archscripts scripts include/config/kernel.release \
	asm-generic $(version_h) include/generated/utsrelease.h \
	include/generated/compile.h include/generated/autoconf.h \
	include/generated/rustc_cfg remove-stale-files

prepare0: archprepare
	$(Q)$(MAKE) $(build)=scripts/mod
	$(Q)$(MAKE) $(build)=. prepare

prepare: prepare0
ifdef CONFIG_RUST
	+$(Q)$(CONFIG_SHELL) $(srctree)/scripts/rust_is_available.sh
	$(Q)$(MAKE) $(build)=rust
endif
```

这段说明：Rust 构建被挂在 `prepare` 阶段，且只有在 `CONFIG_RUST` 为真时才执行。

## 1.5 Kbuild 变量体系

每个包含 Kbuild 文件的目录，都会由 `scripts/Makefile.build` 初始化一组变量（第 20-42 行）：

```make
obj-y :=
obj-m :=
lib-y :=
lib-m :=
always-y :=
always-m :=
targets :=
subdir-y :=
subdir-m :=
ccflags-y :=
asflags-y :=
ldflags-y :=
rustflags-y :=
subdir-ccflags-y :=
subdir-asflags-y :=
```

### 1.5.1 obj-y 与 obj-m

- `obj-y += foo.o`：编入内核（最终进入 `vmlinux`）
- `obj-m += foo.o`：编成可加载模块
- 同一对象同时出现在两者时，只保留内置版本：

```make
obj-m := $(filter-out $(obj-y),$(obj-m))
```

- 目录项 `obj-y += foo/` 会被替换为 `foo/built-in.a`；`obj-m += foo/` 会被替换为 `foo/modules.order`（第 61-78 行）。

### 1.5.2 复合对象

若存在 `foo-objs`、`foo-y`、`foo-m`、`foo-` 等变量，则 `foo.o` 是复合对象：

```make
obj-$(CONFIG_XYZ) += foo.o
foo-objs := a.o b.o
```

`Makefile.build` 用 `multi-search` / `real-search` 展开这些变量，得到真正的编译单元 `real-obj-y` / `real-obj-m`。

### 1.5.3 lib-y

`lib-y` 汇总为当前目录的 `lib.a`（`lib-y := $(filter-out $(obj-y), $(sort $(lib-y) $(lib-m)))`），用于归档式库代码；`KBUILD_VMLINUX_LIBS` 会在最终链接时以 `--start-group ... --end-group` 处理。

### 1.5.4 flags 变量

- `ccflags-y` / `asflags-y` / `ldflags-y` / `rustflags-y`：当前目录
- `subdir-ccflags-y` / `subdir-asflags-y`：当前目录及所有子目录
- 单文件覆盖：`CFLAGS_foo.o`、`AFLAGS_foo.o`、`RUSTFLAGS_foo.o`
- 移除：`CFLAGS_REMOVE_foo.o`、`ccflags-remove-y`

`scripts/Makefile.lib` 的 `_c_flags`/`_rust_flags`/`_a_flags` 负责把这些变量与 `KBUILD_CFLAGS` 等组合，并结合 KASAN、KCSAN、KCOV、UBSAN、GCOV 等开关按文件注入 instrumentation 选项。

## 1.6 编译、归档与链接流程

### 1.6.1 进入目录

`scripts/Kbuild.include` 第 113 行定义了构建入口：

```make
build := -f $(srctree)/scripts/Makefile.build obj
```

因此 `$(MAKE) $(build)=drivers/foo` 等价于 `make -f scripts/Makefile.build obj=drivers/foo`。

`scripts/Makefile.build` 开头（第 7-42 行）设置 `src := $(srcroot)/$(obj)`，清空并初始化变量，然后依次包含：

```make
-include $(objtree)/include/config/auto.conf
include $(srctree)/scripts/Kbuild.include
include $(srctree)/scripts/Makefile.compiler
include $(kbuild-file)          # 目录下的 Kbuild 或 Makefile
include $(srctree)/scripts/Makefile.lib
```

`kbuild-file` 由 `scripts/Kbuild.include` 解析：优先 `Kbuild`，其次 `Makefile`。

### 1.6.2 编译单个 C 文件

`scripts/Makefile.build` 第 288-290 行：

```make
$(obj)/%.o: $(obj)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)
```

`cc_o_c` 规则内部使用 `$(_c_flags)` 调用编译器，并在需要时运行 `recordmcount`（`CONFIG_FTRACE_MCOUNT_USE_RECORDMCOUNT`）、objtool、`genksyms`（`CONFIG_MODVERSIONS`），最后生成 `.cmd` 依赖文件。`if_changed_rule` 保证只在命令或依赖变化时重新编译。

### 1.6.3 目录内归档 built-in.a

`Makefile.build` 第 472-477 行：

```make
quiet_cmd_ar_builtin = AR      $@
      cmd_ar_builtin = rm -f $@; \
	$(AR) cDPrST $@ $(real-prereqs)

$(obj)/built-in.a: $(real-obj-y) FORCE
```

`cDPrST` 参数创建 **无符号表、确定性、薄归档**，这是内核增量构建与可重现构建的基础。

### 1.6.4 顶层归档与链接

顶层 `Makefile` 第 1330-1331 行：

```make
KBUILD_VMLINUX_OBJS := built-in.a $(patsubst %/, %/lib.a, $(filter %/, $(libs-y)))
KBUILD_VMLINUX_LIBS := $(filter-out %/, $(libs-y))
```

链接分三步：

1. `vmlinux_a`：`scripts/Makefile.vmlinux_a` 用 `$(AR) cDPrST` 生成 `vmlinux.a`，并通过 `scripts/head-object-list.txt` 把头部对象调整到最前（`cmd_ar_builtin_fixup`）。
2. `vmlinux_o`：`scripts/Makefile.vmlinux_o` 用 `$(LD) -r` 把 `vmlinux.a` 与 `KBUILD_VMLINUX_LIBS` 链接成可重定位的 `vmlinux.o`，同时运行 objtool、section mismatch 检查，并生成 `modules.builtin` / `modules.builtin.modinfo`。
3. `vmlinux`：`scripts/Makefile.vmlinux` 调用 `scripts/link-vmlinux.sh`，结合 `arch/$(SRCARCH)/kernel/vmlinux.lds` 完成最终链接、BTF 生成、sorttable、strip 等后处理。

### 1.6.5 模块的两阶段构建

模块构建被拆成两步：

- **阶段一**：`scripts/Makefile.build` 编译 `.o`，链接出 `<mod>.o`，生成 `<mod>.mod` 与 `modules.order`。
- **阶段二**：`scripts/Makefile.modpost` 读取 `modules.order`，运行 `scripts/mod/modpost`，为每个模块生成 `<mod>.mod.c`（写入 vermagic、`MODULE_INFO`、导出符号 CRC 等），并生成 `Module.symvers`。
- **最终链接**：`scripts/Makefile.modfinal` 用 `$(LD) -r -T scripts/module.lds` 把 `<mod>.o`、`<mod>.mod.o`、`.module-common.o` 链接成 `<mod>.ko`。

`Makefile.modpost` 开头的注释完整描述了这四个步骤。

## 1.7 include/generated 的生成物

当前 `include/generated/` 包含：`autoconf.h`（`conf_write_autoconf()` 生成的 C 宏）、`utsrelease.h`（`UTS_RELEASE`）、`compile.h`（`scripts/mkcompile_h`）、`bounds.h`、`asm-offsets.h`、`rq-offsets.h`、`timeconst.h`、`vdso-offsets.h`、`rustc_cfg`（Rust `--cfg` 列表）以及 `uapi/`。它们都是生成头文件，不进入版本控制，`make clean` 时删除。

## 1.8 交叉编译与 O= 外部构建

交叉编译关键变量：

- `ARCH`：目标架构（如 `arm64`、`x86`）
- `SRCARCH`：归一化后的架构目录名（如 `x86`）
- `CROSS_COMPILE`：工具链前缀，如 `aarch64-linux-gnu-`
- `LLVM=1`：使用 clang/llvm 工具链

顶层 `Makefile` 第 170-213 行处理 `O=`：`O=` 优先于环境变量 `KBUILD_OUTPUT`；`objtree` 指向输出目录，`srctree` 指向源码目录；输出目录不存在时先创建，无法 `realpath` 则报错；`abs_output == CURDIR` 时走 in-tree 构建。`outputmakefile` 目标会在输出目录生成转发 `Makefile` 并导出 `KBUILD_OUTPUT`。

```sh
make O=/tmp/build defconfig
make O=/tmp/build -j$(nproc)
make O=/tmp/build modules_install INSTALL_MOD_PATH=/tmp/rootfs
```

## 1.9 当前 .config 对本子系统的影响

当前 `.config` 是刻意裁剪的调试配置：

- **`CONFIG_MODULES` 未启用**（`.config` 第 788 行 `# CONFIG_MODULES is not set`）。
  - `kernel/Makefile` 第 61 行 `obj-$(CONFIG_MODULES) += module/`，因此整个 `kernel/module/` 目录不会被编译。
  - `obj-m` 在 Kbuild 中为空，`scripts/Makefile.modpost` 的 `-M` 分支不会启用，`.ko` 不会生成。
  - `scripts/Makefile.build`、`scripts/Makefile.lib`、`scripts/Makefile.modpost`、`scripts/Makefile.modfinal` 源码仍然存在，只是当前配置下不参与产物。
- **`CONFIG_RUST` 不存在**（`.config` 中检索不到该符号，只有 `CONFIG_HAVE_RUST=y` 与 `CONFIG_RUSTC_*` 等工具链探测项）。
  - `rust/` 目录源码完整存在，但由于没有 `CONFIG_RUST`，`prepare` 不会调用 `scripts/rust_is_available.sh`，`rust/Makefile` 中的 `obj-$(CONFIG_RUST)` 全部为空。
- **`CONFIG_KEXEC_FILE=y`、`CONFIG_AUDIT=y`** 与本篇构建主题无直接冲突，但会影响 `kernel/` 下的对象选择；详见后续篇章。

需要强调：**源码存在 ≠ 当前配置启用**。阅读 Kbuild 逻辑时，`obj-$(CONFIG_FOO)` 是否为空，决定了对应目录/文件是否进入 `vmlinux.a`。

## 1.10 关键文件清单

- `/home/louis/code/linux/Makefile`：顶层入口，版本、`O=`、`prepare`、`vmlinux` 目标
- `/home/louis/code/linux/scripts/Kbuild.include`：`build`、`if_changed`、`filechk` 等基础设施
- `/home/louis/code/linux/scripts/Makefile.build`：目录级编译、`built-in.a`、子目录递归
- `/home/louis/code/linux/scripts/Makefile.lib`：`obj-y`/`obj-m` 展开、flags 组合
- `/home/louis/code/linux/scripts/Makefile.modpost`：阶段二 modpost
- `/home/louis/code/linux/scripts/Makefile.modfinal`：`.ko` 最终链接
- `/home/louis/code/linux/scripts/Makefile.vmlinux`：`vmlinux` 最终链接
- `/home/louis/code/linux/scripts/Makefile.vmlinux_a`：`vmlinux.a` 归档
- `/home/louis/code/linux/scripts/Makefile.vmlinux_o`：`vmlinux.o` 重定位链接
- `/home/louis/code/linux/scripts/kconfig/`：`lexer.l`、`parser.y`、`confdata.c`、`mconf.c`、`conf.c`
- `/home/louis/code/linux/scripts/kconfig/Makefile`：各 `*config` 目标
- `/home/louis/code/linux/include/generated/`：配置与偏移生成头
- `/home/louis/code/linux/Documentation/kbuild/kconfig-language.rst`：Kconfig 语言权威说明
- `/home/louis/code/linux/Documentation/kbuild/makefiles.rst`：Kbuild 变量与规则说明
- `/home/louis/code/linux/Documentation/kbuild/kbuild.rst`：构建目标与环境变量
- `/home/louis/code/linux/Documentation/kbuild/modules.rst`：外部模块构建
