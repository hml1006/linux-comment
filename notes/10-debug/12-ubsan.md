# UBSAN (Undefined Behavior SANitizer)

## 概述

UBSAN 是 Linux 内核的未定义行为检测工具，用于检测整数溢出、除零错误、数组越界、空指针解引用等未定义行为。UBSAN 通过在编译时插桩相关操作，在运行时检测并报告这些问题。

## 架构设计

```
┌─────────────────────────────────────────────────────────────────────┐
│                         UBSAN Architecture                          │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                        Compiler Instrumentation              │   │
│  │                                                             │   │
│  │  • 编译时在每个可能触发未定义行为的操作前插入检查代码        │   │
│  │  • __ubsan_handle_*() 系列函数                              │   │
│  │  • 收集源代码位置信息和类型描述符                           │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                        Runtime Check                         │   │
│  │                                                             │   │
│  │  • 根据检测类型调用对应的处理函数                            │   │
│  │  • 检查是否已经报告过（避免重复报告）                        │   │
│  │  • 根据检测结果生成详细错误报告                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      Error Reporting                        │   │
│  │                                                             │   │
│  │  • ubsan_prologue() - 输出错误位置和类型                    │   │
│  │  • ubsan_epilogue() - 输出栈追踪和结束标记                  │   │
│  │  • 显示操作数的值和类型信息                                 │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 检测类型

| 检测类型 | 配置项 | 处理函数 | 描述 |
|----------|--------|----------|------|
| **整数加法溢出** | `CONFIG_UBSAN_INTEGER_WRAP` | `__ubsan_handle_add_overflow()` | 检测有符号/无符号整数加法溢出 |
| **整数减法溢出** | `CONFIG_UBSAN_INTEGER_WRAP` | `__ubsan_handle_sub_overflow()` | 检测有符号/无符号整数减法溢出 |
| **整数乘法溢出** | `CONFIG_UBSAN_INTEGER_WRAP` | `__ubsan_handle_mul_overflow()` | 检测有符号/无符号整数乘法溢出 |
| **整数取反溢出** | `CONFIG_UBSAN_INTEGER_WRAP` | `__ubsan_handle_negate_overflow()` | 检测对 INT_MIN 取反的溢出 |
| **除零错误** | `CONFIG_UBSAN_DIV_ZERO` | `__ubsan_handle_divrem_overflow()` | 检测整数除以零或对 INT_MIN / -1 |
| **隐式类型转换** | `CONFIG_UBSAN_IMPLICIT_CONVERSION` | `__ubsan_handle_implicit_conversion()` | 检测不安全的隐式类型转换 |
| **数组越界** | `CONFIG_UBSAN_BOUNDS` | `__ubsan_handle_out_of_bounds()` | 检测数组索引越界 |
| **移位溢出** | `CONFIG_UBSAN_SHIFT` | `__ubsan_handle_shift_out_of_bounds()` | 检测移位操作溢出 |
| **空指针解引用** | `CONFIG_UBSAN_ALIGNMENT` | `__ubsan_handle_type_mismatch()` | 检测空指针或类型不匹配的访问 |
| **对齐错误** | `CONFIG_UBSAN_ALIGNMENT` | `__ubsan_handle_alignment_assumption()` | 检测内存对齐假设失败 |
| **无效值加载** | `CONFIG_UBSAN_BOOL` / `CONFIG_UBSAN_ENUM` | `__ubsan_handle_load_invalid_value()` | 检测加载无效的 bool 或 enum 值 |
| **不可达代码** | `CONFIG_UBSAN_UNREACHABLE` | `__ubsan_handle_builtin_unreachable()` | 检测执行到 `__builtin_unreachable()` |

## 核心数据结构

### type_descriptor

```c
struct type_descriptor {
    u16 type_kind;         /* 类型种类: type_kind_int=0, type_kind_float=1 */
    u16 type_info;         /* 类型信息: bit 0=是否有符号, bit 1-15=log2(位宽) */
    char type_name[];      /* 类型名称字符串 */
};
```

该结构描述被检测值的类型信息。

### source_location

```c
struct source_location {
    const char *file_name; /* 源代码文件名 */
    union {
        unsigned long reported; /* 是否已报告（用于去重） */
        struct {
            u32 line;       /* 行号 */
            u32 column;     /* 列号 */
        };
    };
};
```

该结构描述源代码中的位置信息，同时用于去重。

### overflow_data

```c
struct overflow_data {
    struct source_location location; /* 源代码位置 */
    struct type_descriptor *type;    /* 操作数类型 */
};
```

该结构用于整数溢出检测的数据。

### out_of_bounds_data

```c
struct out_of_bounds_data {
    struct source_location location; /* 源代码位置 */
    struct type_descriptor *array_type; /* 数组类型 */
    struct type_descriptor *index_type; /* 索引类型 */
};
```

该结构用于数组越界检测的数据。

### shift_out_of_bounds_data

```c
struct shift_out_of_bounds_data {
    struct source_location location; /* 源代码位置 */
    struct type_descriptor *lhs_type; /* 被移位值类型 */
    struct type_descriptor *rhs_type; /* 移位量类型 */
};
```

该结构用于移位溢出检测的数据。

## 工作流程

```
┌─────────────────────────────────────────────────────────────────────┐
│                     UBSAN Detection Flow                            │
│                                                                     │
│  编译时:                                                             │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  • Clang 编译器在每个可能触发 UB 的操作前插入检查代码         │   │
│  │  • 生成类型描述符和源代码位置信息                           │   │
│  │  • 调用 __ubsan_handle_*() 系列函数                        │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                            │                                        │
│                            ▼                                        │
│  运行时:                                                             │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  1. suppress_report() - 检查是否需要抑制报告                │   │
│  │     • current->in_ubsan > 0 - 递归调用，避免死循环         │   │
│  │     • was_reported() - 检查是否已经报告过，避免重复         │   │
│  │                      │                                        │   │
│  │                      ▼                                        │   │
│  │  2. ubsan_prologue() - 输出错误头部信息                     │   │
│  │     • 输出错误类型和源代码位置                              │   │
│  │     • 增加 current->in_ubsan 计数                          │   │
│  │                      │                                        │   │
│  │                      ▼                                        │   │
│  │  3. 处理函数 - 输出详细错误信息                             │   │
│  │     • 输出操作数的值                                        │   │
│  │     • 输出类型名称                                          │   │
│  │                      │                                        │   │
│  │                      ▼                                        │   │
│  │  4. ubsan_epilogue() - 输出栈追踪和结束标记                 │   │
│  │     • dump_stack() - 输出栈追踪                            │   │
│  │     • 减少 current->in_ubsan 计数                          │   │
│  │     • check_panic_on_warn() - 根据配置决定是否 panic        │   │
│  └─────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

