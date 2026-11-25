#!/usr/bin/env python3
"""Generate notes/SUMMARY.md for the mdBook from the repository README.md.

Usage:
    python3 notes/00-overview/tools/gen_summary.py

The root README.md remains the GitHub landing page; this script only builds the
mdBook table of contents from its section/link structure.

Layout of the generated summary:
    # N. 章          -> mdBook part title (sidebar section header)
    - [N.x 节]()     -> level-1 chapter without a page, with nested children
        - [页](...)  -> level-2 chapter (folded by `output.html.fold.level = 1`)
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
README = (ROOT / 'README.md').read_text()
LINK_RE = re.compile(r'\[([^\]]+)\]\(([^)]+)\)')

# 11.6 SSD 专题中已并入主书的原理类分组（目录已扁平化，无嵌套 SUMMARY.md）
SSD_BOOKS = [
    ('nand原理', [
        ('简介', 'README.md'),
        ('闪存物理特性', 'physical.md'),
        ('闪存内部结构原理', 'arch.md'),
        ('命令', 'cmd.md'),
        ('FTL', 'ftl.md'),
    ]),
    ('nvme原理', [
        ('简介', 'intro.md'),
        ('nvme设备基本原理', 'arch.md'),
        ('nvme数据结构定义', 'define.md'),
        ('nvme命令', 'cmd.md'),
    ]),
]

seen = set()
lines = [
    '# Summary', '',
    '[简介](00-overview/kernel-map.md)', '',
    '# 0. 总览', '',
    '- [编译快照与文件统计](00-overview/analysis_report.md)', '',
]
seen.update({'00-overview/kernel-map.md', '00-overview/analysis_report.md'})

# 当前节（无页面章节）在 lines 中的下标；None 表示当前不在节内
group_index = None
# 当前节的子项缩进（根级 0，节内 4，子书内 8）
cur_indent = 0


def add(title: str, rel: str) -> bool:
    rel = rel.split('#')[0]
    if not rel.endswith('.md') or rel in seen or Path(rel).name == 'SUMMARY.md':
        return False
    f = ROOT / 'notes' / rel
    if not f.exists() or f.stat().st_size == 0:
        return False
    seen.add(rel)
    lines.append(' ' * cur_indent + f'- [{title}]({rel})')
    return True


def flush_group() -> None:
    """Drop the pending group item if it ended up with no children."""
    global group_index
    if group_index is not None:
        if len(lines) == group_index + 1:
            lines.pop()
        group_index = None


def part(title: str) -> None:
    global cur_indent
    flush_group()
    cur_indent = 0
    lines.extend(['', f'# {title}', ''])


def start_group(title: str, indent: int) -> None:
    global group_index, cur_indent
    flush_group()
    group_index = len(lines)
    lines.append(' ' * indent + f'- [{title}]()')
    cur_indent = indent + 4


def emit_ssd() -> None:
    """Emit 11.6 SSD 专题：阅读说明 + NAND Flash + 已扁平化的 nand/nvme 原理分组。

    原子书（lightnvm代码分析/zns）已删除；nand原理/nvme原理 的页面与图片
    已从 src/ 上移一层并入主书，不再存在嵌套 SUMMARY.md。
    """
    add('SSD 笔记阅读说明', '04-storage/ssd/README.md')
    add('NAND Flash 解密', '04-storage/ssd/nand-flash.md')
    sub_indent = cur_indent
    for book, pages in SSD_BOOKS:
        start_group(book, sub_indent)
        for title, fn in pages:
            add(title, f'04-storage/ssd/{book}/{fn}')
    flush_group()


skip_until_h2 = False
skip_section = False
ssd_done = False
for raw in README.splitlines():
    line = raw.rstrip()
    if line.startswith('## '):
        title = line[3:].strip()
        skip_until_h2 = title in ('快速入口', '目录', '文档目录结构') or title.startswith('附录')
        skip_section = False
        if not skip_until_h2:
            part(title)
        continue
    if skip_until_h2:
        continue
    if line.startswith('### '):
        title = line[4:].strip()
        if title == '11.6 SSD 专题':
            start_group(title, 0)
            emit_ssd()
            ssd_done = True
            skip_section = True
            continue
        skip_section = False
        start_group(title, 0)
        continue
    if skip_section or '](' not in line:
        continue
    for m in LINK_RE.finditer(line):
        title, target = m.group(1).strip(), m.group(2).strip().split('#')[0]
        if not target.startswith('notes/') or '/ssd/' in target:
            continue
        add(title, target[len('notes/'):])

flush_group()
if not ssd_done:
    emit_ssd()

left = []
for f in sorted((ROOT / 'notes').rglob('*.md')):
    rel = str(f.relative_to(ROOT / 'notes'))
    if rel in seen or rel == 'SUMMARY.md' or rel.startswith('00-overview/tools/'):
        continue
    if f.name == 'SUMMARY.md' or f.stat().st_size == 0:
        continue
    left.append((f.stem, rel))
if left:
    part('附录：未分类')
    for title, rel in left:
        add(title, rel)

lines.append('')
(ROOT / 'notes/SUMMARY.md').write_text('\n'.join(lines))
print(f'chapters: {len(seen)}, summary lines: {len(lines)}, fallback: {len(left)}')
