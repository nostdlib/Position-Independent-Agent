[< Back to Project Root](../README.md)

# Source Architecture

The codebase is organized in four layers, each building on the one below. No layer may depend on a higher layer.

```
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 4: BEACON                                                     │
│  Command dispatch, WebSocket message loop, shell management          │
│  src/beacon/                                                         │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 3: LIBRARIES                                                  │
│  Crypto (SHA-2, ChaCha20-Poly1305, ECC), TLS 1.3, HTTP, WebSocket, │
│  DNS-over-HTTPS, JPEG encoding                                       │
│  src/lib/                                                            │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 2: PLATFORM                                                   │
│  Console, Filesystem, Memory, Screen, Socket, System utilities       │
│  → Kernel interfaces (Windows, Linux, FreeBSD, macOS, Solaris, UEFI)│
│  src/platform/                                                       │
├─────────────────────────────────────────────────────────────────────┤
│  Layer 1: CORE                                                       │
│  Types, strings, memory ops, hashing, encoding, math, PRNG          │
│  src/core/                                                           │
└─────────────────────────────────────────────────────────────────────┘
```

## Documentation Index

### Layer 1: Core
| Module | README | Key Techniques |
|---|---|---|
| [Core](core/) | [core/README.md](core/README.md) | Build-unique DJB2 seeding via FNV-1a, branchless Base64 without lookup tables, UTF-8↔UTF-16 surrogate pairs, word-at-a-time memset/memcpy, xorshift64 PRNG, zero-cost Result type |

### Layer 2: Platform
| Module | README | Key Techniques |
|---|---|---|
| [Platform](platform/) | [platform/README.md](platform/README.md) | Master index for all platform modules and kernel interfaces |
| [Console](platform/console/) | [platform/console/README.md](platform/console/README.md) | Streaming UTF-16→UTF-8 conversion, ZwWriteFile vs WriteConsoleW |
| [Filesystem](platform/fs/) | [platform/fs/README.md](platform/fs/README.md) | 14-variant struct stat offset map, Solaris missing d_type, drive bitmask enumeration |
| [Memory](platform/memory/) | [platform/memory/README.md](platform/memory/README.md) | Size-header trick for munmap, mmap2 page-shift, FreeBSD i386 inline asm hack |
| [Screen](platform/screen/) | [platform/screen/README.md](platform/screen/README.md) | Linux three-tier capture (X11→DRM→fbdev), macOS fork-based crash isolation |
| [Socket](platform/socket/) | [platform/socket/README.md](platform/socket/README.md) | Windows AFD driver internals, UEFI busy-poll async, i386 socketcall multiplexer |
| [System](platform/system/) | [platform/system/README.md](platform/system/README.md) | 5 PTY creation variants, PEB environment walking, SMBIOS UUID extraction |

### Layer 2: Kernel Interfaces
| Platform | README | Key Techniques |
|---|---|---|
| [Windows](platform/kernel/windows/) | [README.md](platform/kernel/windows/README.md) + [5 deep-dives](platform/kernel/windows/) | PEB walking, PE parsing, indirect syscalls, SSN resolution |
| [Linux](platform/kernel/linux/) | [README.md](platform/kernel/linux/README.md) | MIPS branch delay slot, i386 EBP save/restore, 7-arch syscall tables |
| [FreeBSD](platform/kernel/freebsd/) | [README.md](platform/kernel/freebsd/README.md) | RDX rval[1] clobbering, RISC-V T0 error indicator, early-clobber constraints |
| [macOS](platform/kernel/macos/) | [README.md](platform/kernel/macos/README.md) | svc #0x80 + X16, Mach traps, dyld ASLR slide calculation |
| [Solaris](platform/kernel/solaris/) | [README.md](platform/kernel/solaris/README.md) | int $0x91, multiplexed syscalls, getdents SIGSYS quirk |
| [UEFI](platform/kernel/uefi/) | [README.md](platform/kernel/uefi/README.md) | WRMSR GS_BASE, NOINLINE GUID trick, protocol function tables |
| [iOS](platform/kernel/ios/) | [README.md](platform/kernel/ios/README.md) | Same XNU kernel as macOS |
| [Android](platform/kernel/android/) | [README.md](platform/kernel/android/README.md) | Same Linux kernel, SELinux/Seccomp differences |
| [NetBSD](platform/kernel/netbsd/) | [README.md](platform/kernel/netbsd/README.md) | Not implemented |

### Layer 3: Libraries
| Module | README | Key Techniques |
|---|---|---|
| [Libraries](lib/) | [lib/README.md](lib/README.md) | Traits-based SHA-2, constant-time ECC Montgomery ladder, branchless ChaCha20-Poly1305, full TLS 1.3 handshake, DNS-over-HTTPS, fast DCT JPEG |

### Layer 4: Beacon
| Module | README | Key Techniques |
|---|---|---|
| [Beacon](beacon/) | [beacon/README.md](beacon/README.md) | Command dispatch table, full connection pipeline (DNS→TLS→HTTP→WebSocket), streaming JPEG over WebSocket |

## Position Independence: How It's Achieved

The entire binary has **only a `.text` section** — no `.data`, `.rdata`, `.rodata`, or `.bss`. This is accomplished through:

1. **PIC Transform LLVM Pass** (`tools/pic-transform/`): Converts global constants (strings, floats, arrays) into stack-local allocations with immediate stores. Uses register barriers (`asm("": "+r"(val))`) to prevent the optimizer from coalescing stack data back into `.rodata`.

2. **Compile-Time Hashing**: DJB2 hashes of symbol names are computed at compile time (`consteval`) and embedded as immediate constants — no string literals in the binary.

3. **No CRT**: Custom implementations of `memset`, `memcpy`, `memmove`, `operator new`/`delete`, and all string operations.

4. **No Static Data**: All "constants" (round constants for SHA-2, Huffman tables for JPEG, ECC curve parameters) are generated by `static` methods that return values on the stack.

5. **Stack-Based GUIDs** (UEFI): Protocol GUIDs constructed field-by-field at runtime with `NOINLINE` to prevent constant folding.

6. **No Import Tables** (Windows): All DLL functions resolved at runtime via PEB walking + PE export parsing.

## Build System

See `cmake/` for the build system:
- `CompilerFlags.cmake` — C++23, `-nostdlib`, `-fno-exceptions`, `-fno-rtti`, `-fno-jump-tables`
- `PICTransform.cmake` — LLVM pass acquisition and integration
- `Target.cmake` — Platform-specific link scripts and post-build processing
