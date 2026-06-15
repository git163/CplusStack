#!/usr/bin/env python3
"""
离线解析 swp_stack_trace 打印的原始堆栈。

用法：
    # 默认：解析 /tmp/swp_crash.log，二进制为 ./build/src/example/crash_demo
    ./symbolize_stack.py

    # 从标准输入读取（需指定二进制路径）
    ./run.sh 2>&1 | ./symbolize_stack.py ./build/src/example/crash_demo

    # 显式指定二进制路径和日志文件
    ./symbolize_stack.py ./build/src/example/crash_demo /tmp/swp_crash.log

默认值可在脚本底部 DEFAULT_BINARY 和 DEFAULT_LOG 修改。

说明：
- Linux：调用 addr2line / llvm-addr2line 将 PC 翻译成 file:line。
- macOS：调用 atos；自动根据堆栈中的符号地址和 nm/otool 计算 ASLR slide。
"""

import platform
import re
import shutil
import subprocess
import sys

STACK_RE = re.compile(r"^(#\d+\s+)(0x[0-9a-fA-F]+)(\s+.*)$")
FUNC_OFFSET_RE = re.compile(r"(\S+)\+0x([0-9a-fA-F]+)")


def find_tool():
    """查找可用的符号化工具。macOS 优先 atos，Linux 优先 addr2line。"""
    is_darwin = platform.system() == "Darwin"

    if is_darwin:
        # macOS 上 GNU addr2line 通常无法解析 Mach-O，优先 atos
        atos_path = shutil.which("atos")
        if atos_path:
            return "atos", atos_path

    for name in ("addr2line", "llvm-addr2line"):
        path = shutil.which(name)
        if path:
            return "addr2line", path

    if not is_darwin:
        atos_path = shutil.which("atos")
        if atos_path:
            return "atos", atos_path

    return None, None


def parse_input(lines):
    """解析输入，返回堆栈条目列表和 PC 列表。"""
    entries = []
    pcs = []

    for line in lines:
        stack_match = STACK_RE.match(line)
        if stack_match:
            prefix, pc, suffix = stack_match.groups()
            pcs.append(pc)
            entries.append((line, prefix, pc, suffix))
        else:
            entries.append((line, None, None, None))

    return entries, pcs


def symbolize_addr2line(binary, tool_path, pcs, entries):
    """使用 addr2line 解析。"""
    result = subprocess.run(
        [tool_path, "-e", binary] + pcs,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"addr2line failed: {result.stderr}", file=sys.stderr)
        return [line.rstrip() for line, _, _, _ in entries]

    locations = [loc.strip() for loc in result.stdout.strip().split("\n")]
    return combine(entries, locations)


def normalize_symbol(symbol):
    """统一符号格式：macOS nm 对符号可能多一个前导下划线。"""
    return symbol.lstrip("_")


