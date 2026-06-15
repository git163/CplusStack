#!/bin/bash
# 运行 crash_demo 示例（先编译后执行）
# crash_demo 会故意触发崩溃信号并打印堆栈，退出码非 0 是预期行为。

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
DEMO_BIN="$BUILD_DIR/src/example/crash_demo"

need_build=false
if [ ! -d "$BUILD_DIR" ]; then
    need_build=true
elif [ ! -x "$DEMO_BIN" ]; then
    need_build=true
fi

if [ "$need_build" = true ]; then
    echo "Building crash_demo..."
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -DBUILD_TESTING=OFF
    cmake --build "$BUILD_DIR" --target crash_demo -j
    if [ $? -ne 0 ]; then
        echo "Build failed"
        exit 1
    fi
    echo "Build finished"
fi

CRASH_TYPE="${1:-sigsegv}"
echo "Running crash_demo with crash type: $CRASH_TYPE (it will crash intentionally)..."
"$DEMO_BIN" "$@"
exit_code=$?

echo ""
echo "crash_demo exited with code $exit_code (non-zero is expected due to signal)"