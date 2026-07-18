#!/usr/bin/env python3
"""
Convert all Mermaid diagrams in notes/syscall to plain ASCII text.
Uses simple, readable text formats instead of complex box-drawing.
"""

import re
import os

SYS_DIR = os.path.dirname(os.path.abspath(__file__))

# ─── sequence diagram → simple text ──────────────────────────────────────

def seq_to_ascii(lines):
    """
    Convert sequenceDiagram to a simple text format:
      Participant1 -> Participant2: message
    """
    pnames = {}
    msgs = []
    in_alt = in_else = False
    alt_t = alt_tt = ""
    alt_ms = []
    els_ms = []

    for line in lines:
        s = line.strip()
        if not s or s.startswith('sequenceDiagram') or s.startswith('%%'):
            continue
        if s.startswith('Note '):
            # Note right of User: text
            nm = re.match(r'Note\s+(right|left)\s+of\s+(\S+)\s*:\s*(.*)', s)
            if nm:
                note_text = nm.group(3).strip()
                item = ('__note__', note_text)
                if in_alt:
                    (els_ms if in_else else alt_ms).append(item)
                else:
                    msgs.append(item)
            continue
        if s.startswith('participant '):
            rest = s[12:].strip()
            m = re.match(r'(\S+)\s+as\s+(.+)', rest)
            if m:
                pnames[m.group(1).strip()] = m.group(2).strip()
            else:
                pnames[rest] = rest
            continue
        if s.startswith('alt '):
            in_alt, in_else = True, False
            alt_t, alt_ms, els_ms = s[4:].strip(), [], []
            continue
        if s.startswith('else '):
            if in_alt:
                in_else = True
                alt_tt = s[5:].strip()
            continue
        if s == 'end':
            if in_alt:
                msgs.append(('__alt__', alt_t, alt_tt, list(alt_ms), list(els_ms)))
                in_alt = False
            continue

        # Use split by arrow pattern to handle no-space-around-arrow (e.g. User->>Syscall)
        parts = re.split(r'\s*(-{1,2}>{1,2})\s*', s, maxsplit=1)
        if len(parts) >= 3:
            sender_key = parts[0].strip()
            arrow_str = parts[1].strip()
            rest = parts[2].strip()
            # rest is "Receiver: text" or just "Receiver"
            if ':' in rest:
                recv_key, tx = rest.split(':', 1)
                recv_key = recv_key.strip()
                tx = tx.strip()
            else:
                recv_key = rest.strip()
                tx = ''
            sd = pnames.get(sender_key, sender_key)
            rv = pnames.get(recv_key, recv_key)
            arrow = '-->' if '--' in arrow_str else '->'
            item = (sd, arrow, rv, tx)
            if in_alt:
                (els_ms if in_else else alt_ms).append(item)
            else:
                msgs.append(item)

    if not msgs and not alt_ms and not els_ms:
        return ["[empty sequence diagram]"]

    res = []

    def fmt_msg(sd, arrow, rv, tx, indent=''):
        if tx:
            return indent + f"  {sd} {arrow} {rv}: {tx}"
        else:
            return indent + f"  {sd} {arrow} {rv}"

    def render_item(it, indent=''):
        if it[0] == '__note__':
            return indent + f"  // {it[1]} //"
        else:
            return fmt_msg(it[0], it[1], it[2], it[3], indent)

    for m in msgs:
        if m[0] == '__alt__':
            _, at, att, am, em = m
            if at:
                res.append(f"  [{at}]")
            for x in am:
                res.append(render_item(x, '  '))
            if att and em:
                res.append(f"  [{att}]")
                for x in em:
                    res.append(render_item(x, '  '))
        elif m[0] == '__note__':
            res.append(f"  // {m[1]} //")
        else:
            res.append(fmt_msg(*m))

    return res


# ─── flowchart → simple text ─────────────────────────────────────────────

