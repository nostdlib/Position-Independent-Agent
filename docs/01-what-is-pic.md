# What Is Position-Independent Code?

Before reading any source code in this project, you need to understand a handful of foundational concepts. Skip this and the rest of the docs will feel like they're written in a foreign language.

---

## Programs vs Shellcode

A normal compiled program (an `.exe` on Windows, an ELF binary on Linux) depends on a lot of infrastructure to run. The operating system loads it at a predictable address, maps its sections into memory with the right permissions, resolves references to shared libraries (DLLs, `.so` files), and runs startup code that initializes global variables, sets up `stdin`/`stdout`, and parses command-line arguments. Only then does your `main()` function get called.

Shellcode doesn't get any of that. It's a blob of raw machine instructions that gets dropped into memory at an arbitrary address — maybe via process injection, maybe via an exploit, maybe via a custom loader. There's no OS loader parsing headers. There's no runtime initializing globals. The code just starts executing from byte zero.

"Position-independent" means the code makes no assumptions about where in memory it landed. No hardcoded addresses, no references to data at fixed offsets, no reliance on external libraries being loaded. Think of a normal program as a house with a fixed address and plumbing connections to city utilities. Shellcode is a self-contained camping tent you can pitch anywhere.

---

## Binary Sections

Compiled binaries aren't just a flat stream of instructions. They're divided into **sections**, each with a specific purpose:

| Section | What It Holds | Permissions |
|---------|--------------|-------------|
| `.text` | Executable code (your actual instructions) | Read + Execute |
| `.data` | Initialized global variables | Read + Write |
| `.rodata` / `.rdata` | Read-only data: string literals, lookup tables, constants | Read |
| `.bss` | Uninitialized global variables (zeroed at startup) | Read + Write |
| `.got` / `.plt` | Dynamic linking stubs (Global Offset Table, Procedure Linkage Table) | Read / Read + Execute |

Different binary formats use different naming conventions — ELF (Linux) calls it `.rodata`, PE (Windows) calls it `.rdata`, Mach-O (macOS) uses `__TEXT,__text` and `__DATA,__data` — but the concept is identical.

**The Golden Rule of this project:** the compiled binary must have ONLY a `.text` section. No `.data`, no `.rodata`, no `.bss`, no `.got`, no `.plt`. Nothing but code.

Why? If you have a single section, a loader can just copy the bytes into memory and jump to byte zero. If you have multiple sections, the loader needs to parse a section table, map each section to a separate memory region with different permissions, fix up cross-section references, and handle relocations. At that point you've reimplemented an OS loader, which defeats the whole purpose of shellcode.

One section = dumb copy + jump. That's the trick.

---

## What Is a Syscall?

Programs can't directly talk to hardware. They ask the operating system kernel to do things on their behalf — read a file, open a network socket, allocate memory. The mechanism for this is the **system call** (syscall).

In a normal program, you call a C library function like `write()`, which internally triggers the syscall. The C library is a convenience layer that handles the low-level details. A **direct syscall** skips the C library entirely and talks to the kernel via a special CPU instruction:

```
// Writing "Hello" to stdout on x86_64 Linux:
//   RAX = 1        (syscall number for write)
//   RDI = 1        (file descriptor: stdout)
//   RSI = buffer   (pointer to data)
//   RDX = 5        (number of bytes)
//   syscall         (CPU instruction — traps into kernel)
//   RAX now contains: bytes written, or -errno on failure
```

The `syscall` instruction flips the CPU from user mode to kernel mode. The kernel reads the registers, does the work, stuffs the result back in RAX, and returns.

This project uses direct syscalls because there is no C library. We compile with `-nostdlib`, so `write()`, `printf()`, `malloc()` — none of them exist. Direct syscalls are the only way to talk to the OS.

Each OS and CPU architecture has different syscall numbers. "write" is syscall 1 on x86_64 Linux, syscall 4 on i386 Linux, and syscall 64 on ARM64 Linux. Same operation, different numbers. This is one of the things that makes cross-platform support so much work.

---

## OS, Kernel, and C Library

These three terms get conflated constantly. They're different things:

**Kernel:** The actual core of the operating system. The Linux kernel, the Windows NT kernel, XNU on macOS. It manages hardware, memory, processes, and provides syscalls.

**C Library (libc):** A user-space library that wraps syscalls into a friendly API. glibc on Linux, musl on Alpine, msvcrt on Windows. When you call `printf()` or `malloc()`, you're calling libc, which internally makes syscalls.

**Operating System:** The full package — kernel plus C library plus system utilities plus everything else.

Most programs depend heavily on the C library. This project bypasses it entirely. The "Platform" layer in this codebase is a custom replacement for the subset of libc functionality the agent actually needs.

---

## What Is Freestanding Code?

C and C++ programs come in two flavors:

**Hosted:** A normal program. It has access to `printf`, `malloc`, `std::string`, exceptions, RTTI, and the full standard library. The compiler and linker assume a runtime environment exists.

**Freestanding:** Code that has NO runtime support whatsoever. No standard library, no startup code, no heap, no exception handling. This is the environment you'd be in writing an OS kernel, a bootloader, or firmware.

The compiler flags that create a freestanding environment (from `cmake/CompilerFlags.cmake`):

