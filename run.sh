#!/bin/bash
# 运行 stack 主程序（先编译后执行）

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# 检查是否需要重新编译
need_build=false
if [ ! -d "$BUILD_DIR" ]; then
    need_build=true
elif [ ! -x "$BUILD_DIR/stack" ]; then
    need_build=true
fi

if [ "$need_build" = true ]; then
    echo "正在编译..."
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR"
    cmake --build "$BUILD_DIR" -j
    if [ $? -ne 0 ]; then
        echo "编译失败"
        exit 1
    fi
    echo "编译完成"
fi

"$BUILD_DIR/stack" "$@"