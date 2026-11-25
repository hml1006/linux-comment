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

### 1.3.3 conf 的输入模式

所有配置前端（`mconf`、`nconf`、`gconf`、`qconf`）最终都调用同一个 `conf` 程序理解配置语义。`conf` 用枚举描述输入模式（`scripts/kconfig/conf.c` 第 23-40 行）：

```c
enum input_mode {
	oldaskconfig, syncconfig, oldconfig,
	allnoconfig, allyesconfig, allmodconfig, alldefconfig,
	randconfig, defconfig, savedefconfig,
	listnewconfig, helpnewconfig, olddefconfig,
	yes2modconfig, mod2yesconfig, mod2noconfig,
};
```

`getopt_long()` 把长选项映射到 `input_mode_opt`（第 610-620 行），其中 `syncconfig` 分支会额外把 `sync_kconfig` 置 1，并安装空的 `conf_set_message_callback()` 以屏蔽“configuration written to …”这类提示（第 680-688 行）——因为在构建过程中它只是内部同步步骤。

读取已有配置的路径在第 711-733 行分成三组：

| 模式 | 基线文件 | 说明 |
| --- | --- | --- |
| `defconfig` | `optarg` 指定的 defconfig | `conf_read(defconfig_file)`，文件不存在直接报错退出 |
| `oldconfig` / `olddefconfig` / `syncconfig` / `savedefconfig` / `listnewconfig` / `helpnewconfig` | `.config` | `conf_read(NULL)` |
| `allnoconfig` / `allyesconfig` / `allmodconfig` / `alldefconfig` / `randconfig` | `KCONFIG_ALLCONFIG` 环境变量指向的片段（如果有） | 第 734 行起 |

`randconfig` 的随机种子来由 `set_randconfig_seed()`（第 87-113 行）决定：优先取 `KCONFIG_SEED` 环境变量，否则用当前时间，并把实际种子打印成 `KCONFIG_SEED=0x...`，便于复现。每个 bool/tristate 被选为 `y` 的概率由 `KCONFIG_PROBABILITY` 控制（第 189-227 行），`allyesconfig` 等目标也复用同一套“新符号填充”逻辑 `conf_set_all_new_symbols()`（第 175 行）。

`KCONFIG_ALLCONFIG` 的含义是：把某个片段当作“用户预先给定值”，其余符号仍按 `all*config` 规则填充。`merge_config.sh` 正是靠它把多个片段合并后的临时文件喂给 `conf`（见 1.3.4）。

### 1.3.4 defconfig、片段合并与 localmodconfig

`scripts/kconfig/Makefile` 才是各种 `make xxxconfig` 的真正实现。它先确定 Kconfig 入口与默认 defconfig：

```make
ifdef KBUILD_KCONFIG
Kconfig := $(KBUILD_KCONFIG)
else
Kconfig := Kconfig
endif

ifndef KBUILD_DEFCONFIG
KBUILD_DEFCONFIG := defconfig
endif
```

`KCONFIG_DEFCONFIG_LIST` 决定 `defconfig` 目标在“没有显式片段”时去哪里找基线（第 19-28 行）：

```make
export KCONFIG_DEFCONFIG_LIST :=
ifndef cross_compiling
kernel-release := $(shell uname -r)
KCONFIG_DEFCONFIG_LIST += \
	/lib/modules/$(kernel-release)/.config \
	/etc/kernel-config \
	/boot/config-$(kernel-release)
endif
KCONFIG_DEFCONFIG_LIST += arch/$(SRCARCH)/configs/$(KBUILD_DEFCONFIG)
```

也就是说：交叉编译时只看 `arch/$(SRCARCH)/configs/$(KBUILD_DEFCONFIG)`；本机构建时优先复用运行内核的配置（`/boot/config-$(uname -r)` 等），这也解释了为什么在发行版内核源码树上直接 `make defconfig` 会“像当前系统配置”。

各前端目标由 `config_rule` 宏批量生成（第 47-55 行）：

```make
config-prog	:= conf
menuconfig-prog	:= mconf
nconfig-prog	:= nconf
gconfig-prog	:= gconf
xconfig-prog	:= qconf

define config_rule
PHONY += $(1)
$(1): $(obj)/$($(1)-prog)
	$(Q)$$< $(silent) $(Kconfig)
...
```

`defconfig` 与 `%_defconfig` 的区别值得注意（第 87-100 行）：

```make
defconfig: $(obj)/conf
ifneq ($(wildcard $(srctree)/arch/$(SRCARCH)/configs/$(KBUILD_DEFCONFIG)),)
	@$(kecho) "*** Default configuration is based on '$(KBUILD_DEFCONFIG)'"
	$(Q)$< $(silent) --defconfig=arch/$(SRCARCH)/configs/$(KBUILD_DEFCONFIG) $(Kconfig)
else
	@$(kecho) "*** Default configuration is based on target '$(KBUILD_DEFCONFIG)'"
	$(Q)$(MAKE) -f $(srctree)/Makefile $(KBUILD_DEFCONFIG)
endif

%_defconfig: $(obj)/conf
	$(Q)$< $(silent) --defconfig=arch/$(SRCARCH)/configs/$@ $(Kconfig)
```

- `make defconfig`：用 `KBUILD_DEFCONFIG`（默认字面量 `defconfig`）。如果 `arch/arm64/configs/defconfig` 这种文件存在就直接读；否则把 `defconfig` 当成另一个 make 目标递归处理。
- `make xxx_defconfig`：直接读 `arch/$(SRCARCH)/configs/xxx_defconfig`，这是最常见的入口（例如 `make ARCH=arm64 defconfig` 实际走的是第一种）。
- `make savedefconfig`：以当前 `.config` 为基线，输出“最小 defconfig”（仅与默认值不同的符号）。

