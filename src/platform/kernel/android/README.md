[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# Android Kernel Interface

Position-independent Android syscall layer. Re-exports Linux kernel definitions since Android runs the Linux kernel with identical syscall ABIs.

## Same Kernel, Same Syscalls

Android uses the **Linux kernel** — the syscall ABI is identical at the binary level. Same trap instructions, same register conventions, same negative-return error model, same per-architecture syscall number tables.

All files in this directory include the Linux equivalents:

```c
// android/syscall.h → includes linux/syscall.h
// android/system.h → includes linux/system.h
```

## What Differs from Desktop Linux

The syscall interface is byte-for-byte identical, but the runtime environment differs:

### SELinux Mandatory Access Control

Android enforces strict SELinux policies. Some syscalls that work on desktop Linux may be denied by policy — not by the kernel, but by the SELinux security module checking the calling process's security context. The syscall itself succeeds at the kernel level but returns `-EACCES`.

### Seccomp-BPF Syscall Filtering

Android apps run under a Seccomp-BPF filter that **blocks syscalls not on the allowlist**. Blocked syscalls return `-EPERM` or kill the process with `SIGSYS`. The filter is set up by zygote before the app process starts.

### Bionic vs glibc

Android uses Bionic libc instead of glibc — but since this runtime bypasses libc entirely (direct syscalls via inline assembly), this difference is invisible.

### Device-Specific Paths

Some filesystem paths differ:
- Framebuffer: `/dev/graphics/fb0` through `/dev/graphics/fb7` (instead of `/dev/fb*`)
- Machine ID: `/proc/sys/kernel/random/boot_id` (fallback when `/etc/machine-id` is absent)

For the full technical details of syscall dispatch, architecture-specific assembly, error handling, and constant values, see the [Linux Kernel Interface](../linux/README.md).
