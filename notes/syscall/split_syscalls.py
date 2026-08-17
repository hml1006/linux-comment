#!/usr/bin/env python3
"""
Split all system calls into individual documents organized by category subdirectories.

This script:
1. Parses arm64-syscall-table.md to get all syscalls by category
2. Creates subdirectory structure for each category
3. Copies existing individual files to the appropriate subdirectory
4. Splits combined files into individual syscall files
5. Creates basic template files for syscalls without existing content
6. Generates the new README.md section 5 content
"""

import os
import re
import shutil

SYS_DIR = os.path.dirname(os.path.abspath(__file__))
TABLE_FILE = os.path.join(SYS_DIR, 'arm64-syscall-table.md')

# Category mapping: (num, name, dir_name)
CATEGORIES = [
    (1,  "文件描述符操作", "fd-ops"),
    (2,  "文件 I/O", "file-io"),
    (3,  "文件元数据与属性", "file-meta"),
    (4,  "扩展属性 (xattr)", "xattr"),
    (5,  "文件系统挂载与结构", "filesystem-mount"),
    (6,  "目录与路径操作", "directory-path"),
    (7,  "内存管理", "memory"),
    (8,  "进程控制", "process"),
    (9,  "进程调度", "sched"),
    (10, "进程凭证与权限", "cred-perm"),
    (11, "信号处理", "signal"),
    (12, "定时器与时间", "timer-time"),
    (13, "文件与目录事件监控", "fs-notify"),
    (14, "事件通知 epoll", "epoll"),
    (15, "异步 I/O (AIO)", "aio"),
    (16, "异步 I/O (io_uring)", "io-uring"),
    (17, "网络与Socket", "net-socket"),
    (18, "进程间通信 IPC", "ipc"),
    (19, "POSIX 消息队列", "posix-mq"),
    (20, "权限与安全", "security"),
    (21, "密钥管理", "key-management"),
    (22, "BPF 与追踪", "bpf-trace"),
    (23, "内核模块与 kexec", "module-kexec"),
    (24, "线程同步 (futex)", "futex"),
    (25, "内核通知与监控", "kernel-notify"),
    (26, "NUMA 内存策略", "numa-memory"),
    (27, "系统标识与信息", "system-info"),
    (28, "用户与组关系", "user-group"),
    (29, "内存文件系统", "memfd"),
    (30, "其他/杂项", "misc"),
]

# Syscalls that have existing individual files → (old_filename, new_filename)
EXISTING_FILES = {
    'close':         ('close-syscall-full-path.md', 'close.md'),
    'close_range':   ('close_range-syscall.md', 'close_range.md'),
    'dup':           ('dup-syscall.md', 'dup.md'),
    'dup3':          ('dup3-syscall.md', 'dup3.md'),
    'read':          ('read-syscall.md', 'read.md'),
    'write':         ('write-syscall.md', 'write.md'),
    'readv':         ('readv-syscall-full-path.md', 'readv.md'),
    'writev':        ('writev-syscall-full-path.md', 'writev.md'),
    'pread64':       ('pread64-syscall.md', 'pread64.md'),
    'pwrite64':      ('pwrite64-syscall.md', 'pwrite64.md'),
    'preadv':        ('preadv-pwritev-syscall.md', 'preadv.md'),
    'pwritev':       ('preadv-pwritev-syscall.md', 'pwritev.md'),
    'splice':        ('splice-syscall.md', 'splice.md'),
    'tee':           ('splice-syscall.md', 'tee.md'),
    'vmsplice':      ('splice-syscall.md', 'vmsplice.md'),
    'sendfile':      ('sendfile-copy-range-syscall.md', 'sendfile.md'),
    'copy_file_range': ('sendfile-copy-range-syscall.md', 'copy_file_range.md'),
    'openat':        ('open-syscall.md', 'open.md'),
}

