# 1. 审计子系统（Audit）

## 1.1 概述

Linux Audit 是内核中的安全事件记录与规则过滤框架：内核把 syscall、文件访问、配置变更、LSM 决策等事件格式化为 netlink 消息交给 `auditd`，`auditctl` 通过 netlink 下发规则和开关。

当前 `.config` 为 `CONFIG_AUDIT=y`、`CONFIG_HAVE_ARCH_AUDITSYSCALL=y`、`CONFIG_AUDITSYSCALL=y`、`CONFIG_AUDIT_GENERIC=y`、`CONFIG_AUDIT_ARCH_COMPAT_GENERIC=y`。

`kernel/Makefile:97-98`：

```make
obj-$(CONFIG_AUDIT) += audit.o auditfilter.o
obj-$(CONFIG_AUDITSYSCALL) += auditsc.o audit_watch.o audit_fsnotify.o audit_tree.o
```

因此当前内核包含核心状态/netlink（`kernel/audit.c`）、规则引擎（`kernel/auditfilter.c`）、每任务上下文与 syscall 审计（`kernel/auditsc.c`），以及 watch/tree 支持。

## 1.2 用户态接口与开关

### 1.2.1 audit 状态

`include/linux/audit.h:160` 定义 `AUDIT_OFF=0`、`AUDIT_ON=1`、`AUDIT_LOCKED=2`，`kernel/audit.c:76` 初始化为 `u32 audit_enabled = AUDIT_OFF;`。内核命令行 `audit=0|1|2` 由 `audit_enable()`（`audit.c:1779`）解析；设为 2 进入 `AUDIT_LOCKED`，用户态不能再关闭。

### 1.2.2 工具与 netlink

| 工具 | 作用 |
| --- | --- |
| `auditd` | 守护进程，注册为日志接收者 |
| `auditctl` | 下发规则、启停审计、设置 backlog/rate limit |
| `ausearch` / `aureport` | 离线检索与报表（纯用户态） |

`audit.c:1704` 创建 `NETLINK_AUDIT` 内核套接字：

```c
aunet->sk = netlink_kernel_create(net, NETLINK_AUDIT, &cfg);
```

控制消息由 `audit_receive()`（`audit.c:1601`）在 `audit_ctl_lock()` 下逐条交给 `audit_receive_msg()`；记录由 `audit_send_reply()`（第 1031 行）或 `kauditd_thread()`（第 883 行）回送。`auditd_conn` 保存 auditd 的 pid/portid，`auditd_test_task()` 判断当前任务是否是 auditd；另有 multicast 组支持多个监听者。

## 1.3 关键数据结构

### 1.3.1 audit_context（每任务）

`kernel/audit.h:109`。核心字段：

```c
struct audit_context {
	int dummy;                  /* 必须是第一个成员 */
	enum { AUDIT_CTX_UNUSED, AUDIT_CTX_SYSCALL, AUDIT_CTX_URING } context;
	enum audit_state state, current_state;
	struct audit_stamp stamp;   /* (时间, serial) 事件标识 */
	int major, uring_op;
	unsigned long argv[4];
	long return_code;
	u64 prio;
	int return_valid;

	struct audit_names preallocated_names[AUDIT_NAMES];
	int name_count;
	struct list_head names_list;
	char *filterkey;            /* 触发规则携带的 key */
	struct path pwd;
	struct audit_aux_data *aux, *aux_pids;
	struct sockaddr_storage *sockaddr;
	size_t sockaddr_len;

	pid_t ppid;                 /* 任务身份快照 */
	kuid_t uid, euid, suid, fsuid;
	kgid_t gid, egid, sgid, fsgid;
	unsigned long personality;
	int arch;

	pid_t target_pid;           /* ptrace 目标 */
	kuid_t target_auid, target_uid;
	unsigned int target_sessionid;
	struct lsm_prop target_ref;
	char target_comm[TASK_COMM_LEN];

	struct audit_tree_refs *trees, *first_trees;
	struct list_head killed_trees;
	int tree_count;

	int type;
	union {
		struct { int nargs; long args[6]; } socketcall;
		struct { ... } ipc;
		struct { int fd; int flags; } mmap;
		struct open_how openat2;
		struct { int argc; } execve;
		struct { const char *name; } module;
		/* ... */
	};
};
```

