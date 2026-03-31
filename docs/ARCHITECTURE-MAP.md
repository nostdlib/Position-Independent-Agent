# Architecture Map

Auto-generated from knowledge graph analysis of 295 source files, 543 code elements, and 568 dependency relationships.

---

## Dependency Flow

```
                    +-----------+
                    |  Loaders  |  (Python, PowerShell)
                    +-----+-----+
                          |
                          | loads .bin
                          v
                    +-----------+
                    |  Entry    |  src/entry_point.cc
                    |  Point    |
                    +-----+-----+
                          |
                          | calls start()
                          v
                    +-----------+
                    |  Beacon   |  src/beacon/
                    |  (6 files)|  Command loop, handlers, shell, screenshots
                    +-----+-----+
                          |
                          | uses
                          v
              +-----------+-----------+
              |         Lib           |  src/lib/
              |      (30 files)       |  Crypto, TLS, HTTP, DNS, WebSocket, JPEG
              +-----------+-----------+
                          |
                          | uses
                          v
              +-----------+-----------+
              |       Platform        |  src/platform/
              |      (81 files)       |  Console, FS, Socket, Screen, Memory, System
              |  posix/ windows/ uefi/|
              +-----------+-----------+
                          |
                          | uses
                          v
              +-----------+-----------+
              |        Kernel         |  src/platform/kernel/
              |      (81 files)       |  Raw syscalls, PEB, PE parsing, UEFI protocols
              |  linux/ windows/      |
              |  macos/ freebsd/      |
              |  solaris/ uefi/       |
              +-----------+-----------+
                          |
                          | uses
                          v
              +-----------+-----------+
              |         Core          |  src/core/
              |      (31 files)       |  Types, memory, strings, math, algorithms
              +-----------+-----------+
```

Side layers:
- **Build System** (27 files in `cmake/`): Toolchain, flags, pic-transform integration, PIC verification
- **Tools** (3 files in `tools/pic-transform/`): The LLVM pass that makes it all work
- **Tests** (33 files in `tests/`): Comprehensive test suites

---

## Layer Detail: Core (31 files)

The freestanding runtime foundation. No OS knowledge, no syscalls — pure C++ that works on any platform.

**Types** (`src/core/types/`):
- `primitives.h` — INT8-UINT64, USIZE, BOOL, PVOID, variadic macros. All from compiler builtins.
- `result.h` — Tagged union `Result<T,E>`. Ok or Err, never both. `[[nodiscard]]` forces checking.
- `span.h` — Non-owning buffer view. Dynamic and static extent variants. Smart string literal handling.
- `error.h` — 50+ error codes organized by module. Chains errors for full propagation paths.
- `ip_address.h/.cc` — IPv4/IPv6 parsing and storage. Union-based for memory efficiency.
- `uuid.h` — Version 4 UUID generation per RFC 9562.
- `point.h`, `rgb.h` — Simple geometry and pixel types.

**Memory** (`src/core/memory/`):
- `memory.cc` — Hand-rolled memset/memcpy/memmove/memcmp. Word-at-a-time optimization. Marked `extern "C"` for linker compatibility. `COMPILER_RUNTIME` attribute prevents optimizer interference.
- `memory.h` — C function declarations + C++ Memory class wrapper.

**Strings** (`src/core/string/`):
- `string.h` — 1000+ lines. Length, compare, search, number conversion, UTF encoding. Templated for CHAR and WCHAR.
- `string.cc` — Float-to-string and string-to-float without printf/strtod.
- `string_formatter.h` — Printf-style formatting with type erasure and callback-based output.

**Algorithms** (`src/core/algorithms/`):
- `djb2.h` — Hash function with build-unique seed (FNV-1a of `__DATE__`). `constexpr` for runtime, `consteval` for compile-time only. Used to resolve Windows API functions without storing string names.
- `base64.h/.cc` — Encoding/decoding without lookup tables. Arithmetic character mapping.

**Math** (`src/core/math/`):
- `prng.h` — xorshift64 PRNG. Division-free random letter generation.
- `bitops.h` — Bit rotation (ROL/ROR) patterns for SHA-256 and ChaCha20.
- `byteorder.h` — Big-endian byte swapping via compiler builtins.
- `math.h` — Min, Max, Abs, Clamp.

