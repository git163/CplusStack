#include "swp_stack_trace.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

// 崩溃日志文件路径。
constexpr const char* k_crash_log_path = "/tmp/swp_crash.log";

// 崩溃信号 handler：将堆栈写入文件并输出到 stderr，再恢复默认 handler 并重新抛出信号，
// 以保留原有 core dump 行为。
// 注意：print_stacktrace 设计为 async-signal-safe（仅使用 write() 与 libunwind，
// 不调用 malloc/printf 等不可重入函数），因此可在信号 handler 中安全调用。
void crash_signal_handler(int sig) noexcept {
    swp_stack_trace::print_stacktrace(STDERR_FILENO);

    int log_fd = ::open(k_crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
        swp_stack_trace::print_stacktrace(log_fd);
        ::close(log_fd);
    }

    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, nullptr) != 0) {
        _exit(EXIT_FAILURE);
    }

    if (raise(sig) != 0) {
        _exit(EXIT_FAILURE);
    }
}

void install_handler(int sig, bool use_altstack = false) noexcept {
    struct sigaction sa;
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = use_altstack ? SA_ONSTACK : 0;
    if (sigaction(sig, &sa, nullptr) != 0) {
        std::perror("sigaction");
        std::exit(EXIT_FAILURE);
    }
}

// ---------- 各种崩溃触发函数 ----------

// 除零 → SIGFPE
__attribute__((noinline)) void trigger_sigfpe() {
#if defined(__x86_64__)
    unsigned long dummy = 1;
    asm volatile("xor %%rax, %%rax; div %0" : : "r"(dummy) : "rax", "rdx");
#elif defined(__arm64__) || defined(__aarch64__)
    // ARM64 上整数除零不触发 SIGFPE，使用 __builtin_trap() fallback。
    __builtin_trap();
#else
    volatile int a = 1;
    volatile int b = 0;
    volatile int c = a / b;  // NOLINT
    (void)c;
#endif
}

// 空指针解引用 → SIGSEGV
__attribute__((noinline)) void trigger_sigsegv() {
    volatile int* p = nullptr;
    *p = 42;  // NOLINT
}

// 非法指令 → SIGILL
__attribute__((noinline)) void trigger_sigill() {
#if defined(__arm64__) || defined(__aarch64__)
    asm volatile(".long 0x00000000");  // NOLINT
#elif defined(__x86_64__)
    asm volatile("ud2");  // NOLINT
#else
    __builtin_trap();
#endif
}

// abort() → SIGABRT
__attribute__((noinline)) void trigger_sigabrt() {
    volatile int a = 1;
    volatile int b = 2;
    volatile __attribute__((unused)) int c = a + b;
    std::abort();
}

// 总线错误 → SIGBUS
__attribute__((noinline)) void trigger_sigbus() {
#if defined(__x86_64__)
    // mmap 一页后 mprotect 为 PROT_NONE，写入末端触发 SIGBUS
    long page_size = ::sysconf(_SC_PAGESIZE);
    void* p = ::mmap(nullptr, static_cast<std::size_t>(page_size),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        std::abort();
    }
    ::mprotect(p, static_cast<std::size_t>(page_size), PROT_NONE);
    auto* cp = static_cast<char*>(p);
    cp[page_size - 1] = 'x';  // NOLINT
    ::munmap(p, static_cast<std::size_t>(page_size));
#else
    // ARM64 上非对齐访问更敏感，使用奇数地址大概率触发 SIGBUS
    volatile int* up = reinterpret_cast<volatile int*>(0x1);
    *up = 42;  // NOLINT
#endif
}

// 断点陷阱 → SIGTRAP
__attribute__((noinline)) void trigger_sigtrap() {
    __builtin_trap();
}

// ---------- 工业生产级崩溃场景 ----------

// 栈溢出：无限递归耗尽栈空间 → SIGSEGV
// 需要 sigaltstack 分配备用信号栈，否则 handler 本身因栈耗尽无法执行。
__attribute__((noinline)) void trigger_stack_overflow() {
    volatile char buf[1024];  // NOLINT — 每次递归在栈上分配 1KB，快速耗尽栈
    buf[0] = 'x';
    trigger_stack_overflow();
}

// 重复释放：同一指针 free 两次 → SIGABRT（需 glibc/jemalloc 检测）
__attribute__((noinline)) void trigger_double_free() {
    void* p = ::malloc(64);
    if (p == nullptr) {
        std::abort();
    }
    ::free(p);
    ::free(p);  // NOLINT — double free，allocator 检测后 abort()
}

// 纯虚函数调用：在构造函数/析构函数中调用纯虚函数 → SIGABRT
// 工业场景中常见于多态对象生命周期管理不当。
namespace {
class pure_virtual_base {
public:
    pure_virtual_base() {
        // 构造期间 vtable 尚未指向派生类 → 调用纯虚函数 → __cxa_pure_virtual / abort
        pure_virtual_call();
    }
    virtual void pure_virtual_call() = 0;
    virtual ~pure_virtual_base() = default;
};

class pure_virtual_derived : public pure_virtual_base {
public:
    void pure_virtual_call() override {}
};
}  // namespace

__attribute__((noinline)) void trigger_pure_virtual() {
    pure_virtual_derived obj;
    (void)obj;
}

// 只读内存写入：向 .rodata 段写入 → SIGSEGV
__attribute__((noinline)) void trigger_write_rodata() {
    // 字符串字面量位于 .rodata（只读数据段），写入触发 SIGSEGV
    char* s = const_cast<char*>("immutable string");  // NOLINT
    s[0] = 'X';  // NOLINT
}