`preallocated_names[AUDIT_NAMES]` 内嵌在结构中，避免每次 syscall 分配 `audit_names`。

### 1.3.2 enum audit_state

`kernel/audit.h:28`：

```c
enum audit_state {
	AUDIT_STATE_DISABLED,  /* 不建上下文 */
	AUDIT_STATE_BUILD,     /* 建上下文并填充，但未必要写记录 */
	AUDIT_STATE_RECORD,    /* 退出时必写记录 */
};
```

规则 action `AUDIT_NEVER`/`AUDIT_POSSIBLE`/`AUDIT_ALWAYS`（`include/uapi/linux/audit.h:188`）分别对应三态。

### 1.3.3 audit_krule 与 audit_field

`include/linux/audit.h:51`：

```c
struct audit_krule {
	u32 pflags, flags, listnr, action;
	u32 mask[AUDIT_BITMASK_SIZE];   /* 64 * 32 位 syscall 位图 */
	u32 buflen, field_count;
	char *filterkey;
	struct audit_field *fields;
	struct audit_field *arch_f, *inode_f;
	struct audit_watch *watch;
	struct audit_tree *tree;
	struct audit_fsnotify_mark *exe;
	struct list_head rlist, list;
	u64 prio;
};

struct audit_field {
	u32 type;
	union {
		u32 val;
		kuid_t uid;
		kgid_t gid;
		struct { char *lsm_str; void *lsm_rule; };
	};
	u32 op;
};
```

### 1.3.4 audit_entry 与规则表

```c
struct audit_entry {            /* kernel/audit.h:50 */
	struct list_head list;
	struct rcu_head rcu;
	struct audit_krule rule;
};
```

`kernel/auditfilter.c:39` 定义 8 条过滤链 `audit_filter_list[AUDIT_NR_FILTERS]` 与 `audit_rules_list[8]`；`AUDIT_NR_FILTERS == 8`，文件中有 `#if` 一致性检查。

### 1.3.5 audit_buffer

```c
struct audit_buffer {
	struct sk_buff *skb;
	struct sk_buff_head skb_list;   /* 多 skb 组成一个事件 */
	struct audit_context *ctx;
	struct audit_stamp stamp;
	gfp_t gfp_mask;
};
```

## 1.4 规则、过滤链与匹配

### 1.4.1 过滤器类型

`include/uapi/linux/audit.h:173`：

```c
#define AUDIT_FILTER_USER	0x00  /* 用户态消息 */
#define AUDIT_FILTER_TASK	0x01  /* 任务创建时 */
#define AUDIT_FILTER_ENTRY	0x02  /* syscall 入口 */
#define AUDIT_FILTER_WATCH	0x03  /* 文件监视 */
#define AUDIT_FILTER_EXIT	0x04  /* syscall 退出 */
#define AUDIT_FILTER_EXCLUDE	0x05  /* 记录创建前排除 */
#define AUDIT_FILTER_FS		0x06  /* __audit_inode_child */
#define AUDIT_FILTER_PREPEND	0x10  /* 插到链首 */
```

### 1.4.2 规则下发

`auditctl` 构造 `struct audit_rule_data`（`include/uapi/linux/audit.h:518`）：

```c
struct audit_rule_data {
	__u32 flags;        /* AUDIT_PER_TASK / AUDIT_PER_CALL / AUDIT_PREPEND */
	__u32 action;       /* AUDIT_NEVER / POSSIBLE / ALWAYS */
	__u32 field_count;
	__u32 mask[AUDIT_BITMASK_SIZE];
	__u32 fields[AUDIT_MAX_FIELDS];
	__u32 values[AUDIT_MAX_FIELDS];
	__u32 fieldflags[AUDIT_MAX_FIELDS];
	__u32 buflen;
	char buf[];
};
```

`AUDIT_MAX_FIELDS = 64`、`AUDIT_BITMASK_SIZE = 64`。内核路径：`audit_receive_msg()` → `audit_rule_command()`（`auditfilter.c:1141`）→ `audit_data_to_entry()`（第 448 行）→ `audit_add_rule()`/`audit_del_rule()`，在 `audit_filter_mutex` 保护下修改链表并用 RCU 发布。字符串与 LSM 字段通过 `security_audit_rule_init()` 编译。