**Compiler** (`src/core/compiler/`):
- `compiler.h` — ENTRYPOINT, FORCE_INLINE, NOINLINE, DISABLE_OPTIMIZATION macros.
- `compiler_builtins.h` — RISC-V 32 CLZ/CTZ replacements (binary search, no lookup tables).
- `compiler_runtime.*.cc` — Software 64-bit division/modulo/shift for 32-bit architectures. ARM EABI naked functions. IEEE-754 float conversion.

**Encoding** (`src/core/encoding/`):
- `utf16.h` — UTF-16 to UTF-8 conversion with surrogate pair handling.

**Binary I/O** (`src/core/binary/`):
- `binary_reader.h` / `binary_writer.h` — Sequential reading/writing with big-endian multi-byte support. Used for TLS record parsing.

---

## Layer Detail: Kernel (81 files)

Raw OS interface. Syscall numbers, inline assembly, and OS-specific structures.

**Linux** (`src/platform/kernel/linux/`): 15 files
- `syscall.h` + 7 arch-specific `syscall.*.h` — Syscall numbers per architecture. MIPS64 has different values from everyone else.
- `system.h` + 7 arch-specific `system.*.h` — `System::Call` overloads using inline asm (`syscall`, `svc #0`, `int $0x80`, `ecall`).
- `platform_result.h` — Converts negative returns to `Error`.

**Windows** (`src/platform/kernel/windows/`): 23 files
- `peb.h/.cc` — Reads PEB from TEB register (GS:[0x60] on x86_64, FS:[0x30] on i386). Walks module list.
- `pe.h/.cc` — Parses PE export tables. Resolves function addresses by DJB2 hash.
- `ntdll.h/.cc` — NT Native API wrappers. Indirect syscalls on x86, direct calls on ARM64.
- `kernel32.h/.cc`, `gdi32.h/.cc`, `user32.h/.cc` — Win32 API wrappers via runtime resolution.
- `system.h` + 4 arch-specific headers — System::Call overloads with SSN resolution and syscall gadgets.
- `windows_types.h` — Windows structure definitions (UNICODE_STRING, OBJECT_ATTRIBUTES, etc.)

**macOS** (`src/platform/kernel/macos/`): 8 files
- `syscall.h` — BSD syscall numbers with 0x2000000 class prefix on x86_64.
- `dyld.h/.cc` — Dynamic loader that resolves framework functions by parsing Mach-O symbol tables.
- `mach.h` — Mach IPC trap for 7-argument system calls.

**FreeBSD** (`src/platform/kernel/freebsd/`): 6 files
- BSD-style syscalls. `sin_len` field in sockaddr. AF_INET6 = 28.

**Solaris** (`src/platform/kernel/solaris/`): 5 files
- Oracle Solaris syscall numbers. i386 uses `int $0x91` instead of Linux's `int $0x80`.

**UEFI** (`src/platform/kernel/uefi/`): 14 files
- No syscalls. Protocol-based API via System Table function pointer tables.
- EFI_BOOT_SERVICES, EFI_RUNTIME_SERVICES, TCP4/TCP6, GOP, FILE_PROTOCOL, etc.
- Context stored in CPU register (GS base MSR / TPIDR_EL0) to avoid global variables.

**Android** (3 files): Forwards to Linux.
**iOS** (4 files): Forwards to macOS (shared XNU kernel).

---

## Layer Detail: Platform (81 files)

Portable abstractions over the kernel layer. One API, multiple implementations.

**Console** (`src/platform/console/`): 6 files — Printf-style output via direct syscalls (POSIX write(), Windows ZwWriteFile, UEFI OutputString).

**Filesystem** (`src/platform/fs/`): 18 files — File, Directory, DirectoryIterator, Path with POSIX/Windows/UEFI backends. RAII handles, move-only.

**Socket** (`src/platform/socket/`): 4 files — TCP with POSIX BSD sockets, Windows AFD IOCTLs, UEFI TCP4/TCP6. Non-blocking connect with timeout.

**Screen** (`src/platform/screen/`): 6 files — Screen capture. Linux: X11 protocol / DRM / fbdev triple fallback. macOS: CoreGraphics via dlopen with fork crash isolation. Windows: GDI BitBlt. UEFI: GOP.

**Memory** (`src/platform/memory/`): 5 files — Virtual memory via mmap/VirtualAlloc/AllocatePool. POSIX stores size header for munmap.

**System** (`src/platform/system/`): ~40 files — DateTime, Environment, MachineID, Process, Pipe, PTY, ShellProcess, Random. Each with posix/windows/uefi implementations.

---

## Layer Detail: Lib (30 files)