片段合并由 `%.config` 目标承担（第 102-108 行）：

```make
configfiles = $(wildcard $(srctree)/kernel/configs/$(1) $(srctree)/arch/$(SRCARCH)/configs/$(1))

cmd_merge_fragments = $(srctree)/scripts/kconfig/merge_config.sh -m $(KCONFIG_CONFIG) $(config-fragments)

%.config: $(obj)/conf
	$(if $(config-fragments),, $(error $@ fragment does not exists on this architecture))
	$(call cmd,merge_fragments)
	$(Q)$(MAKE) -f $(srctree)/Makefile olddefconfig
```

`merge_config.sh` 自身的关键行为（`scripts/kconfig/merge_config.sh` 第 38-55、278-294 行）：

| 选项 | 效果 |
| --- | --- |
| 默认 | `ALLTARGET=alldefconfig`，即缺失符号取 Kconfig 默认值 |
| `-n` | 改用 `allnoconfig`，缺失符号一律 `# CONFIG_FOO is not set` |
| `-m` | 只合并不执行 make（Kbuild 的 `%.config` 就是这么用的） |
| `-y` | 同一符号内置 `y` 优先于模块 `m` |
| `-s` | 严格模式：片段之间互相重定义即报错 |
| `-r` / `-Q` | 报告/抑制冗余项与被覆盖项的警告 |

脚本先把片段合并到临时文件，最后执行：

```sh
make KCONFIG_ALLCONFIG=$TMP_FILE $OUTPUT_ARG $ALLTARGET
```

`localmodconfig` / `localyesconfig` 走的是另一条路（第 57-70 行）：用 `scripts/kconfig/streamline_config.pl` 读取当前系统已加载模块列表，把未使用的 `=m` 符号关掉（`localmodconfig`）或转成 `=y`（`localyesconfig`），再用 `conf --oldconfig` 收缩。`LMC_KEEP` 环境变量可以保留指定符号，避免误裁。可以把它看作“按运行态反推配置”，在做最小化裁剪时极有用，但它依赖 `lsmod` 可见的模块列表，因此只适用于模块化的发行版内核。

`tinyconfig` 则组合了 `KCONFIG_ALLCONFIG=kernel/configs/tiny-base.config` 与 `allnoconfig`，再合并 `tiny.config`。

### 1.3.5 符号求值：dir_dep / rev_dep / implied

`select`/`imply` 不是语法糖，而是在 `scripts/kconfig/symbol.c` 里被显式建模的三种依赖方向：

| 结构字段 | 来源语句 | 求值位置 |
| --- | --- | --- |
| `dir_dep` | `depends on` | `sym_calc_visibility()`，`symbol.c` 第 238-246 行 |
| `rev_dep` | `select`（反向依赖） | 同函数第 247-253 行 |
| `implied` | `imply`（弱反向依赖） | 同函数第 254-260 行 |

`sym_calc_visibility()`（第 212 行起）先把所有 `prompt` 的可见表达式求或，得到 `sym->visible`；再在没有显式 `depends on` 时把 `dir_dep` 默认为 `yes`。`sym_calc_value()`（第 411 行起）在此之上计算最终三态值：

```c
	calc_newval:
		if (sym->dir_dep.tri < sym->rev_dep.tri)
			sym_warn_unmet_dep(sym);
		newval.tri = EXPR_OR(newval.tri, sym->rev_dep.tri);
```

这段（第 495-497 行）就是“`select` 不检查目标自身依赖”的落点：当 `rev_dep` 比 `dir_dep` 更强时，Kconfig 只打印 unmet dependency 警告，仍然把目标抬到被选中的值。`Documentation/kbuild/kconfig.select-break` 讨论的正是这种语义带来的隐蔽问题。

求值结果被 `SYMBOL_VALID` 缓存（第 420-423 行），任何影响依赖的赋值都会通过 `sym_set_changed()` 清掉缓存并递归重算。只有满足 `SYMBOL_WRITE` 的符号才会进入 `autoconf.h` / `auto.conf`（`confdata.c` 第 1158-1160 行）：

```c
	for_all_symbols(sym)
		if ((sym->flags & SYMBOL_WRITE) && sym->name)
			print_symbol(file, sym);
```

`conf_write_autoconf()` 在写文件前还会调用 `conf_touch_deps()`（第 1059-1131 行），为每个值发生变化的符号 `touch` 一个空文件 `include/config/<SYM>`。本树 `include/config/` 下有 5219 个条目，其中 `include/config/PREEMPT_RT` 是一个 0 字节文件：

```sh
$ ls -la include/config/PREEMPT_RT
-rw-r--r-- 1 louis louis 0 Jul 17 10:24 include/config/PREEMPT_RT
```

这些空文件不是残留垃圾，而是 **Make 的依赖节点**：`scripts/Makefile.lib` 生成 `c_flags` 时会把 `-include config/PREEMPT_RT` 之类的引用写进 `.cmd`，于是“配置项变化 → 对应 .o 重编”这条链就成立了（详见 1.11 的 `.cmd` 解剖）。

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

### 1.4.1 syncconfig 的触发链

`auto.conf` 不是每次构建都重写，而是作为 Make 目标按时间戳按需生成。顶层 `Makefile` 里这条链分三步：

```make
include $(objtree)/include/config/auto.conf       # 第 810 行
...
include include/config/auto.conf.cmd              # 第 883 行
...
%/config/auto.conf %/config/auto.conf.cmd %/generated/autoconf.h %/generated/rustc_cfg: $(KCONFIG_CONFIG)
	$(Q)$(kecho) "  SYNC    $@"
	$(Q)$(MAKE) -f $(srctree)/Makefile syncconfig
```

