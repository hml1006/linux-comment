import os
import re
import sys
import json
import argparse
from clang.cindex import Index, CompilationDatabase, TokenKind, CursorKind
from typing import Dict, List, Optional, Tuple, Any
from dataclasses import dataclass, field, asdict
from enum import Enum
import argparse

struct = None
@dataclass
class FieldInfo:
    """字段信息"""
    name: str
    description: str
    indent_level: int = 0
    
    def to_dict(self) -> Dict[str, Any]:
        """转换为字典"""
        return {
            "name": self.name,
            "description": self.description,
            "indent_level": self.indent_level
        }


@dataclass
class ParsedStruct:
    """解析后的结构体信息"""
    name: str
    summary: str
    fields: List[FieldInfo]
    description: str = ""
    
    def to_dict(self) -> Dict[str, Any]:
        """转换为字典"""
        return {
            "struct_name": self.name,
            "summary": self.summary,
            "field_count": len(self.fields),
            "fields": [field.to_dict() for field in self.fields],
            "description": self.description
        }


class KernelDocParser:
    """内核文档解析器"""
    
    def __init__(self):
        # 匹配结构体定义: struct device - 基础设备结构体
        self.struct_pattern = re.compile(r'struct\s+(\w+)\s*-\s*(.+)')
        
        # 匹配字段注释: @字段名: 描述
        self.field_pattern = re.compile(r'@(\w+):\s*(.+)')
        
        # 匹配星号行: 移除开头的星号和空格
        self.star_line_pattern = re.compile(r'^\s*\*\s*(.*)$')
    
    def _clean_line(self, line: str) -> Tuple[str, bool]:
        """
        清理一行，返回内容和是否为空行
        
        Args:
            line: 原始行
            
        Returns:
            (清理后的内容, 是否为空行)
        """
        # 匹配星号开头的行
        match = self.star_line_pattern.match(line)
        if match:
            content = match.group(1).rstrip()
            # 空行是指只有星号或者星号后只有空白的行
            is_empty = not content.strip()
            return content, is_empty
        return line.strip(), not line.strip()
    
    def parse(self, comment_text: str) -> Optional[ParsedStruct]:
        """
        解析内核文档注释
        
        关键逻辑:
        1. 第一行的 "struct name - summary" 是结构体摘要
        2. 以 @ 开头的行是字段注释，字段描述可以跨多行（通过缩进判断）
        3. 详细描述从字段注释结束后的第一个非空且不以@开头的星号行开始
        """
        lines = comment_text.strip().split('\n')
        
        # 阶段1: 解析结构体名称和摘要
        struct_name = ""
        summary = ""
        fields: List[FieldInfo] = []
        description_lines: List[str] = []
        
        # 状态机
        parsing_summary = False
        parsing_fields = False
        parsing_description = False
        current_field = None
        found_description_start = False
        
        # 上一个非空行的内容（用于判断字段描述是否结束）
        last_nonempty_line = ""
        
        for i, line in enumerate(lines):
            # 跳过注释开始和结束标记
            stripped_line = line.strip()
            if stripped_line in ['/**', '*/']:
                continue
            
            # 清理当前行
            content, is_empty = self._clean_line(line)
            
            # 处理结构体定义行（第一行）
            if not struct_name and not summary and content:
                match = self.struct_pattern.search(content)
                if match:
                    struct_name = match.group(1)
                    summary = match.group(2).strip()
                    parsing_summary = True
                    continue
            
            # 阶段2: 解析字段注释
            # 字段注释总是以 @字段名: 开头
            if content.startswith('@') and ':' in content:
                # 如果是新字段开始，保存前一个字段
                if current_field:
                    fields.append(current_field)
                
                # 解析新字段
                field_match = self.field_pattern.search(content)
                if field_match:
                    field_name = field_match.group(1)
                    # 字段描述的起始部分（冒号后的内容）
                    initial_desc = field_match.group(2).strip()
                    
                    # 计算缩进级别（从行开始到星号的位置）
                    indent_match = re.match(r'^(\s*)\*', line)
                    indent_level = len(indent_match.group(1)) if indent_match else 0
                    
                    current_field = FieldInfo(
                        name=field_name,
                        description=initial_desc,
                        indent_level=indent_level
                    )
                    parsing_fields = True
                    parsing_description = False
                    found_description_start = False
                continue
            
            # 阶段3: 处理字段描述的续行
            elif current_field and parsing_fields and not parsing_description:
                # 检查当前行是否是字段描述的续行
                # 关键判断：续行应该有足够的缩进，且不是空行，且不是新字段的开始
                
                if not is_empty:
                    # 计算当前行的缩进
                    indent_match = re.match(r'^(\s*)\*', line)
                    current_indent = len(indent_match.group(1)) if indent_match else 0
                    
                    # 检查是否可能是字段描述的续行
                    is_continuation = (
                        current_indent >= current_field.indent_level and  # 缩进足够
                        not content.startswith('@') and  # 不是新字段
                        not found_description_start  # 还没找到详细描述开始
                    )
                    
                    if is_continuation:
                        # 添加到字段描述
                        if current_field.description:
                            current_field.description += "\n" + content
                        else:
                            current_field.description = content
                        last_nonempty_line = content
                    else:
                        # 这可能已经是详细描述的开始
                        # 关键检查：内容是否看起来像详细描述而不是字段描述
                        looks_like_description = (
                            len(content) > 10 and  # 有一定长度
                            not content.startswith('示例：') and  # 不是示例
                            not content.startswith('详见') and  # 不是引用
                            ':' not in content  # 不包含冒号（字段特征）
                        )
                        
                        if looks_like_description:
                            # 结束当前字段
                            fields.append(current_field)
                            current_field = None
                            parsing_fields = False
                            parsing_description = True
                            found_description_start = True
                            description_lines.append(content)
                else:
                    parsing_description = True
                continue
            
            # 阶段4: 收集详细描述
            if parsing_description or (not parsing_fields and not current_field):
                if not is_empty and not content.startswith('@'):
                    # 这是详细描述内容
                    description_lines.append(content)
                    found_description_start = True
                elif is_empty and found_description_start:
                    # 详细描述中的空行
                    description_lines.append("")
        
        # 添加最后一个字段（如果有）
        if current_field:
            fields.append(current_field)
        
        # 清理和合并详细描述
        description = ""
        if description_lines:
            # 合并段落
            paragraphs = []
            current_para = []
            
            for line in description_lines:
                if line.strip():  # 非空行
                    current_para.append(line.strip())
                elif current_para:  # 空行且当前有段落
                    paragraphs.append(" ".join(current_para))
                    current_para = []
            
            # 处理最后一个段落
            if current_para:
                paragraphs.append(" ".join(current_para))
            
            description = "\n\n".join(paragraphs)
        
        # 清理字段描述中的多余空格
        for field in fields:
            # 移除多余的空行和空格
            lines = field.description.split('\n')
            cleaned_lines = [line.strip() for line in lines if line.strip()]
            field.description = "\n".join(cleaned_lines)
        
        return ParsedStruct(
            name=struct_name,
            summary=summary,
            fields=fields,
            description=description
        )
    
    def parse_file(self, file_path: str) -> Optional[ParsedStruct]:
        """从文件解析"""
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                content = f.read()
            return self.parse(content)
        except Exception as e:
            print(f"读取文件时出错: {e}", file=sys.stderr)
            return None

