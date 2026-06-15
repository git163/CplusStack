# stack

<one-line description>

## Requirements

- CMake ≥ 3.20
- C++17-capable compiler (GCC ≥ 7, Clang ≥ 5, MSVC ≥ 19.14)
- Optional but recommended: GDB or LLDB for debugging; VSCode + the recommended extensions for the best experience.

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

Default build type is `RelWithDebInfo` — optimized but keeps debug info, so crashes still produce usable stack traces. For a true release build:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

## Run

```bash
./build/stack
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Test files live under `tests/` with the `Test*.cpp` naming pattern — `tests/CMakeLists.txt` auto-discovers them via `gtest_discover_tests`, so adding a new test is just `tests/TestMyModule.cpp`.

## Debug

Open the folder in VSCode (`code .`). The project ships launch configurations (`.vscode/launch.json`):

- **(gdb) Launch stack** / **(lldb) Launch stack** — build + debug the main binary. Pick gdb on Linux, lldb on macOS.
- **(gdb) Run unit_tests** / **(lldb) Run unit_tests** — build + debug the test binary directly. Set breakpoints in any `Test*.cpp` and hit F5.

The pre-launch task is `cmake build`, so the binary is always up-to-date when you start a debug session.

If you don't use VSCode, attach from the command line:

```bash
gdb --args ./build/stack     # Linux
lldb ./build/stack           # macOS
```

## Project layout

- `docs/` — design docs and plans (use `docs/plan-template.md`)
- `src/` — implementation (`.cpp`)
- `src/include/` — public headers (Google C++ style)
- `tests/` — GTest unit tests (`Test*.cpp`)

## Conventions

See `CLAUDE.md` at the project root.