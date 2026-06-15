#!/usr/bin/env python3
"""
离线解析 swp_stack_trace 打印的原始堆栈。

用法：
    ./run.sh 2>&1 | ./symbolize_stack.py ./build/src/example/crash_demo
    ./run_tests.sh 2>&1 | ./symbolize_stack.py ./build/tests/unit_tests

说明：
- 本工具读取标准输入中的堆栈文本，提取 PC 地址。
- 调用 addr2line / llvm-addr2line 将 PC 翻译成 file:line。
- 将翻译结果追加到原始行末尾。
- Linux 下可直接使用；macOS 上若无可用的 addr2line，会给出提示。
"""

import re
import shutil
import subprocess
import sys

STACK_RE = re.compile(r"^(#\d+\s+)(0x[0-9a-fA-F]+)(\s+.*)$")


def find_addr2line():
    """查找可用的 addr2line 工具。"""
    for name in ("addr2line", "llvm-addr2line"):
        path = shutil.which(name)
        if path:
            return path
    return None


def symbolize(binary, addr2line, lines):
    """逐行解析堆栈，返回增强后的输出列表。"""
    # 收集所有 PC 地址及其所在行信息
    entries = []
    pcs = []
    for line in lines:
        m = STACK_RE.match(line)
        if m:
            prefix, pc, suffix = m.groups()
            pcs.append(pc)
            entries.append((line, prefix, pc, suffix))
        else:
            entries.append((line, None, None, None))

    if not pcs:
        return [line.rstrip() for line, _, _, _ in entries]

    # 批量调用 addr2line
    result = subprocess.run(
        [addr2line, "-e", binary] + pcs,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"addr2line failed: {result.stderr}", file=sys.stderr)
        return [line.rstrip() for line, _, _, _ in entries]

    locations = [loc.strip() for loc in result.stdout.strip().split("\n")]

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
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <binary>", file=sys.stderr)
        print("Example: ./run.sh 2>&1 | ./symbolize_stack.py ./build/src/example/crash_demo", file=sys.stderr)
        sys.exit(1)

    binary = sys.argv[1]
    addr2line = find_addr2line()
    if not addr2line:
        print(
            "Error: neither 'addr2line' nor 'llvm-addr2line' found in PATH.\n"
            "On Linux, install binutils (e.g. apt-get install binutils).\n"
            "On macOS, you may use 'atos -o <binary> -l <load_addr> <pc>...' "
            "or generate a dSYM bundle for symbolication.",
            file=sys.stderr,
        )
        sys.exit(1)

    lines = sys.stdin.readlines()
    for line in symbolize(binary, addr2line, lines):
        print(line)


if __name__ == "__main__":
    main()
