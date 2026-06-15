#!/usr/bin/env python3
"""
遍历所有 crash_demo 用例并离线解析堆栈。

用法：
    ./test_all_crashes.py                           # 清空编译 → 测试全部
    ./test_all_crashes.py --no-clean                 # 不清空，增量编译
    ./test_all_crashes.py --no-build                 # 跳过编译（已有二进制时）
    ./test_all_crashes.py ./build/src/example/crash_demo --no-build

默认行为：先删除 build 目录，重新 cmake 配置编译，再跑全部 18 种测试。
"""

import os
import shutil
import subprocess
import sys

# 可在此处修改默认值
DEFAULT_BINARY = "./build/src/example/crash_demo"
DEFAULT_BUILD_DIR = "./build"
CRASH_LOG = "/tmp/swp_crash.log"
SYMBOLIZE_SCRIPT = "./symbolize_stack.py"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

CRASH_TYPES = [
    "sigfpe", "sigsegv", "sigill", "sigabrt", "sigbus", "sigtrap",
    "stack_overflow", "double_free", "pure_virtual",
    "write_rodata", "stack_buf_of",
    "wild_ptr_write", "null_virtual", "use_after_free",
    "invalid_delete", "terminate", "heap_buf_of", "exec_nx",
    "sigsys", "sigquit", "sigpipe", "sigalrm",
]


def build_demo(clean: bool) -> bool:
    """编译 crash_demo。clean=True 时先删除 build 目录。"""
    build_dir = os.path.join(SCRIPT_DIR, DEFAULT_BUILD_DIR)
    if clean and os.path.isdir(build_dir):
        print("Cleaning build directory...")
        shutil.rmtree(build_dir)

    print("Building crash_demo...")
    result = subprocess.run(
        ["cmake", "-S", SCRIPT_DIR, "-B", build_dir, "-DBUILD_TESTING=OFF"],
        capture_output=False,
    )
    if result.returncode != 0:
        print("cmake configure failed", file=sys.stderr)
        return False
    result = subprocess.run(
        ["cmake", "--build", build_dir, "--target", "crash_demo", "-j"],
        capture_output=False,
    )
    if result.returncode != 0:
        print("Build failed", file=sys.stderr)
        return False
    print("Build finished\n")
    return True


def run_one(binary: str, crash_type: str) -> bool:
    """运行一次 crash_demo，返回是否成功触发（非 0 退出码即成功）。"""
    try:
        os.remove(CRASH_LOG)
    except OSError:
        pass

    result = subprocess.run(
        [binary, crash_type],
        capture_output=True,
        text=True,
    )
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
    skip_build = "--no-build" in sys.argv
    skip_clean = "--no-clean" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if args:
        binary = args[0]

    if not os.path.isfile(SYMBOLIZE_SCRIPT):
        print(f"Error: {SYMBOLIZE_SCRIPT} not found", file=sys.stderr)
        sys.exit(1)

    if not skip_build:
        if not build_demo(clean=not skip_clean):
            sys.exit(1)

    if not os.path.isfile(binary):
        print(f"Error: binary not found: {binary}", file=sys.stderr)
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
