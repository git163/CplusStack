// swp_stack_trace 模块的冒烟测试：验证 print_stacktrace 可被正常调用且不崩溃。

#include <gtest/gtest.h>
#include <unistd.h>

#include <string>

#include "swp_stack_trace.h"

namespace {

class stderr_redirector {
public:
    explicit stderr_redirector(int pipe_write_fd) {
        saved_stderr_ = ::dup(STDERR_FILENO);
        if (saved_stderr_ < 0) {
            return;
        }
        if (::dup2(pipe_write_fd, STDERR_FILENO) < 0) {
            ::close(saved_stderr_);
            saved_stderr_ = -1;
            return;
        }
        ::close(pipe_write_fd);
    }

    ~stderr_redirector() {
        if (saved_stderr_ >= 0) {
            (void)::dup2(saved_stderr_, STDERR_FILENO);
            ::close(saved_stderr_);
        }
    }

    bool ok() const { return saved_stderr_ >= 0; }

    // 禁止拷贝
    stderr_redirector(const stderr_redirector&) = delete;
    stderr_redirector& operator=(const stderr_redirector&) = delete;

private:
    int saved_stderr_ = -1;
};

void call_print_stacktrace() {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);
}

} // namespace

// 基础冒烟测试：print_stacktrace 在普通调用场景下不应崩溃或抛异常。
TEST(SwpStackTrace, does_not_crash) {
    call_print_stacktrace();
    SUCCEED();
}

// 验证 print_stacktrace 的输出包含当前测试函数的 mangled 名子串。
TEST(SwpStackTrace, output_contains_current_function) {
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);

    {
        stderr_redirector redirect(pipefd[1]);
        ASSERT_TRUE(redirect.ok());
        swp_stack_trace::print_stacktrace(STDERR_FILENO);
    }

    // 读取捕获的输出；4096 字节足以容纳常规调用栈。
    char buffer[4096] = {};
    const ssize_t n = ::read(pipefd[0], buffer, sizeof(buffer) - 1);
    ASSERT_GE(n, 0) << "read from pipe failed";
    ::close(pipefd[0]);

    ASSERT_GT(n, 0);
    const std::string output(buffer, static_cast<std::size_t>(n));

    // mangled 名中必然包含当前测试函数名的子串
    EXPECT_NE(output.find("output_contains_current_function"), std::string::npos)
        << "Stack trace output was:\n" << output;
}
