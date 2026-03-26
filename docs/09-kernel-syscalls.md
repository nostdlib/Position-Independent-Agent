# Kernel Interface: Talking to the Operating System

This document explains how position-independent code communicates with operating
system kernels across Linux, Windows, macOS, FreeBSD, and UEFI firmware. There
is no standard library here, no libc, no Win32 API layer. Every interaction with
the OS is a raw interface call -- a syscall instruction, a PEB walk, or a
function pointer table lookup. Understanding this layer is non-negotiable if you
want to understand the rest of the project.

If you have not read [01-what-is-pic.md](01-what-is-pic.md) and
[04-entry-point.md](04-entry-point.md), start there. This document assumes you
understand why the code cannot use import tables or relocations.

---

## Linux Syscalls at the CPU Level

A syscall is a hardware-mediated transition from user mode to kernel mode. On
x86_64 Linux, the contract is dead simple:

1. Load the syscall number into `RAX`.
2. Load arguments into `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9` (in that order).
3. Execute the `syscall` instruction.
4. The CPU switches privilege levels, the kernel reads the registers, does the
   work, and puts the return value back in `RAX`.

A concrete example -- writing "Hello" to stdout:

```
// System::Call(SYS_WRITE, STDOUT_FILENO, buffer, 5)
//
//   RAX = 1        (syscall number for write)
//   RDI = 1        (file descriptor: stdout)
//   RSI = buffer   (pointer to data)
//   RDX = 5        (number of bytes)
//   syscall         (trap into kernel)
//   RAX now holds the return value (bytes written, or negative errno)
```

That is the entire userspace-kernel contract on Linux. No function calls, no
stack frames crossing the boundary, no shared memory tricks. Registers in,
instruction fires, registers out.

On ARM64, the mechanism is similar but uses `SVC #0` instead of `syscall`, with
arguments in `X0`-`X5` and the syscall number in `X8`. On RISC-V, it is `ecall`
with `A7` holding the number and `A0`-`A5` for arguments.

The project wraps all of this behind architecture-specific inline assembly. See
`src/platform/kernel/linux/syscall.x86_64.h` and its siblings for the actual
implementation.

---

## Why Syscall Numbers Differ Per Architecture

If you look at the header `src/platform/kernel/linux/syscall.h`, you will see
this immediately:

```cpp
#if defined(ARCHITECTURE_X86_64)
#include "platform/kernel/linux/syscall.x86_64.h"
#elif defined(ARCHITECTURE_I386)
#include "platform/kernel/linux/syscall.i386.h"
#elif defined(ARCHITECTURE_AARCH64)
#include "platform/kernel/linux/syscall.aarch64.h"
#elif defined(ARCHITECTURE_ARMV7A)
#include "platform/kernel/linux/syscall.armv7a.h"
#elif defined(ARCHITECTURE_RISCV64)
#include "platform/kernel/linux/syscall.riscv64.h"
#elif defined(ARCHITECTURE_RISCV32)
#include "platform/kernel/linux/syscall.riscv32.h"
#elif defined(ARCHITECTURE_MIPS64)
#include "platform/kernel/linux/syscall.mips64.h"
#else
#error "Unsupported architecture"
#endif
```

Seven different headers, seven different syscall number tables. Here is why:

| Syscall | x86_64 | i386 | ARM64 |
|---------|--------|------|-------|
| write   | 1      | 4    | 64    |
| read    | 0      | 3    | 63    |
| open    | 2      | 5    | --    |
| openat  | 257    | 295  | 56    |
| close   | 3      | 6    | 57    |

Each architecture's table was designed independently, at different points in
Linux's history. x86_64 got a clean slate when the 64-bit port was written.
i386 inherited the original Linux numbering from the early 1990s. ARM64 follows
a newer rationalized convention. MIPS inherited from IRIX/SVR4, making its
numbers completely alien compared to everything else.

It gets worse. Some syscalls do not exist on all architectures. ARM64 has no
`open()` -- only `openat()` (directory-relative). RISC-V 32-bit has no
`lseek()`, only `llseek()` for 64-bit file offsets. Even constant values
diverge: `O_CREAT` is `0x0040` on most architectures but `0x0100` on MIPS,
because MIPS inherited IRIX values.

The project handles this with per-architecture headers and `#if` blocks in the
shared `syscall.h`. There is no way to abstract over this cleanly; you just
have to define every number for every target.