完整调用链：

```
make
 → include/config/auto.conf 过期（.config 更新）
 → 匹配多目标模式规则（Makefile:904）
 → make -f Makefile syncconfig
 → scripts/kconfig/conf --syncconfig Kconfig
 → conf_write_autoconf()
 → 写 autoconf.h、rustc_cfg、autoconf.cmd、auto.conf（最后）
```

`auto.conf.cmd` 由 `conf_write_autoconf_cmd()`（`confdata.c` 第 1018-1057 行）生成，内容是 `deps_config := Kconfig scripts/Kconfig.include init/Kconfig ...` 的完整 Kconfig 依赖列表。本树这个文件 1763 行、含 1697 处 `Kconfig` 路径引用：

```make
autoconfig := include/config/auto.conf

deps_config := \
	Kconfig \
	scripts/Kconfig.include \
	init/Kconfig \
...
```

因此只要任何被 `source` 的 Kconfig 文件被修改，`make` 就会重新执行 `syncconfig`，而不需要手动 `make oldconfig`。这也解释了“改了 Kconfig 的 help 文本却触发大量重编”的现象：`auto.conf.cmd` 更新 → `auto.conf` 更新 → 那些把 `include/config/<SYM>` 写进 `.cmd` 的目标被判定为过期。

注意第 916-924 行的另一条路径：外部模块构建（`may-sync-config` 为假）不会自动 sync，而是检查 `autoconf.h`/`rustc_cfg`/`auto.conf` 是否存在，缺失时直接报错并提示：

```
***  ERROR: Kernel configuration is invalid. The following files are missing:
***    - include/generated/autoconf.h
***  Run "make oldconfig && make prepare" on kernel source to fix it.
```

### 1.4.2 Rust 相关目标

顶层 `Makefile` 第 1970-2005 行集中定义 Rust 目标：

| 目标 | 实现 | 说明 |
| --- | --- | --- |
| `rustavailable` | `scripts/rust_is_available.sh` + 打印 “Rust is available!” | 第 1974-1976 行，不需要 `CONFIG_RUST` |
| `rustdoc` | `prepare` → `make -f scripts/Makefile.build obj=rust rustdoc` | 第 1981-1983 行 |
| `rusttest` | 同上，跑 doctest / KUnit 包装 | 第 1986-1988 行 |
| `rustfmt` / `rustfmtcheck` | `find` 全树 `*.rs`，跳过 vendored crate 与 `*generated*` | 第 1993-2005 行 |

`rustfmt` 的 `-path` 剪枝列表（`rust/proc-macro2`、`rust/quote`、`rust/syn`、`rust/zerocopy`、`rust/zerocopy-derive`）正好对应 `rust/` 下 vendored 的上游 crate——这些代码不按内核风格格式化。

### 1.4.3 Make 版本与工具链检查

顶层 `Makefile` 第 14-15 行用 `.FEATURES` 判断 `output-sync` 是否存在，以此强制 GNU Make >= 4.0：

```make
ifeq ($(filter output-sync,$(.FEATURES)),)
$(error GNU Make >= 4.0 is required. Your Make version is $(MAKE_VERSION))
endif
```

第 49 行的 `MAKEFLAGS += -rR` 关闭内置规则与内置变量，避免隐式规则意外匹配（例如把 `foo.o` 当成 `foo.c` 编译），这也是 Kbuild 所有规则都显式写出的原因。

`C=` 语法的静态检查器要先用 `scripts/checker-valid.sh` 验证可用，否则第 1280-1285 行会把 `KBUILD_CHECKSRC` 降级为 0 并告警。`KBUILD_IMAGE ?= vmlinux`（第 1295 行）决定默认构建产物，`arch/$(SRCARCH)/Makefile` 可以覆盖它（例如 `Image.gz`）。

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

### 1.5.5 变量的路径前缀化

`Makefile.build` 在处理完 `obj-y`/`obj-m` 的语义之后，还会做一次“加目录前缀”（第 112-122 行）：

```make
ifneq ($(obj),.)
extra-y		:= $(addprefix $(obj)/, $(extra-y))
always-y	:= $(addprefix $(obj)/, $(always-y))
targets		:= $(addprefix $(obj)/, $(targets))
obj-m		:= $(addprefix $(obj)/, $(obj-m))
lib-y		:= $(addprefix $(obj)/, $(lib-y))
real-obj-y	:= $(addprefix $(obj)/, $(real-obj-y))
real-obj-m	:= $(addprefix $(obj)/, $(real-obj-m))
multi-obj-m	:= $(addprefix $(obj)/, $(multi-obj-m))
subdir-ym	:= $(addprefix $(obj)/, $(subdir-ym))
endif
```

注意 `obj-y` 没有出现在这里——它在第 74-78 行已经被替换成 `foo/built-in.a` 形式（子目录情形），而当前目录的 `real-obj-y` 才需要前缀。这个顺序很重要：`real-search` 必须在前缀化之前完成，否则 `-objs`/`-y` 变量名拼不出来。

另一个容易忽略的细节是 `need-builtin` / `need-modorder` 两个开关（第 68-78 行）：父目录向下递归时通过

```make
$(Q)$(MAKE) $(build)=$@ \
need-builtin=$(if $(filter $@/built-in.a, $(subdir-builtin)),1) \
need-modorder=$(if $(filter $@/modules.order, $(subdir-modorder)),1) \
$(filter $@/%, $(single-subdir-goals))
```