# Combined files → list of syscalls they contain
# The syscall at index 0 gets the original file's content; others get extracted sections
COMBINED_FILES = {
    'stat-xattr-syscall.md': [
        'stat', 'fstat', 'newfstatat', 'statx', 'getdents64', 'faccessat', 'faccessat2',
        'chdir', 'fchdir', 'fchmod', 'fchmodat', 'fchmodat2', 'fchownat', 'fchown',
        'umask', 'linkat', 'unlinkat', 'symlinkat', 'mknodat', 'mkdirat', 'renameat',
        'renameat2', 'readlinkat', 'truncate', 'ftruncate', 'fallocate', 'file_getattr',
        'file_setattr',
        'setxattr', 'lsetxattr', 'fsetxattr', 'getxattr', 'lgetxattr', 'fgetxattr',
        'listxattr', 'llistxattr', 'flistxattr', 'removexattr', 'lremovexattr',
        'fremovexattr', 'setxattrat', 'getxattrat', 'listxattrat', 'removexattrat',
    ],
    'filesystem-mount-syscall.md': [
        'mount', 'umount2', 'pivot_root', 'chroot', 'sync', 'syncfs', 'fsync',
        'fdatasync', 'sync_file_range', 'swapon', 'swapoff', 'acct', 'quotactl',
        'quotactl_fd', 'statmount', 'listmount',
    ],
    'sync-syscall.md': [
        'sync', 'syncfs', 'fsync', 'fdatasync', 'sync_file_range',
    ],
    'directory-path-syscall.md': [
        'getcwd', 'openat', 'openat2', 'open_tree', 'open_tree_attr',
        'name_to_handle_at', 'open_by_handle_at', 'ioctl', 'flock',
    ],
    'memory-syscall-analysis.md': [
        'mmap', 'munmap', 'mremap', 'brk', 'mprotect', 'msync', 'mlock', 'munlock',
        'mlockall', 'munlockall', 'mincore', 'madvise', 'process_madvise',
        'remap_file_pages', 'pkey_mprotect', 'pkey_alloc', 'pkey_free', 'mseal',
        'map_shadow_stack',
    ],
    'numa-memory-syscall.md': [
        'mbind', 'set_mempolicy', 'get_mempolicy', 'migrate_pages', 'move_pages',
        'set_mempolicy_home_node',
    ],
    'process-syscall-analysis.md': [
        'exit', 'exit_group', 'clone', 'clone3', 'execve', 'execveat', 'wait4',
        'waitid', 'set_tid_address', 'unshare', 'getpid', 'getppid', 'gettid',
        'sysinfo', 'prctl', 'process_mrelease',
    ],
    'sched-cred-signal-syscall.md': [
        # sched
        'sched_setparam', 'sched_setscheduler', 'sched_getscheduler', 'sched_getparam',
        'sched_setaffinity', 'sched_getaffinity', 'sched_yield', 'sched_get_priority_max',
        'sched_get_priority_min', 'sched_rr_get_interval', 'sched_setattr', 'sched_getattr',
        # cred
        'setuid', 'getuid', 'setgid', 'getgid', 'seteuid', 'geteuid', 'setegid', 'getegid',
        'setreuid', 'setregid', 'setresuid', 'getresuid', 'setresgid', 'getresgid',
        'setfsuid', 'setfsgid', 'setpgid', 'getpgid', 'setsid', 'getsid', 'setgroups',
        'getgroups', 'capget', 'capset', 'personality',
        # signal
        'kill', 'tkill', 'tgkill', 'rt_sigaction', 'rt_sigprocmask', 'rt_sigpending',
        'rt_sigtimedwait', 'rt_sigqueueinfo', 'rt_sigreturn', 'rt_sigsuspend', 'sigaltstack',
        'pidfd_send_signal', 'signalfd4',
    ],
    'timer-time-syscall.md': [
        'timer_create', 'timer_settime', 'timer_gettime', 'timer_getoverrun', 'timer_delete',
        'timerfd_create', 'timerfd_settime', 'timerfd_gettime', 'clock_settime', 'clock_gettime',
        'clock_getres', 'clock_nanosleep', 'clock_adjtime', 'nanosleep', 'gettimeofday',
        'settimeofday', 'adjtimex',
    ],
    'inotify-fanotify-syscall.md': [
        'inotify_init1', 'inotify_add_watch', 'inotify_rm_watch', 'fanotify_init', 'fanotify_mark',
    ],
    'epoll-aio-io_uring-syscall.md': [
        'epoll_create1', 'epoll_ctl', 'epoll_pwait', 'epoll_pwait2', 'eventfd2',
        'io_setup', 'io_destroy', 'io_submit', 'io_cancel', 'io_getevents', 'io_pgetevents',
        'io_uring_setup', 'io_uring_enter', 'io_uring_register',
    ],
    'net-socket-syscall.md': [
        'socket', 'socketpair', 'bind', 'listen', 'accept', 'accept4', 'connect',
        'getsockname', 'getpeername', 'sendto', 'recvfrom', 'setsockopt', 'getsockopt',
        'shutdown', 'sendmsg', 'recvmsg', 'sendmmsg', 'recvmmsg',
    ],
    'ipc-syscall.md': [
        'msgget', 'msgctl', 'msgrcv', 'msgsnd', 'semget', 'semctl', 'semtimedop', 'semop',
        'shmget', 'shmctl', 'shmat', 'shmdt', 'pipe2',
    ],
    'posix-mq-syscall.md': [
        'mq_open', 'mq_unlink', 'mq_timedsend', 'mq_timedreceive', 'mq_notify', 'mq_getsetattr',
    ],
    'security-syscall.md': [
        'seccomp', 'landlock_create_ruleset', 'landlock_add_rule', 'landlock_restrict_self',
        'lsm_get_self_attr', 'lsm_set_self_attr', 'lsm_list_modules', 'getrandom',
    ],
    'key-management-syscall.md': [
        'add_key', 'request_key', 'keyctl',
    ],
    'bpf-trace-syscall.md': [
        'bpf', 'perf_event_open', 'ptrace',
    ],
    'module-kexec-syscall.md': [
        'init_module', 'finit_module', 'delete_module', 'kexec_load', 'kexec_file_load',
    ],
    'futex-syscall.md': [
        'futex', 'futex_waitv', 'futex_wake', 'futex_wait', 'futex_requeue',
        'set_robust_list', 'get_robust_list',
    ],
    'kernel-notify-syscall.md': [
        'syslog', 'reboot',
    ],
    'system-info-syscall.md': [
        'uname', 'sethostname', 'setdomainname', 'getcpu', 'times', 'cachestat', 'listns',
    ],
    'user-group-relation-syscall.md': [
        'pidfd_open', 'pidfd_getfd', 'setns', 'rseq', 'rseq_slice_yield', 'membarrier', 'kcmp',
    ],
    'memfd-syscall.md': [
        'memfd_create', 'memfd_secret',
    ],
    'misc-syscall.md': [
        'restart_syscall', 'vhangup', 'readahead', 'getitimer', 'setitimer',
        'getrlimit', 'setrlimit', 'getrusage', 'prlimit64', 'process_vm_readv', 'process_vm_writev',
    ],
}


