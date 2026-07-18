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

## 6. 参考

- 源码位置：`kernel/exec_domain.c`
- 头文件：`include/uapi/linux/personality.h`
- [ARM64 系统调用表](../arm64-syscall-table.md#进程凭证与权限)