---

## The i386 socketcall Multiplexer

On i386 Linux, there are no individual syscalls for socket operations. Every
network call -- `socket()`, `bind()`, `connect()`, `send()`, `recv()` -- goes
through a single multiplexer: `SYS_SOCKETCALL` (number 102).

The calling convention:

```
RAX = 102              (SYS_SOCKETCALL)
EBX = operation code   (1=socket, 2=bind, 3=connect, 9=send, 10=recv)
ECX = pointer to argument array on the stack
```

You pack the real arguments into a contiguous array, then pass a pointer to
that array. The kernel unpacks them on the other side.

Why does this exist? The i386 syscall table was designed in the early days when
Linux networking was new. Rather than allocating a dozen syscall numbers, the
developers bundled all of networking under one. On x86_64, ARM64, and every
architecture designed after i386, each socket operation has its own syscall
number. But i386 is stuck with this for backward compatibility.

This is one of those details that makes cross-architecture networking code
genuinely painful. The socket abstraction layer (see
[ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md) for the directory layout) has to
handle both paths.

---

## Windows: Walking the PEB Step by Step

Windows is a fundamentally different problem. On Linux, you issue syscalls with
known numbers. On Windows, syscall numbers change between OS versions -- they
are not a stable interface. The stable interface is the DLL export table, but
position-independent code cannot use import tables.

The solution: walk the Process Environment Block (PEB) at runtime to find
loaded DLLs and resolve function addresses by hash. This is defined in
`src/platform/kernel/windows/peb.h` and `peb.cc`.

### Step 1: Get the PEB pointer

The PEB pointer lives at a fixed offset in the Thread Environment Block (TEB),
which the CPU maps to a segment register:

```cpp
PPEB GetCurrentPEB(VOID)
{
    PPEB peb;
#if defined(PLATFORM_WINDOWS_X86_64)
    __asm__("movq %%gs:%1, %0" : "=r"(peb) : "m"(*(PUINT64)(0x60)));
#elif defined(PLATFORM_WINDOWS_I386)
    __asm__("movl %%fs:%1, %0" : "=r"(peb) : "m"(*(PUINT32)(0x30)));
#elif defined(PLATFORM_WINDOWS_AARCH64)
    __asm__("ldr %0, [x18, #%1]" : "=r"(peb) : "i"(0x60));
#endif
    return peb;
}
```

On x86_64, `GS:[0x60]` points to the PEB. On i386, it is `FS:[0x30]`. On
ARM64, register `X18` holds the TEB base, and the PEB is at offset `0x60`.
These offsets are baked into Windows and have not changed across versions.

### Step 2: Walk the loaded module list

The PEB contains a pointer to `PEB_LDR_DATA`, which holds the heads of three
circular doubly-linked lists of loaded modules:

```
PEB
  -> LoaderData (PEB_LDR_DATA)
       -> InLoadOrderModuleList
       -> InMemoryOrderModuleList
       -> InInitializationOrderModuleList
```

Each node in these lists is an `LDR_DATA_TABLE_ENTRY` containing the module's
base address, entry point, and name (as a `UNICODE_STRING`). The code walks
`InMemoryOrderModuleList`, hashing each module's `BaseDllName` with DJB2 and
comparing against the target hash:

```cpp
PVOID GetModuleHandleFromPEB(UINT64 moduleNameHash)
{
    PPEB peb = GetCurrentPEB();
    PLIST_ENTRY list = &peb->LoaderData->InMemoryOrderModuleList;
    PLIST_ENTRY entry = list->Flink;

    while (entry != list)
    {
        PLDR_DATA_TABLE_ENTRY module = CONTAINING_RECORD(
            entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderModuleList);

        if (module->BaseDllName.Buffer != nullptr &&
            Djb2::Hash(module->BaseDllName.Buffer) == moduleNameHash)
            return module->DllBase;

        entry = entry->Flink;
    }
    return nullptr;
}
```

### Step 3: Parse the PE export table

Once you have the module base address, you parse its PE headers to find the
export directory. Walk the `AddressOfNames` array, hash each exported function
name, and when the hash matches, look up the corresponding address in
`AddressOfFunctions`. This is handled by `GetExportAddress()` in
`src/platform/kernel/windows/pe.h`.

### Step 4: Use the resolved function pointer