def read_file(fp):
    """Read a file's content."""
    if not os.path.exists(fp):
        return ''
    with open(fp, encoding='utf-8') as f:
        return f.read()


def write_file(fp, content):
    """Write content to a file."""
    with open(fp, 'w', encoding='utf-8') as f:
        f.write(content)


def parse_detailed_table():
    """Parse the detailed syscall table to get syscalls by category."""
    content = read_file(TABLE_FILE)
    if not content:
        print("ERROR: Cannot read table file")
        return {}

    # Split by category sections
    # Pattern: ### N. CategoryName
    section_pattern = re.compile(r'^### (\d+)\.\s*(.+?)$', re.MULTILINE)
    syscall_pattern = re.compile(r'^\|\s*(\d+)\s*\|\s*([a-z0-9_]+)\s*\|\s*(.*?)\s*\|\s*(.*?)\s*\|$', re.MULTILINE)

    # Find all section headers
    sections = list(section_pattern.finditer(content))

    result = {}
    for i, m in enumerate(sections):
        cat_num = int(m.group(1))
        cat_name = m.group(2).strip()
        # Content between this section and the next (or end of file)
        start = m.end()
        end = sections[i + 1].start() if i + 1 < len(sections) else len(content)
        section_content = content[start:end]

        syscalls = []
        for sm in syscall_pattern.finditer(section_content):
            num = int(sm.group(1))
            name = sm.group(2)
            args = sm.group(3).strip()
            desc = sm.group(4).strip()
            syscalls.append((num, name, args, desc))

        result[(cat_num, cat_name)] = syscalls

    return result


def get_category_dir(cat_num):
    """Get directory name for a category number."""
    for num, name, dir_name in CATEGORIES:
        if num == cat_num:
            return dir_name
    return None


def create_basic_syscall(name, args, desc, cat_name, dir_path):
    """Create a basic template file for a syscall."""
    fp = os.path.join(dir_path, f'{name}.md')
    if os.path.exists(fp):
        return False

    cat_anchor = cat_name.lower().replace(' ', '-')
    # Remove parentheses for anchor
    cat_anchor = re.sub(r'[()]', '', cat_anchor)

    content = f"""# {name} 系统调用分析

## 1. 概述

{desc}

**原型：**

```c
SYSCALL_DEFINE...({name}, {args})
```

## 2. 使用场景

- 待补充

## 3. 函数调用栈

```
{name} (系统调用入口)
└── 待补充
```

## 4. 关键数据结构

```c
// 待补充
```

## 5. 流程图

```
  - 用户态调用 {name}
    v
    - 系统调用入口
    v
    - 待补充
```

## 6. 使用示例

```c
// 待补充
```

## 7. 参考

- [ARM64 系统调用表](../arm64-syscall-table.md#{cat_anchor})
"""
    write_file(fp, content)
    return True