## 关键函数

### ubsan_prologue()

```c
static void ubsan_prologue(struct source_location *loc, const char *reason);
```

输出错误报告的头部信息，包括错误类型和源代码位置。

### ubsan_epilogue()

```c
static void ubsan_epilogue(void);
```

输出栈追踪和报告结束标记，根据配置决定是否触发 panic。

### suppress_report()

```c
static bool suppress_report(struct source_location *loc);
```

检查是否需要抑制报告：
- `current->in_ubsan > 0`：递归调用，避免死循环
- `was_reported()`：检查是否已经报告过

### was_reported()

```c
static bool was_reported(struct source_location *location);
```

检查该位置的错误是否已经报告过，使用 `REPORTED_BIT`（第 31 位）标记。

### __ubsan_handle_add_overflow()

```c
void __ubsan_handle_add_overflow(void *data, void *lhs, void *rhs);
```

处理整数加法溢出错误。

### __ubsan_handle_divrem_overflow()

```c
void __ubsan_handle_divrem_overflow(void *_data, void *lhs, void *rhs);
```

处理整数除法/取余溢出错误（包括除零和 INT_MIN / -1）。

### __ubsan_handle_out_of_bounds()

```c
void __ubsan_handle_out_of_bounds(void *_data, void *index);
```

处理数组索引越界错误。

