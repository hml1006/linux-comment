#!/usr/bin/env python3
"""
Cross-check system call classifications between:
1. arm64-syscall-table.md (detailed table)
2. README.md (section 5)
3. Actual files on disk (subdirectories)

Report:
- Syscalls in table but missing from README
- Syscalls in README but not in table
- Syscalls in table but missing from files
- Syscalls in files but not in table
- Syscalls in wrong category in README vs table
"""

import os
import re
import sys

SYS_DIR = os.path.dirname(os.path.abspath(__file__))
TABLE_FILE = os.path.join(SYS_DIR, 'arm64-syscall-table.md')
README_FILE = os.path.join(SYS_DIR, '../../README.md')

# Category mapping: (table_section_num, readme_section_num, name, dir_name)
CATEGORIES = [
    (1,  "5.1",  "文件描述符操作", "fd-ops"),
    (2,  "5.2",  "文件 I/O", "file-io"),
    (3,  "5.3",  "文件元数据与属性", "file-meta"),
    (4,  "5.4",  "扩展属性 (xattr)", "xattr"),
    (5,  "5.5",  "文件系统挂载与结构", "filesystem-mount"),
    (6,  "5.6",  "目录与路径操作", "directory-path"),
    (7,  "5.7",  "内存管理", "memory"),
    (8,  "5.8",  "进程控制", "process"),
    (9,  "5.9",  "进程调度", "sched"),
    (10, "5.10", "进程凭证与权限", "cred-perm"),
    (11, "5.11", "信号处理", "signal"),
    (12, "5.12", "定时器与时间", "timer-time"),
    (13, "5.13", "文件与目录事件监控", "fs-notify"),
    (14, "5.14", "事件通知 (epoll)", "epoll"),
    (15, "5.15", "异步 I/O (AIO)", "aio"),
    (16, "5.16", "异步 I/O (io_uring)", "io-uring"),
    (17, "5.17", "网络与Socket", "net-socket"),
    (18, "5.18", "进程间通信 (IPC)", "ipc"),
    (19, "5.19", "POSIX 消息队列", "posix-mq"),
    (20, "5.20", "权限与安全", "security"),
    (21, "5.21", "密钥管理", "key-management"),
    (22, "5.22", "BPF 与追踪", "bpf-trace"),
    (23, "5.23", "内核模块与 kexec", "module-kexec"),
    (24, "5.24", "线程同步 (futex)", "futex"),
    (25, "5.25", "内核通知与监控", "kernel-notify"),
    (26, "5.26", "NUMA 内存策略", "numa-memory"),
    (27, "5.27", "系统标识与信息", "system-info"),
    (28, "5.28", "用户与组关系", "user-group"),
    (29, "5.29", "内存文件系统", "memfd"),
    (30, "5.30", "其他杂项", "misc"),
]

def read_file(fp):
    if not os.path.exists(fp):
        return ''
    with open(fp, encoding='utf-8') as f:
        return f.read()

def parse_detailed_table():
    """Parse the detailed syscall table to get syscalls by category."""
    content = read_file(TABLE_FILE)
    section_pattern = re.compile(r'^### (\d+)\.\s*(.+?)$', re.MULTILINE)
    syscall_pattern = re.compile(r'^\|\s*(?:\d+|—)\s*\|\s*([a-z0-9_]+)\s*\|', re.MULTILINE)
    sections = list(section_pattern.finditer(content))
    result = {}
    for i, m in enumerate(sections):
        cat_num = int(m.group(1))
        cat_name = m.group(2).strip()
        start = m.end()
        end = sections[i + 1].start() if i + 1 < len(sections) else len(content)
        section_content = content[start:end]
        syscalls = set()
        for sm in syscall_pattern.finditer(section_content):
            name = sm.group(1)
            syscalls.add(name)
        result[(cat_num, cat_name)] = syscalls

    # Also parse the summary table for syscalls referenced there
    # (for syscalls like stat, seteuid, setegid that are in summary but not in detailed)
    summary_syscalls = {}
    # Summary table pattern: | [分类名](#...) | syscall1, syscall2, ... | N |
    summary_pattern = re.compile(r'^\|\s*\[.+?\]\(#.+?\)\s*\|\s*(.+?)\s*\|\s*(\d+)\s*\|$', re.MULTILINE)
    for sm in summary_pattern.finditer(content):
        names_str = sm.group(1)
        count = int(sm.group(2))
        names = [n.strip() for n in re.split(r'[,/]\s*', names_str) if n.strip()]
        summary_syscalls[names_str] = set(names)

    return result, summary_syscalls

