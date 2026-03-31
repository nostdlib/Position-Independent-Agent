# Onboarding Guide

Welcome to Position-Independent-Agent. This guide will get you up to speed on the codebase, its architecture, and where to start reading.

---

## Project Overview

**What is this?** A cross-platform remote agent written in C++23 that compiles to position-independent, zero-dependency shellcode. Everything — crypto, TLS 1.3, WebSocket, HTTP, DNS, JPEG encoding — is implemented from scratch. No libc, no standard library, no dynamic linking.

**Languages:** C++ (dominant), CMake, linker scripts, Python, PowerShell
**Build system:** CMake with custom LLVM compiler pass
**Targets:** 8 platforms (Windows, Linux, macOS, FreeBSD, Solaris, UEFI, Android, iOS) across 7 architectures (i386, x86_64, armv7a, aarch64, riscv32, riscv64, mips64)

---

## Architecture

The codebase follows a strict layered architecture. Each layer depends only on layers below it — never sideways or upward.

```
+---------------------------------------------------------------+
|  Beacon (6 files)                                              |
|  Agent command loop, handlers, shell, screen capture           |
+---------------------------------------------------------------+
|  Lib (30 files)                                                |
|  Crypto, TLS 1.3, HTTP, DNS, WebSocket, JPEG, containers      |
+---------------------------------------------------------------+
|  Platform (81 files)                                           |
|  Console, filesystem, sockets, screen, memory, system utils    |
|  Per-platform implementations: posix/, windows/, uefi/         |
+---------------------------------------------------------------+
|  Kernel (81 files)                                             |
|  Raw syscall definitions and OS API wrappers                   |
|  Per-OS: linux/, windows/, macos/, freebsd/, solaris/, uefi/   |
+---------------------------------------------------------------+
|  Core (31 files)                                               |
|  Types, memory ops, strings, math, algorithms, compiler RT     |
+---------------------------------------------------------------+
```

Supporting layers outside the main stack:

| Layer | Files | What it does |
|-------|-------|-------------|
| Build System | 27 | CMake modules, linker scripts, platform configs, post-build verification |
| Tests | 33 | Test suites for every major subsystem |
| Tools | 3 | The pic-transform LLVM pass (the core innovation) |
| Loaders | 2 | Python and PowerShell shellcode loaders |
| Entry Point | 1 | `src/entry_point.cc` — the bootstrap bridge |

---

## Guided Tour

Read the codebase in this order. Each step builds on the previous one.

### 1. The Entry Point: Where Shellcode Begins
**Read:** `src/entry_point.cc`, `src/core/compiler/compiler.h`

Every position-independent binary starts executing at a single function. `entry_point.cc` is the first byte of the `.text` section — the build system verifies this. It bootstraps platform context (UEFI handles on firmware, nothing on POSIX), calls `start()`, and terminates via `ExitProcess`. The `ENTRYPOINT` macro in `compiler.h` handles architecture-specific quirks like x86_64 stack alignment.

### 2. Freestanding Primitives: No Standard Library
**Read:** `src/core/types/primitives.h`, `src/core/types/span.h`, `src/core/types/result.h`, `src/core/types/error.h`

With `-nostdlib`, there are no standard types. `primitives.h` defines every fundamental type (`INT8` through `UINT64`, `BOOL`, `USIZE`, variadic macros) using compiler builtins. `span.h` provides a zero-cost buffer view like `std::span`. `result.h` implements Rust-style `Result<T,E>` for error handling since exceptions are disabled. These three files are the vocabulary types used everywhere.

### 3. CRT-Free Memory and Strings
**Read:** `src/core/memory/memory.cc`, `src/core/memory/memory.h`, `src/core/string/string.h`

Without libc, `memcpy`/`memset`/`memmove` are implemented by hand with word-at-a-time optimization. `string.h` provides a full `StringUtils` class — comparison, searching, number conversion, UTF encoding — all without CRT. These replace the C runtime foundation that normal programs take for granted. The compiler generates calls to `memcpy`/`memset` implicitly, so these must exist or you get linker errors.

### 4. Direct Syscalls: Talking to the Kernel
**Read:** `src/platform/kernel/linux/syscall.h`, `src/platform/kernel/linux/system.x86_64.h`, `src/platform/kernel/linux/platform_result.h`

