# Entry Point: Where Shellcode Begins

Every C and C++ program you have ever written starts at `main()`. This one
does not. Understanding why -- and what replaces it -- is the key to
understanding how position-independent code bootstraps itself from nothing.

**Prerequisites:** [01 - What Is PIC](01-what-is-pic.md), [02 - Project Overview](02-project-overview.md), [03 - Build System](03-build-system.md).

**Primary source files:**
- `src/entry_point.cc` -- the entry point itself (33 lines)
- `src/core/compiler/compiler.h` lines 89-103 -- the `ENTRYPOINT` macro
- `src/platform/kernel/uefi/efi_context.h` -- the EFI_CONTEXT struct
- `src/platform/kernel/uefi/efi_context.x86_64.h` -- GS-base register storage
- `src/platform/kernel/uefi/efi_context.aarch64.h` -- TPIDR_EL0 register storage
- `src/beacon/main.cc` -- the `start()` function that entry_point calls

---

## 1. Why There Is No `main()`

When you compile a normal program, the resulting binary does not jump straight
to `main()`. The compiler links in **C Runtime** (CRT) startup code that
initializes the stack, parses `argc`/`argv`, sets up `stdin`/`stdout`,
constructs global C++ objects, and only then calls `main()`.

This project compiles with `-nostdlib` (see [01 - What Is PIC](01-what-is-pic.md#what-is-freestanding-code)).
No C library, no CRT. The OS kernel (or UEFI firmware, or shellcode injector)
jumps directly to byte zero of the binary. Whatever function lives at byte zero
IS the entry point. We call ours `entry_point()` and use a macro plus linker
configuration to guarantee it lands at offset 0. Naming it `main` would imply
a runtime environment that does not exist.

---

## 2. The ENTRYPOINT Macro

From `src/core/compiler/compiler.h` (lines 99-103):

```c
#if defined(ARCHITECTURE_X86_64) && !defined(PLATFORM_WINDOWS) && !defined(PLATFORM_UEFI)
#define ENTRYPOINT extern "C" __attribute__((noreturn, force_align_arg_pointer))
#else
#define ENTRYPOINT extern "C" __attribute__((noreturn))
#endif
```

Each piece exists for a specific reason:

**`extern "C"`** -- C++ mangles function names (`entry_point` becomes
`_Z11entry_pointv`). The linker is told to use `entry_point` as the entry
symbol (via `-e entry_point` on ELF, `/Entry:entry_point` on PE/COFF). If the
name is mangled, the linker cannot find it. `extern "C"` disables mangling.

**`__attribute__((noreturn))`** -- The entry point ends with `ExitProcess()`,
which terminates the process. There is nobody to return to. This attribute
lets the compiler omit the function epilogue. Without it, a `ret` instruction
would pop a garbage address and jump into the void.

**`__attribute__((force_align_arg_pointer))`** -- Only on x86_64 POSIX. Fixes
a subtle stack alignment problem that deserves its own section.

---

## 3. The x86_64 Stack Alignment Problem

The x86_64 System V ABI requires RSP to be 16-byte aligned at a `CALL`
instruction. `CALL` pushes an 8-byte return address, so when the callee's
first instruction runs, RSP is at `16n - 8`. The compiler relies on this to
align SSE/AVX stack slots. If the assumption breaks, any `movaps` or `movdqa`
instruction segfaults.

When shellcode starts, there was no `CALL`. RSP could be at any alignment:

```
  Normal function entry (after CALL):

      +------------------+ <-- 0x7fff_fff0  (16-byte aligned)
      | return address   |  8 bytes pushed by CALL
      +------------------+ <-- 0x7fff_ffe8  (RSP = 16n-8, as compiler expects)
      | saved RBP        |  push rbp
      +------------------+ <-- 0x7fff_ffe0  (back to 16n)
      | local variables  |
      |       ...        |

  Shellcode entry (jumped directly, no CALL):

      +------------------+ <-- 0x7fff_fff0  (RSP = 16n, NOT 16n-8)
      | ???????????????? |  no return address was pushed
      +------------------+
      | saved RBP        |  push rbp
      +------------------+ <-- 0x7fff_ffe8  (MISALIGNED -- 16n-8 instead of 16n)
      | local variables  |
      |       ...        |
```

The crash is nondeterministic. At `-O0` it might work fine. At `-O1` the
optimizer uses SSE instructions and it segfaults. Nobody can explain why.

`force_align_arg_pointer` emits a prologue that realigns RSP regardless of
its incoming value. One attribute, one extra instruction, hours of debugging
saved.

**Why not Windows and UEFI?** They use the Microsoft x64 calling convention.
UEFI firmware calls the entry point with a proper `CALL` instruction, so RSP
is already at `16n - 8`. The attribute would generate redundant code.

---

## 4. The Actual Entry Point Code

Here is `src/entry_point.cc` in its entirety:

```cpp
INT32 start();

#if defined(PLATFORM_UEFI)
ENTRYPOINT EFI_STATUS EFIAPI entry_point(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
#else
ENTRYPOINT INT32 entry_point(VOID)
#endif
{
#if defined(PLATFORM_UEFI)
    EFI_CONTEXT efiContext = {};
    efiContext.ImageHandle = ImageHandle;
    efiContext.SystemTable = SystemTable;
    SetEfiContextRegister(efiContext);
    SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, nullptr);
#endif
    INT32 exitCode = start();
    ExitProcess(exitCode);
}
```

Two conditional compilation paths. A forward declaration, a platform-specific
bootstrap block, and a call to `start()`. That is the entire bridge between
"raw bytes in memory" and "a working agent."

---

## 5. UEFI Entry Point: A Different Signature

On every platform except UEFI, the entry point takes no arguments. Linux and
other POSIX systems give you nothing -- you make raw syscalls. Windows gives
you nothing -- you walk the PEB to find `ntdll.dll`. UEFI is different:

```cpp
ENTRYPOINT EFI_STATUS EFIAPI entry_point(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
```

**`ImageHandle`** -- an opaque handle identifying this loaded application.
Required for UEFI protocol lookups.

**`SystemTable`** -- a pointer to the EFI System Table: boot services, runtime
services, console I/O, everything. Without it, you cannot allocate memory,
open a network connection, or print a single character.

---

## 6. EFI_CONTEXT: Why a CPU Register, Not a Global Variable

The `SystemTable` pointer must be accessible throughout the entire program.
The natural instinct is a global variable. That creates a `.data` section,
which violates the Golden Rule (see [01 - What Is PIC](01-what-is-pic.md#binary-sections)).
One global variable and the entire position-independence guarantee collapses.

The solution: store the pointer in a **CPU register** preserved across function
calls. The `EFI_CONTEXT` struct (`src/platform/kernel/uefi/efi_context.h`):

```cpp
struct EFI_CONTEXT
{
    EFI_HANDLE ImageHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    BOOL NetworkInitialized;
    BOOL DhcpConfigured;
    BOOL TcpStackReady;
};
```

It is allocated on the stack in `entry_point()`, populated, and then a pointer
to it is written into a CPU register:

**x86_64** uses the GS segment base register via `WRMSR` to the `IA32_GS_BASE`
MSR (0xC0000101). The code uses `WRMSR`/`RDMSR` rather than the faster
`WRGSBASE`/`RDGSBASE` because UEFI firmware may not have enabled
`CR4.FSGSBASE`. UEFI applications run in ring 0, so MSR access is available:

```cpp
// src/platform/kernel/uefi/efi_context.x86_64.h
inline VOID SetEfiContextRegister(EFI_CONTEXT &ctx)
{
    UINT64 value = reinterpret_cast<UINT64>(&ctx);
    UINT32 low = static_cast<UINT32>(value);
    UINT32 high = static_cast<UINT32>(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(IA32_GS_BASE), "a"(low), "d"(high) : "memory");
}
```

**AArch64** uses `TPIDR_EL0` (Thread Pointer ID Register) -- a single `MSR`
instruction:

```cpp
// src/platform/kernel/uefi/efi_context.aarch64.h
inline VOID SetEfiContextRegister(EFI_CONTEXT &ctx)
{
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(&ctx) : "memory");
}
```

Both registers are normally used by operating systems for thread-local storage.
In a freestanding UEFI environment, nobody else is using them. Any function
anywhere in the codebase can call `GetEfiContext()` to read the register back.

Because `efiContext` lives on the stack of `entry_point()` -- and
`entry_point()` never returns -- the stack frame remains valid for the entire
lifetime of the program. The pointer always points to live memory.

---

## 7. The Watchdog Timer

```cpp
SystemTable->BootServices->SetWatchdogTimer(0, 0, 0, nullptr);
```

UEFI firmware ships with a watchdog timer, typically 5 minutes. If an
application exceeds the timeout, the firmware hard-resets the system. The agent
runs an infinite command loop. Without disabling the watchdog, you get a reboot
every 5 minutes. `SetWatchdogTimer(0, ...)` sets the timeout to zero, which
disables it.

---

## 8. The `start()` Forward Declaration and `beacon/main.cc`

Line 9 of `entry_point.cc`:

```cpp
INT32 start();
```

This forward declaration tells the compiler "a function called `start` exists
somewhere." The linker finds it later. The definition lives in
`src/beacon/main.cc`:

```cpp
INT32 start()
{
    const CHAR url[] = "https://relay.nostdlib.workers.dev/agent";
    // ... registers command handlers ...
    while (1)
    {
        // connect to relay, read commands, dispatch to handlers, reconnect on failure
    }
    return 1;
}
```

The separation is deliberate. `entry_point.cc` handles platform-specific
bootstrap: stack alignment, UEFI context, watchdog. It knows about the
differences between Linux, Windows, UEFI, and every other target. `start()`
knows none of that. It assumes the platform layer is ready and runs agent
logic.

```
OS/Firmware/Loader
    |
    +---> entry_point()          [src/entry_point.cc]
              |
              +--- (UEFI only) store EFI_CONTEXT in CPU register
              +--- (UEFI only) disable watchdog timer
              |
              +---> start()      [src/beacon/main.cc]
              |        |
              |        +--- register command handlers
              |        +--- WebSocket connect -> command loop -> reconnect
              |
              +---> ExitProcess()  [platform-specific, never returns]
```

In practice, `start()` never returns because of its `while(1)` loop. The
`ExitProcess` call is a safety net -- if the loop breaks, the process
terminates cleanly rather than falling off the end of a `noreturn` function.

---

## 9. Placing entry_point at Offset Zero

Defining `entry_point()` with the right attributes is necessary but not
sufficient. The linker must place it at the first byte of `.text`. The build
system enforces this with **symbol ordering files** -- for example,
`cmake/data/function.order.linux` contains one line: `entry_point`. The linker
flag `--symbol-ordering-file` (ELF), `/ORDER:@` (PE/COFF), or `-order_file`
(Mach-O) places this symbol first.

On macOS and iOS, release builds compile `entry_point.cc` without LTO
(`-fno-lto`) because Apple's linker places non-LTO sections before
LTO-generated sections, guaranteeing correct ordering even when LTO inserts
constant-pool data.

A post-build step (`cmake/scripts/VerifyPICMode.cmake`) parses the linker map
and confirms `entry_point` is the first symbol. If it is not, the build fails.
This is a hard guarantee, not a best-effort hope. See
[03 - Build System](03-build-system.md) for the full pipeline.

---

## 10. Recap

| Problem | Solution |
|---------|----------|
| No CRT to call `main()` | Custom `entry_point()` with `extern "C"` linkage |
| Linker might strip unused function | `noreturn` attribute + symbol ordering files |
| RSP misaligned on x86_64 POSIX entry | `force_align_arg_pointer` attribute |
| UEFI SystemTable needed globally without `.data` section | Stack-allocated `EFI_CONTEXT`, pointer in CPU register |
| UEFI watchdog resets machine every 5 min | `SetWatchdogTimer(0, 0, 0, nullptr)` |

After bootstrap, `entry_point()` calls `start()` and the agent takes over.
Everything from this point -- WebSocket connections, command dispatch,
screenshots, shell access -- builds on these 33 lines.

---

**Next:** The platform layer that `start()` relies on. See `docs/ONBOARDING.md`
for the reading order.