def parse_readme_syscalls():
    """Parse README.md section 5 to get syscalls by category."""
    content = read_file(README_FILE)
    # Find section 5
    section5_start = content.find('## 5. 系统调用')
    section5_end = content.find('## 6.', section5_start)
    if section5_start == -1:
        print("ERROR: Cannot find section 5 in README.md")
        return {}
    if section5_end == -1:
        section5_end = len(content)
    section5 = content[section5_start:section5_end]

    result = {}
    # Find each subsection: ### 5.N 分类名
    subsection_pattern = re.compile(r'^### (5\.\d+)\s+(.+?)$', re.MULTILINE)
    subsections = list(subsection_pattern.finditer(section5))

    for i, m in enumerate(subsections):
        sec_num = m.group(1)  # e.g. "5.1"
        sec_name = m.group(2).strip()
        start = m.end()
        end = subsections[i+1].start() if i+1 < len(subsections) else len(section5)
        content = section5[start:end]

        # Find all markdown links: [name](./path)
        link_pattern = re.compile(r'\[([^\]]+)\]\(\./notes/syscall/[^/]+/([^./]+)\.md\)')
        syscalls = set()
        for lm in link_pattern.finditer(content):
            # The link text and the filename should match
            name = lm.group(2)  # Use filename (without extension) as syscall name
            syscalls.add(name)

        result[(sec_num, sec_name)] = syscalls

    return result

def get_files_on_disk():
    """Get syscall files by category from disk."""
    result = {}
    for cat_num, readme_sec, cat_name, dir_name in CATEGORIES:
        dir_path = os.path.join(SYS_DIR, dir_name)
        if os.path.exists(dir_path):
            files = set()
            for f in os.listdir(dir_path):
                if f.endswith('.md'):
                    files.add(f.replace('.md', ''))
            result[(cat_num, readme_sec, cat_name, dir_name)] = files
        else:
            result[(cat_num, readme_sec, cat_name, dir_name)] = set()
    return result

