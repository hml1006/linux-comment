# fchmodat2 系统调用

## 原理与功能

`fchmodat2` 是 Linux 6.6 引入的 `fchmodat` 扩展版本，允许在 `flags` 参数中传递更多标志位。原 `fchmodat` 的 `flags` 参数在旧版本内核中被忽略（历史原因），而 `fchmodat2` 正确实现了所有标志。

### 功能说明

- 正确支持 `AT_SYMLINK_NOFOLLOW` 标志
- 正确支持 `AT_EMPTY_PATH` 标志
- 为未来扩展预留更多标志位支持
- 与 `fchmodat` 共享同一个 `do_fchmodat` 实现

### 与 fchmodat 的区别

| 特性 | `fchmodat` | `fchmodat2` |
|------|-----------|-------------|
| flags 参数 | 旧版本被忽略 | 正确实现 |
| `AT_SYMLINK_NOFOLLOW` | 可能无效（取决于内核版本） | 始终有效 |
| `AT_EMPTY_PATH` | 可能无效（取决于内核版本） | 始终有效 |
| 引入内核版本 | 2.6.16 | 6.6 |

## 使用场景

- 需要确保 `AT_SYMLINK_NOFOLLOW` 生效的权限修改
- 需要 `AT_EMPTY_PATH` 标志的文件描述符操作
- 新代码中替代 `fchmodat` 的更安全选择

## API 及使用案例

### 函数原型

```c
#include <fcntl.h>
#include <sys/stat.h>

int fchmodat2(int dirfd, const char *pathname, mode_t mode, int flags);
```

### 参数说明

| 参数 | 类型 | 说明 |
|------|------|------|
| `dirfd` | `int` | 目录 fd |
| `pathname` | `const char*` | 文件路径 |
| `mode` | `mode_t` | 新权限模式 |
| `flags` | `int` | `AT_SYMLINK_NOFOLLOW`、`AT_EMPTY_PATH` |

### 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>

int main() {
    // 正确支持 AT_SYMLINK_NOFOLLOW
    if (fchmodat2(AT_FDCWD, "symlink", 0600, AT_SYMLINK_NOFOLLOW) == -1) {
        perror("fchmodat2");
        return 1;
    }
    printf("符号链接权限已修改（不跟随链接）\n");

    // 使用 AT_EMPTY_PATH 通过 fd 操作
    int fd = open("example.txt", O_RDONLY);
    if (fd >= 0) {
        if (fchmodat2(fd, "", 0644, AT_EMPTY_PATH) == 0)
            printf("通过 fd 成功修改权限\n");
        close(fd);
    }

    return 0;
}
```

## 执行流程

```
fchmodat2(dirfd, pathname, mode, flags)
  └─ syscall(__NR_fchmodat2, dirfd, pathname, mode, flags)
       └─ __arm64_sys_fchmodat2()
            └─ do_fchmodat(dfd, filename, mode, flags)  // fs/open.c:670
                 ├─ 验证 flags 合法性:
                 │    (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)
                 ├─ 选择 lookup_flags:
                 │    └─ flags & AT_SYMLINK_NOFOLLOW ? 0 : LOOKUP_FOLLOW
                 ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 │    └─ 如果 flags & AT_EMPTY_PATH 且 pathname 为空:
                 │         └─ 直接使用 dirfd 对应的 file 对象
                 └─ chmod_common(&path, mode)            // fs/open.c:621
```

## 函数调用栈

```
fchmodat2(dirfd, pathname, mode, flags)
  └─ syscall(__NR_fchmodat2, dirfd, pathname, mode, flags)
       └─ __arm64_sys_fchmodat2()
            └─ do_fchmodat(dfd, filename, mode, flags)  // fs/open.c:670
                 ├─ 验证 flags 合法性
                 ├─ filename_lookup(dfd, name, lookup_flags, &path, NULL)
                 └─ chmod_common(&path, mode)            // fs/open.c:621
                      └─ notify_change() → ext4_setattr()
```

## 关键数据结构

```c
// include/uapi/linux/fcntl.h
#define AT_SYMLINK_NOFOLLOW 0x100   // 不跟随符号链接
#define AT_EMPTY_PATH       0x1000  // 允许通过 fd 操作空路径
```

## 备注

- ARM64 系统调用号为 #452
- Linux 6.6 引入
- 修复了 `fchmodat` 中 flags 参数被忽略的问题
- 推荐在新代码中使用 `fchmodat2` 替代 `fchmodat`
- 与 `fchmodat` 共享同一个内核实现 `do_fchmodat`

## 参考

- 内核源码: `fs/open.c` (`SYSCALL_DEFINE4(fchmodat2)`, `do_fchmodat`, `chmod_common`)
- `include/uapi/linux/fcntl.h` — `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`