Instead of calling libc wrappers, the agent issues raw syscalls via inline assembly. Each OS/architecture has its own `System::Call` overloads — `svc #0` on AArch64, `syscall` on x86_64, `int $0x80` on i386. The `syscall.h` headers define per-platform syscall numbers. `platform_result.h` converts raw return values into the `Result<T,E>` type.

### 5. Windows Kernel Layer: PEB Walking and PE Parsing
**Read:** `src/platform/kernel/windows/peb.cc`, `src/platform/kernel/windows/pe.cc`, `src/core/algorithms/djb2.h`

Windows doesn't expose syscall numbers publicly. Instead, the agent walks the Process Environment Block (PEB) to find loaded DLLs, parses their PE export tables, and resolves API function pointers using DJB2 compile-time hashes. The hash seed changes every build (derived from `__DATE__`) so the same API name produces different constants each compilation — defeats signature scanning.

### 6. Platform Abstraction: Portable OS Services
**Read:** `src/platform/memory/allocator.h`, `src/platform/socket/socket.h`, `src/platform/fs/file.h`, `src/platform/console/console.h`, `src/platform/platform.h`

Above the raw kernel layer sits a portable abstraction: `Allocator` for heap memory (via mmap/VirtualAlloc/EFI), `Socket` for TCP, `File`/`Directory` for filesystem access, `Console` for output. Each has per-platform implementations (posix/, windows/, uefi/) selected at compile time. This is how one codebase spans 8 operating systems.

### 7. Custom Cryptography
**Read:** `src/lib/crypto/sha2.h`, `src/lib/crypto/ecc.h`, `src/lib/crypto/chacha20.h`

TLS 1.3 requires serious crypto, all implemented from scratch. SHA-256/SHA-384 use traits-based templates with round constants embedded as immediates (not lookup tables — those would create `.rodata`). ECC implements NIST P-256/P-384 for ECDH key exchange. ChaCha20-Poly1305 provides the AEAD cipher for TLS record encryption. Every constant that would normally be a table is instead computed or stored on the stack.

### 8. TLS 1.3 Client
**Read:** `src/lib/network/tls/tls_client.h`, `src/lib/network/tls/tls_cipher.h`, `src/lib/network/tls/tls_hkdf.h`

Full TLS 1.3 handshake: ClientHello/ServerHello, ECDH key share, HKDF key schedule, encrypted extensions, certificate processing. `TlsCipher` manages the key schedule, `ChaCha20Encoder` handles bidirectional record encryption, `TlsBuffer` provides dynamic memory for record I/O. The result is encrypted TCP without OpenSSL.

### 9. DNS, HTTP, and WebSocket
**Read:** `src/lib/network/dns/dns_client.h`, `src/lib/network/http/http_client.h`, `src/lib/network/websocket/websocket_client.h`

DNS uses DNS-over-HTTPS (queries via TLS to Cloudflare/Google). `HttpClient` handles URL parsing, connection creation with IPv6-to-IPv4 fallback, and HTTP/1.1 request/response. `WebSocketClient` implements RFC 6455 on top of TLS: opening handshake, masked framing, fragmentation reassembly, control frames. This is the C2 transport stack.

### 10. The Beacon: Command Loop
**Read:** `src/beacon/main.cc`, `src/beacon/commands.h`, `src/beacon/commandsHandler.cc`

The beacon is the top-level agent logic. `start()` registers command handlers as a function pointer array, connects via WebSocket to the relay server, and enters a receive-dispatch-respond loop with automatic reconnection. 9 command types: system info, directory listing, file read, file hash, shell I/O (write/read/reset), display enumeration, screenshot. Each handler allocates a response buffer and returns ownership to the caller.

### 11. Shell and Screen Capture
**Read:** `src/beacon/shell.h`, `src/beacon/screen_capture.h`, `src/lib/image/jpeg_encoder.h`

Shell wraps platform `ShellProcess` (PTY on POSIX, cmd.exe pipes on Windows) for interactive remote access. Screen capture uses platform-specific framebuffer reading, binary image differencing for dirty-rectangle detection, and a from-scratch baseline JPEG encoder. Float constants in the DCT are encoded as `UINT32` immediates to stay PIC-compliant.