class Field:
        def __init__(self, name, type):
                self.name = name
                self.type = type
                self.comment = ''
                self.line = -1
                
        def set_comment(self, comment):
                self.comment = comment

        def set_line(self, line):
                self.line = line

class Arg:
        def __init__(self, name, type):
                self.name = name
                self.type = type

class Method:
        def __init__(self, name, return_type):
                self.name = name
                self.return_type = return_type
                self.args = []
                self.comment = ''
                self.line = -1

        def set_comment(self, comment):
                self.comment = comment

        def set_line(self, line):
                self.line = line
        
        def add_arg(self, arg: Arg):
                self.args.append(arg)

class Struct:
        def __init__(self, name):
                self.name = name
                self.fields = []
                self.methods = []
                self.comment = ''
                self.line = -1
                
        def add_field(self, field):
                self.fields.append(field)
        
        def add_method(self, method):
                self.methods.append(method)
        
        def set_comment(self, comment):
                self.comment = comment
        
        def set_line(self, line):
                self.line = line

source_file_name = None
struct_name = ''
struct_found = False

def node_children_num(node):
        i = 0
        for node in node.get_children():
                kind_str = str(node.kind).split('.')[-1]
                print(f'{node.spelling} i = {i} {kind_str}')
                i += 1

def parse_field(node, depth = 0):
        if node.kind != CursorKind.FIELD_DECL:
                return
        field_type = node.type.spelling
        field_name = node.spelling
        field = Field(field_name, field_type)
        struct.add_field(field)