def flow_to_ascii(lines):
    """
    Convert graph TB/TD/flowchart TD to a simple indented text format.
    """
    nodes = {}
    edges = []
    subgraphs = []
    cur_sg = None
    cur_sg_title = ""
    cur_sg_nodes = []

    for line in lines:
        s = line.strip()
        if s.startswith('graph ') or s.startswith('flowchart '):
            continue
        if s.startswith('subgraph '):
            m = re.match(r'subgraph\s+(\S+)\[(.*)\]', s)
            if m:
                cur_sg = m.group(1).strip()
                cur_sg_title = m.group(2).strip()
            else:
                m2 = re.match(r'subgraph\s+(.+)', s)
                if m2:
                    cur_sg = m2.group(1).strip()
                    cur_sg_title = cur_sg
            cur_sg_nodes = []
            continue
        if s == 'end':
            if cur_sg:
                subgraphs.append((cur_sg, cur_sg_title, list(cur_sg_nodes)))
                cur_sg = None
                cur_sg_nodes = []
            continue
        if not s or s.startswith('%%'):
            continue

        def extract_node(part):
            """Extract node ID and label from a string like 'A[text]' or 'B{text}' or just 'C'."""
            part = part.strip()
            nm = re.match(r'(\S+?)\s*\[([^\]]*)\]', part)
            if nm:
                nid, label = nm.group(1).strip(), nm.group(2).strip()
                nodes.setdefault(nid, (label, 'box'))
                if cur_sg and nid not in cur_sg_nodes:
                    cur_sg_nodes.append(nid)
                return nid
            nm = re.match(r'(\S+?)\s*\{([^}]*)\}', part)
            if nm:
                nid, label = nm.group(1).strip(), nm.group(2).strip()
                nodes.setdefault(nid, (label, 'diamond'))
                if cur_sg and nid not in cur_sg_nodes:
                    cur_sg_nodes.append(nid)
                return nid
            # Plain node ID
            nm = re.match(r'(\S+)', part)
            if nm:
                nid = nm.group(1).strip()
                if nid not in nodes:
                    pass  # may be defined later
                return nid
            return None

        # Check if this line is an edge: left --> right
        edge_m = re.match(r'(.+?)\s*-->\s*(.*)', s)
        if edge_m:
            f_part = edge_m.group(1).strip()
            rest = edge_m.group(2).strip()
            f = extract_node(f_part)
            if f:
                # Parse right side: optional |label| then node
                label = ''
                label_m = re.match(r'\|(.+?)\|\s*(.*)', rest)
                if label_m:
                    label = label_m.group(1).strip()
                    t_part = label_m.group(2).strip()
                else:
                    t_part = rest
                t = extract_node(t_part)
                if t:
                    edges.append((f, t, label))
            continue

        # Standalone node definition
        extract_node(s)

    if not nodes:
        return ["[empty flowchart]"]

    res = []
    processed = set()
    added_edges = set()

    def get_label(nid):
        return nodes.get(nid, (nid, 'box'))[0]

    def get_type(nid):
        return nodes.get(nid, (nid, 'box'))[1]

    # Draw subgraphs
    for sg_name, sg_title, sg_nodes in subgraphs:
        if not sg_nodes:
            continue
        res.append('')
        res.append(f"  === {sg_title} ===")
        for nid in sg_nodes:
            if nid in nodes:
                lbl = get_label(nid)
                nt = get_type(nid)
                if nt == 'diamond':
                    res.append(f"    ? {lbl}")
                else:
                    res.append(f"    - {lbl}")
                processed.add(nid)
        # Edges within subgraph
        for f, t, lbl in edges:
            if f in sg_nodes and t in sg_nodes and (f, t) not in added_edges:
                fl = get_label(f)
                tl = get_label(t)
                if lbl:
                    res.append(f"    | {lbl}")
                res.append(f"    v")
                added_edges.add((f, t))

    # Remaining nodes
    remaining = [nid for nid in nodes if nid not in processed]
    if remaining:
        targets = {e[1] for e in edges}
        roots = [n for n in remaining if n not in targets] or [remaining[0]]

        def draw(nid, depth=0, visited=None):
            if visited is None:
                visited = set()
            if nid in visited:
                return
            visited.add(nid)
            indent = '  ' * depth
            lbl = get_label(nid)
            nt = get_type(nid)
            if nt == 'diamond':
                res.append(f"{indent}  ? {lbl}")
            else:
                res.append(f"{indent}  - {lbl}")
            processed.add(nid)
            for f, t, lbl in edges:
                if f == nid and (f, t) not in added_edges:
                    if lbl:
                        res.append(f"{indent}    | {lbl}")
                    res.append(f"{indent}    v")
                    added_edges.add((f, t))
                    if t not in visited:
                        draw(t, depth + 1, visited)

        for r in roots:
            draw(r)

    # Orphan nodes
    for nid in nodes:
        if nid not in processed:
            lbl = get_label(nid)
            nt = get_type(nid)
            if nt == 'diamond':
                res.append(f"  ? {lbl}")
            else:
                res.append(f"  - {lbl}")

    return res