High-level functionality built on Platform.

**Crypto** (`src/lib/crypto/`): 10 files
- ChaCha20-Poly1305 AEAD (RFC 8439). Poly1305 uses 5x26-bit limbs for 32-bit compatibility.
- SHA-256/SHA-384 with traits templates. Constants filled on stack via NOINLINE functions.
- HMAC-SHA256/384 with cached key state for efficient reuse.
- ECC P-256/P-384 with Montgomery ladder (constant-time). Jacobian coordinates.
- ChaCha20Encoder for TLS 1.3 record-layer encryption.

**TLS** (`src/lib/network/tls/`): 8 files — Full TLS 1.3 client: handshake state machine, HKDF key derivation, transcript hashing, cipher state management.

**HTTP** (`src/lib/network/http/`): 2 files — HTTP/1.1 client with URL parsing, DNS resolution, rolling-window header detection.

**DNS** (`src/lib/network/dns/`): 2 files — DNS-over-HTTPS. Fallback: Cloudflare 1.1.1.1 -> 1.0.0.1 -> Google 8.8.8.8 -> 8.8.4.4.

**WebSocket** (`src/lib/network/websocket/`): 2 files — RFC 6455. Opening handshake, masked framing, fragmentation, Close/Ping/Pong.

**Image** (`src/lib/image/`): 4 files — Baseline JPEG encoder. Binary image differencing and dirty-rectangle detection for incremental screenshots.

**Containers** (`src/lib/`): `vector.h` — Move-only dynamic array. `runtime.h` — Aggregate include.

---

## Layer Detail: Beacon (6 files)

The agent itself.

- `main.cc` — `start()` function. Builds handler array, infinite reconnect loop, WebSocket receive-dispatch-respond.
- `commands.h` — 9 command types, Context struct (holds Shell and ScreenCapture state), CommandHandler typedef.
- `commandsHandler.cc` — Handler implementations. Wire format structs with `#pragma pack(push, 1)`. UTF-16 path decoding.
- `shell.h/.cc` — Wraps ShellProcess. Polling read with 5s initial / 100ms subsequent timeout.
- `screen_capture.h` — Graphics context with dual-buffer (current/previous) for dirty-rect diffing.

---

## Layer Detail: Tools (3 files)

- `PICTransformPass.cpp` — The core LLVM pass. Multi-phase: (0) float constants to integer bitcasts, (0b) x86-specific lowering, (1) function pointers to PC-relative asm, (2) global constants to stack allocations with immediate stores. Uses register barriers to prevent re-optimization.
- `PICTransformOpt.cpp` — Standalone CLI driver for the pass.
- `PICTransformPass.h` — Pass class declaration.

---

## Layer Detail: Build System (27 files)

**Pipeline:** Toolchain.cmake (freestanding) -> Options.cmake (validate platform/arch) -> Triples.cmake (target triple) -> CompilerFlags.cmake (-nostdlib etc.) -> Sources.cmake (discover files) -> PICTransform.cmake (find/build pass) -> Target.cmake (create executable) -> Platform/*.cmake (per-platform flags) -> PostBuild.cmake (extract .bin, verify PIC)

**Key scripts:**
- `VerifyPICMode.cmake` — Scans linker map for forbidden sections. Build fails if .rodata/.data/.bss/.got/.plt found.
- `ExtractBinary.cmake` — Extracts .text section to .bin, generates disassembly and strings output.
- `PatchElfOSABI.cmake` — Patches ELF byte 7 for FreeBSD (9) / Solaris (6).

**Linker scripts** (`cmake/data/`): Merge .rodata into .text for architectures where LTO generates constant pools. Custom PHDRS for Solaris (no PT_PHDR without PT_INTERP).

---

## Cross-Cutting Concerns

**Everything is position-independent.** No global variables. No string tables. No vtables. Constants are stack-allocated or encoded as immediates. The build system verifies this after every compilation.

**Everything is freestanding.** No libc, no C++ stdlib. memcpy, division, float conversion — all reimplemented. The compiler runtime files alone are ~1700 lines of hand-written division and IEEE-754 code.

**Everything handles 8 platforms.** Conditional compilation via `#if defined(PLATFORM_LINUX)` etc. Platform-specific files in subdirectories. One header declares the interface, platform directories provide implementations.

**Error handling is via Result<T,E>.** No exceptions. No setjmp/longjmp. Every syscall and I/O operation returns a Result. Errors chain for full propagation paths.
