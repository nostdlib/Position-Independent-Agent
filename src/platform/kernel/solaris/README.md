[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# Solaris Kernel Interface

Position-independent Solaris/illumos syscall layer supporting **3 architectures**. Unique SVR4 conventions: `int $0x91` trap gate on i386, multiplexed process/group syscalls, and aggressive legacy syscall removal in Solaris 11.4.

## The `int $0x91` Trap Gate (i386)

Solaris i386 uses `int $0x91` — **not** `int $0x80` (Linux/FreeBSD). This is the SVR4 fast system call trap gate. The calling convention mirrors FreeBSD i386 (stack-based arguments with dummy return address), not Linux i386 (register-based):

```asm
pushl $0          ; dummy return address (SVR4 convention)
pushl arg1
pushl arg2
; ...
int $0x91         ; ← Solaris-specific trap gate
jnc 1f            ; carry flag error check (BSD-style)
negl %%eax
1:
addl $N, %%esp    ; clean up stack
```

The 6-argument variant has the same EBP/frame-pointer conflict as FreeBSD i386 — uses `"g"` constraint for the 6th argument.

## Error Handling: Carry Flag

Solaris uses the **BSD carry-flag** model across all architectures:

| Architecture | Instruction | Error Handling |
|---|---|---|
| **x86_64** | `syscall` | `jnc 1f; negq %%rax; 1:` |
| **i386** | `int $0x91` | `jnc 1f; negl %%eax; 1:` |
| **AArch64** | `svc #0` | `b.cc 1f; neg x0, x0; 1:` |

### AArch64: Standard Conventions

Solaris AArch64 follows standard conventions (unlike macOS):
- `svc #0` (not `svc #0x80`)
- Syscall number in `X8` (not `X16`)
- **X1 clobbered by rval[1]** — same as FreeBSD

All `System::Call` overloads are `NOINLINE` to prevent LTO from miscompiling the carry-flag + `"+r"(x1)` constraint pattern.

## Multiplexed Syscalls

Solaris groups related operations under single syscall numbers with sub-operation codes:

### forksys (142)

A single syscall for all fork variants:

```c
// fork:     System::Call(SYS_FORKSYS, 0, 0)
// vfork:    System::Call(SYS_FORKSYS, 1, 0)
// forkall:  System::Call(SYS_FORKSYS, 2, 0)
```

Subcode 0 = fork, 1 = vfork (shared address space), 2 = forkall (fork all LWPs — Solaris-specific for multi-threaded processes).

### pgrpsys (39)

Process group and session operations:

```c
// getpgrp:  System::Call(SYS_PGRPSYS, 0)
// setpgrp:  System::Call(SYS_PGRPSYS, 1)
// getsid:   System::Call(SYS_PGRPSYS, 2, pid)
// setsid:   System::Call(SYS_PGRPSYS, 3)
// getpgid:  System::Call(SYS_PGRPSYS, 4, pid)
// setpgid:  System::Call(SYS_PGRPSYS, 5, pid, pgid)
```

This design comes from SVR4's system call table, where related operations share a single trap number.

## Legacy Syscall Removal (Solaris 11.4)

Oracle removed many "traditional" syscalls from Solaris 11.4. Code **must** use `*at` variants:

| Removed | Replacement | Notes |
|---|---|---|
| `open` | `openat` (68) | `AT_FDCWD` for current directory |
| `stat` / `fstat` | `fstatat` (66) | Single syscall for all stat variants |
| `unlink` | `unlinkat` (76) | With or without `AT_REMOVEDIR` |
| `mkdir` | `mkdirat` (102) | Relative to directory fd |
| `rmdir` | `unlinkat` + `AT_REMOVEDIR` | No standalone rmdir |

The legacy numbers still exist in the syscall table (for compatibility), but calling them on Solaris 11.4 may fail.

## Solaris-Unique Constant Values

Solaris has some of the most divergent constant values of any platform:

| Constant | Solaris | Linux | FreeBSD | Why |
|---|---|---|---|---|
| `AT_FDCWD` | `0xffd19553` (-665133) | `-100` | `-100` | Solaris-unique magic value |
| `AT_REMOVEDIR` | `0x01` | `0x200` | `0x0800` | Simplest value |
| `O_CREAT` | `0x100` | `0x040` | `0x200` | SVR4 heritage |
| `O_NONBLOCK` | `0x80` | `0x800` | `0x04` | SVR4 heritage |
| `O_DIRECTORY` | `0x1000000` | varies | `0x20000` | Very large value |
| `MAP_ANONYMOUS` | `0x100` | `0x20` | `0x1000` | Unique |
| `CLOCK_REALTIME` | `3` | `0` | `0` | Different ID assignment |
| `EINPROGRESS` | `150` | `115` | `36` | SVR4 errno numbering |
| `SOCK_STREAM` | `2` | `1` | `1` | SVR4 swaps STREAM/DGRAM |
| `SOCK_DGRAM` | `1` | `2` | `2` | SVR4 swaps STREAM/DGRAM |

The `AT_FDCWD` value (`0xffd19553`) is particularly notable — it's a deliberately unlikely file descriptor value chosen to be unique.

`SOCK_STREAM` and `SOCK_DGRAM` are **swapped** compared to every other platform (SVR4 heritage, also shared by MIPS Linux).

## The Missing d_type: Solaris dirent

Solaris `dirent` lacks a `d_type` field:

```c
struct SolarisDirent64 {
    UINT64 Ino;       // inode number
    UINT64 Off;       // offset to next entry
    UINT16 Reclen;    // record length
    CHAR   Name[];    // filename — NO TYPE FIELD
};
```

To determine if an entry is a file or directory, a **separate `fstatat` call is required for every entry**. This makes Solaris directory iteration ~2x more expensive than platforms with `d_type`.

### getdents vs getdents64

On Solaris, 64-bit processes that call `getdents64` receive **SIGSYS** (bad syscall). The workaround: use `getdents` (the "32-bit" variant, syscall 81), which returns native 64-bit dirent structures when called from a 64-bit process. This is a Solaris kernel quirk — `getdents64` (syscall 213) is only for 32-bit processes accessing large directories.

## Socket Constants (BSD-Style)

Despite being SVR4-derived, Solaris socket options use BSD-style values:

```c
SOL_SOCKET   = 0xFFFF    // same as FreeBSD/macOS (not 1 like Linux)
SO_ERROR     = 0x1007    // same as FreeBSD/macOS (not 4 like Linux)
SO_RCVTIMEO  = 0x1006    // same as FreeBSD/macOS (not 20 like Linux)
```

This is because Solaris integrated the BSD socket layer wholesale, inheriting its constant values.
