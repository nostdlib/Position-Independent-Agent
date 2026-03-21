[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# NetBSD Kernel Interface

**Status: Not Implemented**

This platform is a placeholder for future NetBSD kernel support. No syscall wrappers, structures, or inline assembly have been implemented yet.

## Expected Architecture Support

NetBSD supports a wide range of architectures. Likely targets for this project:

| Architecture | Trap Instruction | Expected Error Model |
|---|---|---|
| **x86_64** | `syscall` | BSD carry-flag |
| **i386** | `int $0x80` | BSD carry-flag |
| **AArch64** | `svc #0` | BSD carry-flag |

## Required Components

- `syscall.h` — NetBSD syscall numbers and BSD constants
- `system.h` — Architecture dispatcher
- `system.x86_64.h` / `system.i386.h` / `system.aarch64.h` — Inline assembly
- `platform_result.h` — Error conversion (`result::FromNetBSD`)

### Syscall Categories Needed

- **File I/O:** open, close, read, write, lseek, openat
- **File Operations:** stat, fstat, unlink, unlinkat, fstatat
- **Directory Operations:** mkdir, rmdir, getdents
- **Memory:** mmap, munmap
- **Network:** socket, connect, bind, sendto, recvfrom, setsockopt, getsockopt, poll
- **Process:** fork, execve, exit, wait4, kill, setsid, dup2, pipe
- **Time:** clock_gettime, gettimeofday
- **Console / PTY:** posix_openpt, ioctl

## Notes

- NetBSD uses the BSD carry-flag error model (same as FreeBSD and macOS)
- Syscall numbers differ from FreeBSD despite shared BSD heritage
- `AT_FDCWD` = `-100` on NetBSD (same as FreeBSD/Linux)
- Socket option constants follow BSD conventions (`SOL_SOCKET` = `0xFFFF`)
