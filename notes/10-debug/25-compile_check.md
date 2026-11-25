# 编译时静态检查

## 概述

编译时静态检查是 Linux 内核提供的一系列在编译阶段执行的代码验证工具，用于在代码运行前发现潜在的错误和问题。这些工具利用编译器（如 Clang）的静态分析能力，在不运行代码的情况下检测代码缺陷。

### 工作原理

```
┌─────────────────────────────────────────────────────────────┐
│                     编译流程                               │
│                                                             │
│  源代码 (.c/.h)                                             │
│       ↓                                                     │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              预处理器 (cpp)                           │   │
│  │  - 展开宏定义                                         │   │
│  │  - 处理条件编译                                       │   │
│  │  - 生成预处理后的代码                                 │   │
│  └──────────────────────────────────────────────────────┘   │
│       ↓                                                     │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              编译器前端 (Clang/GCC)                    │   │
│  │  - 词法分析 / 语法分析                                │   │
│  │  - 语义分析                                          │   │
│  │  - 类型检查                                          │   │
│  │  - 静态分析 (Context Analysis, UBSAN 插桩)           │   │
│  └──────────────────────────────────────────────────────┘   │
│       ↓                                                     │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              编译器后端                                │   │
│  │  - 中间代码生成                                       │   │
│  │  - 优化                                              │   │
│  │  - 目标代码生成                                       │   │
│  └──────────────────────────────────────────────────────┘   │
│       ↓                                                     │
│  目标代码 (.o)                                              │
└─────────────────────────────────────────────────────────────┘
```

## Clang 上下文与锁分析

### 概述

Context Analysis 是一种语言扩展，用于静态检查用户定义的"上下文锁"是否正确获取和释放。最常见的应用是锁安全性检查，确保内核同步原语的使用规则不被违反。

该功能需要 Clang 22 或更高版本。

### 编译配置

```
CONFIG_WARN_CONTEXT_ANALYSIS=y       # 启用上下文分析
CONFIG_WARN_CONTEXT_ANALYSIS_ALL=y   # 全树启用 (不推荐)
```

### 使用方法

**按模块启用**:

```makefile
# 在模块的 Makefile 中
CONTEXT_ANALYSIS_mymodule.o := y
```

**按目录启用**:

```makefile
# 在目录的 Makefile 中
CONTEXT_ANALYSIS := y
```

### 编程模型

#### 基本概念

```c
/* 使用 __guarded_by 标注数据被哪个锁保护 */
struct my_data {
    spinlock_t lock;
    int counter __guarded_by(&lock);
};

/* 使用 acquire/release 标注锁操作函数 */
void my_lock(spinlock_t *lock) __acquires(lock);
void my_unlock(spinlock_t *lock) __releases(lock);
```

#### 上下文锁类型

| 锁类型 | 描述 |
|--------|------|
| `raw_spinlock_t` | 原始自旋锁 |
| `spinlock_t` | 自旋锁 |
| `rwlock_t` | 读写锁 |
| `mutex` | 互斥锁 |
| `seqlock_t` | 顺序锁 |
| `bit_spinlock` | 位自旋锁 |
| RCU | 读-拷贝-更新 |
| SRCU | 可睡眠 RCU |
| `rw_semaphore` | 读写信号量 |
| `local_lock_t` | 本地锁 |
| `ww_mutex` | 愿望-写互斥锁 |

#### 示例

```c
struct example {
    mutex mtx;
    int value __guarded_by(&mtx);
};

void example_func(struct example *e)
{
    mutex_lock(&e->mtx);      /* 编译器知道 mtx 已获取 */
    e->value = 42;            /* OK: 在 mtx 保护下访问 */
    mutex_unlock(&e->mtx);    /* 编译器知道 mtx 已释放 */
    e->value = 0;             /* ERROR: 未持有 mtx 时访问 */
}
```

### 上下文环境

编译器为每个程序点计算一个"上下文环境"，描述该点静态已知的活跃上下文集合。

```
上下文环境示例:
┌─────────────────────────────────────────────────────────────┐
│ void foo(struct data *d)                                    │
│ {                                                           │
│   mutex_lock(&d->mtx);    ← 上下文环境: {&d->mtx}          │
│                                                             │
│   bar(d);                 ← 上下文环境: {&d->mtx}          │
│                                                             │
│   mutex_unlock(&d->mtx);  ← 上下文环境: {}                  │
│ }                                                           │
└─────────────────────────────────────────────────────────────┘
```

### 支持的注解

| 注解 | 用途 |
|------|------|
| `__guarded_by(lock)` | 标注数据受锁保护 |
| `__acquires(lock)` | 标注函数获取锁 |
| `__releases(lock)` | 标注函数释放锁 |
| `__exclusive_locks_required(lock)` | 要求独占锁 |
| `__shared_locks_required(lock)` | 要求共享锁 |
| `__locks_excluded(lock)` | 要求锁未被持有 |
| `__assert_locked(lock)` | 断言锁已持有 |

### 抑制机制

当分析产生误报时，可以使用抑制文件：

```bash
# scripts/context-analysis-suppression.txt
# 格式: 文件:行号:函数名
kernel/sched/fair.c:1234:select_task_rq_fair
```

### 代码位置

```
Documentation/dev-tools/context-analysis.rst    # 官方文档
scripts/Makefile.context-analysis                # Makefile 支持
scripts/context-analysis-suppression.txt        # 抑制列表
include/linux/compiler-context-analysis.h       # 编译器接口
```