告诉子目录“你到底需不需要产出 `built-in.a` / `modules.order`”。这使得 `make mm/` 这种单目录构建不会去生成整个模块顺序文件。

如果某个 Makefile 只在 `obj-m` 里写了对象、却没有 `need-modorder`（例如用 `subdir-y` 递归进入一个模块目录），`Makefile.build` 第 128-133 行会给出明确警告：

```make
ifeq ($(need-modorder),)
ifneq ($(obj-m),)
$(warning $(patsubst %.o,'%.ko',$(obj-m)) will not be built even though obj-m is specified.)
$(warning You cannot use subdir-y/m to visit a module Makefile. Use obj-y/m instead.)
endif
endif
```

### 1.5.6 modname / modfile 与“是否属于模块”

`scripts/Makefile.lib` 开头（第 5-11 行）从复合对象反推模块名：

```make
modname-multi = $(sort $(foreach m,$(multi-obj-ym),\
	$(if $(filter $*.o, $($(m:%.o=%))),$(m:%.o=%),)))

__modname = $(or $(modname-multi),$(basetarget))
modname = $(subst $(space),:,$(__modname))
modfile = $(addprefix $(obj)/,$(__modname))
```

- `modname-multi` 处理“一个 `.o` 同时被多个复合模块引用”的情况，结果是冒号分隔的多个名字；
- `modfile` 是 `$(obj)/<modname>`，供 `RUST_MODFILE` 与依赖文件使用；
- `modname_flags`（第 22-23 行）把它变成 `-DKBUILD_MODNAME=` 与 `-D__KBUILD_MODNAME=`。

`part-of-module`（第 153 行）决定一个对象是走内核 flags 还是模块 flags：

```make
part-of-module = $(if $(filter $(basename $@).o, $(real-obj-m)),y)
is-kernel-object = $(or $(part-of-builtin),$(part-of-module))
quiet_modtag = $(if $(part-of-module),[M],   )
```

于是编译日志里 `CC [M]` 的 `[M]` 就来自 `quiet_modtag`；而 `modkern_cflags`（第 156-159 行）在模块与内置之间切换 `KBUILD_CFLAGS_MODULE` / `KBUILD_CFLAGS_KERNEL`，并额外加 `modfile_flags`（`-DKBUILD_MODFILE=`）。`is-kernel-object` 还被 KASAN/KCOV/KCSAN 的“是否对内核对象插桩”判断复用。

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

### 1.6.6 单文件规则的完整展开

`$(obj)/%.o: $(obj)/%.c ... FORCE` 只调用 `if_changed_rule,cc_o_c`，真正的规则定义在 `scripts/Makefile.lib`（第 265-280 行）：

```make
quiet_cmd_cc_o_c = CC $(quiet_modtag)  $@
      cmd_cc_o_c = $(CC) $(c_flags) -c -o $@ $< \
		$(cmd_ld_single) \
		$(cmd_objtool)

define rule_cc_o_c
	$(call cmd_and_fixdep,cc_o_c)
	$(call cmd,save_c_flags)
	$(call cmd,checksrc)
	$(call cmd,checkdoc)
	$(call cmd,gen_objtooldep)
	$(call cmd,gen_symversions_c)
	$(call cmd,record_mcount)
	$(call cmd,warn_shared_object)
endef
```

逐项拆开：

| 步骤 | 条件 | 产物/副作用 |
| --- | --- | --- |
| `cmd_and_fixdep` | 总是 | 编译出 `.o`，`fixdep` 读 `.d`，写 `.<name>.o.cmd` |
| `save_c_flags` | `CONFIG_LTO_CLANG_THIN_DIST` | 把 `_c_flags` 存进 `.cmd`，供后续一致性检查 |
| `checksrc` / `checkdoc` | `C=1/2`、`W=1` | sparse / kerneldoc 检查 |
| `gen_objtooldep` | `CONFIG_OBJTOOL` | 给 `.cmd` 追加对 `objtool` 二进制的依赖 |
| `gen_symversions_c` | `CONFIG_MODVERSIONS` | 对导出符号跑 `genksyms`/`gendwarfksyms`，结果追加进 `.cmd` |
| `record_mcount` | `CONFIG_FTRACE_MCOUNT_USE_RECORDMCOUNT` | 给 `.o` 打 `__mcount_loc` |
| `warn_shared_object` | `W=1` | 复合对象多模块共用的告警 |

`c_flags` 本身由 `Makefile.lib` 第 170-173 行拼装：

```make
c_flags        = -Wp,-MMD,$(depfile) $(NOSTDINC_FLAGS) $(LINUXINCLUDE)     \
		 $(_c_flags) $(modkern_cflags)                           \
		 $(basename_flags) $(modname_flags)
```

`-Wp,-MMD,$(depfile)` 让编译器自己吐出 `.d` 依赖文件，`depfile` 由 `Kbuild.include` 第 45 行定义为 `$(dot-target).d`。`LINUXINCLUDE` 则是整套 `-I` 与 `-include`（`compiler-version.h`、`kconfig.h`、`compiler_types.h`）的组合。

### 1.6.7 `.cmd` 与 if_changed：增量构建的核心

`scripts/Kbuild.include` 第 36 行定义：

```make
dot-target = $(dir $@).$(notdir $@)
```

因此目标 `mm/page_alloc.o` 的 `dot-target` 是 `mm/.page_alloc.o`，命令文件就是 `mm/.page_alloc.o.cmd`、依赖临时文件是 `mm/.page_alloc.o.d`。`cmd_and_savecmd`（第 199-201 行）保存完整命令行：

