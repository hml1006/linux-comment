# adjtimex 系统调用分析

## 1. 概述

`adjtimex()` 用于读取和设置内核时钟调整参数，是 NTP（网络时间协议）调整系统时钟的核心接口。它可以读取/设置时钟频率偏移、最大误差、估计误差、时间偏移等参数，用于精确同步系统时间。

**原型：**

```c
SYSCALL_DEFINE1(adjtimex, struct __kernel_timex __user *, txc_p)
```

| 参数 | 类型 | 描述 |
|------|------|------|
| txc_p | timex* | 输入/输出参数，包含时钟调整参数 |

## 2. 使用场景

- NTP 服务：ntpd 使用它精确调整系统时钟
- 时钟频率补偿：调节晶体振荡器的频率偏差
- 读取时钟状态：获取当前时钟精度、误差等信息
- 设置时区：通过 timex 中的 timezone 字段

## 3. 函数调用链

```
adjtimex(txc_p)                                       // kernel/time/time.c:269
  │
  ├─ copy_from_user(&txc, txc_p, sizeof(struct __kernel_timex))
  │
  ├─ do_adjtimex(&txc)                                 // kernel/time/timekeeping.c:2754
  │    │
  │    └─ __do_adjtimex(&tk_core, txc, &result)        // kernel/time/timekeeping.c:2694
  │         │
  │         ├─ timekeeping_validate_timex(txc)          // 验证参数
  │         │
  │         ├─ 若 txc->modes & ADJ_SETOFFSET:
  │         │   └─ timekeeping_set_offset()             // 设置时间偏移
  │         │
  │         ├─ 若 txc->modes 有调整标志:
  │         │   └─ ntp_adjtimex(tks->id, txc, &ts, &tai, &result->ad)
  │         │        └─ process_adjtimex_modes()         // NTP 调整逻辑
  │         │
  │         └─ 填充 result 中的状态信息
  │
  └─ copy_to_user(txc_p, &txc, sizeof(struct __kernel_timex))
```

## 4. 关键数据结构

```c
// 时钟调整参数结构
struct __kernel_timex {
    unsigned int modes;        // 操作模式（ADJ_OFFSET, ADJ_FREQUENCY 等）
    __kernel_long_t offset;    // 时间偏移（微秒）
    __kernel_long_t freq;      // 频率偏移（ppm * 2^16）
    __kernel_long_t maxerror;  // 最大误差（微秒）
    __kernel_long_t esterror;  // 估计误差（微秒）
    int status;                // 时钟状态
    __kernel_long_t constant;  // PLL 时间常数
    __kernel_long_t precision; // 时钟精度（微秒，只读）
    __kernel_long_t tolerance; // 频率容限（ppm * 2^16，只读）
    struct timeval time;       // 当前时间（只读）
    __kernel_long_t tick;      // tick 微秒数
    __kernel_long_t ppsfreq;   // PPS 频率
    __kernel_long_t jitter;    // PPS 抖动
    int shift;                 // PPS 间隔
    __kernel_long_t stabil;    // PPS 稳定度
    __kernel_long_t jitcnt;    // PPS 抖动计数
    __kernel_long_t calcnt;    // PPS 校准计数
    __kernel_long_t errcnt;    // PPS 错误计数
    __kernel_long_t stbcnt;    // PPS 稳定度计数
    int tai;                   // TAI 偏移
    // ...
};

// modes 标志位
#define ADJ_OFFSET          0x0001  // 时间偏移
#define ADJ_FREQUENCY       0x0002  // 频率偏移
#define ADJ_MAXERROR        0x0004  // 最大误差
#define ADJ_ESTERROR        0x0008  // 估计误差
#define ADJ_STATUS          0x0010  // 时钟状态
#define ADJ_TIMECONST       0x0020  // PLL 时间常数
#define ADJ_TICK            0x4000  // Tick 值
#define ADJ_OFFSET_SINGLESHOT 0x8001 // 单次偏移（微秒）
#define ADJ_SETOFFSET       0x0100  // 精确设置时间（纳秒）
```

## 5. 流程图

```
用户态: adjtimex(&txc)
    │
    ▼
SYSCALL_DEFINE1(adjtimex)
    │
    ├─ 从用户空间拷贝 txc 结构
    │
    └─ do_adjtimex()
         │
         ├─ timekeeping_validate_timex()  // 验证参数合法性
         │
         ├─ 处理 modes:
         │   ├─ ADJ_SETOFFSET → timekeeping_set_offset()
         │   ├─ ADJ_OFFSET → ntp 调整偏移
         │   ├─ ADJ_FREQUENCY → ntp 调整频率
         │   ├─ ADJ_MAXERROR → 设置最大误差
         │   ├─ ADJ_ESTERROR → 设置估计误差
         │   ├─ ADJ_STATUS → 设置时钟状态
         │   ├─ ADJ_TIMECONST → 设置 PLL 常数
         │   └─ ADJ_TICK → 设置 tick 值
         │
         ├─ 填充状态信息（只读字段）
         │
         └─ 返回时钟状态码
              ├─ TIME_OK    = 0  时钟同步
              ├─ TIME_INS   = 1  插入闰秒
              ├─ TIME_DEL   = 2  删除闰秒
              ├─ TIME_OOP   = 3  闰秒发生中
              ├─ TIME_WAIT  = 4  闰秒等待
              └─ TIME_ERROR = 5  时钟不同步
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| EINVAL | 无效参数 | modes 中的标志无效或参数值越界 |
| EPERM | 权限不足 | 非特权用户尝试设置参数 |
| EFAULT | 内存错误 | txc_p 指针指向不可访问地址 |

## 7. 使用示例

```c
#include <stdio.h>
#include <sys/timex.h>
#include <unistd.h>

int main(void)
{
    struct timex txc = {0};

    /* 读取当前时钟状态 */
    if (adjtimex(&txc) == -1) {
        perror("adjtimex");
        return 1;
    }

    printf("Clock status:\n");
    printf("  offset:    %ld us\n", txc.offset);
    printf("  freq:      %ld (ppm * 2^16)\n", txc.freq);
    printf("  maxerror:  %ld us\n", txc.maxerror);
    printf("  esterror:  %ld us\n", txc.esterror);
    printf("  status:    %d\n", txc.status);
    printf("  precision: %ld us\n", txc.precision);
    printf("  tolerance: %ld (ppm * 2^16)\n", txc.tolerance);
    printf("  tick:      %ld us\n", txc.tick);

    return 0;
}
```

## 8. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#定时器与时间)
- kernel/time/timekeeping.c:`do_adjtimex()` - 核心实现
- kernel/time/ntp.c:`ntp_adjtimex()` - NTP 调整逻辑
- include/uapi/linux/timex.h - timex 结构及标志定义