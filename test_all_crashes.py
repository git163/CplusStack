#!/usr/bin/env python3
"""
遍历所有 crash_demo 用例并离线解析堆栈。

用法：
    ./test_all_crashes.py
    ./test_all_crashes.py ./build/src/example/crash_demo

默认二进制路径为 ./build/src/example/crash_demo，
日志路径为 /tmp/swp_crash.log（与 crash_demo 硬编码路径一致）。
"""

import os
import subprocess
import sys

# 可在此处修改默认值
DEFAULT_BINARY = "./build/src/example/crash_demo"
CRASH_LOG = "/tmp/swp_crash.log"
SYMBOLIZE_SCRIPT = "./symbolize_stack.py"

CRASH_TYPES = [
    "sigfpe", "sigsegv", "sigill", "sigabrt", "sigbus", "sigtrap",
    "stack_overflow", "double_free", "pure_virtual",
    "write_rodata", "stack_buf_of",
    "wild_ptr_write", "null_virtual", "use_after_free",
    "invalid_delete", "terminate", "heap_buf_of", "exec_nx",
]


def run_one(binary: str, crash_type: str) -> bool:
    """运行一次 crash_demo，返回是否成功触发（非 0 退出码即成功）。"""
    # 清空上一次的日志
    try:
        os.remove(CRASH_LOG)
    except OSError:
        pass

    result = subprocess.run(
        [binary, crash_type],
        capture_output=True,
        text=True,
    )
    # 非 0 退出码 = 成功触发信号
    return result.returncode != 0


def symbolize_one(binary: str) -> str:
    """用 symbolize_stack.py 解析当前的日志文件。"""
    if not os.path.isfile(CRASH_LOG):
        return "(no log file found)"

    result = subprocess.run(
        [sys.executable, SYMBOLIZE_SCRIPT, binary, CRASH_LOG],
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() or "(symbolization produced no output)"


def main():
    binary = DEFAULT_BINARY
    if len(sys.argv) > 1:
        binary = sys.argv[1]

    if not os.path.isfile(binary):
        print(f"Error: binary not found: {binary}", file=sys.stderr)
        sys.exit(1)
    if not os.path.isfile(SYMBOLIZE_SCRIPT):
        print(f"Error: {SYMBOLIZE_SCRIPT} not found", file=sys.stderr)
        sys.exit(1)

    print(f"Running all crash types against: {binary}")
    print(f"Log file: {CRASH_LOG}")
    print("=" * 70)

    for crash_type in CRASH_TYPES:
        print(f"\n--- {crash_type} ---")
        ok = run_one(binary, crash_type)
        if not ok:
            print(f"  WARNING: {crash_type} did NOT crash (exit code 0)")
            continue

        output = symbolize_one(binary)
        if not output or output == "(no log file found)":
            print(f"  WARNING: no log output produced")
        else:
            print(output)

    print("\n" + "=" * 70)
    print("All done.")


if __name__ == "__main__":
    main()
