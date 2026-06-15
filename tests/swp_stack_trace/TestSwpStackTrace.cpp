// swp_stack_trace 模块的冒烟测试：验证 print_stacktrace 可被正常调用且不崩溃。

#include <gtest/gtest.h>
#include <unistd.h>

#include "swp_stack_trace.h"

namespace {

void call_print_stacktrace() {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);
}

} // namespace

// 基础冒烟测试：print_stacktrace 在普通调用场景下不应崩溃或抛异常。
TEST(SwpStackTrace, does_not_crash) {
    call_print_stacktrace();
    SUCCEED();
}