You now have a raw function pointer to, say, `NtCreateFile` inside
`ntdll.dll`. Cast it and call it. The project resolves function pointers at
first use and keeps them on the stack or in registers -- never in global
variables, since that would require a `.data` section. See
[01-what-is-pic.md](01-what-is-pic.md) for why data sections are forbidden.

Every single Windows API call in this project goes through this pipeline.

---

## CONTAINING_RECORD: Pointer Arithmetic for Embedded Lists

Windows kernel structures use intrusive linked lists. A `LIST_ENTRY` (with
`Flink` and `Blink` pointers) is embedded inside a larger structure. When you
iterate the list, you have a pointer to the `LIST_ENTRY`, but you need the
containing structure.

`CONTAINING_RECORD` does the arithmetic:

```cpp
#define CONTAINING_RECORD(address, type, field) \
    ((type *)((PCHAR)(address) - (USIZE)(&((type *)0)->field)))
```

In plain terms: take the pointer to the embedded field, subtract the field's
offset within the structure, and you get the structure's base address. If
`InMemoryOrderModuleList` sits at offset 16 within `LDR_DATA_TABLE_ENTRY`,
then `structBase = listEntryPtr - 16`.

This is the same idea as the Linux kernel's `container_of` macro. It shows up
everywhere in PEB-walking code and is worth understanding once so you can read
the rest without stumbling.

---

## NTSTATUS: The NT Native Error System

Windows has two parallel error systems. The familiar Win32 error codes (from
`GetLastError()`) are decimal numbers used by `kernel32.dll` functions.
`NTSTATUS` values are 32-bit hex codes used by `ntdll.dll`, the NT Native API.

Since this project calls the NT Native API directly -- `ZwCreateFile` instead
of `CreateFile`, `NtAllocateVirtualMemory` instead of `VirtualAlloc` --
everything returns `NTSTATUS`.

The encoding:

| Prefix     | Meaning        | Example                         |
|------------|----------------|---------------------------------|
| 0x00000000 | Success        | STATUS_SUCCESS                  |
| 0x00000001+ | Informational | STATUS_BUFFER_OVERFLOW          |
| 0x80000000+ | Warning       | STATUS_BUFFER_OVERFLOW (alt)    |
| 0xC0000000+ | Error         | STATUS_ACCESS_DENIED (0xC0000022) |

The `NT_SUCCESS(status)` macro checks whether the status is non-negative
(success or informational). The project never touches Win32 error codes.

---

## The AFD Driver: Windows Networking Without Winsock

Normal Windows programs use Winsock2 (`ws2_32.dll`) for networking. Under the
hood, Winsock talks to the AFD (Auxiliary Function Driver), a kernel driver
that handles TCP/IP socket operations. This project skips Winsock entirely and
talks to AFD directly.

The process:

1. Open `\\Device\\Afd\\Endpoint` via `ZwCreateFile` with special Extended
   Attributes that tell the kernel "this is a socket, not a regular file."
2. Issue IOCTLs to the resulting handle for all socket operations:

```
IOCTL_AFD_BIND    = 0x12003
IOCTL_AFD_CONNECT = 0x12007
IOCTL_AFD_SEND    = 0x1201F
IOCTL_AFD_RECV    = 0x12017
```

This is undocumented Microsoft territory. The AFD driver interface is not part
of any public SDK. But it is stable across Windows versions (Winsock itself
depends on it), and it avoids any dependency on `ws2_32.dll` -- which matters
because loading extra DLLs in position-independent code adds complexity and
detection surface.

See the socket implementation at `src/platform/socket/windows/socket.cc`.

---

## macOS and the XNU Kernel

macOS runs XNU, a hybrid of the Mach microkernel and FreeBSD's BSD layer. The
BSD syscall interface is conceptually similar to Linux, but the numbers are
different, and on x86_64 there is an important twist: syscall numbers carry a
class prefix.

BSD syscalls are `0x2000000 + number`:

```
write = 0x2000004
read  = 0x2000003
open  = 0x2000005
```

Mach syscalls use class `0x1000000`. The class prefix tells the kernel which
subsystem should handle the call. On ARM64 macOS, the convention is closer to
standard ARM64 Linux (arguments in `X0`-`X5`, syscall number in `X16`, trap
via `SVC #0x80`), but the numbers still differ.