### 12. The PIC Transform
**Read:** `tools/pic-transform/PICTransformPass.h`, `tools/pic-transform/PICTransformPass.cpp`

The key innovation. This custom LLVM pass rewrites compiler IR to eliminate `.data`, `.rodata`, `.bss`, and `.got` sections. String literals become stack-allocated immediate stores. Float constants become integer bit patterns loaded into FP registers. Function pointers become PC-relative inline assembly. Without this pass, standard C++ compilation produces binaries with data sections that can't run as raw shellcode.

### 13. Build System
**Read:** `cmake/Common.cmake`, `cmake/CompilerFlags.cmake`, `cmake/Target.cmake`, `cmake/scripts/VerifyPICMode.cmake`

The CMake pipeline: freestanding toolchain setup, platform-specific flags and linker scripts that merge all sections into `.text`, PIC transform integration, then post-build steps that extract the `.text` section into a raw binary and verify position-independence by scanning the linker map for forbidden sections. If `.rodata` or `.data` exists, the build fails.

### 14. Loaders
**Read:** `loaders/python/loader.py`, `loaders/windows/powershell/loader.ps1`

The compiled `.bin` is raw machine code. The Python loader auto-detects OS/architecture, downloads the correct binary, and executes via mmap+mprotect on POSIX or process injection (VirtualAllocEx + CreateRemoteThread) on Windows. The PowerShell loader does the same for Windows-only environments. Memory is never simultaneously writable and executable (W^X compliance).

---

## Key Concepts

These patterns show up everywhere in the codebase:

**Position Independence (PIC):** The binary must have ONLY a `.text` section. No `.data`, `.rodata`, `.bss`, `.got`, or `.plt`. This means no global variables, no string literal tables, no vtables. Everything lives on the stack or in registers.

**Result<T, E>:** Error handling without exceptions. Every fallible function returns `Result<T, Error>`. Callers must check `IsOk()` before accessing `Value()`. Errors chain — you can see the full propagation path from root cause to failure site.

**Span<T>:** Non-owning buffer view. Bundles a pointer and size together. Used everywhere instead of raw pointer+length pairs. Prevents the classic "swapped arguments" bug.

**DJB2 Hashing:** Non-cryptographic hash used for Windows API resolution. The seed changes every build (from `__DATE__`), so the same function name produces different hash values each compilation. `consteval` ensures compile-time evaluation.

**RAII + Move-Only:** File, Socket, Process, etc. are move-only types with deleted copy constructors. Destructors automatically close handles. `operator new` is deleted — stack-only allocation.

**COMPILER_RUNTIME / NOINLINE / optnone:** Attributes that prevent the optimizer from undoing PIC transforms. `optnone` stops the compiler from hoisting stack values back into `.rodata`. `NOINLINE` prevents constant folding across function boundaries.

---

## Complexity Hotspots

These files are the most complex in the codebase. Approach them with care (and coffee):

| File | Why it's complex |
|------|-----------------|
| `tools/pic-transform/PICTransformPass.cpp` | 1177 lines. Multi-phase LLVM Module pass doing float replacement, function pointer rewriting, and global-to-stack transformation |
| `src/lib/crypto/ecc.cc` | 865 lines. Big-integer arithmetic, modular reduction, Montgomery ladder scalar multiplication |
| `src/lib/network/tls/tls_client.cc` | 860 lines. Full TLS 1.3 state machine with handshake, key derivation, and record processing |
| `src/core/compiler/compiler_runtime.armv7a.cc` | 738 lines. ARM EABI division, naked assembly, IEEE-754 float conversion |
| `src/lib/crypto/chacha20.cc` | 727 lines. Poly1305 MAC with 26-bit limb arithmetic, ChaCha20 quarter rounds, AEAD construction |
| `src/lib/image/jpeg_encoder.cc` | 1051 lines. Baseline JPEG with Arai-Agui-Nakajima DCT, Huffman tables, all PIC-safe |
| `src/lib/network/dns/dns_client.cc` | 683 lines. DNS wire format construction, DoH transport, fallback across 4 resolvers |
| `src/platform/screen/posix/screen.cc` | 2483 lines. Three fallback methods: X11 protocol, DRM dumb buffers, fbdev |
| `src/core/string/string_formatter.h` | 1106 lines. Printf-style formatter with type erasure, callback-based output |
| `src/platform/kernel/windows/ntdll.h` | 943 lines. NT Native API declarations, structures, SSN resolution |