| Flag | Effect |
|------|--------|
| `-nostdlib` | Don't link the C standard library |
| `-fno-exceptions` | Disable C++ try/catch (exception tables land in data sections) |
| `-fno-rtti` | Disable `dynamic_cast`/`typeid` (type info tables land in data sections) |
| `-fno-builtin` | Don't replace functions like `memcpy` with intrinsics that might reference external symbols |
| `-fno-asynchronous-unwind-tables` | Don't generate `.eh_frame` (unwind tables are data sections) |
| `-ffunction-sections` | Put each function in its own section (enables dead code elimination) |

Every one of these flags exists for one of two reasons: prevent extra sections from being generated, or survive without a runtime. The compiler *wants* to put things in data sections — these flags are the first line of defense.

---

## CPU Architectures

This project targets 7 CPU architectures. Each has different registers, instruction formats, and syscall conventions:

| Architecture | Description | Where You'll Find It |
|-------------|-------------|---------------------|
| **x86 (i386)** | 32-bit Intel/AMD | Legacy PCs, VMs, embedded |
| **x86_64** | 64-bit Intel/AMD | Modern desktops, servers |
| **aarch64 (ARM64)** | 64-bit ARM | Phones, Apple Silicon Macs, Raspberry Pi 4+ |
| **armv7a** | 32-bit ARM | Older phones, tons of embedded devices |
| **riscv32** | 32-bit RISC-V | Open-source ISA, growing ecosystem |
| **riscv64** | 64-bit RISC-V | Open-source ISA, servers, SBCs |
| **mips64** | 64-bit MIPS | Routers, embedded gear, some servers |

Each architecture creates work in this project: different syscall numbers, different inline assembly for `System::Call`, different compiler runtime helpers (32-bit CPUs need software 64-bit division), different binary format quirks.

---

## Cross-Compilation and Target Triples

**Cross-compilation** means compiling on one machine for a different target. You sit at your x86_64 Linux laptop and produce a binary for an ARM Android phone, or a RISC-V dev board, or a Windows machine.

The compiler needs to know what target you're building for. A **target triple** encodes this as `<architecture>-<vendor>-<os>-<environment>`:

| Triple | Meaning |
|--------|---------|
| `x86_64-unknown-linux-gnu` | 64-bit Intel, Linux, GNU libc ABI |
| `i386-unknown-linux-gnu` | 32-bit Intel, Linux, GNU libc ABI |
| `arm64-apple-macos11` | ARM64, macOS 11+ |
| `x86_64-pc-windows-gnu` | 64-bit Intel, Windows, MinGW ABI |
| `aarch64-unknown-freebsd` | ARM64, FreeBSD |

The compiler uses the triple to select instruction encoding, calling conventions, and the object file format. This project sets `CMAKE_SYSTEM_NAME = Generic` in the toolchain file, which tells CMake "we're not targeting any specific OS" — a freestanding environment that cross-compiles for all 8 platforms from a single host.

---

## Binary Formats

| Format | Used By | Section Names |
|--------|---------|--------------|
| **ELF** (Executable and Linkable Format) | Linux, FreeBSD, Solaris, Android | `.text`, `.rodata`, `.data` |
| **PE/COFF** (Portable Executable) | Windows, UEFI | `.text`, `.rdata`, `.data` |
| **Mach-O** | macOS, iOS | `__TEXT,__text`, `__DATA,__data` |

The build system handles format differences per-platform. Different linker flags, different section naming, different post-build verification — but the source code doesn't need to care.

---

## UEFI

UEFI (Unified Extensible Firmware Interface) replaced the old BIOS as the firmware that runs before your OS boots. UEFI applications run in a pre-OS environment with their own API — networking, filesystem access, display output, all provided by the firmware itself.

This means the agent can run even before Windows or Linux loads. UEFI uses PE/COFF format with a special `EFI_APPLICATION` subsystem and provides its own protocol-based API instead of syscalls. It's a genuinely different execution environment from everything else the project targets. See `docs/04-entry-point.md` for how UEFI bootstrapping works.

---

## What "No Standard Library" Means In Practice

The core layer of this project (`src/core/`) reimplements `memset`, `memcpy`, `strlen`, `strcmp`, `printf`-style formatting, Base64, hashing, math operations, even 64-bit division on 32-bit CPUs. All from scratch.

"Why not just use the ones that already exist?" Because they don't exist here. Functions like `memcpy` normally come from the C library (libc), which is a shared library loaded by the OS. Position-independent shellcode can't depend on shared libraries being present.

Here's the thing that catches people: even with `-nostdlib`, the **compiler itself** still generates calls to `memset` and `memcpy` behind your back — zeroing a struct, copying a buffer, that kind of thing. If these functions don't exist at link time, you get `undefined reference to 'memset'`. So the project must provide its own implementations just to satisfy the linker.

This is one of the hardest aspects of the whole project. Reimplementing the things you take for granted teaches you what the standard library actually does under the hood. See `docs/06-memory-and-strings.md` for the details.

---

## LLVM/Clang and Why Not GCC

The toolchain requires LLVM 22+ / Clang. This isn't arbitrary — the project uses a custom LLVM pass (`pic-transform`) that modifies the compiler's intermediate representation to eliminate data sections. LLVM passes can only run inside the LLVM/Clang pipeline. GCC has a completely different internal architecture with no equivalent extension point. See `docs/14-pic-transform.md` for how the pass works.

---

## Next Steps

Now that you have the vocabulary, start with `docs/02-project-overview.md` to understand the architecture, then follow the reading order in `docs/ONBOARDING.md`.