```make
cmd_and_savecmd =                                                            \
	$(cmd);                                                              \
	printf '%s\n' 'savedcmd_$@ := $(make-cmd)' > $(dot-target).cmd
```

`if_changed` 的判定逻辑（第 169-197 行）由三部分相或：

```make
cmd-check = $(filter-out $(subst $(space),$(space_escape),$(strip $(savedcmd_$@))), \
                         $(subst $(space),$(space_escape),$(strip $(cmd_$1))))
newer-prereqs = $(filter-out $(PHONY),$?)
check-FORCE = $(if $(filter FORCE, $^),,$(warning FORCE prerequisite is missing))
if-changed-cond = $(newer-prereqs)$(cmd-check)$(check-FORCE)
if_changed = $(if $(if-changed-cond),$(cmd_and_savecmd),@:)
```

1. `newer-prereqs`：有新依赖（`$?`）→ 重编；
2. `cmd-check`：保存的命令行与当前展开不一致（例如改了 `CFLAGS_foo.o`）→ 重编；
3. `check-FORCE`：规则里忘了写 `FORCE` 时给出 `FORCE prerequisite is missing` 警告。

`KBUILD_NOCMDDEP=1` 可以关掉第 2 条，只按时间戳判断——调试“命令没变却重编”时有用，但会失去 flags 变化触发的重编能力。

### 1.6.8 顶层三步链接的实际入口

第 1.6.4 节描述的 `vmlinux_a`/`vmlinux_o`/`vmlinux` 在顶层 `Makefile` 里是三个显式 PHONY 目标（第 1342-1370 行）：

```make
PHONY += vmlinux_a
vmlinux_a: $(KBUILD_VMLINUX_OBJS) scripts/head-object-list.txt FORCE
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.vmlinux_a

vmlinux.o modules.builtin.modinfo modules.builtin: vmlinux_o
	@:

vmlinux: vmlinux.o $(KBUILD_LDS) modpost
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.vmlinux
```

- `vmlinux_a` 调用 `Makefile.vmlinux_a`，后者先 `$(AR) cDPrST built-in-fixup.a $(KBUILD_VMLINUX_OBJS)`，再用 `$(AR) mPi` 把 `scripts/head-object-list.txt` 列出的入口对象（如 `arch/arm64/kernel/head.o`）移到归档最前面。这相当于旧 `head-y` 机制的替代品，只在链接脚本无法表达入口顺序的架构上保留。最终 `vmlinux.a` 在非 ThinLTO 配置下就是 `built-in-fixup.a` 的拷贝（`cmd_copy`）；ThinLTO 时则按 `vmlinux.thinlto-index` 把对象逐个替换成 `*.thinlto-native.o` 后重新归档。
- `vmlinux_o` 调用 `Makefile.vmlinux_o`：`$(LD) -r` 生成 `vmlinux.o`，同时跑 objtool（`delay-objtool` 场景）、section mismatch 检查，并产出 `modules.builtin`、`modules.builtin.modinfo`。若开了 `CONFIG_LTO_CLANG`，还会用 `scripts/generate_initcall_order.pl` 生成 `.tmp_initcalls.lds` 以固定 initcall 顺序。
- `vmlinux` 调用 `Makefile.vmlinux` → `scripts/link-vmlinux.sh`。脚本里的关键阶段（`link-vmlinux.sh` 第 110-215 行）：

```
vmlinux.o
 → scripts/tracepoint-update vmlinux.o
 → kallsyms（scripts/kallsyms 生成 .S，再链接）
 → BTF（CONFIG_DEBUG_INFO_BTF 时由 pahole 生成 .BTF）
 → sorttable（scripts/sorttable，按符号名排序 extable）
 → 最终 vmlinux / System.map
```

`KBUILD_LDS` 被 export 为 `arch/$(SRCARCH)/kernel/vmlinux.lds`（第 1334 行），这正是把 `.head.text`、`.text`、`.init.text` 等段按架构约定排布的唯一依据。

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

### 1.8.1 本仓库 build.sh 的实际流程

`/home/louis/code/linux/build.sh` 把“改配置 → 交互确认 → 全内置 → 编译 → 生成编译数据库”串成一条流水线，逐行看：

```sh
#cp arch/arm64/configs/defconfig .config

sed -i 's/^# CONFIG_GDB_SCRIPTS is not set/CONFIG_GDB_SCRIPTS=y/' .config
sed -i 's/^# CONFIG_DYNAMIC_DEBUG is not set/CONFIG_DYNAMIC_DEBUG=y/' .config
sed -i 's/^# CONFIG_DYNAMIC_DEBUG_CORE is not set/CONFIG_DYNAMIC_DEBUG_CORE=y/' .config
sed -i 's/^CONFIG_DEBUG_INFO_REDUCED=y/CONFIG_DEBUG_INFO_REDUCED=n/' .config

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- menuconfig

sed -i 's/=m/=y/g' .config

make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j12
./scripts/clang-tools/gen_compile_commands.py
```

几个值得注意的工程细节：