def main():
    print("=" * 70)
    print("COMPREHENSIVE SYSTEM CALL CROSS-CHECK")
    print("=" * 70)

    # Parse data sources
    (table_syscalls, summary_syscalls) = parse_detailed_table()
    readme_syscalls = parse_readme_syscalls()
    disk_syscalls = get_files_on_disk()

    # Build lookup: table syscall name -> (cat_num, cat_name)
    table_lookup = {}
    for (cat_num, cat_name), syscalls in table_syscalls.items():
        for s in syscalls:
            table_lookup[s] = (cat_num, cat_name)

    # Build lookup: readme syscall name -> (sec_num, sec_name)
    readme_lookup = {}
    for (sec_num, sec_name), syscalls in readme_syscalls.items():
        for s in syscalls:
            readme_lookup[s] = (sec_num, sec_name)

    # Build lookup: disk syscall name -> (cat_num, dir_name)
    disk_lookup = {}
    for (cat_num, readme_sec, cat_name, dir_name), syscalls in disk_syscalls.items():
        for s in syscalls:
            disk_lookup[s] = (cat_num, dir_name, cat_name)

    # Collect all syscall names from all sources
    all_table = set()
    for s in table_syscalls.values():
        all_table.update(s)
    all_readme = set()
    for s in readme_syscalls.values():
        all_readme.update(s)
    all_disk = set()
    for s in disk_syscalls.values():
        all_disk.update(s)

    print(f"\nTotal syscalls in detailed table: {len(all_table)}")
    print(f"Total syscalls in README: {len(all_readme)}")
    print(f"Total syscall files on disk: {len(all_disk)}")

    # ============================================
    # CHECK 1: Syscalls in table but missing from README
    # ============================================
    print("\n" + "-" * 70)
    print("CHECK 1: Syscalls in detailed table but MISSING from README")
    print("-" * 70)
    missing_from_readme = all_table - all_readme
    if missing_from_readme:
        for s in sorted(missing_from_readme):
            cat = table_lookup.get(s, "?")
            print(f"  MISSING: {s} (table category: {cat[1]})")
    else:
        print("  NONE - all table syscalls are in README ✓")

    # ============================================
    # CHECK 2: Syscalls in README but not in table
    # ============================================
    print("\n" + "-" * 70)
    print("CHECK 2: Syscalls in README but NOT in detailed table")
    print("-" * 70)
    extra_in_readme = all_readme - all_table
    if extra_in_readme:
        for s in sorted(extra_in_readme):
            sec = readme_lookup.get(s, "?")
            print(f"  EXTRA: {s} (README section: {sec[0]} {sec[1]})")
    else:
        print("  NONE - all README syscalls are in table ✓")

    # ============================================
    # CHECK 3: Syscalls in table but missing from files on disk
    # ============================================
    print("\n" + "-" * 70)
    print("CHECK 3: Syscalls in detailed table but MISSING from files on disk")
    print("-" * 70)
    missing_from_disk = all_table - all_disk
    if missing_from_disk:
        for s in sorted(missing_from_disk):
            cat = table_lookup.get(s, "?")
            print(f"  MISSING: {s} (table category: {cat[1]})")
    else:
        print("  NONE - all table syscalls have files on disk ✓")

    # ============================================
    # CHECK 4: Syscalls in files but not in table
    # ============================================
    print("\n" + "-" * 70)
    print("CHECK 4: Syscalls in files on disk but NOT in detailed table")
    print("-" * 70)
    extra_on_disk = all_disk - all_table
    if extra_on_disk:
        for s in sorted(extra_on_disk):
            info = disk_lookup.get(s, "?")
            print(f"  EXTRA: {s} (disk directory: {info[1]}, category: {info[2]})")
    else:
        print("  NONE - all disk files correspond to table syscalls ✓")

    # ============================================
    # CHECK 5: Classification mismatch between table and README
    # ============================================
    print("\n" + "-" * 70)
    print("CHECK 5: Classification mismatch (syscall in different categories)")
    print("-" * 70)
    common = all_table & all_readme
    mismatches = []
    for s in sorted(common):
        table_cat = table_lookup.get(s)
        readme_cat = readme_lookup.get(s)
        if table_cat and readme_cat:
            # Map table section number to README section number
            table_num = table_cat[0]
            readme_sec = readme_cat[0]
            # Map: table_num 1 -> readme_sec "5.1", etc.
            expected_readme_sec = f"5.{table_num}"
            if readme_sec != expected_readme_sec:
                mismatches.append((s, table_cat, readme_cat))

    if mismatches:
        for s, t, r in mismatches:
            print(f"  MISMATCH: {s}")
            print(f"    Table: {t[0]}. {t[1]}")
            print(f"    README: {r[0]} {r[1]}")
    else:
        print("  NONE - all syscalls in correct categories ✓")

    # ============================================
    # CHECK 6: Classification mismatch between table and files on disk
    # ============================================
    print("\n" + "-" * 70)
    print("CHECK 6: Syscall file in wrong directory vs table category")
    print("-" * 70)
    disk_mismatches = []
    for s in sorted(common):
        table_cat = table_lookup.get(s)
        disk_info = disk_lookup.get(s)
        if table_cat and disk_info:
            table_num = table_cat[0]
            disk_cat_num = disk_info[0]
            if table_num != disk_cat_num:
                disk_mismatches.append((s, table_cat, disk_info))

    if disk_mismatches:
        for s, t, d in disk_mismatches:
            print(f"  MISMATCH: {s}")
            print(f"    Table: {t[0]}. {t[1]}")
            print(f"    Disk: {d[1]}/ ({d[2]})")
    else:
        print("  NONE - all files in correct directories ✓")

    # ============================================
    # CHECK 7: Summary table vs detailed table
    # ============================================
    print("\n" + "-" * 70)
    print("CHECK 7: Summary table syscalls not in detailed table")
    print("-" * 70)
    # Flatten summary syscalls
    all_summary = set()
    for key, names in summary_syscalls.items():
        all_summary.update(names)
    in_summary_not_detailed = all_summary - all_table
    if in_summary_not_detailed:
        for s in sorted(in_summary_not_detailed):
            print(f"  SUMMARY-ONLY: {s} (in summary table but no detailed entry)")
    else:
        print("  NONE - all summary syscalls have detailed entries ✓")

    # ============================================
    # CHECK 8: Per-category count comparison
    # ============================================
    print("\n" + "-" * 70)
    print("CHECK 8: Per-category count comparison")
    print("-" * 70)
    print(f"{'Category':<25} {'Table':>6} {'README':>6} {'Disk':>6}")
    print("-" * 45)
    for cat_num, readme_sec, cat_name, dir_name in CATEGORIES:
        table_count = len(table_syscalls.get((cat_num, cat_name), set()))
        readme_count = 0
        for (sec, name), syscalls in readme_syscalls.items():
            if sec == readme_sec:
                readme_count = len(syscalls)
                break
        disk_count = len(disk_syscalls.get((cat_num, readme_sec, cat_name, dir_name), set()))
        marker = ""
        if table_count != readme_count or readme_count != disk_count:
            marker = "  <<< MISMATCH"
        print(f"  {cat_name:<23} {table_count:>6} {readme_count:>6} {disk_count:>6}{marker}")

    print("\n" + "=" * 70)
    print("CHECK COMPLETE")
    print("=" * 70)

if __name__ == '__main__':
    main()