// 栈缓冲区溢出：溢出触发栈 canary → SIGABRT（__stack_chk_fail）
// 需要 -fstack-protector-strong 编译选项。
__attribute__((noinline)) void trigger_stack_buf_overflow() {
    char buf[16];
    // 写入远超 buf 大小，覆盖栈上的 canary → __stack_chk_fail → SIGABRT
    for (int i = 0; i < 4096; ++i) {
        buf[i] = 'A';  // NOLINT
    }
}

// ---------- 多层调用封装 ----------

// 在 crash_level_3 → crash_level_2 → crash_level_1 层叠下触发目标信号，
// 便于观察较深的调用栈。
typedef void (*trigger_fn_t)();
trigger_fn_t g_trigger_fn = nullptr;

__attribute__((noinline)) void crash_level_1() {
    g_trigger_fn();
}

__attribute__((noinline)) void crash_level_2() {
    crash_level_1();
}

__attribute__((noinline)) void crash_level_3() {
    crash_level_2();
}

// ---------- 普通堆栈打印 ----------

__attribute__((noinline)) void deep_call_1() {
    std::cerr << "\nStack trace from deep_call_1:" << std::endl;
    swp_stack_trace::print_stacktrace(STDERR_FILENO);
}

__attribute__((noinline)) void deep_call_2() {
    deep_call_1();
}

__attribute__((noinline)) void deep_call_3() {
    deep_call_2();
}

__attribute__((noinline)) void deep_call_4() {
    deep_call_3();
}

// ---------- 使用说明 ----------

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <crash_type>" << std::endl;
    std::cerr << "  crash types:" << std::endl;
    std::cerr << "    sigfpe          division by zero" << std::endl;
    std::cerr << "    sigsegv         null pointer dereference" << std::endl;
    std::cerr << "    sigill          illegal instruction" << std::endl;
    std::cerr << "    sigabrt         abort()" << std::endl;
    std::cerr << "    sigbus          bus error (mmap boundary / misaligned)" << std::endl;
    std::cerr << "    sigtrap         breakpoint trap" << std::endl;
    std::cerr << "    stack_overflow  infinite recursion (stack exhaustion)" << std::endl;
    std::cerr << "    double_free     double free (heap corruption)" << std::endl;
    std::cerr << "    pure_virtual    pure virtual call in ctor" << std::endl;
    std::cerr << "    write_rodata    write to read-only .rodata" << std::endl;
    std::cerr << "    stack_buf_of    stack buffer overflow (canary fail)" << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            print_usage(argc > 0 ? argv[0] : "crash_demo");
            return EXIT_FAILURE;
        }

        const char* crash_type = argv[1];

        // 根据参数选择触发函数
        if (std::strcmp(crash_type, "sigfpe") == 0) {
            g_trigger_fn = trigger_sigfpe;
        } else if (std::strcmp(crash_type, "sigsegv") == 0) {
            g_trigger_fn = trigger_sigsegv;
        } else if (std::strcmp(crash_type, "sigill") == 0) {
            g_trigger_fn = trigger_sigill;
        } else if (std::strcmp(crash_type, "sigabrt") == 0) {
            g_trigger_fn = trigger_sigabrt;
        } else if (std::strcmp(crash_type, "sigbus") == 0) {
            g_trigger_fn = trigger_sigbus;
        } else if (std::strcmp(crash_type, "sigtrap") == 0) {
            g_trigger_fn = trigger_sigtrap;
        } else if (std::strcmp(crash_type, "stack_overflow") == 0) {
            g_trigger_fn = trigger_stack_overflow;
        } else if (std::strcmp(crash_type, "double_free") == 0) {
            g_trigger_fn = trigger_double_free;
        } else if (std::strcmp(crash_type, "pure_virtual") == 0) {
            g_trigger_fn = trigger_pure_virtual;
        } else if (std::strcmp(crash_type, "write_rodata") == 0) {
            g_trigger_fn = trigger_write_rodata;
        } else if (std::strcmp(crash_type, "stack_buf_of") == 0) {
            g_trigger_fn = trigger_stack_buf_overflow;
        } else {
            std::cerr << "Unknown crash type: " << crash_type << std::endl;
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        // 安装全部信号 handler。
        // 栈溢出场景需要 SA_ONSTACK + sigaltstack，否则 handler 无法执行。
        bool need_altstack = (std::strcmp(crash_type, "stack_overflow") == 0);
        install_handler(SIGFPE);
        install_handler(SIGSEGV, need_altstack);
        install_handler(SIGILL);
        install_handler(SIGABRT);
        install_handler(SIGBUS);
        install_handler(SIGTRAP);

        // 配置备用信号栈，仅用于 handler 执行，正常程序不使用。
        if (need_altstack) {
            static char alt_stack[SIGSTKSZ];
            stack_t ss;
            ss.ss_sp = alt_stack;
            ss.ss_size = sizeof(alt_stack);
            ss.ss_flags = 0;
            if (sigaltstack(&ss, nullptr) != 0) {
                std::perror("sigaltstack");
                return EXIT_FAILURE;
            }
        }

        // 先打印一次正常深度调用栈
        std::cerr << "Printing stack trace from a deep call stack..." << std::endl;
        deep_call_4();

        // 然后触发指定类型的崩溃
        std::cerr << "\nAbout to crash with signal type: " << crash_type << "..." << std::endl;
        crash_level_3();
    } catch (const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "unknown unexpected exception" << std::endl;
        return EXIT_FAILURE;
    }

    return 0;
}
