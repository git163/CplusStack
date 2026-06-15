cmake_minimum_required(VERSION 3.20)
project(swp_stack_trace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认使用 RelWithDebInfo：优化 + 保留调试符号
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# 调试信息：所有 build type 都保留 -g（GCC/Clang 专用）
if(NOT MSVC)
    add_compile_options(-g)
endif()

# 公共头文件
set(PUBLIC_HEADERS
    src/include/swp_stack_trace.h
)

# 静态库
add_library(${PROJECT_NAME} STATIC src/swp_stack_trace.cpp ${PUBLIC_HEADERS})
target_include_directories(${PROJECT_NAME} PUBLIC src/include)
# macOS 上 libunwind 通常由 libSystem 提供，无需显式链接；
# Linux 下需要链接独立的 libunwind。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(${PROJECT_NAME} PRIVATE unwind)
endif()

# 示例程序
add_subdirectory(src/example)

# 启用测试
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
