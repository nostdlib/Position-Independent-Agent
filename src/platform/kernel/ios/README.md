[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# iOS (XNU) Kernel Interface

Position-independent iOS syscall layer for **AArch64 only**. Re-exports macOS XNU definitions since iOS shares the same kernel and BSD syscall ABI.

## Same Kernel, Same Syscalls

iOS runs on the **XNU kernel** — the exact same kernel as macOS. The syscall interface is identical at the binary level:

- Same syscall numbers (class 2, `0x2000000` prefix)
- Same `svc #0x80` trap instruction (not `svc #0`)
- Same `X16` register for syscall number (not `X8`)
- Same carry-flag error model (`b.cc 1f; neg x0, x0; 1:`)
- Same X1 clobbering for rval[1]
- Same BSD constant values (`O_CREAT` = 0x200, `SOL_SOCKET` = 0xFFFF, etc.)

All files in this directory simply include the macOS equivalents:

```c
// ios/syscall.h → includes macos/syscall.h
// ios/system.h → includes macos/system.h
// ios/system.aarch64.h → includes macos/system.aarch64.h
```

## What iOS Lacks

While the syscall interface is identical, iOS differs at higher levels:
- **No dyld framework resolution** — iOS apps use a different code signing and loading model
- **Stricter sandbox** — many syscalls are blocked by the App Sandbox
- **AArch64 only** — no x86_64 (that's the iOS Simulator, which runs macOS)

For the full technical details of the syscall dispatch, carry-flag handling, Mach traps, and BSD constants, see the [macOS Kernel Interface](../macos/README.md).
