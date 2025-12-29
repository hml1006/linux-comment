import os
from clang.cindex import Index, CompilationDatabase

def traverse_ast(node, depth=0):
        """
        递归遍历AST节点。
        node: 当前游标节点。
        depth: 当前深度，用于缩进显示。
        """
        # 打印当前节点的基本信息：缩进、节点类型、名称、位置
        indent = '  ' * depth
        node_name = node.spelling or node.displayname or ''  # 优先使用拼写名，其次显示名
        kind_str = str(node.kind).split('.')[-1]  # 提取枚举类型名

        # 获取源代码位置（行:列）
        loc = node.location
        pos_info = f"[{loc.line}:{loc.column}]" if loc.file else "[系统文件]"

        print(f"{indent}{kind_str}: '{node_name}' {pos_info}")

        # 2. 递归遍历所有子节点
        for child in node.get_children():
                traverse_ast(child, depth + 1)

if __name__ == "__main__":
        build_dir = "/home/louis/code/linux"
        source_file = '/home/louis/code/linux/include/linux/device.h'
        # source_file = '/home/louis/code/linux/drivers/base/core.c'
        remove = {'--', 'aarch64-linux-gnu-gcc', '-mabi=lp64', '-fno-allow-store-data-races', '-fmin-function-alignment=4', '-fconserve-stack', '-femit-struct-debug-baseonly', '-fzero-init-padding-bits=all'}
        compdb = CompilationDatabase.fromDirectory(build_dir)
        index = Index.create()
        cmds = compdb.getCompileCommands(source_file)
        print(cmds[0].arguments)
        print(os.path.basename(cmds[0].filename))
        args = [arg for arg in cmds[0].arguments]

        good_args = []
        for arg in args:
                if arg not in remove:
                        if arg.find('"') != -1:
                                arg = arg.replace('"', '\"')
                                arg = '"{}"'.format(arg)
                        if arg.find(os.path.basename(source_file)) != -1:
                                continue
                        good_args.append(arg)
        argstr = ' '.join(good_args)
        print(good_args)

        tu = index.parse(source_file, good_args) 

        traverse_ast(tu.cursor)