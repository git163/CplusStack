# symbolize_stack.py 补函数名 demangle

- 日期: 2026-06-16
- 作者: Claude Code / tshua
- 状态: 已批准
- 关联: [symbolize_stack.py](../symbolize_stack.py)、[docs/swp-stack-trace-principle.md](swp-stack-trace-principle.md)

## 背景

[symbolize_stack.py](../symbolize_stack.py) 当前只把 PC 翻译成 `file:line`，**没有翻译函数名**：

- Linux 路径调用 `addr2line` 时没有加 `-f`，丢了函数名
- macOS 路径调 `atos` 时虽然拿到了 demangled 名，但被 [symbolize_stack.py:199](symbolize_stack.py#L199) 的正则 `\(([^()]+:\d+)\)\s*$` 默默丢弃

结果是：用户跑完脚本后，输出里函数名仍然是 `_ZN12_GLOBAL__N_113crash_level_1Ev` 这种 mangled 串，可读性差。

## 目标

- [x] Linux 路径：让 `addr2line -f` 给出函数名，脚本解析并插入到输出
- [x] macOS 路径：让 `atos` 的 demangled 函数名也被保留到输出
- [x] 输出格式保持向后兼容：原有 `at <file>:<line>` 后缀不变，函数名替换 mangled 名

## 非目标

- 不修改 [src/swp_stack_trace.cpp](../src/swp_stack_trace.cpp) 的运行时输出（demangle 必须留在离线侧，参见 [swp-stack-trace-principle.md:343](swp-stack-trace-principle.md#L343)）
- 不引入新的外部依赖（仍只用 `addr2line` / `atos` / 系统工具）
- 不做符号 cache / 持久化

## 方案

### 数据流（不变的部分用灰色，新增部分加粗）

```
原始 mangled 行
  #2  0x102062d10  _ZN12_GLOBAL__N_113crash_level_1Ev+0x28
                │
                │  parse_input()
                ▼
  entries[i] = (line, prefix="#2  ", pc="0x102062d10",
                suffix="  _ZN...crash_level_1Ev+0x28")
                │
                │  symbolize_addr2line(binary, tool, pcs, entries)
                ▼  ── 新增 -f flag ────────────────────────────
  addr2line 输出（每个 PC 2 行）:
    crash_level_1()                                  ← func（已 demangle）
    /Users/.../crash_demo.cpp:73                     ← file:line
                │
                ▼  ── 新增 parse ────────────────────────────
  locations[i] = (func="crash_level_1()", loc="crash_demo.cpp:73")
                │
                │  combine(entries, locations)
                ▼
  #2  0x102062d10  crash_level_1()+0x28  at crash_demo.cpp:73
```

### 关键设计点

1. **保留 mangled 名作为 offset 锚点**：原行里的 `+0x28` 来自 libunwind 的 `unw_get_proc_name` 返回的 offset，必须保留。新输出把 mangled 名替换为 demangled 名，`+0x28` 跟随其后：`crash_level_1()+0x28`。

2. **`addr2line -f` 输出格式**：每个 PC 对应 2 行（func + file:line），脚本按 2 行/PC 切片。`addr2line` 内部已经做了 demangle（基于 ELF `.debug_info` 中的 DWARF 信息），不需要再调 `c++filt`。

3. **`atos` 输出格式**：单行 `<demangled_func> (in <bin>) (<file>:<line>)`。正则拆三段：函数名（首个 `(` 之前）+ `in` 段（丢弃）+ file:line（末尾 `(...)`）。

4. **`??` 兜底**：
   - `addr2line` 函数名查不到时返回 `??`，file:line 返回 `??:0` 或 `??:?`
   - 此时输出 `#2  0x...  ???+0x28  at crash_demo.cpp:73`（用 `???` 兜底）
   - 如果 file:line 也是 `??:0`，整行降级到原样（保留 mangled 名 + 不带 `at`）

### 接口变更

```python
# 旧
def symbolize_addr2line(binary, tool_path, pcs, entries) -> list[str]
def symbolize_atos(binary, tool_path, entries, pcs) -> list[str]

# 新增内部辅助
def _split_addr2line_output(stdout: str, pc_count: int)
    -> list[tuple[str, str]]   # [(func, file:line), ...]
def _parse_atos_line(line: str) -> tuple[str, str]
    # (func, file:line)，失败时 ("??", "??:0")

# combine 内部签名调整
def combine(entries, locations) -> list[str]
    # locations: list[(func, file:line)] 而不是 list[str]
```

## 影响范围

- 文件：仅 [symbolize_stack.py](../symbolize_stack.py)
- 不影响 C++ 库代码
- 不影响 [run.sh](../run.sh) / [run_tests.sh](../run_tests.sh) / [test_all_crashes.py](../test_all_crashes.py)
- 输出格式微调（mangled → demangled），但 `at <file>:<line>` 后缀位置不变，下游 grep 仍能定位

## 风险与对策

| 风险 | 对策 |
|---|---|
| `addr2line` 在某些发行版不支持 `-f` | binutils 自 2.17 起就支持 `-f`，几乎所有现代 Linux 都满足；如失败脚本降级为旧行为 |
| `addr2line -f` 输出偶发空行 | 解析时跳过空行，每 2 行一组取 func + loc |
| `atos` 在某些 macOS 版本输出格式略变 | 正则取首个 `(` 之前 + 末尾 `(file:line)`，容错强 |
| demangle 失败（stripped binary） | 函数名显示 `???`，不影响 file:line 输出 |
| 函数名含括号（少见） | `atos` 路径用 `\(` 锚定第一个左括号；`addr2line` 输出按行切，括号无歧义 |

## 实施步骤

- [x] 写 plan 文档（本文件）
- [x] 改 [symbolize_addr2line](symbolize_stack.py#L72)：加 `-f`，解析 2 行/PC
- [x] 改 [symbolize_atos](symbolize_stack.py#L173)：正则拆 func + loc
- [x] 改 [combine](symbolize_stack.py#L208)：locations 改为 `(func, loc)` 元组，输出合并 mangled offset
- [x] 手动用 [crash_demo](../src/example/crash_demo.cpp) 触发崩溃，跑脚本验证
- [x] 跑 [test_all_crashes.py](../test_all_crashes.py) 端到端验证

## 测试计划

1. **手动触发 segfault**：跑 `./run.sh sigsegv`，脚本解析 `/tmp/swp_crash.log`，人工肉眼确认函数名已 demangle
2. **批量回归**：跑 `python3 test_all_crashes.py`，验证 30 种 crash 全部解析成功、解析到正确的 `file:line`
3. **边界**：构造一个 stripped binary（`strip -s`），验证 demangle 失败时输出 `???` 不报错

## 引用

- [docs/swp-stack-trace-principle.md](swp-stack-trace-principle.md) — 为什么 demangle 必须离线
- [man addr2line(1)](https://man7.org/linux/man-pages/man1/addr2line.1.html) — `-f` 选项说明
- [man atos(1)](https://www.manpagez.com/man/1/atos/) — macOS 符号化工具