### __ubsan_handle_shift_out_of_bounds()

```c
void __ubsan_handle_shift_out_of_bounds(void *_data, void *lhs, void *rhs);
```

处理移位操作溢出错误。

### __ubsan_handle_type_mismatch()

```c
void __ubsan_handle_type_mismatch(struct type_mismatch_data *data, void *ptr);
```

处理类型不匹配错误，包括：
- 空指针解引用
- 对齐错误
- 对象大小不匹配

### __ubsan_handle_builtin_unreachable()

```c
void __ubsan_handle_builtin_unreachable(void *_data);
```

处理不可达代码错误，会触发 panic。

## 错误报告格式

### 整数溢出示例

```
UBSAN: signed-integer-overflow in drivers/mydriver/main.c:123:45
1234567890 + (-9876543210) cannot be represented in type 'int'
---[ end trace ]---
```

### 除零错误示例

```
UBSAN: division-overflow in drivers/mydriver/main.c:123:45
division by zero
---[ end trace ]---
```

### 数组越界示例

```
UBSAN: array-index-out-of-bounds in drivers/mydriver/main.c:123:45
index 10 is out of range for type 'int [5]'
---[ end trace ]---
```

### 空指针解引用示例

```
UBSAN: null-ptr-deref in drivers/mydriver/main.c:123:45
load of null pointer of type 'struct my_struct *'
---[ end trace ]---
```

## 编译配置

| 配置项 | 说明 |
|--------|------|
| `CONFIG_UBSAN` | 启用 UBSAN 支持 |
| `CONFIG_UBSAN_INTEGER_WRAP` | 启用整数溢出检测 |
| `CONFIG_UBSAN_DIV_ZERO` | 启用除零检测 |
| `CONFIG_UBSAN_BOUNDS` | 启用数组越界检测 |
| `CONFIG_UBSAN_SHIFT` | 启用移位溢出检测 |
| `CONFIG_UBSAN_ALIGNMENT` | 启用对齐检测 |
| `CONFIG_UBSAN_BOOL` | 启用 bool 类型检测 |
| `CONFIG_UBSAN_ENUM` | 启用 enum 类型检测 |
| `CONFIG_UBSAN_UNREACHABLE` | 启用不可达代码检测 |
| `CONFIG_UBSAN_TRAP` | 启用陷阱模式（直接触发异常） |
| `CONFIG_UBSAN_KVM_EL2` | 在 KVM EL2 层处理 UBSAN 陷阱 |

## 陷阱模式

当启用 `CONFIG_UBSAN_TRAP` 时，UBSAN 不生成详细报告，而是直接触发陷阱：

```c
const char *report_ubsan_failure(u32 check_type)
{
    switch (check_type) {
    case ubsan_out_of_bounds:
        return "UBSAN: array index out of bounds";
    case ubsan_shift_out_of_bounds:
        return "UBSAN: shift out of bounds";
    case ubsan_divrem_overflow:
        return "UBSAN: divide/remainder overflow";
    // ... 其他类型
    }
}
```

陷阱模式的优势：
- 性能开销更小（不需要生成详细报告）
- 可以在早期捕获问题

## 性能影响

| 方面 | 影响 |
|------|------|
| **内存开销** | 较小，主要是类型描述符和位置信息 |
| **CPU 开销** | 中等，取决于检测类型的多少 |
| **启动时间** | 影响较小 |

## 使用场景

1. **开发阶段**：在开发过程中启用 UBSAN，检测未定义行为
2. **测试阶段**：在 CI/CD 流程中运行测试，确保没有未定义行为
3. **问题排查**：当系统出现不稳定时，启用 UBSAN 复现问题

## 代码位置

| 文件 | 说明 |
|------|------|
| `lib/ubsan.c` | UBSAN 运行时处理函数 |
| `lib/ubsan.h` | UBSAN 数据结构定义 |
| `include/linux/ubsan.h` | 内核 UBSAN 接口 |
| `Documentation/dev-tools/ubsan.rst` | UBSAN 文档 |