# ─── clean broken ASCII from first-run ──────────────────────────────────

def is_broken_ascii_block(text):
    """Detect if a plain code block contains the broken ASCII from the first run."""
    lines = text.strip().split('\n')
    if len(lines) < 2:
        return False
    # Check for the distinctive broken ASCII patterns:
    # 1. Contains ···· (multiple middle dots)
    # 2. Second line has ──── characters (separator line)
    if '····' in text:
        return True
    if '────────' in text:
        return True
    return False

def clean_file(fp):
    """Remove broken ASCII code blocks from files.
    
    Uses line-by-line parsing with state tracking to correctly identify
    plain ``` blocks (no language specifier like ```c), which is not
    possible with simple regex due to ambiguous ``` markers.
    """
    with open(fp, encoding='utf-8') as f:
        lines = f.readlines()

    out = []
    i = 0
    n = len(lines)
    modified = False
    # State: None = outside block, 'c' = inside C code block, 'plain' = inside plain block
    state = None
    # Buffer for a plain block being collected
    plain_start = None
    plain_lines = []

    while i < n:
        line = lines[i]
        stripped = line.rstrip('\n\r')

        if state is None:
            # Outside any block — look for opening markers
            if stripped == '```c':
                # Entering a C code block
                state = 'c'
                out.append(line)
                i += 1
            elif stripped == '```':
                # Entering a plain code block — start buffering
                state = 'plain'
                plain_start = i
                plain_lines = []
                i += 1
            else:
                out.append(line)
                i += 1
        elif state == 'c':
            # Inside a C code block — look for closing marker
            if stripped == '```':
                # Closing the C code block
                state = None
                out.append(line)
                i += 1
            else:
                out.append(line)
                i += 1
        elif state == 'plain':
            # Inside a plain code block — look for closing marker
            if stripped == '```':
                # Closing the plain block — check if it's broken ASCII
                content = ''.join(plain_lines)
                if is_broken_ascii_block(content):
                    # Remove the entire block
                    modified = True
                    # Don't add anything to out
                else:
                    # Keep the block as-is
                    out.append(lines[plain_start])
                    out.extend(plain_lines)
                    out.append(line)
                state = None
                plain_start = None
                plain_lines = []
                i += 1
            else:
                plain_lines.append(line)
                i += 1

    if modified:
        nc = ''.join(out)
        # Clean up excessive blank lines left by removed blocks
        nc = re.sub(r'\n{3,}', '\n\n', nc)
        with open(fp, 'w', encoding='utf-8') as f:
            f.write(nc)
        return True
    return False


# ─── main ────────────────────────────────────────────────────────────────

def convert_file(fp):
    with open(fp, encoding='utf-8') as f:
        c = f.read()
    pat = r'```mermaid\n(.*?)```'
    def repl(m):
        txt = m.group(1)
        lines = [l.rstrip() for l in txt.split('\n')]
        fl = lines[0].strip() if lines else ''
        if fl.startswith('sequenceDiagram'):
            out = seq_to_ascii(lines)
        elif fl.startswith('graph ') or fl.startswith('flowchart '):
            out = flow_to_ascii(lines)
        else:
            out = ['[unknown: ' + fl + ']']
        return '```\n' + '\n'.join(out) + '\n```'
    nc = re.sub(pat, repl, c, flags=re.DOTALL)
    if nc != c:
        with open(fp, 'w', encoding='utf-8') as f:
            f.write(nc)
        return True
    return False

def main():
    files = sorted(os.listdir(SYS_DIR))
    cnt = 0
    clean_cnt = 0
    for fn in files:
        if fn.endswith('.md') and fn not in ('convert_mermaid.py', 'arm64-syscall-table.md'):
            fp = os.path.join(SYS_DIR, fn)
            if convert_file(fp):
                print(f"  Converted: {fn}")
                cnt += 1
            if clean_file(fp):
                print(f"  Cleaned:   {fn}")
                clean_cnt += 1
    print(f"\nDone. {cnt} files converted, {clean_cnt} files cleaned.")

if __name__ == '__main__':
    main()