# getrandom 系统调用分析

## 1. 概述

`getrandom` 用于从内核的随机数生成器（CRNG）获取高质量的伪随机数。它是对 `/dev/urandom` 和 `/dev/random` 设备文件的系统调用级替代方案，避免了对文件描述符的依赖。

**原型：**

```c
SYSCALL_DEFINE3(getrandom, char __user *, ubuf, size_t, len, unsigned int, flags)
```

**参数：**
- `ubuf`：用户空间缓冲区，用于接收随机数
- `len`：请求的字节数
- `flags`：行为标志位

**flags 标志：**
- `GRND_NONBLOCK` (0x01)：若 CRNG 未就绪，不阻塞直接返回 -EAGAIN
- `GRND_RANDOM` (0x02)：从 /dev/random（阻塞池）读取，而非 /dev/urandom
- `GRND_INSECURE` (0x04)：即使 CRNG 未就绪也返回数据（可能为低熵值）

## 2. 使用场景

- 生成密钥材料（对称密钥、非对称密钥对）
- 生成随机盐值（密码哈希、TLS 握手）
- 生成随机 nonce（IV、序列号）
- 地址空间布局随机化（ASLR）种子
- 任何需要密码学安全随机数的场景

## 3. 函数调用栈

```
getrandom(ubuf, len, flags)                          // drivers/char/random.c
  ├─ flags 校验（GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE）
  ├─ GRND_INSECURE 与 GRND_RANDOM 互斥检查
  ├─ [CRNG 未就绪且非 GRND_INSECURE]
  │    ├─ GRND_NONBLOCK → 返回 -EAGAIN
  │    └─ 阻塞 → wait_for_random_bytes()              // 等待 CRNG 初始化
  ├─ import_ubuf(ITER_DEST, ubuf, len, &iter)         // 构建 iov_iter
  └─ get_random_bytes_user(&iter)                      // 填充用户缓冲区
       └─ chacha20_block(chacha_state, buf, ...)       // ChaCha20 加密块生成
```

## 4. 关键数据结构

### 4.1 iov_iter（迭代器抽象）

```c
// include/linux/uio.h
struct iov_iter {
    u8 iter_type;                   // 迭代类型（ITER_IOVEC, ITER_KVEC, ITER_BVEC 等）
    u8 data_source;                 // ITER_SOURCE (读) 或 ITER_DEST (写)
    size_t iov_offset;              // 当前 iov 内的偏移
    size_t count;                   // 剩余字节数
    union {
        const struct iovec *iov;    // 用户空间 iovec 数组
        struct kvec *kvec;          // 内核空间 iovec 数组
        struct bvec_iter *bvec;     // 块设备迭代器
    };
};
```

### 4.2 ChaCha20 状态块

```c
// drivers/char/random.c
#define CHACHA20_BLOCK_SIZE 64
#define CHACHA20_KEY_SIZE   32
#define CHACHA20_NONCE_SIZE 8

struct chacha20_state {
    u32 constants[4];               // 常量: "expand 32-byte k"
    u32 key[8];                     // 256 位密钥
    u32 counter;                    // 块计数器
    u32 nonce[2];                   // 64 位 nonce
};
```

## 5. 流程图

```
用户态调用 getrandom(ubuf, len, flags)
    │
    v
┌─────────────────────────────────────┐
│ 检查 flags 合法性                    │
│ - 未知标志 → 返回 -EINVAL           │
│ - GRND_INSECURE|GRND_RANDOM → -EINVAL│
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ CRNG 是否已初始化?                   │
├─────────┬───────────────────────────┘
│ 是      │ 否（且非 GRND_INSECURE）
│         v
│    ┌────────────────────────┐
│    │ GRND_NONBLOCK 设置?     │
│    ├──────┬─────────────────┘
│    │ 是   │ 否
│    │      v
│    │ 返回  wait_for_random_bytes()
│    │ -EAGAIN  │ 等待初始化完成
│    │         │ 可能被信号中断
│    │         v
│    └──────返回 -ERESTARTSYS/EINTR
│
v
┌─────────────────────────────────────┐
│ import_ubuf() → 构建 iov_iter       │
└─────────────────────────────────────┘
    │
    v
┌─────────────────────────────────────┐
│ get_random_bytes_user(&iter)        │
│ ┌─────────────────────────────────┐ │
│ │ 循环填充用户缓冲区直到 count=0  │ │
│ │ 每次填充 CHACHA20_BLOCK_SIZE    │ │
│ │ 使用 ChaCha20 加密空块生成随机数 │ │
│ │ copy_to_user() 拷贝到用户空间   │ │
│ └─────────────────────────────────┘ │
└─────────────────────────────────────┘
    │
    v
返回实际写入的字节数
```

## 6. 错误处理

| 错误码 | 含义 | 触发条件 |
|--------|------|----------|
| `-EINVAL` | 无效参数 | flags 包含未知位，或同时设置了 GRND_INSECURE 和 GRND_RANDOM |
| `-EAGAIN` | 重试 | CRNG 未就绪且设置了 GRND_NONBLOCK |
| `-ERESTARTSYS` | 信号中断 | 等待 CRNG 初始化时被信号中断 |
| `-EFAULT` | 内存错误 | ubuf 指向不可写地址 |

## 7. 使用示例

```c
#include <sys/random.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define KEY_SIZE 32

int main(void)
{
    unsigned char key[KEY_SIZE];
    ssize_t ret;

    /* 获取 32 字节密码学安全随机数 */
    ret = getrandom(key, sizeof(key), 0);
    if (ret != sizeof(key)) {
        perror("getrandom");
        exit(EXIT_FAILURE);
    }

    printf("Generated %zd bytes of random data:\n", ret);
    for (size_t i = 0; i < ret; i++)
        printf("%02x", key[i]);
    printf("\n");

    /* 非阻塞尝试 */
    unsigned char buf[16];
    ret = getrandom(buf, sizeof(buf), GRND_NONBLOCK);
    if (ret == -1 && errno == EAGAIN) {
        printf("CRNG not ready yet, try again later\n");
    }

    /* 不安全的随机数（CRNG 未初始化也返回） */
    unsigned char insecure[8];
    ret = getrandom(insecure, sizeof(insecure), GRND_INSECURE);
    if (ret == sizeof(insecure)) {
        printf("Got %zd bytes (possibly low entropy)\n", ret);
    }

    return 0;
}
```

## 8. 参考

- 源码位置：`drivers/char/random.c`
- 头文件：`include/uapi/linux/random.h`（GRND_* 标志定义）
- [ARM64 系统调用表](../arm64-syscall-table.md#权限与安全)