def extract_sections(content, syscall_names):
    """Extract content sections related to specific syscalls from a combined file.
    Returns a dict of {syscall_name: extracted_content_or_None}.
    """
    result = {name: None for name in syscall_names}

    # Try to find section headers mentioning each syscall
    lines = content.split('\n')
    # Find markdown section headers (## or ### or ####)
    # that mention a syscall name
    sections_found = {}
    current_syscall = None
    current_lines = []

    for line in lines:
        # Check if this line is a section header mentioning a syscall
        matched = None
        header_match = re.match(r'^(#+)\s+(.*)', line)
        if header_match:
            header_text = header_match.group(2)
            for name in syscall_names:
                # Check if the header text mentions the syscall name
                if re.search(r'\b' + re.escape(name) + r'\b', header_text, re.IGNORECASE):
                    matched = name
                    break

        if matched:
            # Save previous section
            if current_syscall:
                sections_found[current_syscall] = '\n'.join(current_lines)
            current_syscall = matched
            current_lines = [line]
        elif current_syscall:
            current_lines.append(line)

    # Save last section
    if current_syscall:
        sections_found[current_syscall] = '\n'.join(current_lines)

    # Update result with found sections
    for name, section_content in sections_found.items():
        result[name] = section_content

    return result


def generate_readme_section(all_categories):
    """Generate the README section 5 content."""
    cat_num_to_dir = {num: d for num, name, d in CATEGORIES}

    lines = []
    lines.append("## 5. 系统调用")
    lines.append("")
    lines.append("- [系统调用分类列表](./notes/syscall/arm64-syscall-table.md)")
    lines.append("")

    for cat_num, cat_name, dir_name in CATEGORIES:
        dir_path = os.path.join(SYS_DIR, dir_name)
        if not os.path.exists(dir_path):
            continue

        files = sorted([f for f in os.listdir(dir_path) if f.endswith('.md')])
        if not files:
            continue

        # Build links
        links = []
        for f in files:
            name = f.replace('.md', '')
            links.append(f"[{name}](./notes/syscall/{dir_name}/{f})")

        # Group in rows of 5
        lines.append(f"### {cat_num}. {cat_name}")
        lines.append("")
        for i in range(0, len(links), 5):
            row = " | ".join(links[i:i+5])
            lines.append(f"- {row}")
        lines.append("")

    return '\n'.join(lines)