1. 第一行 `cp ... defconfig .config` 被注释掉——脚本假定 `.config` 已经存在，只做增量修改。这也解释了当前 `.config` 为什么仍保留 `arm64` 与大量 `HAVE_*` 探测项。
2. 前 4 条 `sed` 在 **`menuconfig` 之前**执行，属于“先改配置再交互”；后 3 条 `if` 检查（第 8-22 行）只是兜底：若 `sed` 因配置里是别的写法（例如 `CONFIG_GDB_SCRIPTS` 根本不存在）而没命中，就用 `>>` 追加。
3. `sed -i 's/=m/=y/g' .config` 在 `menuconfig` **之后**执行，把所有模块强制转成内置。这直接导致后续构建中 `obj-m` 全部为空、`scripts/Makefile.modpost` 的模块分支不起作用——与当前 `.config` 的“全内置调试内核”策略一致。
4. 编译命令显式给 `ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-` 与 `-j12`；没有 `O=`，所以是 in-tree 构建，`.o`/`.cmd` 直接散落在源码目录（本树 `mm/.page_alloc.o.cmd` 就是这次构建的产物）。
5. 最后一步调用 `gen_compile_commands.py`（见 1.12），从 `.cmd` 反推出 `compile_commands.json`，供 clangd/IDE 使用。

注意：`sed` 直接改 `.config` 不会自动更新 `include/config/auto.conf`；是下一次 `make` 触发 1.4.1 描述的 `syncconfig` 链才真正生效。

### 1.8.2 compile-only.sh 与增量编译

`/home/louis/code/linux/compile-only.sh` 只有两行有效内容：

```sh
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j12
# ./scripts/clang-tools/gen_compile_commands.py
```

它假设 `.config` 已经由 `build.sh` 准备好，只做纯编译。这种拆分适合“改代码 → 反复编译”的迭代；由于 Kbuild 的 `.cmd` 机制（1.6.7），未改动的目录几乎不产生编译命令，只有 `$(obj)/` 目录规则和 `built-in.a` 的时间戳检查在跑。

若观察到“只改了一个头文件却重编大量文件”，可以按下面的顺序排查：

- `include/generated/autoconf.h` 或 `include/config/auto.conf` 时间戳被 `syncconfig` 更新（配置真的变了）；
- 某个 `*.cmd` 里的 `deps_*` 列出了被修改的头文件（正常依赖传播）；
- 修改了 `include/linux/compiler_types.h` 这类被 `-include` 强制包含的头（几乎所有文件都依赖）。

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

## 1.11 `.cmd` 文件解剖：以 `mm/.page_alloc.o.cmd` 为例

本树是 in-tree 构建，因此每个编译单元旁边都留着一个隐藏的 `.cmd` 文件。`mm/.page_alloc.o.cmd` 共 1484 行、63 KB，是理解 Kbuild 增量构建最直接的样本。它的逻辑结构只有四段：

```make
savedcmd_mm/page_alloc.o := ccache aarch64-linux-gnu-gcc -Wp,-MMD,mm/.page_alloc.o.d -nostdinc ...

source_mm/page_alloc.o := mm/page_alloc.c

deps_mm/page_alloc.o := \
    $(wildcard include/config/PREEMPT_RT) \
    $(wildcard include/config/SMP) \
    ...
  include/linux/page-isolation.h \
  include/linux/scatterlist.h \
    $(wildcard include/config/NEED_SG_DMA_LENGTH) \
...

mm/page_alloc.o: $(deps_mm/page_alloc.o)

$(deps_mm/page_alloc.o):
```

| 段落 | Make 变量 | 作用 |
| --- | --- | --- |
| 第 1 行 | `savedcmd_<target>` | 完整命令行，用于 `cmd-check` 比较 |
| 第 3 行 | `source_<target>` | 源文件路径，供 `gen_compile_commands.py` 与调试使用 |
| 第 5 行起 | `deps_<target>` | 头文件与 **配置符号** 依赖列表 |
| 末尾 | `target: deps` + 空规则 | 把依赖挂到目标上，并声明 `deps_*` 为空规则避免报错 |

两个细节特别值得记住：

1. **配置项也是依赖**：`$(wildcard include/config/PREEMPT_RT)` 指向 1.3.5 里 `conf_touch_deps()` 创建的空文件。若 `CONFIG_PREEMPT_RT` 被关闭，`include/config/PREEMPT_RT` 被删除，`$(wildcard ...)` 展开为空，依赖集合发生变化（更直接地说，`.cmd` 与 `auto.conf` 都更新），于是该文件重编。
2. **`$(wildcard ...)` 而非直接路径**：符号被删除时 Make 不会因为“依赖不存在”而报错，只会静默视为无此依赖。

`fixdep` 的工作就是把编译器 `-MMD` 生成的 `.d` 里出现的 `include/config/...` 与 `include/generated/...` 转写进 `.cmd` 的 `deps_*`，并把 `#` 转义成 `$(pound)`。这也是 `gen_compile_commands.py` 需要先做 `prefix.replace('$(pound)', '#')` 的原因（见 1.12）。

若开启 `CONFIG_LTO_CLANG_THIN_DIST`，`.cmd` 里还会多出一行 `saved_c_flags_mm/page_alloc.o := ...`；开启 `CONFIG_OBJTOOL` 时则会追加一行 `mm/page_alloc.o: $(wildcard tools/objtool/objtool)`，保证 objtool 升级后对象被重新校验。两者分别见 `Makefile.lib` 第 259-263、219 行。

## 1.12 gen_compile_commands.py：从 `.cmd` 到 `compile_commands.json`

`scripts/clang-tools/gen_compile_commands.py` 是一个“反向工程”工具：它不重新推导编译参数，而是直接复用 Kbuild 已经记录好的 `.cmd`。核心实现只有三处。

第一处是文件与行的匹配模式（第 21-26 行）：

```python
_FILENAME_PATTERN = r'^\..*\.cmd$'
_LINE_PATTERN = r'^(saved)?cmd_[^ ]*\.o := (?P<command_prefix>.* )(?P<file_path>[^ ]*\.[cS]) *(;|$)'
_EXCLUDE_DIRS = ['.git', 'Documentation', 'include', 'tools']
```

