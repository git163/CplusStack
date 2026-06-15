# tests/CMakeLists.txt — unit test entry
# 使用本地 third_party/googletest，避免 FetchContent 网络下载。
# 随项目成长，推荐拆成 tests/<模块>/CMakeLists.txt 各模块独立。

# 使用本地 GoogleTest
add_subdirectory(${CMAKE_SOURCE_DIR}/src/third_party/googletest googletest-build)

include(GoogleTest)

# Windows 下强制使用 shared CRT，避免链接错误
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

# 自动发现 tests/ 下所有 Test*.cpp（含子目录）
# CONFIGURE_DEPENDS：新增测试文件后下次 build 自动重新 configure
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "Test*.cpp")

add_executable(unit_tests ${TEST_SOURCES})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(unit_tests PRIVATE GTest::gtest GTest::gtest_main swp_stack_trace unwind)
else()
    target_link_libraries(unit_tests PRIVATE GTest::gtest GTest::gtest_main swp_stack_trace)
endif()
target_include_directories(unit_tests PRIVATE ../src/include)

# 保留帧指针并禁用优化，便于在 macOS 等平台上验证多层调用栈的展开。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(unit_tests PRIVATE -fno-omit-frame-pointer -O0)
endif()

# 让每个 TEST() 自动注册为 CTest 用例
gtest_discover_tests(unit_tests)