### 1.4.3 匹配过程

`audit_filter_rules()`（`auditsc.c:464`）遍历每个 `audit_field`，支持：

- 身份：`AUDIT_PID`/`PPID`、`AUDIT_UID`/`EUID`/`SUID`/`FSUID`、`AUDIT_GID` 系列；`AUDIT_ARCH`（`syscall_get_arch()`）
- `AUDIT_PERM`（读写/属性/执行，`audit_match_perm()`）、`AUDIT_FILETYPE`、`AUDIT_EXE`（`audit_exe_compare()`）
- `AUDIT_SUBJ_*`/`AUDIT_OBJ_*`：LSM 标签（`security_audit_rule_match()`）；`AUDIT_WATCH`/`AUDIT_DIR`/`AUDIT_TREE`：inode/目录/树监视

字段全部匹配（AND）后按 `rule->action` 返回状态。syscall 号匹配用位图：`audit_match_class()`（`auditfilter.c:189`）与 `audit_match_class_bits()`，按 `AUDIT_CLASS_*`（READ/WRITE/CHATTR/SIGNAL 及 32 位兼容类）分类后查 `rule->mask`。

### 1.4.4 退出与 inode 过滤

`audit_filter_syscall()`（`auditsc.c:869`）在 auditd 任务之外，持 RCU 读锁调用 `__audit_filter_op(..., &audit_filter_list[AUDIT_FILTER_EXIT], NULL, ctx->major)`。文件相关规则由 `audit_filter_inodes()` 按 inode 哈希桶遍历，只检查本次 syscall 收集到的 `audit_names` 对应子链。

## 1.5 系统调用审计路径

### 1.5.1 上下文分配

`audit_alloc()`（`auditsc.c:1057`）在任务创建时按 `AUDIT_FILTER_TASK` 链计算状态；`AUDIT_STATE_DISABLED` 不分配，否则 `audit_alloc_context()` 创建 `struct audit_context` 并挂到 `task->audit_context`。

### 1.5.2 入口

架构调用 `audit_syscall_entry()`，最终到 `__audit_syscall_entry()`（`auditsc.c:1986`）：

```c
if (!audit_enabled || !context) return;
state = context->state;
if (state == AUDIT_STATE_DISABLED) return;
context->dummy = !audit_n_rules;
if (!context->dummy && state == AUDIT_STATE_BUILD) { context->prio = 0; if (auditd_test_task(current)) return; }
context->arch = syscall_get_arch(current);
context->major = major; context->argv[0..3] = a1..a4;
context->context = AUDIT_CTX_SYSCALL; context->current_state = state;
```

例如 arm64 在 `arch/arm64/kernel/ptrace.c:2429` 调用 `audit_syscall_entry(regs->syscallno, ...)`。`dummy` 表示没有规则需要详细参数，只记录最基本信息系统。

### 1.5.3 退出

`__audit_syscall_exit()`（`auditsc.c:2035`）保存返回值与成功标志，调用 `audit_filter_syscall()` 重新评估退出规则；若状态为 `AUDIT_STATE_RECORD`，`audit_log_exit()` 生成多记录事件：

- `AUDIT_SYSCALL`（1300）：arch、syscall、success、exit、a0-a3、items、ppid/pid、auid/uid/gid 等身份、tty、ses、comm、exe、subj、key
- `AUDIT_PATH`（1302）：每个 `audit_names` 的路径、inode、dev、mode、ouid/ogid、obj；另有 `AUDIT_CWD`（1307）、`AUDIT_EXECVE`（1309）、按需 `AUDIT_IPC`/`AUDIT_SOCKADDR`/`AUDIT_FD_PAIR`/`AUDIT_OBJ_PID`，以及结束标记 `AUDIT_EOE`（1320）

随后重置 `context`，保留结构供下次 syscall 复用。

### 1.5.4 子系统钩子

`audit_inode()`/`__audit_inode_child()`（路径与 inode）、`audit_seccomp()`（`AUDIT_SECCOMP` 1326）、`__audit_ptrace()`（`AUDIT_OBJ_PID` 1318）、`__audit_log_bprm_fcaps()`（`AUDIT_BPRM_FCAPS` 1321）、`audit_log_kern_module()`（`AUDIT_KERN_MODULE` 1330）、`audit_uring_entry()`/`audit_uring_exit()`（`AUDIT_CTX_URING`、`AUDIT_URINGOP` 1336）。