def main():
    print("=" * 60)
    print("Splitting system calls into individual documents")
    print("=" * 60)

    # Step 1: Parse the detailed table
    print("\n[1/6] Parsing arm64-syscall-table.md...")
    categories = parse_detailed_table()
    total_syscalls = sum(len(v) for v in categories.values())
    print(f"  Found {total_syscalls} syscalls across {len(categories)} categories")

    # Build lookup: syscall_name → (cat_num, cat_name, args, desc)
    syscall_lookup = {}
    for (cat_num, cat_name), syscalls in categories.items():
        for num, name, args, desc in syscalls:
            if name not in syscall_lookup:
                syscall_lookup[name] = (cat_num, cat_name, args, desc)

    # Step 2: Create subdirectories
    print("\n[2/6] Creating subdirectories...")
    for cat_num, cat_name, dir_name in CATEGORIES:
        dir_path = os.path.join(SYS_DIR, dir_name)
        os.makedirs(dir_path, exist_ok=True)
        # Check if the category has syscalls in the table
        table_syscalls = categories.get((cat_num, cat_name), [])
        if table_syscalls:
            print(f"  {dir_name}/ ({cat_name}) - {len(table_syscalls)} syscalls")
        else:
            print(f"  {dir_name}/ ({cat_name}) - 0 syscalls (empty)")

    # Step 3: Copy existing individual files to subdirectories
    print("\n[3/6] Copying existing individual files...")
    moved_count = 0
    for syscall_name, (src_name, dst_name) in EXISTING_FILES.items():
        info = syscall_lookup.get(syscall_name)
        if not info:
            print(f"  WARNING: {syscall_name} not found in table, skipping")
            continue
        cat_num, cat_name, args, desc = info
        dir_name = get_category_dir(cat_num)
        if not dir_name:
            print(f"  WARNING: No directory for category {cat_num}, skipping {syscall_name}")
            continue

        src = os.path.join(SYS_DIR, src_name)
        dst = os.path.join(SYS_DIR, dir_name, dst_name)
        if os.path.exists(src):
            shutil.copy2(src, dst)
            print(f"  Copied: {src_name} → {dir_name}/{dst_name}")
            moved_count += 1
        else:
            print(f"  WARNING: {src_name} not found, skipping")

    # Step 4: Split combined files into individual files
    print("\n[4/6] Processing combined files...")
    combined_files_to_delete = []
    extract_count = 0
    template_count = 0

    for combined_name, syscall_names in COMBINED_FILES.items():
        combined_path = os.path.join(SYS_DIR, combined_name)
        content = read_file(combined_path)
        if not content:
            print(f"  WARNING: {combined_name} not found, skipping")
            continue

        print(f"  Processing: {combined_name} ({len(syscall_names)} syscalls)")

        # Try to extract sections
        sections = extract_sections(content, syscall_names)

        # Track which syscalls got content from this file
        file_used = False
        for syscall_name in syscall_names:
            info = syscall_lookup.get(syscall_name)
            if not info:
                print(f"    WARNING: {syscall_name} not in table, creating placeholder")
                cat_num, cat_name = 30, "其他/杂项"
                dir_name = "misc"
                dir_path = os.path.join(SYS_DIR, dir_name)
                os.makedirs(dir_path, exist_ok=True)
                fp = os.path.join(dir_path, f'{syscall_name}.md')
                if not os.path.exists(fp):
                    write_file(fp, f"# {syscall_name} 系统调用\n\n## 概述\n\n待补充\n")
                    template_count += 1
                    print(f"    Created: misc/{syscall_name}.md (placeholder, not in table)")
                continue

            cat_num, cat_name, args, desc = info
            dir_name = get_category_dir(cat_num)
            if not dir_name:
                continue
            dir_path = os.path.join(SYS_DIR, dir_name)
            dst_file = os.path.join(dir_path, f'{syscall_name}.md')

            # Skip if already has a file from EXISTING_FILES
            if os.path.exists(dst_file):
                continue

            section_content = sections.get(syscall_name)
            if section_content:
                write_file(dst_file, section_content)
                file_used = True
                extract_count += 1
            else:
                # Create basic template
                create_basic_syscall(syscall_name, args, desc, cat_name, dir_path)
                template_count += 1

        # Mark combined file for deletion only if we extracted sections from it
        # (not if all files were created as templates)
        if file_used:
            combined_files_to_delete.append(combined_name)

    print(f"  Extracted: {extract_count} files from combined files")
    print(f"  Created templates: {template_count}")

    # Step 5: Create files for remaining syscalls (not covered by any existing file)
    print("\n[5/6] Creating files for remaining syscalls...")
    remaining_count = 0
    for (cat_num, cat_name), syscalls in categories.items():
        dir_name = get_category_dir(cat_num)
        if not dir_name:
            continue
        dir_path = os.path.join(SYS_DIR, dir_name)

        for num, name, args, desc in syscalls:
            fp = os.path.join(dir_path, f'{name}.md')
            if not os.path.exists(fp):
                create_basic_syscall(name, args, desc, cat_name, dir_path)
                remaining_count += 1
                print(f"  Created: {dir_name}/{name}.md (new)")

    if remaining_count == 0:
        print("  All syscalls already have files.")

    # Step 6: Generate the README section
    print("\n[6/6] Generating README section...")
    readme_content = generate_readme_section(categories)

    # Save to a file for manual review
    readme_output = os.path.join(SYS_DIR, 'README-section-5.md')
    write_file(readme_output, readme_content)
    print(f"  README section 5 saved to: {readme_output}")

    # Summary
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)

    total_files = 0
    for cat_num, cat_name, dir_name in CATEGORIES:
        dir_path = os.path.join(SYS_DIR, dir_name)
        if os.path.exists(dir_path):
            files = sorted([f for f in os.listdir(dir_path) if f.endswith('.md')])
            total_files += len(files)
            print(f"  {dir_name}/ ({cat_name}): {len(files)} files")

    print(f"\n  Total syscall files: {total_files}")
    print(f"  Total syscalls in table: {total_syscalls}")
    print(f"  Combined files to delete: {len(combined_files_to_delete)}")

    if combined_files_to_delete:
        print("\n  Combined files to delete:")
        for fn in combined_files_to_delete:
            print(f"    - {fn}")

    print("\nDone!")


if __name__ == '__main__':
    main()