iOS shares the same XNU kernel, so the syscall interface is identical to
macOS. A few macOS-specific concerns: `sysctl` is used for system information
queries, and aggressive `ptrace` restrictions (particularly `PT_DENY_ATTACH`)
affect debugging and process inspection.

The macOS platform code lives under `src/platform/kernel/macos/`.

---

## UEFI: No Kernel, No Syscalls

UEFI is a completely different world. There is no kernel. There are no
syscalls. The firmware hands you a function pointer table and you call into it
directly.

At entry, a UEFI application receives two arguments: an image handle and a
pointer to the `EFI_SYSTEM_TABLE`. That table is defined in
`src/platform/kernel/uefi/efi_system_table.h`:

```cpp
typedef struct
{
    EFI_GUID VendorGuid;
    PVOID VendorTable;
} EFI_CONFIGURATION_TABLE;
```

The System Table contains pointers to Boot Services and Runtime Services, each
of which is a structure full of function pointers -- essentially a vtable.
To find a specific driver (networking, filesystem, display), you call
`LocateProtocol` with a GUID:

```cpp
SystemTable->BootServices->LocateProtocol(&TCP4_GUID, nullptr, &protocol);
```

GUIDs are 128-bit identifiers. In normal code, these would live as constants in
`.rodata`. But position-independent code cannot have a data section. The
solution: construct GUIDs field-by-field on the stack using immediate values
encoded directly in the instructions. The well-known GUIDs (ACPI, SMBIOS, etc.)
are defined as macros in the header:

```cpp
#define EFI_ACPI_20_TABLE_GUID  \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
```

When the compiler sees this in a local variable initializer, it emits `mov`
instructions with the values as immediates -- no memory load from a data
section required. This is the same trick described in
[06-memory-and-strings.md](06-memory-and-strings.md) for string literals.

The UEFI platform code is under `src/platform/kernel/uefi/`.

---

## FreeBSD: BSD Syscalls Without the Quirks

FreeBSD uses BSD-style syscalls with different numbers from Linux, but it avoids
some of Linux's historical baggage. Most notably, there is no `socketcall`
multiplexer. Even on i386 FreeBSD, each socket operation has its own syscall
number. This makes the networking code simpler on FreeBSD than on i386 Linux.

Things that will trip you up:

- **sockaddr structures** have a `sin_len` field. This is a BSD extension that
  Linux dropped. If you zero-initialize a `sockaddr_in` and forget `sin_len`,
  the kernel will reject it.
- **AF_INET6** is `28` on FreeBSD, versus `10` on Linux and `30` on macOS.
  Address family constants are not portable.
- **ELF OSABI byte** must be set to `9` (`ELFOSABI_FREEBSD`). The FreeBSD
  kernel checks this field and flat-out refuses to load binaries with the wrong
  value. Linux is lenient about this; FreeBSD is not. The build system (see
  [03-build-system.md](03-build-system.md)) handles setting this correctly.

The FreeBSD platform code lives under `src/platform/kernel/freebsd/`.

---

## Summary: Five OS Interfaces, One Codebase

| Platform | Mechanism                   | Stability of numbers | Key quirk                          |
|----------|-----------------------------|----------------------|------------------------------------|
| Linux    | `syscall`/`svc`/`ecall`     | Stable per-arch      | Numbers differ per arch; socketcall on i386 |
| Windows  | PEB walk + NT Native API    | DLL exports stable; syscall numbers are not | AFD driver for networking |
| macOS    | BSD syscalls with class prefix | Stable              | `0x2000000 +` prefix on x86_64    |
| FreeBSD  | BSD syscalls                | Stable               | `sin_len` field; OSABI check       |
| UEFI     | Function pointer tables     | Stable per-spec      | No kernel; GUIDs built on stack    |

The platform abstraction layer (described in
[ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md)) unifies all of this behind a
common interface. Code above the platform layer calls `System::Call()` or
resolves functions through the platform API without knowing which of these five
mechanisms is executing underneath.

---

## Further Reading

- [01-what-is-pic.md](01-what-is-pic.md) -- Why data sections and import
  tables are forbidden
- [04-entry-point.md](04-entry-point.md) -- How execution begins without a
  standard runtime
- [05-core-types.md](05-core-types.md) -- The type system used across all
  platforms
- [06-memory-and-strings.md](06-memory-and-strings.md) -- Stack-constructed
  strings and the GUID trick
- [ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md) -- Directory layout of the
  platform abstraction layer