## 1.6 audit_log：记录的生成与排队

### 1.6.1 audit_log_start

`kernel/audit.c:1913`：未初始化或未通过 `audit_filter(type, AUDIT_FILTER_EXCLUDE)` 时返回 NULL。随后处理 backlog：队列长度超过 `audit_backlog_limit` 时，可睡眠的调用者等待 kauditd 腾出空间；否则 `audit_log_lost("backlog limit exceeded")` 并返回 NULL。注释说明 auditd 自身和持有 `audit_cmd_mutex` 的配置者永不阻塞，否则队列无法排空。

### 1.6.2 audit_buffer_alloc

`audit.c:1835`：从 `audit_buffer_cache` 取 buffer，`nlmsg_new(AUDIT_BUFSIZ, gfp_mask)`（`AUDIT_BUFSIZ = 1024`）分配 skb，`nlmsg_put(skb, 0, 0, type, 0, 0)` 写入类型。`audit_get_stamp()` 生成 `(ctime, serial)`，同一 syscall 内所有记录共享该二元组。

### 1.6.3 格式化

`audit_log_format()`/`audit_log_vformat()` 类似 printk 但写入 skb；`audit_log_n_hex()` 十六进制；`audit_log_n_string()`/`audit_log_untrustedstring()` 对不可信字符串转义；`audit_log_d_path()` 输出 `struct path`。单 skb 写满时自动追加到 `ab->skb_list`，形成多记录事件。

### 1.6.4 audit_log_end

`audit.c:2813` 的 `audit_log_end()` 逐个 `skb_dequeue(&ab->skb_list)` 并调用 `__audit_log_end()`，然后 `wake_up_interruptible(&kauditd_wait)`、释放 buffer。

`__audit_log_end()`（第 2786 行）：`audit_rate_check()` 失败则 `audit_log_lost("rate limit exceeded")` 并丢弃；通过则修正 `nlmsg_len`、`skb_queue_tail(&audit_queue, skb)`。不在中断上下文直接 netlink 发送，交给 `kauditd_thread`。

### 1.6.5 kauditd_thread

`audit.c:883` 的 `kauditd_thread()` 持续从 `audit_queue` 取 skb，按 auditd 连接信息单播，并在有 multicast 监听者时同时 multicast；失败或队列满时重试或计入 lost。

## 1.7 auditd / netlink 交互细节

### 1.7.1 控制消息

`audit_receive_msg()`（`audit.c:1255`）处理：

| 类型 | 值 | 作用 |
| --- | --- | --- |
| `AUDIT_GET` / `AUDIT_SET` | 1000/1001 | 读取/设置 `audit_status` |
| `AUDIT_LIST_RULES` | 1013 | 列出规则 |
| `AUDIT_ADD_RULE` / `AUDIT_DEL_RULE` | 1011/1012 | 增删规则 |
| `AUDIT_TTY_GET/SET` | 1016/1017 | TTY 输入审计 |
| `AUDIT_GET_FEATURE` / `SET_FEATURE` | 1019/1018 | 特性位（如 loginuid 不可变） |
| `AUDIT_TRIM` / `MAKE_EQUIV` / `AUDIT_SIGNAL_INFO` | 1014/1015/1010 | 监视树维护、查询信号来源 |

`struct audit_status`（`include/uapi/linux/audit.h:472`）携带 `mask`、`enabled`、`failure`、`pid`、`rate_limit`、`backlog_limit`、`lost`、`backlog`、`backlog_wait_time` 等。

### 1.7.2 配置变更与失败模式

`AUDIT_SET` 等操作生成 `AUDIT_CONFIG_CHANGE`（1305），包含 `op=`、`audit_enabled=`、`res=`；规则增删由 `audit_log_rule_change()` 记录 `add_rule`/`remove_rule`。`audit_status.failure` 控制日志失败行为：0 静默、1 `printk`、2 `panic`（对应 `audit_failure=`）。

### 1.7.3 锁模型

`auditfilter.c:25-36`：`audit_filter_mutex` 同步规则链写与阻塞读；过滤遍历用 RCU 读，修改规则必须“复制—替换”。

## 1.8 auditctl 规则字段示例

