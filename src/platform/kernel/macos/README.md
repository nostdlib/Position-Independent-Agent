[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# macOS (XNU) Kernel Interface

Position-independent macOS/XNU syscall layer for **x86_64** and **AArch64** (Apple Silicon). Combines BSD syscalls, Mach kernel traps, and runtime dyld framework resolution via Mach-O binary parsing to locate `dlopen`/`dlsym` without link-time dependencies.

## XNU Dual-Personality Kernel

XNU has two syscall interfaces:
- **BSD syscalls** (class 2) — POSIX file/network/process operations, prefixed with `0x2000000`
- **Mach traps** (class 1) — kernel IPC, task management, using **negative** syscall numbers

Both are invoked through the same `syscall`/`svc` instruction, distinguished by the syscall number.

## BSD Syscall Dispatch

### x86_64: Standard with Carry Flag

```asm
mov rax, 0x2000003    ; SYS_READ = SYSCALL_CLASS_UNIX | 3
mov rdi, arg1
mov rsi, arg2
mov rdx, arg3
syscall
jnc 1f                ; carry clear = success
negq %%rax            ; carry set = negate errno
1:
```

Same carry-flag pattern as FreeBSD. **RDX is clobbered by rval[1]** (secondary return value), requiring `"+r"` constraint.

### AArch64: Two Key Differences from Linux

```asm
mov x16, 0x2000003    ; syscall number in X16 (NOT x8)
mov x0, arg1
; ...
svc #0x80             ; supervisor call 0x80 (NOT svc #0)
b.cc 1f               ; branch on carry clear
neg x0, x0            ; negate errno
1:
```

Two critical divergences from Linux ARM64:
1. **`svc #0x80`** instead of `svc #0` — the immediate value matters on XNU
2. **Syscall number in `X16`** instead of `X8` — X8 is a regular argument register on macOS

**X1 is clobbered by rval[1]** just like RDX on x86_64.

## Mach Traps: Negative Syscall Numbers

Mach kernel traps use negative numbers and do **not** set the carry flag — they return `kern_return_t` directly:

| Trap | Number | Purpose |
|---|---|---|
| `mach_task_self` | -28 | Get current task port |
| `mach_reply_port` | -26 | Allocate a reply port |
| `mach_msg` | -31 | Send/receive IPC message |

### 7-Argument mach_msg_trap

`mach_msg` takes 7 arguments — one more than the standard `System::Call` supports (0-6). The runtime uses custom assembly:

**x86_64:**
```asm
pushq %[notify]        ; push 7th arg onto stack
mov rax, -31           ; MACH_TRAP_MACH_MSG
syscall
addq $8, %%rsp         ; clean up stack
```

**AArch64:**
```asm
mov x16, -31           ; trap number
; x0-x5 hold args 1-6
mov x6, arg7           ; 7th arg in x6 (ARM64 has plenty of registers)
svc #0x80
```

## Dynamic Framework Resolution via dyld

The most complex technique in the macOS layer. Position-independent code can't have import tables pointing to system frameworks, and macOS lacks a PEB-like structure to walk loaded modules. The solution: use Mach IPC to find dyld, parse its Mach-O symbol table to extract `_dlopen` and `_dlsym`, then use those to load frameworks.

### Resolution Flow

```
1. mach_task_self()                           → get task port
      │
2. mach_msg(task_info, TASK_DYLD_INFO)        → via Mach IPC
      │                                          returns dyld_all_image_infos address
      │
3. Parse dyld's Mach-O headers:
      ├─ Find __TEXT segment (vmAddr for ASLR slide)
      ├─ Find __LINKEDIT segment (symbol table location)
      ├─ Find LC_SYMTAB load command (symbol/string table offsets)
      │
4. Compute ASLR slide:
      slide = (USIZE)header - textSeg→VmAddr
      │
5. Compute linked symbol table base:
      linkeditBase = slide + linkeditSeg→VmAddr - linkeditSeg→FileOff
      │
6. Walk Nlist64 symbol table:
      for each symbol:
        name = stringTable[nlist.strIndex]
        if Djb2::Hash(name) == hash("_dlopen"):
          dlopen = slide + nlist.value
      │
7. dlopen("CoreGraphics.framework/CoreGraphics")
      │
8. dlsym(handle, "CGMainDisplayID")           → function pointer
```

### ASLR Slide Calculation

The key insight is that the Mach-O `__TEXT` segment has a compile-time `VmAddr`, but ASLR loads the binary at a random address. The **slide** is the difference:

```c
USIZE slide = (USIZE)machHeader - textSegment->VmAddr;
```

All symbol table addresses (stored as file offsets from the binary's preferred base) need this slide added to get their actual runtime addresses.

### Mach-O Structures

| Structure | Magic/Cmd | Purpose |
|---|---|---|
| `MachHeader64` | `0xFEEDFACF` | Mach-O 64-bit header, contains `ncmds` load commands |
| `SegmentCommand64` | `LC_SEGMENT_64` (0x19) | Describes a memory segment (`__TEXT`, `__LINKEDIT`) |
| `SymtabCommand` | `LC_SYMTAB` (0x02) | Symbol table and string table offsets |
| `Nlist64` | — | Symbol entry: `strIndex`, `type`, `sect`, `value` (address) |

## BSD Constants (Shared with FreeBSD)

macOS and FreeBSD share BSD heritage — most constants are identical:

| Constant | Value | Same as FreeBSD |
|---|---|---|
| `O_CREAT` | `0x0200` | Yes |
| `O_NONBLOCK` | `0x0004` | Yes |
| `MAP_ANONYMOUS` | `0x1000` | Yes |
| `SOL_SOCKET` | `0xFFFF` | Yes |
| `EINPROGRESS` | `36` | Yes |

Notable macOS-specific differences:

| Constant | macOS | FreeBSD | Linux |
|---|---|---|---|
| `AT_FDCWD` | `-2` | `-100` | `-100` |
| `AT_REMOVEDIR` | `0x0080` | `0x0800` | `0x200` |
| `O_DIRECTORY` | `0x100000` | `0x20000` | varies |

## Structures

| Structure | Notes |
|---|---|
| `BsdDirent64` | Has `Seekoff` field (not `Off` like Linux/FreeBSD) |
| `Timeval` | Both `Sec` and `Usec` are **8 bytes** (kernel ABI for 64-bit processes) |
| `Pollfd` | Standard |
