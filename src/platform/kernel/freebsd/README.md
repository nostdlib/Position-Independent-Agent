[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# FreeBSD Kernel Interface

Position-independent FreeBSD syscall layer supporting **4 architectures** with BSD carry-flag error semantics. Syscall numbers are shared across all architectures (unlike Linux), but calling conventions and error signaling mechanisms differ per architecture.

## Error Handling: The Carry Flag Pattern

FreeBSD uses the **BSD carry-flag** model — on error, the processor carry flag is set and the return register holds a **positive** errno (not negated like Linux):

### x86_64: jnc/neg Pattern

```asm
syscall
jnc 1f            ; jump if carry clear (success)
negq %%rax        ; error: negate errno to make it negative
1:                ; success: rax already has the result
```

After this normalization, the return value looks like Linux (negative on error), so `result::FromFreeBSD()` handles it identically to `result::FromLinux()`.

### The RDX Clobbering Problem

FreeBSD kernel writes a **secondary return value (rval[1])** to `RDX` for certain syscalls (e.g., `fork` returns child PID in RAX and parent PID in RDX; `pipe` returns read fd in RAX and write fd in RDX).

This creates a subtle bug: for syscalls with 3+ arguments, `RDX` holds arg3 as **input** — but the kernel overwrites it with rval[1] as **output**. The code solves this by marking RDX as `"+r"` (input+output) in all overloads where it carries an argument:

```c
register USIZE r_rdx __asm__("rdx") = arg3;  // input
__asm__ volatile(
    "syscall\n" "jnc 1f\n" "negq %%rax\n" "1:\n"
    : "+r"(r_rax), "+r"(r_rdx)   // rdx is BOTH input AND clobbered output
    ...
```

Without `"+r"`, the compiler assumes RDX is unchanged after the syscall and may reuse its value — leading to data corruption.

### RISC-V 64: T0 as Error Indicator (No Carry Flag)

RISC-V has no carry flag. FreeBSD uses **T0 (X5)** as a dedicated error indicator:

```
T0 = 0  →  success (A0 = result)
T0 != 0 →  failure (A0 = positive errno)
```

Interesting implementation detail: the **syscall number also goes in T0** (not A7 like Linux RISC-V). The kernel reads T0 before `ecall`, then overwrites it with the error flag after.

The code uses **early-clobber `&` constraints** on all argument registers:

```c
register USIZE r_a0 __asm__("a0") = arg1;
__asm__ volatile(
    "mv t0, %[num]\n"   // load syscall number into T0
    "ecall\n"
    "beqz t0, 1f\n"     // check error flag
    "neg a0, a0\n"       // negate errno
    "1:\n"
    : "+&r"(r_a0)        // ← early-clobber: prevents allocating
    : [num] "r"(number)  //    'number' to the same register as r_a0
```

Without early-clobber, the compiler might assign `number` and `r_a0` to the same physical register — overwriting the syscall number with arg1 before the `mv t0, %[num]` instruction executes.

### i386: Stack-Based Arguments (NOT Registers)

**Critical difference from Linux i386:** FreeBSD i386 passes arguments **on the stack**, not in registers. The code pushes a **dummy return address** first (FreeBSD kernel expects this), then pushes arguments in reverse order:

```asm
pushl $0          ; dummy return address (FreeBSD convention)
pushl arg1        ; first argument
pushl arg2        ; second argument
; ...
int $0x80
jnc 1f
negl %%eax
1:
addl $N, %%esp    ; clean up: N = 4*(argcount + 1)
```

The 6-argument variant has a special challenge: it needs `EBP` for the 6th argument, but `EBP` may be the frame pointer. The code uses `"g"` constraint (general register or memory) instead of a register constraint to avoid conflicts under LTO with `-fno-omit-frame-pointer`.

## Shared Syscall Numbers

Unlike Linux's per-architecture numbering, FreeBSD uses **the same syscall numbers across all architectures**. Some notable values:

| Syscall | Number | Notes |
|---|---|---|
| `lseek` | 478 | Much higher than Linux (8) — renumbered in FreeBSD 12 |
| `fstat` | 551 | Modern fstat variant |
| `getdirentries` | 554 | FreeBSD 12+ ABI |
| `mmap` | 477 | Renumbered for 64-bit compatibility |
| `posix_openpt` | 504 | PTY master allocation (no `/dev/ptmx` open) |

## Constant Differences from Linux

Many "standard" constants have different values on FreeBSD due to BSD heritage:

| Constant | FreeBSD | Linux | Why |
|---|---|---|---|
| `O_CREAT` | `0x0200` | `0x0040` | BSD vs System V heritage |
| `O_NONBLOCK` | `0x0004` | `0x0800` | BSD assigns low bits first |
| `MAP_ANONYMOUS` | `0x1000` | `0x20` | Different allocation of flag bits |
| `SOL_SOCKET` | `0xFFFF` | `1` | BSD uses "magic" level value |
| `EINPROGRESS` | `36` | `115` | BSD errno numbering |
| `CLOCK_MONOTONIC` | `4` | `1` | Different clock ID assignment |
| `AT_REMOVEDIR` | `0x0800` | `0x200` | Different flag bits |
| `O_NOCTTY` | `0x8000` | `0x100` | Different bit positions |

## FreeBSD Dirent Structure

```c
struct FreeBsdDirent {
    UINT64 Fileno;    // inode number
    UINT64 Off;       // offset to next entry
    UINT16 Reclen;    // total record size
    UINT8  Type;      // DT_REG, DT_DIR, etc.
    UINT8  Pad0;
    UINT16 Namlen;    // filename length (BSD-specific field)
    UINT16 Pad1;
    CHAR   Name[];    // null-terminated filename
};
```

The `Namlen` field is a BSD-ism — Linux's `dirent64` doesn't have it (the name length is derived from `Reclen` minus the header size).

## NOINLINE and LTO

Every `System::Call` overload is `NOINLINE` to prevent LTO from:
- Inlining the carry-flag check (`jnc`/`neg`) and optimizing it away
- Reordering register stores across the `syscall`/`int $0x80` boundary
- On RISC-V, miscompiling the T0 error-indicator pattern
