#include "swp_stack_trace.h"

#include <cstddef>
#include <cstdint>
#include <libunwind.h>
#include <unistd.h>

namespace swp_stack_trace {
namespace {

constexpr std::size_t k_name_buffer_size = 256;
constexpr std::size_t k_hex_buffer_size = 64;
constexpr int k_decimal_buffer_size = 16;

// 异步安全地写入 C 字符串。
// 使用 const char* 而非 std::string_view，因为在信号 handler 中构造/传递
// string_view 可能依赖非 async-signal-safe 的操作。
void write_string(int fd, const char* str) {
    if (fd < 0 || str == nullptr) {
        return;
    }
    std::size_t len = 0;
    while (str[len] != '\0') {
        ++len;
    }
    while (len > 0) {
        const ssize_t written = ::write(fd, str, len);
        if (written < 0) {
            return;
        }
        str += static_cast<std::size_t>(written);
        len -= static_cast<std::size_t>(written);
    }
}

// 异步安全地写入单个字符。忽略 write 返回值：
// 单字符写入失败无法恢复，且不能分配缓冲区重试。
void write_char(int fd, char c) {
    if (fd < 0) {
        return;
    }
    (void)::write(fd, &c, 1);
}

// 异步安全地写入无符号整数十六进制。
void write_hex(int fd, std::uintptr_t value) {
    if (fd < 0) {
        return;
    }
    char buf[k_hex_buffer_size];
    int i = 0;
    do {
        const int digit = static_cast<int>(value & 0xF);
        buf[i++] = static_cast<char>((digit < 10) ? ('0' + digit) : ('a' + digit - 10));
        value >>= 4;
    } while (value != 0);
    write_string(fd, "0x");
    while (i > 0) {
        write_char(fd, buf[--i]);
    }
}

// 异步安全地写入非负十进制整数。
void write_decimal(int fd, unsigned value) {
    if (fd < 0) {
        return;
    }
    char buf[k_decimal_buffer_size];
    int i = 0;
    do {
        buf[i++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    while (i > 0) {
        write_char(fd, buf[--i]);
    }
}

} // namespace

void print_stacktrace(int fd) noexcept {
    if (fd < 0) {
        return;
    }

    unw_cursor_t cursor;
    unw_context_t context;

    // 在信号 handler 中，若无法获取上下文则静默返回，
    // 避免在崩溃路径上再次引入复杂处理。
    if (unw_getcontext(&context) != 0) {
        return;
    }
    if (unw_init_local(&cursor, &context) != 0) {
        return;
    }

    unsigned frame = 0;
    // 使用原始数组而非 std::array，确保在 async-signal-safe 路径上不依赖
    // 任何可能分配内存或抛异常的容器操作。
    char name_buffer[k_name_buffer_size];

    while (unw_step(&cursor) > 0) {
        unw_word_t pc = 0;
        if (unw_get_reg(&cursor, UNW_REG_IP, &pc) != 0) {
            continue;
        }

        unw_word_t offset = 0;
        const int name_result = unw_get_proc_name(&cursor, name_buffer, sizeof(name_buffer), &offset);

        write_char(fd, '#');
        write_decimal(fd, frame++);
        write_string(fd, "  ");
        write_hex(fd, static_cast<std::uintptr_t>(pc));
        write_string(fd, "  ");

        if (name_result == 0 && name_buffer[0] != '\0') {
            write_string(fd, name_buffer);
            write_string(fd, "+0x");
            write_hex(fd, static_cast<std::uintptr_t>(offset));
        } else {
            write_string(fd, "???");
        }
        write_char(fd, '\n');
    }
}

} // namespace swp_stack_trace
