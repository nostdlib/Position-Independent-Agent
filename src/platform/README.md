[< Back to Source](../README.md) | [< Back to Project Root](../../README.md)

# Platform Layer

Cross-platform abstraction providing OS-independent interfaces for I/O, networking, memory, display, and process management. Every implementation uses **direct syscalls or firmware protocols** — no CRT, no SDK, no libc.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        platform.h  (aggregate header)               │
├────────┬──────────┬─────────┬─────────┬──────────┬──────────────────┤
│Console │Filesystem│ Memory  │ Screen  │ Socket   │     System       │
│        │          │         │         │          │ DateTime Random  │
│ Write  │ File     │Allocator│GetDevice│ Create   │ MachineID Env    │
│ Logger │ Directory│ new/del │ Capture │ Open     │ Pipe Process     │
│        │ Iterator │         │         │ Read     │ Pty ShellProcess │
│        │ Path     │         │         │ Write    │ SystemInfo       │
├────────┴──────────┴─────────┴─────────┴──────────┴──────────────────┤
│                         Kernel Interface                            │
│ ┌─────────┬─────────┬───────┬─────┬──────────┬────────┬──────────┐  │
│ │ Windows │  Linux  │FreeBSD│macOS│ Solaris  │  UEFI  │ Android  │  │
│ │ PEB/PE  │ syscall │syscall│ XNU │ int 0x91 │Protocol│ (Linux)  │  │
│ │ Indirect│ svc/int │int 80 │svc80│ svc/ecall│ Tables │          │  │
│ │ Syscall │ ecall   │ ecall │     │          │        │  iOS     │  │
│ └─────────┴─────────┴───────┴─────┴──────────┴────────┴──────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## Modules

- [Console](console/README.md) — Streaming UTF-16→UTF-8 conversion, `ZwWriteFile` vs `WriteConsoleW`, ANSI-colored structured logging
- [Filesystem](fs/README.md) — RAII file I/O, 14-variant `struct stat` offsets, Solaris missing `d_type`, Windows drive bitmask enumeration
- [Memory](memory/README.md) — Size-header trick for `munmap`, `mmap2` page-shift, FreeBSD i386 inline asm for 64-bit `off_t`
- [Screen](screen/README.md) — Linux three-tier capture (X11→DRM→fbdev), macOS fork-based CoreGraphics probing, UEFI GOP
- [Socket](socket/README.md) — Windows AFD driver networking, i386 `socketcall` multiplexer, UEFI busy-poll TCP, BSD `sin_len`
- [System](system/README.md) — 5 PTY creation variants, PEB environment walking, SMBIOS UUID, `fork`+`execve` process model

## Kernel Interfaces

- [Windows](kernel/windows/README.md) — PEB walking, PE parsing, indirect syscalls, NTDLL/Win32 wrappers (x86_64, i386, ARM64, ARM32)
- [Linux](kernel/linux/README.md) — 7 architectures, MIPS `$a3` error flag, i386 EBP save/restore, per-arch syscall numbers
- [FreeBSD](kernel/freebsd/README.md) — Carry-flag error model, RDX `rval[1]` clobbering, RISC-V T0 error indicator (x86_64, i386, AArch64, RV64)
- [macOS](kernel/macos/README.md) — `svc #0x80` + X16, Mach traps, dyld ASLR slide + Mach-O parsing (x86_64, AArch64)
- [Solaris](kernel/solaris/README.md) — `int $0x91` SVR4 trap, multiplexed syscalls, `getdents` SIGSYS quirk (x86_64, i386, AArch64)
- [UEFI](kernel/uefi/README.md) — Protocol function tables, `WRMSR` GS_BASE, Microsoft x64 ABI, NOINLINE GUID trick (x86_64, AArch64)
- [iOS](kernel/ios/README.md) — Same XNU kernel as macOS (AArch64)
- [Android](kernel/android/README.md) — Same Linux kernel, SELinux/Seccomp differences
- [NetBSD](kernel/netbsd/README.md) — Not yet implemented

## Platform Support Matrix

### Module × Platform

| Module | Windows | Linux | Android | macOS | iOS | FreeBSD | Solaris | UEFI |
|---|---|---|---|---|---|---|---|---|
| **Console** | Full | Full | Full | Full | Full | Full | Full | Full |
| **Filesystem** | Full | Full | Full | Full | Full | Full | Full | Full |
| **Memory** | Full | Full | Full | Full | Full | Full | Full | Full |
| **Screen** | Full | Full | Full | Full | Stub | Full | Full | Full |
| **Socket** | Full (AFD) | Full | Full | Full | Full | Full | Full | Full (TCP proto) |
| **DateTime** | Full | Full | Full | Full | Full | Full | Full | Full |
| **Random** | Full | Full | Full | Full | Full | Full | Full | Full |
| **MachineID** | SMBIOS | /etc/machine-id | boot_id | /etc/machine-id | — | /etc/machine-id | — | Stub |
| **Environment** | PEB | /proc/self/environ | /proc/self/environ | Stub | Stub | Stub | Stub | Stub |
| **Pipe** | Full | Full | Full | Full | Full | Full | Full | — |
| **Process** | Full | Full | Full | Full | Full | Full | Full | — |
| **PTY** | — | Full | Full | Full | Full | Full | Full | — |
| **ShellProcess** | cmd.exe | /bin/sh | /bin/sh | /bin/sh | /bin/sh | /bin/sh | /bin/sh | — |

### Architecture × Platform

| | i386 | x86_64 | ARMv7-A | AArch64 | RV32 | RV64 | MIPS64 |
|---|---|---|---|---|---|---|---|
| **Windows** | Yes | Yes | Yes | Yes | — | — | — |
| **Linux** | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| **Android** | Yes | Yes | Yes | Yes | — | — | — |
| **macOS** | — | Yes | — | Yes | — | — | — |
| **iOS** | — | — | — | Yes | — | — | — |
| **FreeBSD** | Yes | Yes | — | Yes | — | Yes | — |
| **Solaris** | Yes | Yes | — | Yes | — | — | — |
| **UEFI** | — | Yes | — | Yes | — | — | — |

## Design Principles

1. **Zero Dependencies** — No CRT, no SDK headers, no libc. All platform access is via direct syscalls or firmware protocols.
2. **Position-Independent** — No static data, no import tables, no relocations. All code runs from `.text`-only memory.
3. **RAII Everywhere** — File, Socket, Pipe, Process, Pty, ShellProcess are all move-only, stack-only RAII types with automatic cleanup.
4. **Uniform Error Handling** — All fallible operations return `Result<T, Error>`. Platform-specific status codes (NTSTATUS, errno, EFI_STATUS) are converted to a unified `Error` type.
5. **Compile-Time Platform Selection** — Platform-specific implementations are selected via `#if defined(PLATFORM_*)` at compile time. No runtime dispatch overhead.
6. **Stack-Only Enforcement** — Core types prohibit heap allocation to ensure predictability in memory-constrained environments.