### 1.8.1 syscall 规则

```sh
auditctl -a always,exit -F arch=b64 -S openat,openat2 -F exit=-EACCES -F auid>=1000 -k file_access
```

映射：`-a always,exit` → `action=AUDIT_ALWAYS`、`listnr=AUDIT_FILTER_EXIT`；`-F arch=b64` → `AUDIT_ARCH` 字段；`-S` → `rule->mask` 中对应 syscall 位；`-F exit=-EACCES` → `AUDIT_EXIT`；`-F auid...` → `AUDIT_LOGINUID` 与 `Audit_greater_equal`/`Audit_not_equal`；`-k` → `filterkey`，输出为 `key="file_access"`。

### 1.8.2 文件监视

`auditctl -w /etc/passwd -p wa -k passwd_change` 生成 `AUDIT_FILTER_WATCH` 规则，`-p wa` 映射为 `AUDIT_PERM` 的写与属性位；内核通过 `audit_watch` + fsnotify 在 inode 变更时收集 `audit_names`。目录监视会创建树/子项监视。

### 1.8.3 常用字段

| auditctl 字段 | 内核 `audit_field.type` |
| --- | --- |
| `pid` / `ppid` | `AUDIT_PID` / `AUDIT_PPID` |
| `uid` / `euid` / `suid` / `fsuid` | `AUDIT_UID` / `AUDIT_EUID` / `AUDIT_SUID` / `AUDIT_FSUID` |
| `gid` / `egid` / `sgid` / `fsgid` | `AUDIT_GID` 等 |
| `auid` | `AUDIT_LOGINUID` |
| `arch` / `exit` / `perm` / `filetype` / `exe` | `AUDIT_ARCH` / `AUDIT_EXIT` / `AUDIT_PERM` / `AUDIT_FILETYPE` / `AUDIT_EXE` |
| `key` | `filterkey`（不是 field） |
| `subj_user` 等 | `AUDIT_SUBJ_USER` 等 LSM 字段 |

比较操作符为 `Audit_equal`、`Audit_not_equal`、`Audit_bitmask`、`Audit_bittest`、`Audit_lt`、`Audit_gt`、`Audit_le`、`Audit_ge`、`Audit_not_*` 等。

## 1.9 当前配置下的行为

| 项 | 当前值 | 说明 |
| --- | --- | --- |
| `CONFIG_AUDIT` | `y` | 核心与规则引擎编译 |
| `CONFIG_AUDITSYSCALL` | `y` | 每任务上下文、syscall 审计、watch/tree 编译 |
| `CONFIG_AUDIT_GENERIC` / `AUDIT_ARCH_COMPAT_GENERIC` | `y` / `y` | 通用与 32 位兼容 syscall 分类 |
| `CONFIG_AUDIT_WATCH` / `AUDIT_TREE` | 无独立符号 | watch/tree 由 `AUDITSYSCALL` 一并编译 |

当前内核审计框架完整可用：`audit=1` 或 `auditctl -e 1` 后开始记录，syscall 审计、路径收集、watch/tree 均生效；默认 `AUDIT_OFF`，未显式开启时不产生记录。没有 auditd 注册时，记录仍进入内核队列，但很快受 backlog/rate limit 影响而丢弃，只能通过 `audit_log_lost()` 的 printk 观察。

## 1.10 关键文件清单

- `/home/louis/code/linux/kernel/audit.c`：核心状态、netlink、`audit_log_start`/`audit_log_end`、kauditd
- `/home/louis/code/linux/kernel/auditsc.c`：`audit_context`、`__audit_syscall_entry/exit`、`audit_filter_rules`
- `/home/louis/code/linux/kernel/auditfilter.c`：规则解析、`audit_filter_list`、syscall 位图匹配
- `/home/louis/code/linux/kernel/audit.h`：`struct audit_context`、`enum audit_state`
- `/home/louis/code/linux/kernel/audit_watch.c`、`audit_fsnotify.c`、`audit_tree.c`：文件/目录监视
- `/home/louis/code/linux/include/linux/audit.h`、`include/uapi/linux/audit.h`：`audit_krule`、`audit_field`、消息类型、`audit_status`、`audit_rule_data`、字段常量
- `/home/louis/code/linux/init/Kconfig`、`kernel/Makefile`：配置与对象选择
