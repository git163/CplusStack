#ifndef SWP_STACK_TRACE_H_
#define SWP_STACK_TRACE_H_

#include <unistd.h>

namespace swp_stack_trace {

// 以异步安全方式将当前调用栈写入 fd。
// 默认输出到 STDERR_FILENO。
// 若 fd 无效或写入失败，静默失败（noexcept）。
void print_stacktrace(int fd = STDERR_FILENO) noexcept;

} // namespace swp_stack_trace

#endif // SWP_STACK_TRACE_H_