`_LINE_PATTERN` 精确对应 1.11 里 `.cmd` 的第一行：`savedcmd_mm/page_alloc.o := <prefix> <file>`。`include/` 被排除是因为那里只有生成头，不产生独立编译单元；`tools/` 用的是另一套构建系统，`.cmd` 格式不同。

第二处是输入路径的类型分派（`main()`，第 197-211 行）：

```python
        if os.path.isdir(path):
            cmdfiles = cmdfiles_in_dir(path)
        elif os.path.basename(path) == 'libgcc.a':
            cmdfiles = []
        elif path.endswith('.a'):
            cmdfiles = cmdfiles_for_a(path, ar)
        elif path.endswith('modules.order'):
            cmdfiles = cmdfiles_for_modorder(path)
        else:
            sys.exit('{}: unknown file type'.format(path))
```

- 目录：递归找到所有 `.*.cmd`；
- `.a`：用 `llvm-ar -t`（`-a/--ar` 可改）列出成员，再映射到 `.<obj>.cmd`；
- `modules.order`：先读 `modules.order`，再读每个 `<mod>.mod`（复合模块的成员清单），逐个映射到 `.cmd`；
- `libgcc.a`：编译器运行时，直接跳过。

第三处是条目构造（`process_line()`，第 152-183 行）：

```python
    prefix = command_prefix.replace('$(pound)', '#')
    abs_path = os.path.realpath(os.path.join(root_directory, file_path))
    if not os.path.exists(abs_path):
        raise ValueError('File %s not found' % abs_path)
    return {
        'directory': root_directory,
        'file': abs_path,
        'command': prefix + file_path,
    }
```

最终输出按 `file` 排序（第 226 行），得到稳定、可 diff 的 `compile_commands.json`。

实际使用：

```sh
# in-tree 构建：直接扫当前目录
./scripts/clang-tools/gen_compile_commands.py

# out-of-tree 构建：指定输出目录
./scripts/clang-tools/gen_compile_commands.py -d /tmp/build -o /tmp/build/compile_commands.json
```

注意 `build.sh` 把它放在最后一步，因为必须先有完整的 `.cmd` 集合；若只编译了部分目录，数据库里也就只有那部分文件。

## 1.13 构建产物与自检

一次完整构建后，本树中可观察到的关键产物及其“生产者”如下：

| 产物 | 生产者 | 说明 |
| --- | --- | --- |
| `<dir>/built-in.a` | `Makefile.build` 的 `cmd_ar_builtin` | 薄归档、无符号表 |
| `<dir>/lib.a` | `Makefile.build` 的 `cmd_ar` | 带符号表，供 `--start-group` |
| `vmlinux.a` | `Makefile.vmlinux_a` | `built-in-fixup.a` 的拷贝/ThinLTO 版本 |
| `vmlinux.o` | `Makefile.vmlinux_o` | 可重定位全量链接结果 |
| `vmlinux` | `Makefile.vmlinux` + `link-vmlinux.sh` | 最终镜像 |
| `System.map` | `scripts/mksysmap`（由 `link-vmlinux.sh` 调用） | 符号表 |
| `modules.builtin` | `modpost` 处理 `vmlinux.o` 时的副产物 | 内置“模块”清单 |
| `modules.builtin.modinfo` | 同上 | 内置模块的 `.modinfo` 汇总 |
| `include/generated/compile.h` | `scripts/mkcompile_h` + `filechk` | `UTS_MACHINE`、编译者/主机/编译器 |
| `include/generated/utsversion.h` | `init/Makefile` 的 `filechk_uts_version` | 单行 `UTS_VERSION`，最长 64 字符 |
| `include/config/<SYM>` | `conf_touch_deps()` | 0 字节依赖节点 |

`include/generated/utsversion.h` 是个容易被误会来源的文件：它不是 `mkcompile_h` 的产物，而是 `init/Makefile` 第 56-62 行用 `filechk` 生成的：

```make
include/generated/utsversion.h: build-version-auto = $(shell $(srctree)/scripts/build-version)
include/generated/utsversion.h: build-timestamp-auto = $(shell LC_ALL=C date)
include/generated/utsversion.h: FORCE
	$(call filechk,uts_version)

$(obj)/version-timestamp.o: include/generated/utsversion.h
CFLAGS_version-timestamp.o := -include include/generated/utsversion.h
```

其中 `preempt-flag-$(CONFIG_PREEMPT_RT) := PREEMPT_RT`（`init/Makefile` 第 30 行）直接决定了字符串里是否出现 `PREEMPT_RT`。本树当前内容正是这一配置的体现：

```c
#define UTS_VERSION "#112 SMP PREEMPT_RT Mon Aug 17 14:46:59 CST 2026"
```

而 `include/generated/compile.h` 才是 `scripts/mkcompile_h` 的输出：

```c
#define UTS_MACHINE		"aarch64"
#define LINUX_COMPILE_BY	"louis"
#define LINUX_COMPILE_HOST	"mengliang-linux"
#define LINUX_COMPILER		"aarch64-linux-gnu-gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0, GNU ld (GNU Binutils for Ubuntu) 2.46"
```

当前 `include/generated/` 的实际内容（`ls` 结果）为：`asm-offsets.h`、`autoconf.h`、`bounds.h`、`compile.h`、`rq-offsets.h`、`rustc_cfg`、`timeconst.h`、`uapi/`、`utsrelease.h`、`utsversion.h`、`vdso-offsets.h`。规模上 `autoconf.h` 与 `include/config/auto.conf` 各 1191 行，`rustc_cfg` 2297 行（每行一个 `--cfg=CONFIG_FOO="y"`）。行数差异说明：`autoconf.h` 只写 `SYMBOL_WRITE` 的符号，`.config` 里大量 `# CONFIG_X is not set` 不会变成宏。