---

## File Map by Layer

### Entry Point (1 file)
| File | What it does |
|------|-------------|
| `src/entry_point.cc` | Bootstrap bridge — first byte of .text, calls start(), exits |

### Core (31 files)
| File | What it does |
|------|-------------|
| `src/core/core.h` | Aggregate include for all core modules |
| `src/core/types/primitives.h` | Fixed-width types, USIZE, TCHAR concept, variadic macros |
| `src/core/types/result.h` | Result<T,E> tagged union for error handling |
| `src/core/types/span.h` | Non-owning buffer view with static/dynamic extent |
| `src/core/types/error.h` | Error codes and chaining mechanism |
| `src/core/types/ip_address.h` | IPv4/IPv6 address parsing and storage |
| `src/core/types/uuid.h` | UUID generation and parsing |
| `src/core/memory/memory.cc` | memset/memcpy/memmove with word-at-a-time optimization |
| `src/core/string/string.h` | Full string utilities (compare, search, convert, UTF) |
| `src/core/string/string_formatter.h` | Printf-style formatter with type erasure |
| `src/core/algorithms/djb2.h` | Build-unique hash for API resolution |
| `src/core/algorithms/base64.h` | Base64 without lookup tables |
| `src/core/math/prng.h` | xorshift64 PRNG |
| `src/core/math/bitops.h` | Bit rotation for crypto |
| `src/core/math/byteorder.h` | Endianness swapping |
| `src/core/compiler/compiler.h` | ENTRYPOINT, FORCE_INLINE, NOINLINE, DISABLE_OPTIMIZATION |
| `src/core/compiler/compiler_runtime.*.cc` | Division/shift/float helpers for 32-bit architectures |
| `src/core/encoding/utf16.h` | UTF-16 to UTF-8 conversion |
| `src/core/binary/binary_reader.h` | Sequential byte reading with big-endian support |
| `src/core/binary/binary_writer.h` | Sequential byte writing with big-endian support |

### Beacon (6 files)
| File | What it does |
|------|-------------|
| `src/beacon/main.cc` | WebSocket connect, receive-dispatch-respond loop |
| `src/beacon/commands.h` | CommandType enum, Context struct, handler typedef |
| `src/beacon/commandsHandler.cc` | All 9 command handlers |
| `src/beacon/shell.h` / `shell.cc` | Interactive shell wrapper |
| `src/beacon/screen_capture.h` | Screen capture data structures and dirty-rect state |

### Lib (30 files)
| File | What it does |
|------|-------------|
| `src/lib/runtime.h` | Aggregate include for entire runtime |
| `src/lib/vector.h` | Move-only dynamic array with fallible allocation |
| `src/lib/crypto/chacha20.*` | ChaCha20-Poly1305 AEAD cipher (RFC 8439) |
| `src/lib/crypto/ecc.*` | ECDH key exchange, P-256/P-384 |
| `src/lib/crypto/sha2.*` | SHA-256/SHA-384 and HMAC |
| `src/lib/crypto/chacha20_encoder.*` | TLS 1.3 record encryption layer |
| `src/lib/network/tls/tls_client.*` | TLS 1.3 handshake state machine |
| `src/lib/network/tls/tls_cipher.*` | Key schedule and HKDF |
| `src/lib/network/http/http_client.*` | HTTP/1.1 client |
| `src/lib/network/dns/dns_client.*` | DNS-over-HTTPS resolver |
| `src/lib/network/websocket/websocket_client.*` | RFC 6455 WebSocket |
| `src/lib/image/jpeg_encoder.*` | Baseline JPEG encoder |
| `src/lib/image/image_processor.*` | Binary diff and dirty-rectangle detection |

---

## Getting Started

1. Read the [README](../README.md) for build instructions
2. Follow the tour above, in order
3. Read [CONTRIBUTING.md](../CONTRIBUTING.md) for coding conventions
4. Check the [book-notes/](../book-notes/) directory for detailed Q&A on every subsystem
5. Run a debug build and step through `entry_point.cc` -> `start()` -> the command loop
