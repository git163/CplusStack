#include "swp_stack_trace.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <stdexcept>
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
// 需要 -fstack-protector 编译选项（已在 CMakeLists.txt 中配置）。
__attribute__((noinline)) void trigger_stack_buf_overflow() {
    char buf[16];
    for (int i = 0; i < 4096; ++i) {
        buf[i] = 'A';  // NOLINT
    }
}

// 野指针写入：向不可访问的地址写入 → SIGSEGV
__attribute__((noinline)) void trigger_wild_ptr_write() {
    auto* p = reinterpret_cast<volatile int*>(0xDEADBEEF);  // NOLINT
    *p = 42;  // NOLINT
}

// 空指针虚函数调用：对 nullptr 调用虚函数 → SIGSEGV
// （vtable 查找需要通过 this 指针访问，this == nullptr → deref null）
struct null_virtual_base {
    virtual void crash_me() { volatile int x = 0; (void)x; }
};
__attribute__((noinline)) void trigger_nullptr_virtual_call() {
    null_virtual_base* p = nullptr;
    p->crash_me();  // NOLINT
}

// 释放后使用：先 free 再写入 → SIGABRT（需 allocator 检测）或 SIGSEGV
// 分配大量小对象耗尽 free list，提高 UAF 被检测到的概率。
__attribute__((noinline)) void trigger_use_after_free() {
    constexpr int n = 256;
    char* bufs[n];
    for (int i = 0; i < n; ++i) {
        bufs[i] = static_cast<char*>(::malloc(64));
        if (bufs[i] == nullptr) std::abort();
        bufs[i][0] = 'x';
    }
    for (int i = 0; i < n; ++i) {
        ::free(bufs[i]);
    }
    // 此时 bufs[0] 已被释放。再次写入，大概率触发检测。
    bufs[0][0] = 'y';  // NOLINT
}

// 非法释放：对栈上的变量调用 delete → SIGABRT
__attribute__((noinline)) void trigger_invalid_delete() {
    int stack_var = 42;
    ::free(&stack_var);  // NOLINT — freeing stack pointer
}

// 未捕获的 C++ 异常导致 std::terminate → SIGABRT
__attribute__((noinline)) void throw_from_noexcept() noexcept {
    throw std::runtime_error("exception from noexcept");  // NOLINT — triggers terminate
}

__attribute__((noinline)) void trigger_terminate_handler() {
    throw_from_noexcept();
}

// 堆上的数组越界写入：写过 heap buffer 末尾 → SIGSEGV
// 使用 1 字节分配并写满一页，确保触发内存访问越界。
__attribute__((noinline)) void trigger_heap_buf_overflow() {
    char* p = static_cast<char*>(::malloc(1));
    if (p == nullptr) std::abort();
    p[0] = 'x';
    // 写入远超分配大小，跨页边界时触发 SIGSEGV
    for (std::size_t i = 0; i < 65536; ++i) {
        p[i] = 'A';  // NOLINT
    }
    ::free(p);
}

// 非法函数调用：跳转到非可执行地址 → SIGSEGV（执行权限缺失）
__attribute__((noinline)) void trigger_exec_non_exec() {
#if defined(__x86_64__)
    long page_size = ::sysconf(_SC_PAGESIZE);
    void* p = ::mmap(nullptr, static_cast<std::size_t>(page_size),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) std::abort();
    // 写一条 ret 指令，但未设置 PROT_EXEC → 执行时 SIGSEGV
    auto* cp = static_cast<unsigned char*>(p);
    cp[0] = 0xC3;  // x86_64 ret
    ::mprotect(p, static_cast<std::size_t>(page_size), PROT_READ);
    auto (*fn)() = reinterpret_cast<void (*)()>(p);
    fn();
    ::munmap(p, static_cast<std::size_t>(page_size));
#else
    long page_size = ::sysconf(_SC_PAGESIZE);
    void* p = ::mmap(nullptr, static_cast<std::size_t>(page_size),
                     PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) std::abort();
    // ARM64: ret = 0xD65F03C0，不设 PROT_EXEC → SIGSEGV
    auto* sp = static_cast<uint32_t*>(p);
    sp[0] = 0xD65F03C0;
    ::mprotect(p, static_cast<std::size_t>(page_size), PROT_READ);
    auto (*fn)() = reinterpret_cast<void (*)()>(p);
    fn();
    ::munmap(p, static_cast<std::size_t>(page_size));
#endif
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
    std::cerr << "    stack_overflow     infinite recursion (stack exhaustion)" << std::endl;
    std::cerr << "    double_free        double free (heap corruption)" << std::endl;
    std::cerr << "    pure_virtual       pure virtual call in ctor" << std::endl;
    std::cerr << "    write_rodata       write to read-only .rodata" << std::endl;
    std::cerr << "    stack_buf_of       stack buffer overflow (canary fail)" << std::endl;
    std::cerr << "    wild_ptr_write     write to wild/invalid pointer" << std::endl;
    std::cerr << "    null_virtual       virtual call on nullptr" << std::endl;
    std::cerr << "    use_after_free     write to freed heap memory" << std::endl;
    std::cerr << "    invalid_delete     free() on stack variable" << std::endl;
    std::cerr << "    terminate          unhandled exception → terminate" << std::endl;
    std::cerr << "    heap_buf_of        heap buffer overflow" << std::endl;
    std::cerr << "    exec_nx            execute non-executable memory" << std::endl;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            print_usage(argc > 0 ? argv[0] : "crash_demo");
            return EXIT_FAILURE;
        }

        const char* crash_type = argv[1];

        // 根据参数选择触发函数（查表法，避免长 if-else 链）
        struct crash_entry {
            const char* name;
            trigger_fn_t fn;
            bool need_altstack;
        };
        const crash_entry entries[] = {
            {"sigfpe",              trigger_sigfpe,              false},
            {"sigsegv",             trigger_sigsegv,             false},
            {"sigill",              trigger_sigill,              false},
            {"sigabrt",             trigger_sigabrt,             false},
            {"sigbus",              trigger_sigbus,              false},
            {"sigtrap",             trigger_sigtrap,             false},
            {"stack_overflow",      trigger_stack_overflow,      true},
            {"double_free",         trigger_double_free,         false},
            {"pure_virtual",        trigger_pure_virtual,        false},
            {"write_rodata",        trigger_write_rodata,        false},
            {"stack_buf_of",        trigger_stack_buf_overflow,  false},
            {"wild_ptr_write",      trigger_wild_ptr_write,      false},
            {"null_virtual",        trigger_nullptr_virtual_call,false},
            {"use_after_free",      trigger_use_after_free,      false},
            {"invalid_delete",      trigger_invalid_delete,      false},
            {"terminate",           trigger_terminate_handler,   false},
            {"heap_buf_of",         trigger_heap_buf_overflow,   false},
            {"exec_nx",             trigger_exec_non_exec,       false},
        };

        bool found = false;
        bool need_altstack = false;
        for (const auto& entry : entries) {
            if (std::strcmp(crash_type, entry.name) == 0) {
                g_trigger_fn = entry.fn;
                need_altstack = entry.need_altstack;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Unknown crash type: " << crash_type << std::endl;
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        // 安装全部信号 handler。
        // 栈溢出场景需要 SA_ONSTACK + sigaltstack，否则 handler 无法执行。
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
