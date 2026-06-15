#!/bin/bash
# 运行 swp_stack_trace 单元测试（先编译后执行）

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

need_build=false
if [ ! -d "$BUILD_DIR" ]; then
    need_build=true
elif [ ! -x "$BUILD_DIR/tests/unit_tests" ]; then
    # 已有 build 目录但缺少测试目标，可能是用 BUILD_TESTING=OFF 配置的，需要重新配置
    need_build=true
    rm -rf "$BUILD_DIR"
fi

if [ "$need_build" = true ]; then
    echo "Building unit tests..."
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR"
    cmake --build "$BUILD_DIR" --target unit_tests -j
    if [ $? -ne 0 ]; then
        echo "Build failed"
        exit 1
    fi
    echo "Build finished"
fi

echo "Running unit tests..."
ctest --test-dir "$BUILD_DIR" --output-on-failure
