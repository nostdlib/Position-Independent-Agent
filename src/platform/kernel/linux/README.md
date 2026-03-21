[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# Linux Kernel Interface

Position-independent Linux syscall layer supporting **7 architectures** — the widest architecture coverage in the project. Each architecture has its own trap instruction, register convention, syscall number table, and in some cases a fundamentally different error model.

## Error Handling: Negative Return vs $A3 Flag

All architectures except MIPS use the **negative return** model:

```
Return >= 0  →  success (value is the result)
Return < 0   →  failure (negate to get errno)
```

### MIPS64: The Odd One Out

MIPS64 is the only Linux architecture that uses a **separate error register** — mirroring the BSD carry-flag pattern rather than Linux's standard negative-return convention:

```asm
syscall                    ; issue syscall
beqz $7, 1f               ; $a3 ($7) = error flag: 0 = success
nop                        ; ← MIPS branch delay slot (mandatory!)
negu $2, $2                ; error: negate $v0 to make it negative
1:
```

The `nop` after `beqz` is **not optional** — MIPS executes the instruction in the branch delay slot (the instruction immediately after a branch) regardless of whether the branch is taken. Without the `nop`, the `negu` would always execute.

MIPS also clobbers an unusually large set of registers: `$1 (at)`, `$3 (v1)`, `$10-$15 (t2-t7)`, `$24-$25 (t8-t9)`, `hi`, `lo` — all explicitly listed in the clobber list.

## Architecture-Specific Syscall Dispatch

### x86_64: The Straightforward Case

```asm
mov rax, <syscall_number>     ; e.g., 0 for read, 1 for write
mov rdi, arg1
mov rsi, arg2
mov rdx, arg3
mov r10, arg4                  ; NOT rcx — syscall clobbers rcx
mov r8, arg5
mov r9, arg6
syscall                        ; rcx and r11 are clobbered
```

Arguments in `RDI, RSI, RDX, R10, R8, R9`. Note `R10` instead of `RCX` — the `syscall` instruction saves `RIP` to `RCX` and `RFLAGS` to `R11`, destroying both.

### i386: Register Passing with EBP Headaches

Linux i386 passes arguments in **registers** (unlike FreeBSD/Solaris i386 which use the stack):

```asm
mov eax, <syscall_number>
mov ebx, arg1
mov ecx, arg2
mov edx, arg3
mov esi, arg4
mov edi, arg5
mov ebp, arg6     ; ← problem: may be frame pointer
int $0x80
```

The 6-argument case is tricky because `EBP` is the frame pointer when `-fno-omit-frame-pointer` is active. The code manually saves and restores it:

```asm
pushl %%ebp           ; save frame pointer
movl %[a6], %%ebp     ; load 6th argument
int $0x80
popl %%ebp            ; restore frame pointer
```

The `arg6` operand uses `"rm"` constraint (register **or memory**) because at `-O0` with frame pointer enabled, all 6 GPRs are tied to operands and `arg6` may need to live on the stack.

### AArch64: Clean Modern Design

```asm
mov x8, <syscall_number>      ; syscall number in x8
mov x0, arg1                   ; x0-x5 for arguments
; ...
svc #0                         ; supervisor call
```

Clean design with no register clobbering. AArch64 exclusively uses the **modern syscall table** — no legacy `open`/`stat`/`unlink`/`mkdir`/`fork`/`dup2`/`pipe`. Must use `openat`/`fstatat`/`unlinkat`/`mkdirat`/`clone`/`dup3`/`pipe2` with `AT_FDCWD` (-100).

### ARMv7-A: Syscall Number in R7

```asm
mov r7, <syscall_number>      ; r7 holds syscall number
mov r0, arg1                   ; r0-r5 for arguments
; ...
svc #0
```

Supports both direct socket syscalls AND the legacy `socketcall` multiplexer (unlike i386 which only has the multiplexer).

### RISC-V 64/32: ecall with A7

```asm
li a7, <syscall_number>       ; a7 (x17) for syscall number
mv a0, arg1                    ; a0-a5 (x10-x15) for arguments
; ...
ecall
```

Same modern-only syscall table as AArch64. **RISC-V 32 has no 32-bit time syscalls** — must use `clock_gettime64`, `ppoll_time64`. The `Timespec` and `Timeval` structures have `INT64` fields (not `SSIZE`) to match the kernel ABI.

## Syscall Number Divergence

Unlike BSD (shared syscall numbers across architectures), Linux assigns **different numbers per architecture**:

| Syscall | x86_64 | i386 | AArch64 | MIPS64 |
|---|---|---|---|---|
| `read` | 0 | 3 | 63 | 5000 |
| `write` | 1 | 4 | 64 | 5001 |
| `open` | 2 | 5 | — | 5002 |
| `close` | 3 | 6 | 57 | 5003 |
| `exit` | 60 | 1 | 93 | 5058 |
| `socket` | 41 | 102* | 198 | 5041 |

*i386 uses `socketcall` multiplexer, not direct socket syscalls.

MIPS64 numbers start at **5000** (5000 + original SVR4 number).

## Constant Value Divergence

POSIX "constants" vary across architectures due to historical ABI inheritance:

| Constant | Generic | MIPS64 | Notes |
|---|---|---|---|
| `O_CREAT` | `0x0040` | `0x0100` | IRIX/SVR4 heritage |
| `O_APPEND` | `0x0400` | `0x0008` | Different bit position |
| `O_NONBLOCK` | `0x0800` | `0x0080` | Different bit position |
| `O_DIRECTORY` | `0x4000`/`0x10000` | `0x10000` | x86 uses 0x10000, ARM uses 0x4000 |
| `MAP_ANONYMOUS` | `0x20` | `0x0800` | MIPS unique |
| `SOL_SOCKET` | `1` | `0xFFFF` | MIPS uses BSD-style |
| `SO_ERROR` | `4` | `0x1007` | MIPS uses BSD-style |
| `EINPROGRESS` | `115` | `150` | MIPS unique |
| `SO_RCVTIMEO` | `20` | `0x1006` | MIPS uses BSD-style; RV32 uses 66 (time64) |

## i386 socketcall Multiplexer

i386 Linux packs all socket operations through a single `SYS_SOCKETCALL` (102):

```c
// Create socket:
USIZE args[] = { domain, type, protocol };
System::Call(102 /*SYS_SOCKETCALL*/, 1 /*SOCKOP_SOCKET*/, (USIZE)args);

// Connect:
USIZE args[] = { fd, (USIZE)addr, addrlen };
System::Call(102, 3 /*SOCKOP_CONNECT*/, (USIZE)args);
```

The kernel reads the argument array from the pointer — one less level of indirection than a typical syscall, but the arguments are packed in memory rather than registers.

## LTO Miscompilation Prevention

All `System::Call` overloads are marked `NOINLINE`. Without this, Link-Time Optimization may:
- Inline the assembly block and optimize away the negative-return check
- Reorder register assignments around the `syscall`/`int $0x80` instruction
- On MIPS, eliminate the branch delay slot `nop`

This is a common pitfall with inline assembly in position-independent code compiled with LTO.

## Shared Structures

| Structure | Fields | Notes |
|---|---|---|
| `LinuxDirent64` | `Ino`, `Off`, `Reclen`, `Type`, `Name[]` | Variable-size records |
| `Timespec` | `Sec`, `Nsec` | **64-bit** fields on RISC-V 32 |
| `Timeval` | `Sec`, `Usec` | **64-bit** fields on RISC-V 32 |
| `Pollfd` | `Fd`, `Events`, `Revents` | Standard across all architectures |