快速自检命令：

```sh
# 查看某个对象的完整编译命令
grep -m1 '^savedcmd_' mm/.page_alloc.o.cmd

# 查看某个配置项影响了哪些文件（反向查找）
grep -rl 'include/config/PREEMPT_RT' --include='*.cmd' . | head

# 查看为什么这个目标被重编（V=2）
make V=2 mm/page_alloc.o
```

`make V=2` 会由 `scripts/Kbuild.include` 的 `why` 机制打印原因，例如：

```
  CC      mm/page_alloc.o
# CC      mm/page_alloc.o - due to command line change
```

`why` 的实现见 `scripts/Kbuild.include` 第 232-257 行，可能的原因包括：`target is PHONY`、`target missing`、`due to: <新依赖列表>`、`command line change`、`missing .cmd file`、`not in $(targets)`。最后一项通常意味着 Makefile 里漏了 `targets +=`。

## 1.14 常见陷阱与排查

| 现象 | 根因 | 处理 |
| --- | --- | --- |
| `*** Configuration file ".config" not found!` | 顶层 `$(KCONFIG_CONFIG)` 规则（`Makefile` 第 885-892 行） | 先 `make defconfig` 或 `make menuconfig` |
| `ERROR: Kernel configuration is invalid` | 外部模块构建时缺少 `autoconf.h`/`auto.conf`（第 916-924 行） | `make oldconfig && make prepare` |
| `'foo.o' will not be built even though obj-m is specified` | 用 `subdir-y` 进入模块目录（`Makefile.build` 第 128-133 行） | 改用 `obj-m += foo/` |
| `FORCE prerequisite is missing` | 自定义规则漏写 `FORCE` | 补 `FORCE`，否则 `if_changed` 失效 |
| 改了 `CFLAGS_foo.o` 却不重编 | 手工删了 `.foo.o.cmd` 或设了 `KBUILD_NOCMDDEP=1` | 删除对应 `.cmd` 或去掉该变量 |
| `Argument list too long` | 单目录对象过多，`ar` 命令行超限 | Kbuild 已用 `printf ... | xargs` 规避（`cmd_ar_builtin`、`cmd_mod`），自定义规则需照做 |
| 切换 `.config` 后大量无关重编 | `auto.conf` 更新引发 `include/config/*` 依赖变化 | 属预期行为；彻底重建用 `make mrproper` |

关于 `mrproper`：它会删掉 `.config`、`include/config/`、`include/generated/` 与所有 `*.cmd`。在调试 Kbuild 问题时，先备份 `.config`，再 `make mrproper` 是排除“陈旧 `.cmd`”最彻底的手段。

## 1.15 与模块加载、Rust 的衔接

本章只讲“怎么把代码编译出来”，后续章节讲“编译产物怎么被内核接纳”：

- `.ko` 的格式、符号解析、重定位与 `struct module` 的建立，见 [02-module-loader.md](02-module-loader.md)；它依赖本章 1.6.5 的两阶段模块构建与 `Module.symvers`。
- `CONFIG_MODULES` 未启用时，`scripts/Makefile.modpost` 的 `-M` 分支、`scripts/Makefile.modfinal` 的 `.ko` 链接都不会执行；源码仍在，只是不产生入口。
- Rust 代码走的是同一套 `Makefile.build` 规则（`rustc_o_rs`），其 `RUST_MODFILE` 环境变量正是本章 1.5.6 的 `modfile`；详见 [../12-rust/01-rust-for-linux.md](../12-rust/01-rust-for-linux.md)。
- Livepatch 补丁本身也是模块，因此同样受 `CONFIG_MODULES` 门控；见 [03-livepatch-kexec.md](03-livepatch-kexec.md)。

## 1.16 关键文件清单（补充）

- `/home/louis/code/linux/build.sh`：本仓库实际构建脚本（改配置 → menuconfig → 全内置 → 编译 → 生成编译数据库）
- `/home/louis/code/linux/compile-only.sh`：仅编译的快捷脚本
- `/home/louis/code/linux/mm/.page_alloc.o.cmd`：`.cmd` 实例（1484 行）
- `/home/louis/code/linux/include/config/auto.conf.cmd`：Kconfig 依赖清单（1763 行）
- `/home/louis/code/linux/include/generated/rustc_cfg`：Rust `--cfg` 列表（2297 行）
- `/home/louis/code/linux/scripts/clang-tools/gen_compile_commands.py`：编译数据库生成器
- `/home/louis/code/linux/scripts/kconfig/conf.c`：输入模式与 `conf` 前端
- `/home/louis/code/linux/scripts/kconfig/confdata.c`：`.config` 读写、`conf_touch_deps`、`conf_write_autoconf`
- `/home/louis/code/linux/scripts/kconfig/symbol.c`：`dir_dep`/`rev_dep`/`implied` 与三态求值
- `/home/louis/code/linux/scripts/kconfig/menu.c`：`menu_finalize` 与菜单树
- `/home/louis/code/linux/scripts/kconfig/preprocess.c`：Kconfig 变量展开预处理器
- `/home/louis/code/linux/scripts/kconfig/merge_config.sh`：配置片段合并
- `/home/louis/code/linux/scripts/kconfig/streamline_config.pl`：`localmodconfig` 裁剪
- `/home/louis/code/linux/scripts/head-object-list.txt`：历史 `head-y` 的替代清单
- `/home/louis/code/linux/scripts/link-vmlinux.sh`：kallsyms/BTF/sorttable 后处理
