// swp_stack_trace 模块的冒烟测试：验证 print_stacktrace 可被正常调用且不崩溃。

#include <gtest/gtest.h>
#include <csignal>
#include <unistd.h>

#include <string>

#include "swp_stack_trace.h"

namespace {

// RAII 包装文件描述符，确保异常/断言失败路径上也能关闭 fd。
class unique_fd {
public:
    explicit unique_fd(int fd) noexcept : fd_(fd) {}

    ~unique_fd() {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
    }

    // 禁止拷贝
    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    // 允许移动
    unique_fd(unique_fd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    bool ok() const noexcept { return fd_ >= 0; }

    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset() noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// RAII 包装 stderr 重定向，析构时自动恢复原始 stderr。
class stderr_redirector {
public:
    explicit stderr_redirector(int pipe_write_fd) {
        saved_stderr_ = ::dup(STDERR_FILENO);
        if (saved_stderr_ < 0) {
            return;
        }
        if (::dup2(pipe_write_fd, STDERR_FILENO) < 0) {
            (void)::close(saved_stderr_);
            saved_stderr_ = -1;
            return;
        }
        (void)::close(pipe_write_fd);
    }

    ~stderr_redirector() {
        if (saved_stderr_ >= 0) {
            (void)::dup2(saved_stderr_, STDERR_FILENO);
            (void)::close(saved_stderr_);
        }
    }

    bool ok() const noexcept { return saved_stderr_ >= 0; }

    stderr_redirector(const stderr_redirector&) = delete;
    stderr_redirector& operator=(const stderr_redirector&) = delete;

private:
    int saved_stderr_ = -1;
};

// RAII 包装 sigaction 恢复，析构时自动恢复旧信号 handler。
class sigaction_restorer {
public:
    sigaction_restorer(int sig, const struct sigaction& old_sa) noexcept
        : sig_(sig), old_sa_(old_sa) {}

    ~sigaction_restorer() {
        (void)::sigaction(sig_, &old_sa_, nullptr);
    }

    sigaction_restorer(const sigaction_restorer&) = delete;
    sigaction_restorer& operator=(const sigaction_restorer&) = delete;

private:
    int sig_;
    struct sigaction old_sa_;
};

constexpr std::size_t k_stack_trace_buffer_size = 4096;

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
    int raw_pipefd[2];
    ASSERT_EQ(::pipe(raw_pipefd), 0);

    unique_fd pipe_read(raw_pipefd[0]);
    unique_fd pipe_write(raw_pipefd[1]);

    {
        stderr_redirector redirect(pipe_write.release());
        ASSERT_TRUE(redirect.ok());
        swp_stack_trace::print_stacktrace(STDERR_FILENO);
    }

    // 读取捕获的输出；k_stack_trace_buffer_size 足以容纳常规调用栈。
    char buffer[k_stack_trace_buffer_size] = {};
    const ssize_t n = ::read(pipe_read.get(), buffer, sizeof(buffer) - 1);
    ASSERT_GE(n, 0) << "read from pipe failed";

    ASSERT_GT(n, 0);
    const std::string output(buffer, static_cast<std::size_t>(n));

    // mangled 名中必然包含当前测试函数名的子串
    EXPECT_NE(output.find("output_contains_current_function"), std::string::npos)
        << "Stack trace output was:\n" << output;
}

// 全局 pipe fd，供信号 handler 写入。
// 信号 handler 无法接收额外上下文，因此使用静态存储期变量。
int signal_test_pipe_fd = -1;

void signal_test_handler(int /*sig*/) {
    // print_stacktrace 设计为 async-signal-safe，可在信号 handler 中调用。
    swp_stack_trace::print_stacktrace(signal_test_pipe_fd);
}

// 验证 print_stacktrace 在信号 handler 上下文中仍能正常工作。
TEST(SwpStackTrace, works_from_signal_handler) {
    int raw_pipefd[2];
    ASSERT_EQ(::pipe(raw_pipefd), 0);

    unique_fd pipe_read(raw_pipefd[0]);
    unique_fd pipe_write(raw_pipefd[1]);

    signal_test_pipe_fd = pipe_write.get();

    struct sigaction old_sa;
    struct sigaction sa;
    sa.sa_handler = signal_test_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ASSERT_EQ(::sigaction(SIGUSR1, &sa, &old_sa), 0);

    // 即使 raise 或 handler 中发生异常，析构时也会恢复旧 handler。
    sigaction_restorer restorer(SIGUSR1, old_sa);

    ::raise(SIGUSR1);

    // 关闭写端，确保 read 能返回 EOF。
    pipe_write.reset();

    // 读取捕获的输出；k_stack_trace_buffer_size 足以容纳常规调用栈。
    char buffer[k_stack_trace_buffer_size] = {};
    const ssize_t n = ::read(pipe_read.get(), buffer, sizeof(buffer) - 1);
    ASSERT_GE(n, 0) << "read from pipe failed";

    ASSERT_GT(n, 0);
    const std::string output(buffer, static_cast<std::size_t>(n));

    // mangled 名中必然包含当前测试函数名的子串
    EXPECT_NE(output.find("works_from_signal_handler"), std::string::npos)
        << "Stack trace from signal handler was:\n" << output;
}