def struct2plantuml(struct):
        print('```plantuml')
        print('@startuml')
        print(f'Struct {struct.name} {{')
        for field in struct.fields:
                print(f'  {field.name}: {field.type}')
        print('--')
        for method in struct.methods:
                print(f'  {method.name}({", ".join([arg.name + ": " + arg.type for arg in method.args])}): {method.return_type}')
        print('}')
        print('@enduml')
        print('```')

def parse_function_arg(node, method: Method, depth = 0):
        loc = node.location
        if os.path.basename(loc.file.name) != source_file_name:
                return
        node_name = node.spelling or node.displayname or ''  # 优先使用拼写名，其次显示名
        if node_name.startswith('_'):
              return
        if node.kind == CursorKind.PARM_DECL:
                arg = Arg(node.spelling, node.type.spelling)
                method.add_arg(arg)

def parse_function(node, depth = 0):
        loc = node.location
        if os.path.basename(loc.file.name) != source_file_name:
                return
        node_name = node.spelling or node.displayname or ''  # 优先使用拼写名，其次显示名
        if node_name.startswith('_'):
              return
        method = Method(node_name, node.result_type.spelling)
        for child in node.get_children():
              parse_function_arg(child, method, depth + 1)
        if len(method.args) > 0:
              arg = method.args[0]
              if arg.type == f"struct {struct_name} *":
                      struct.add_method(method)

def parse_struct(node, depth = 0):
        if not node.is_definition():
                return
        loc = node.location
        if os.path.basename(loc.file.name) != source_file_name:
                return
        for child in node.get_children():
                parse_field(child, depth + 1)
        
def traverse_ast(node, depth=0):
        """
        递归遍历AST节点。
        node: 当前游标节点。
        depth: 当前深度，用于缩进显示。
        """
        global struct
        struct = Struct(struct_name)
        for child in node.get_children():
                if child.kind == CursorKind.STRUCT_DECL and child.spelling == struct_name:
                        parse_struct(child)
                elif child.kind == CursorKind.FUNCTION_DECL:
                        parse_function(child)
        struct2plantuml(struct)

def parse_comment_file(comment_file):
        # 解析
        with open(comment_file, 'r') as f:
                content = f.read()
                parser = KernelDocParser()
                result = parser.parse(content)
                print(result)

if __name__ == "__main__":
        parser = argparse.ArgumentParser(description='Convert Struct to Markdown plantuml')
        parser.add_argument('--directory', type=str, help='compile_commands.json directory', default='/home/louis/code/linux')
        parser.add_argument('--header', type=str, help='Header file path', default='/home/louis/code/linux/include/linux/device.h')
        parser.add_argument('--struct', type=str, help='Struct name', default='device')
        parser.add_argument('--comment', type=str, help='Linux kernel doxygen struct comment file', required=False)
        args = parser.parse_args()
        struct_name = args.struct
        source_file = args.header
        source_file_name = os.path.basename(source_file)

        remove = {'--', 'aarch64-linux-gnu-gcc', '-mabi=lp64', '-fno-allow-store-data-races',
                  '-fmin-function-alignment=4', '-fconserve-stack', '-femit-struct-debug-baseonly',
                  '-fzero-init-padding-bits=all', '-Wno-dangling-pointer', '-Wno-unterminated-string-initialization',
                  '-Wno-stringop-overflow', '-Wno-alloc-size-larger-than', '-Wimplicit-fallthrough=5',
                  '-Werror=designated-init', '-Wno-packed-not-aligned', '-Wno-stringop-truncation',
                  '-Wno-maybe-uninitialized'}
        compdb = CompilationDatabase.fromDirectory(args.directory)
        index = Index.create()
        cmds = compdb.getCompileCommands(source_file)
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

        tu = index.parse(source_file, good_args) 
        traverse_ast(tu.cursor)
        if hasattr(args, 'comment'):
                parse_comment_file(args.comment)