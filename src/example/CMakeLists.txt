add_executable(crash_demo crash_demo.cpp)
target_link_libraries(crash_demo PRIVATE swp_stack_trace)

# 保留帧指针并禁用优化，便于 libunwind 在 macOS 上也能展开更完整的调用栈。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(crash_demo PRIVATE -fno-omit-frame-pointer -O0 -fstack-protector-all)
endif()