def get_symbol_address(binary, symbol_mangled):
    """用 nm 查找 mangled 符号在二进制中的地址。"""
    result = subprocess.run(
        ["nm", "-n", binary],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None

    target = normalize_symbol(symbol_mangled)
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        nm_symbol = parts[-1]
        if normalize_symbol(nm_symbol) == target:
            try:
                return int(parts[0], 16)
            except ValueError:
                continue
    return None


def get_preferred_vmaddr(binary):
    """用 otool -l 获取 __TEXT 段的 vmaddr。"""
    result = subprocess.run(
        ["otool", "-l", binary],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None

    in_text = False
    for line in result.stdout.splitlines():
        if "segname __TEXT" in line:
            in_text = True
        elif in_text and "vmaddr" in line:
            parts = line.split()
            if len(parts) >= 2:
                try:
                    return int(parts[-1], 16)
                except ValueError:
                    return None
    return None


def compute_macos_load_base(binary, entries):
    """
    根据堆栈中第一个可识别的主程序符号，计算实际加载基址。
    关系：runtime_pc = preferred_vmaddr + slide + offset_in_text + offset_in_func
         symbol_file_addr = preferred_vmaddr + offset_in_text
         load_base = preferred_vmaddr + slide
    所以：load_base = runtime_pc - offset_in_func - (symbol_file_addr - preferred_vmaddr)
    """
    preferred_vmaddr = get_preferred_vmaddr(binary)
    if preferred_vmaddr is None:
        print("Warning: failed to get preferred vmaddr from otool -l", file=sys.stderr)
        return None

    for _, _, pc, suffix in entries:
        if pc is None:
            continue
        m = FUNC_OFFSET_RE.search(suffix)
        if not m:
            continue

        func_mangled = m.group(1)
        offset_in_func = int(m.group(2), 16)
        symbol_file_addr = get_symbol_address(binary, func_mangled)
        if symbol_file_addr is None:
            continue

        runtime_pc = int(pc, 16)
        load_base = runtime_pc - offset_in_func - (symbol_file_addr - preferred_vmaddr)
        return load_base

    return None


def symbolize_atos(binary, tool_path, entries, pcs):
    """使用 atos 解析（macOS）。自动计算 load base。"""
    load_base = compute_macos_load_base(binary, entries)
    if load_base is None:
        print(
            "Error: cannot compute macOS load base automatically.\n"
            "Make sure the stack trace contains at least one function from the main binary.",
            file=sys.stderr,
        )
        return [line.rstrip() for line, _, _, _ in entries]

    result = subprocess.run(
        [tool_path, "-o", binary, "-l", hex(load_base)] + pcs,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"atos failed: {result.stderr}", file=sys.stderr)
        return [line.rstrip() for line, _, _, _ in entries]

    # atos 输出示例：
    # (anonymous namespace)::deep_call_1() (in crash_demo) (crash_demo.cpp:99)
    # 提取最后的 (file:line) 部分
    raw_locations = [loc.strip() for loc in result.stdout.strip().split("\n")]
    locations = []
    for loc in raw_locations:
        m = re.search(r"\(([^()]+:\d+)\)\s*$", loc)
        if m:
            locations.append(m.group(1))
        else:
            locations.append("??:0")

    return combine(entries, locations)


def combine(entries, locations):
    """将解析结果和原始行合并。"""
    output = []
    loc_idx = 0
    for line, prefix, pc, suffix in entries:
        if pc is None:
            output.append(line.rstrip())
            continue

        loc = locations[loc_idx] if loc_idx < len(locations) else "??:0"
        loc_idx += 1

        if loc in ("??:0", "??:?", ""):
            output.append(line.rstrip())
        else:
            output.append(f"{line.rstrip()}  at {loc}")

    return output


def main():
    DEFAULT_BINARY = "./build/src/example/crash_demo"
    DEFAULT_LOG = "/tmp/swp_crash.log"

    if len(sys.argv) > 3:
        print(f"Usage: {sys.argv[0]} [<binary> [<log_file>]]", file=sys.stderr)
        print("Examples:", file=sys.stderr)
        print("  ./symbolize_stack.py", file=sys.stderr)
        print("  ./run.sh 2>&1 | ./symbolize_stack.py ./build/src/example/crash_demo", file=sys.stderr)
        print("  ./symbolize_stack.py ./build/src/example/crash_demo /tmp/swp_crash.log", file=sys.stderr)
        sys.exit(1)

    # 无参数时使用默认二进制路径和日志路径
    if len(sys.argv) == 1:
        binary = DEFAULT_BINARY
        log_path = DEFAULT_LOG
    elif len(sys.argv) == 2:
        # 单个参数：二进制路径，从标准输入读取
        binary = sys.argv[1]
        log_path = None
    else:
        # 两个参数：二进制路径 + 日志文件
        binary = sys.argv[1]
        log_path = sys.argv[2]

    tool_type, tool_path = find_tool()
    if not tool_path:
        print(
            "Error: no symbolization tool found.\n"
            "On Linux, install binutils (e.g. apt-get install binutils).\n"
            "On macOS, 'atos' should be available with Xcode Command Line Tools.",
            file=sys.stderr,
        )
        sys.exit(1)

    if log_path:
        try:
            with open(log_path, "r") as f:
                lines = f.readlines()
        except OSError as e:
            print(f"Error: cannot read '{log_path}': {e}", file=sys.stderr)
            sys.exit(1)
    else:
        lines = sys.stdin.readlines()
    entries, pcs = parse_input(lines)

    if not pcs:
        for line, _, _, _ in entries:
            print(line.rstrip())
        return

    if tool_type == "addr2line":
        output = symbolize_addr2line(binary, tool_path, pcs, entries)
    else:
        output = symbolize_atos(binary, tool_path, entries, pcs)

    for line in output:
        print(line)


if __name__ == "__main__":
    main()
