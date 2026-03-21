# Contributing to Position-Independent Agent

Thank you for your interest in contributing to PIA! This guide covers everything you need to build, develop, and submit changes.

## Table of Contents

- [Quick Start](#quick-start)
- [Toolchain Installation](#toolchain-installation)
- [IDE Configuration](#ide-configuration)
- [Project Structure](#project-structure)
- [The Golden Rule: No Data Sections](#the-golden-rule-no-data-sections)
- [Code Style](#code-style)
- [Documentation](#documentation)
- [Naming Conventions](#naming-conventions)
- [Parameters & Returns](#parameters--returns)
- [Error Handling](#error-handling)
- [Memory & Resources](#memory--resources)
- [Patterns](#patterns)
- [Adding Commands](#adding-commands)
- [Testing](#testing)
- [Common Pitfalls](#common-pitfalls)
- [Submitting Changes](#submitting-changes)

---

## Quick Start

**Requirements:** Clang/LLVM 22+, CMake 3.20+, Ninja 1.10+, C++23. See [Toolchain Installation](#toolchain-installation) below.

> **Windows users:** This project must be built using WSL (Windows Subsystem for Linux). Install WSL, then follow the Linux instructions below to set up the toolchain inside your WSL distribution.

```bash
# Clone with submodules
git clone https://github.com/mrzaxaryan/Position-Independent-Agent.git
cd Position-Independent-Agent

# Build
cmake --preset {platform}-{arch}-{build_type}
cmake --build --preset {platform}-{arch}-{build_type}
```

Presets: `windows|linux|macos|ios|freebsd|uefi|solaris|android` x `i386|x86_64|armv7a|aarch64|riscv32|riscv64|mips64` x `debug|release`

---

## Toolchain Installation

### Windows (via WSL)

This project requires LLVM 22.1.1, which is not available as a native Windows package. **You must use WSL (Windows Subsystem for Linux) to build.**

1. Install WSL if you haven't already:
   ```powershell
   wsl --install
   ```
2. Open your WSL terminal and follow the Linux instructions below.

### Linux (Ubuntu/Debian)

```bash
# Install build tools, download LLVM 22 from GitHub releases, and add to PATH
sudo apt-get update && sudo apt-get install -y cmake ninja-build xz-utils libstdc++-14-dev zlib1g-dev libzstd-dev && LLVM_VER=22.1.1 && LLVM_ARCH=$(uname -m | sed 's/aarch64/ARM64/;s/x86_64/X64/') && wget --show-progress -q "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VER}/LLVM-${LLVM_VER}-Linux-${LLVM_ARCH}.tar.xz" -O /tmp/llvm.tar.xz && sudo mkdir -p /usr/local/llvm && sudo tar -xf /tmp/llvm.tar.xz -C /usr/local/llvm --strip-components=1 && rm /tmp/llvm.tar.xz && echo 'export PATH="/usr/local/llvm/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
```

**Note:** To install a different LLVM version, change `LLVM_VER=22.1.1` to your desired version (e.g., `LLVM_VER=23.1.0`).

Verify:

```bash
clang --version && clang++ --version && lld --version && cmake --version && ninja --version
```

### macOS

**Prerequisites:** [Homebrew](https://brew.sh/).

```bash
# Install all dependencies
brew install llvm cmake ninja
```

Add LLVM to your PATH:

```bash
# For Apple Silicon (M1/M2/M3/M4)
echo 'export PATH="/opt/homebrew/opt/llvm/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc

# For Intel Macs
echo 'export PATH="/usr/local/opt/llvm/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

Verify:

```bash
clang --version && clang++ --version && ld64.lld --version && cmake --version && ninja --version
```

---

## IDE Configuration

### Visual Studio Code

This project is designed and optimized for [Visual Studio Code](https://code.visualstudio.com/). When you open the project in VSCode, you will be automatically prompted to install the recommended extensions.

### WSL Integration

If developing on Windows with WSL, navigate to the project directory inside WSL and run `code .`.

**Prerequisites:**
- WSL properly configured on your Windows system
- QEMU, UEFI firmware, and disk image tools (for UEFI testing):
  ```bash
  sudo apt-get update && sudo apt-get install -y qemu-user-static qemu-system-x86 qemu-system-arm ovmf qemu-efi-aarch64 dosfstools mtools
  ```

For more information, see the [VSCode WSL documentation](https://code.visualstudio.com/docs/remote/wsl).

---

## Project Structure

```
├── CMakeLists.txt            # Build configuration (beacon + test modes)
├── CMakePresets.json          # Build presets for all platform/arch combinations
├── cmake/                     # Build system (toolchain, cross-compilation, PIC verification)
│   ├── Common.cmake           # Shared CMake functions
│   ├── CompilerFlags.cmake    # Clang/LLVM flags (C++23, -nostdlib, PIC)
│   ├── Options.cmake          # CMake options (BUILD_TESTS, ENABLE_LOGGING)
│   ├── Toolchain.cmake        # Compiler detection
│   ├── PICTransform.cmake     # pic-transform LLVM pass integration
│   ├── Sources.cmake          # Source file collection & platform filtering
│   ├── Target.cmake           # Executable definition & post-build verification
│   ├── Triples.cmake          # LLVM target triple generation
│   ├── platforms/             # Per-platform CMake modules (Windows, Linux, macOS, etc.)
│   └── data/                  # Linker scripts & function order files
├── src/
│   ├── entry_point.cc         # Unified platform entry point
│   ├── core/                  # Layer 1 — types, strings, memory, math (platform-independent)
│   ├── platform/              # Layer 2 — OS syscalls, file system, sockets, screens (8 platforms)
│   ├── lib/                   # Layer 3 — crypto, TLS 1.3, HTTP, WebSocket, JPEG
│   └── beacon/                # Layer 4 — command handlers, shell, screen capture, WebSocket loop
│       ├── main.cc            # WebSocket message loop and command dispatcher
│       ├── commands.h         # Command types, handler declarations, Context struct
│       ├── commandsHandler.cc # Command handler implementations
│       ├── shell.cc/h         # Interactive shell (PTY on POSIX, cmd.exe on Windows)
│       └── screen_capture.h   # Screen capture context
├── tests/                     # Test suite (31 test suites across all layers)
│   ├── start.cc               # Test harness entry point
│   └── *_tests.h              # Individual test suites
└── tools/
    ├── pic-transform/         # Custom LLVM pass for PIC enforcement
    ├── poly-engine/           # Polymorphic engine
    └── pyloader/              # Cross-platform shellcode loader (Python)
```

The project is a self-contained monorepo. The `cmake/` directory contains the full build system, and `src/` contains the runtime library and agent code in a layered architecture (core → platform → lib → beacon).

---

## The Golden Rule: No Data Sections

The binary must contain **only** a `.text` section. No `.rdata`, `.rodata`, `.data`, or `.bss`. Verified automatically by the post-build step in `cmake/PostBuild.cmake`.

The [pic-transform](https://github.com/mrzaxaryan/pic-transform) LLVM pass runs automatically during compilation and eliminates data sections by converting global constants (strings, floats, arrays) into stack-local immediate stores. This means you can write normal C++ string literals, float constants, and const arrays -- they are transformed automatically.

**Memory protection consequence:** Because the binary has no data sections, the agent can run entirely in **RX (read+execute)** memory — no writable code page is ever needed at runtime. Any change that reintroduces a data section breaks this guarantee.

| Forbidden | Use Instead |
|-----------|-------------|
| Global/static variables | Stack-local variables |
| STL containers/algorithms | Custom PIR implementations |
| Exceptions (`throw`/`try`/`catch`) | `Result<T, Error>` |
| RTTI (`dynamic_cast`, `typeid`) | Static dispatch |
| `(T*, USIZE)` buffer parameter pairs | `Span<T>` / `Span<const T>` |

---

## Code Style

- **Indentation:** Tabs (not spaces)
- **Braces:** Allman style - opening brace on its own line
- **Include guard:** `#pragma once` in every header
- **No STL, no exceptions, no RTTI**

### Function Attributes
- **`FORCE_INLINE`** for force-inlined functions
- **`NOINLINE`** when inlining must be prevented
- **`EMBED_FUNC()`** for registering command handler function pointers

### Compile-Time Evaluation
- Use `constexpr` / `consteval` wherever possible
- Mark every function and variable `constexpr` if it can be evaluated at compile time
- Use `consteval` when evaluation **must** occur at compile time

### Includes
- `lib/runtime.h` → aggregate header for the full runtime (core + platform + lib layers)
- Beacon code includes layer-specific headers: `commands.h`, `runtime.h`, `websocket_client.h`, etc.
- Implementation files must include their **own header first**
- Use header names directly (include paths are set per-layer by CMake)

---

## Documentation

All public APIs and protocol implementations **must** include Doxygen documentation.

- Every public function, struct, and enum requires a `@brief` description
- Include `@param`, `@return`, and `@note` tags as appropriate
- Reference RFCs or specifications where applicable (e.g., `@see RFC 8446 Section 4.1.2`)
- For Windows NT API wrappers, reference the NT function being called

---

## Naming Conventions

Key conventions:

| Kind | Convention | Examples |
|------|-----------|----------|
| Enum names | `PascalCase` | `CommandType`, `StatusCode` |
| Enum values (unscoped) | `PascalCase_Underscore` | `Command_GetSystemInfo`, `StatusSuccess` |
| Functions | `PascalCase` with `Handle_` prefix for commands | `Handle_GetSystemInfoCommand` |
| Local variables | `camelCase` | `commandType`, `responseLength` |
| Struct fields (public) | `PascalCase` | `WireDirectoryEntry::Name`, `SystemInfo::Hostname` |
| Header files | `snake_case.h` | `commands.h` |
| Source files | `camelCase.cc` | `commandsHandler.cc` |

---

## Parameters & Returns

| Style | When | Example |
|-------|------|---------|
| By value | Small register-sized types | `UINT32 ComputeHash(UINT32 input)` |
| By pointer | Output params, nullable | `VOID Handle_GetFileContentCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)` |
| By reference | Non-null params | `static VOID ToWireEntry(const DirectoryEntry &src, WireDirectoryEntry &dst)` |
| `Span<T>` | Contiguous buffer params | `file.Read(Span<UINT8>(buffer, size))` |

- **All fallible functions must return `Result<T, Error>`** (or `Result<void, Error>` when there is no value)
- Command handlers use output pointer parameters (`PPCHAR response, PUSIZE responseLength`) per the protocol convention

---

## Error Handling

There are no exceptions. Every fallible function returns `Result<T, Error>` or `Result<void, Error>`. Command handlers communicate errors via the response buffer using `StatusCode` values.

### Command Handler Pattern

```cpp
VOID Handle_MyCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context)
{
    // Parse request from command buffer
    // ...

    // On error: write status code and return
    if (!result)
    {
        WriteErrorResponse(response, responseLength, StatusCode::StatusError);
        return;
    }

    // On success: allocate response, write status + payload
    *responseLength = sizeof(UINT32) + payloadSize;
    *response = new CHAR[*responseLength];
    *(PUINT32)*response = StatusCode::StatusSuccess;
    // ... write payload ...
}
```

---

## Memory & Resources

- **Avoid heap** unless no alternative. Prefer stack-local variables and fixed-size buffers.
- **`new`/`new[]`/`delete`/`delete[]` are safe** - globally overloaded to route through the platform allocator (HeapAlloc on Windows, mmap on POSIX, AllocatePool on UEFI).
- **Always `delete[]` response buffers** in the caller (see `src/beacon/main.cc` message loop).
- **No RAII destructors for heap objects** - manually manage lifetime with `new`/`delete`.

---

## Patterns

### Wire Protocol

All commands use a binary protocol over WebSocket. Each message starts with a `UINT8` command type byte followed by command-specific payload. Every response begins with a `UINT32` status code (`0` = success, non-zero = error).

### Wire-Format Structs

Use `#pragma pack(push, 1)` for all wire-format structs to ensure consistent layout across platforms. Use `CHAR16` (always 2 bytes) instead of `WCHAR` for wire-format fields.

### Path Decoding

Wire paths arrive as null-terminated UTF-16LE strings. Use `DecodeWirePath()` to convert to native `WCHAR` paths with platform-appropriate separators.

### Data Section Elimination (pic-transform)

The [pic-transform](https://github.com/mrzaxaryan/pic-transform) LLVM pass automatically converts string literals, float/double constants, const arrays, and function pointer references into position-independent code during compilation. No special syntax or macros are needed.

---

## Adding Commands

To add a new command:

1. **Add enum value** in `commands.h`:
   ```cpp
   enum CommandType : UINT8
   {
       // ... existing commands ...
       Command_MyNewCommand = N,
       CommandTypeCount
   };
   ```

2. **Declare handler** in `src/beacon/commands.h`:
   ```cpp
   VOID Handle_MyNewCommandCommand(PCHAR command, USIZE commandLength, PPCHAR response, PUSIZE responseLength, Context *context);
   ```

3. **Implement handler** in `src/beacon/commandsHandler.cc`

4. **Register handler** in `src/beacon/main.cc`:
   ```cpp
   commandHandlers[CommandType::Command_MyNewCommand] = Handle_MyNewCommandCommand;
   ```

5. **Update README.md** with the new command's request/response format

---

## Testing

The project has 31 test suites covering all layers (core, platform, lib). Tests are built as a separate executable using the same codebase.

```bash
# Build and run tests
cmake --preset linux-x86_64-debug -DBUILD_TESTS=ON -DENABLE_LOGGING=ON
cmake --build --preset linux-x86_64-debug
./build/debug/linux/x86_64/cmake/output
```

**Options:**
- `-DBUILD_TESTS=ON` — compile test suite instead of the beacon
- `-DENABLE_LOGGING=ON` — include logging output (recommended for debugging)
- `-DPIR_LOG_LEVEL=STATUS|VERBOSE|DEBUG|QUIET` — set log verbosity

Test files are header-only (`tests/*_tests.h`) and use a lightweight test framework (`tests/tests.h`) with macros like `TEST_EQUAL`, `TEST_TRUE`, `TEST_FALSE`.

---

## Common Pitfalls

1. **Inline asm register clobbers** - On x86_64, declare all volatile registers (RAX, RCX, RDX, R8-R11) as outputs or clobbers
2. **Memory operands with RSP modification** - Never use `"m"` constraints in asm blocks that modify RSP; under `-Oz` the compiler uses RSP-relative addressing
3. **Wire format alignment** - Always use `#pragma pack(push, 1)` for protocol structs
4. **WCHAR vs CHAR16** - Use `CHAR16` in wire structs (always 2 bytes); `WCHAR` is 4 bytes on Linux
5. **Path separators** - Always normalize paths via `DecodeWirePath()` for cross-platform compatibility

---

## Submitting Changes

### Before You Submit

1. Build cleanly for at least one platform/architecture preset
2. Verify the post-build PIC check passes (no data sections)
3. Follow naming conventions and code style above

### Pull Request Requirements

- Use the [pull request template](pull_request_template.md)
- **Report the binary size diff** - build the same preset before and after your change, then include the `.text` section size (exe and bin) in the PR description. Size regressions require justification; prefer `-Oz` builds for the comparison:

   ```bash
   llvm-size build/release/<platform>/<arch>/output.exe
   ```

   Example PR note: `windows-x86_64-release: exe 42 312 -> 42 480 (+168 B), bin 39 888 -> 40 056 (+168 B)`

### Community

- Please read our [Code of Conduct](CODE_OF_CONDUCT.md) before participating
- Report security vulnerabilities privately - see [Security Policy](SECURITY.md)