## Clang 静态分析支持

### 概述

Linux 内核增强了对 Clang 静态分析工具的支持，可以在编译阶段发现代码缺陷。

### 使用方法

```bash
# 使用 scan-build 进行静态分析
scan-build make

# 使用 clang-analyzer
make CC=clang CFLAGS="-analyze"

# 指定分析检查器
scan-build -enable-checker security make
```

### 常用检查器

| 检查器 | 描述 |
|--------|------|
| `core.DivideZero` | 除零检查 |
| `core.NullDereference` | 空指针解引用 |
| `core.StackAddressEscape` | 栈地址逃逸 |
| `security.FloatLoopCounter` | 浮点数循环计数器 |
| `security.insecureAPI.bcmp` | 不安全的 bcmp 使用 |
| `security.insecureAPI.strcpy` | 不安全的 strcpy 使用 |
| `unix.Malloc` | 内存泄漏检查 |
| `unix.MismatchedDeallocator` | 不匹配的内存释放 |
| `unix.Vfork` | vfork 使用检查 |

### 示例输出

```
kernel/sched/fair.c:1234:5: warning: Potential null pointer dereference
  if (!task)
      return;
  task->state = TASK_RUNNING;  // 警告: task 可能为 NULL
```

## DEBUG_BUGVERBOSE_DETAILED

### 概述

`DEBUG_BUGVERBOSE_DETAILED` 扩展了 `DEBUG_BUGVERBOSE`，在 `WARN_ON_ONCE()` 触发时输出条件字符串，帮助开发者更快定位问题。

### 编译配置

```
CONFIG_DEBUG_BUGVERBOSE=y              # 基础详细报告
CONFIG_DEBUG_BUGVERBOSE_DETAILED=y     # 额外输出条件字符串
```

### 效果对比

**未启用 DEBUG_BUGVERBOSE_DETAILED**:

```
WARNING: CPU: 0 PID: 1234 at kernel/sched/fair.c:1234
```

**启用 DEBUG_BUGVERBOSE_DETAILED**:

```
WARNING: CPU: 0 PID: 1234 at kernel/sched/fair.c:1234 [!list_empty(&rq->cfs_tasks)]
```

### 实现机制

```c
#ifdef CONFIG_DEBUG_BUGVERBOSE_DETAILED
# define WARN_CONDITION_STR(cond_str) "[" cond_str "] "
#else
# define WARN_CONDITION_STR(cond_str)
#endif

/* WARN_ON_ONCE 展开后会包含条件字符串 */
#define WARN_ON_ONCE(condition) ({                              \
    static bool __section(".data..once") __warned;              \
    int __ret_warn_on = !!(condition);                          \
    if (unlikely(__ret_warn_on)) {                              \
        if (!__warned) {                                        \
            __warned = true;                                    \
            __warn(__FILE__, __LINE__, __func__,                 \
                   TAINT_WARN, NULL, NULL);                     \
        }                                                       \
    }                                                           \
    unlikely(__ret_warn_on);                                    \
})
```

### 内存开销

- `DEBUG_BUGVERBOSE`: 约 70-100KB
- `DEBUG_BUGVERBOSE_DETAILED`: 额外约 100KB

### 使用场景

当 `WARN_ON_ONCE()` 条件触发时，条件字符串可以帮助开发者快速理解：
- 什么条件触发了警告
- 问题发生的具体逻辑

### 代码位置

```
include/asm-generic/bug.h        # BUG/WARN 宏定义
lib/Kconfig.debug                # 配置选项
lib/bug.c                        # BUG/WARN 实现
```

## 编译时检查总结

| 工具 | 编译器 | 检测内容 | 开销 |
|------|--------|----------|------|
| **Context Analysis** | Clang 22+ | 锁使用正确性 | 编译时间 |
| **Clang Static Analyzer** | Clang | 代码缺陷 | 编译时间 |
| **DEBUG_BUGVERBOSE** | 任意 | BUG() 详细信息 | 70-100KB |
| **DEBUG_BUGVERBOSE_DETAILED** | 任意 | WARN_ON_ONCE() 条件字符串 | 额外 100KB |

## 最佳实践

### 开发阶段

```bash
# 启用所有静态检查
make CC=clang defconfig
scripts/config -e WARN_CONTEXT_ANALYSIS
scripts/config -e DEBUG_BUGVERBOSE
scripts/config -e DEBUG_BUGVERBOSE_DETAILED
make -j$(nproc)
```

### CI/CD 集成

```bash
# 在 CI 中运行静态分析
scan-build --status-bugs make -j$(nproc)
```

### 生产环境

```bash
# 生产环境禁用调试选项以减小内核体积
make CC=clang defconfig
scripts/config -d DEBUG_BUGVERBOSE_DETAILED
scripts/config -d DEBUG_BUGVERBOSE
make -j$(nproc)
```

## 代码位置

```
Documentation/dev-tools/context-analysis.rst    # Context Analysis 文档
scripts/Makefile.context-analysis                # Context Analysis Makefile
include/linux/compiler-context-analysis.h       # Context Analysis 接口
include/asm-generic/bug.h                       # BUG/WARN 宏定义
lib/Kconfig.debug                               # 配置选项
lib/bug.c                                       # BUG/WARN 实现
```