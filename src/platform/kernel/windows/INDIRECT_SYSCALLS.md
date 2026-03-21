# Indirect Syscall Dispatch

[< Back to Windows Kernel README](README.md)

**Files:** [`system.h`](system.h), [`system.cc`](system.cc), [`system.x86_64.h`](system.x86_64.h), [`system.i386.h`](system.i386.h), [`system.aarch64.h`](system.aarch64.h), [`system.armv7a.h`](system.armv7a.h)

Runtime SSN (System Service Number) resolution and architecture-specific indirect syscall dispatch. System calls are routed through instruction gadgets within ntdll.dll — the `syscall`/`svc` instruction executes from ntdll's address range, not from the agent's code.

---

## Table of Contents

- [Why Indirect Syscalls](#why-indirect-syscalls)
- [System Service Numbers (SSNs)](#system-service-numbers-ssns)
- [SSN Resolution Algorithm](#ssn-resolution-algorithm)
- [x86_64 Syscall Dispatch](#x86_64-syscall-dispatch)
- [i386 Syscall Dispatch](#i386-syscall-dispatch)
- [ARM64 Syscall Dispatch](#arm64-syscall-dispatch)
- [ARM32 Syscall Dispatch](#arm32-syscall-dispatch)
- [The ResolveSyscall Macro](#the-resolvesyscall-macro)
- [SYSCALL_ENTRY Structure](#syscall_entry-structure)
- [Argument Limits](#argument-limits)

---

## Why Indirect Syscalls

On Windows, user-mode programs call kernel services through **ntdll.dll** stubs. Each stub:
1. Places the System Service Number (SSN) in a register
2. Executes a `syscall` (x64), `sysenter`/`int 0x2E` (x86), or `svc` (ARM) instruction
3. The kernel dispatches based on the SSN via the System Service Descriptor Table (SSDT)

**Direct syscall** means executing the `syscall` instruction from our own code — the SSN is in `EAX`, and we issue `syscall` directly. This works but the instruction pointer during the transition points to our code, not ntdll.

**Indirect syscall** means finding the actual `syscall; ret` gadget inside ntdll.dll and `call`-ing into it. The `syscall` instruction now executes at an ntdll address, making the call indistinguishable from a legitimate ntdll invocation.

```
Direct Syscall:                    Indirect Syscall:
┌─────────────┐                    ┌─────────────┐
│ Our Code    │                    │ Our Code    │
│ mov eax, SSN│                    │ mov eax, SSN│
│ syscall  ◄──│── RIP here         │ call [gadget]│──┐
│ ...         │                    │ ...         │  │
└─────────────┘                    └─────────────┘  │
                                   ┌─────────────┐  │
                                   │ ntdll.dll   │  │
                                   │ ...         │  │
                                   │ syscall ◄───│──┘ RIP here
                                   │ ret         │
                                   └─────────────┘
```

---

## System Service Numbers (SSNs)

Each `Zw*`/`Nt*` function in ntdll has a unique SSN that identifies the kernel function to call. SSNs are **not stable across Windows versions** — they change with every build. Therefore, SSNs must be resolved at runtime.

The SSN is an index into the SSDT (System Service Descriptor Table) in the kernel.

---

## SSN Resolution Algorithm

`System::ResolveSyscallEntry(functionNameHash)` resolves the SSN for a given `Zw*` function:

```
ResolveSyscallEntry(hash)
  │
  ├─ GetModuleHandleFromPEB(hash("ntdll.dll")) → ntdll base
  │
  ├─ Parse PE export directory (raw offset math)
  │    ├─ NT headers at base + *(UINT32*)(base + 0x3C)
  │    ├─ Export dir RVA at ntHeaders + 0x88 (64-bit) or 0x78 (32-bit)
  │    └─ Build name/function/ordinal table pointers
  │
  ├─ For each export name:
  │    ├─ Skip if not "Zw" prefix
  │    ├─ Skip if Djb2::Hash(name) != functionNameHash
  │    ├─ Get function RVA, skip forwarded exports
  │    │
  │    └─ Architecture-specific SSN + gadget extraction:
  │         ├─ x86_64: scan for 0F 05 C3, count Zw* with lower RVA
  │         ├─ i386:   read SSN from B8 opcode, extract call target
  │         ├─ ARM64:  scan for SVC+RET pair, count Zw* with lower RVA
  │         └─ ARM32:  scan for SVC #1, count Zw* with lower RVA
  │
  └─ Return SYSCALL_ENTRY { Ssn, SyscallAddress }
```

---

## x86_64 Syscall Dispatch

### Stub Format

A typical ntdll x64 stub:

```asm
ZwCreateFile:
  4C 8B D1              mov r10, rcx        ; save 1st arg (syscall clobbers rcx)
  B8 55 00 00 00        mov eax, 0x55       ; SSN
  0F 05                 syscall             ; ◄── gadget target (0F 05)
  C3                    ret                 ; ◄── gadget includes ret (C3)
```

### Gadget Discovery

Scan the first 26 bytes of the stub for the `syscall; ret` sequence:

```c
for (UINT32 k = 0; k < 26; k++) {
    if (funcAddr[k] == 0x0F && funcAddr[k+1] == 0x05 && funcAddr[k+2] == 0xC3) {
        result.SyscallAddress = (PVOID)(funcAddr + k);
        break;
    }
}
```

### SSN Derivation

SSNs on x64 are assigned in order of the function's RVA. The SSN equals the count of `Zw*` exports whose RVA is lower:

```c
result.Ssn = 0;
for (UINT32 j = 0; j < numberOfNames; j++) {
    if (name[0] == 'Z' && name[1] == 'w' && rva < targetFuncRva)
        result.Ssn++;
}
```

### Calling Convention

```
Register    Purpose
────────    ────────────────────────────────
RAX         SSN (System Service Number)
R10         Arg 1 (moved from RCX — syscall clobbers RCX)
RDX         Arg 2
R8          Arg 3
R9          Arg 4
[RSP+0x20]  Arg 5  ┐
[RSP+0x28]  Arg 6  │ 32-byte shadow space (0x00-0x1F) is reserved
[RSP+0x30]  Arg 7  │ above RSP per Microsoft x64 ABI, then args 5+
...         ...    ┘ follow at 0x20, 0x28, etc.
```

Example (5 args):
```asm
sub rsp, 0x28              ; shadow space (32) + arg5 slot (8) = 40 = 0x28
mov [rsp+0x20], arg5       ; 5th arg on stack
call [gadget]              ; indirect call to ntdll's syscall;ret
add rsp, 0x28              ; restore stack
```

---

## i386 Syscall Dispatch

### Stub Formats

i386 ntdll has two known stub formats:

**Old format (Windows XP through Windows 10):**
```asm
ZwCreateFile:
  B8 55 00 00 00        mov eax, 0x55       ; SSN at offset +1
  BA XX XX XX XX        mov edx, KiFastSysCall  ; address at offset +6
  FF 12                 call [edx]          ; native: indirect call
  ; or FF D2            call edx            ; WoW64: direct call
  C2 2C 00              ret 0x2C            ; clean up stack (stdcall)
```

**New format (Windows 11 WoW64):**
```asm
ZwCreateFile:
  B8 55 00 00 00        mov eax, 0x55       ; SSN at offset +1
  E8 00 00 00 00        call $+5            ; push EIP
  5A                    pop edx             ; edx = current EIP
  ...                   (conditional branch logic)
  BA XX XX XX XX        mov edx, addr       ; at offset 0x1F
  FF 12 / FF D2         call [edx] / call edx
```

### SSN Extraction

On i386, the SSN is embedded directly in the stub (`mov eax, <SSN>` = `B8 XX XX XX XX`):

```c
if (funcAddr[0] != 0xB8)
    return result;  // not a valid stub

result.Ssn = (INT32)(*(UINT32*)(funcAddr + 1));  // bytes 1-4
```

### Syscall Target Resolution

The code handles both calling patterns:

```c
if (funcAddr[baOffset + 5] == 0xFF && funcAddr[baOffset + 6] == 0x12)
    result.SyscallAddress = *(PVOID*)rawAddr;   // FF 12 = call [edx] → dereference
else if (funcAddr[baOffset + 5] == 0xFF && funcAddr[baOffset + 6] == 0xD2)
    result.SyscallAddress = rawAddr;            // FF D2 = call edx → direct
```

### Calling Convention

All arguments are pushed onto the stack (right-to-left, stdcall):

```asm
push arg_n         ; last argument first
...
push arg_2
push arg_1
mov eax, SSN       ; System Service Number
call [sysenter]    ; via resolved KiFastSystemCall address
```

---

## ARM64 Syscall Dispatch

### Stub Format

ARM64 ntdll stubs encode the SSN in the `SVC` immediate operand:

```asm
ZwCreateFile:
  SVC #0x55            ; encoded as 0xD4000001 | (0x55 << 5)
  RET                  ; 0xD65F03C0
```

### Gadget Discovery

Scan for the `SVC; RET` instruction pair:

```c
UINT32* instrs = (UINT32*)(base + funcRva);
for (UINT32 k = 0; k < 6; k++) {
    // SVC encoding: 0xD4000001 | (imm16 << 5), mask top/bottom bits
    if ((instrs[k] & 0xFFE0001F) == 0xD4000001 && instrs[k+1] == 0xD65F03C0) {
        result.SyscallAddress = (PVOID)&instrs[k];
        break;
    }
}
```

### Why BLR Instead of SVC Directly?

On ARM64 Windows, the kernel validates that the `SVC` instruction originates from within ntdll.dll's address range. Our code therefore uses `BLR X16` to branch to the ntdll stub, letting the `SVC` execute at the correct address.

### Calling Convention

```
Register    Purpose
────────    ────────────────────────────────
X0-X7       Args 1-8
[SP+...]    Args 9+
X16         Syscall address (BLR target)
X8          SSN (for counting-based derivation, not used by SVC)
```

---

## ARM32 Syscall Dispatch

### Stub Format (Thumb-2)

ARM32 Windows uses Thumb-2 mode. The ntdll stubs look like:

```asm
ZwCreateFile:
  MOV.W R12, #0x55     ; SSN (variable-length Thumb-2 encoding)
  SVC   #1             ; Thumb-2: 0xDF01
  BX    LR             ; Thumb-2: 0x4770
```

### Gadget Discovery

Scan for the `SVC #1` halfword (`0xDF01`):

```c
UINT16* hw = (UINT16*)funcAddr;
for (UINT32 k = 0; k < 16; k++) {
    if (hw[k] == 0xDF01) {
        // Set bit 0 for Thumb interworking
        result.SyscallAddress = (PVOID)((USIZE)funcAddr | 1);
        break;
    }
}
```

### Thumb Interworking

The syscall address has bit 0 set (`| 1`) to indicate Thumb mode when branching via `BLX`:

```
Address: 0x77001234 → actual instruction address
With T bit: 0x77001235 → BLX target (Thumb interworking)
```

### Calling Convention

```
Register    Purpose
────────    ────────────────────────────────
R0-R3       Args 1-4
[SP+...]    Args 5+
R12         Syscall address (BLX target)
```

---

## The ResolveSyscall Macro

```c
#define ResolveSyscall(functionName) \
    System::ResolveSyscallEntry(Djb2::HashCompileTime(functionName))
```

**Compile-time hashing** ensures that the function name string (e.g., `"ZwCreateFile"`) never appears in the binary. The DJB2 hash is computed by the compiler and embedded as an immediate constant in the `.text` section.

Usage:

```c
SYSCALL_ENTRY entry = ResolveSyscall("ZwCreateFile");
if (entry.Ssn != SYSCALL_SSN_INVALID) {
    NTSTATUS status = System::Call(entry, arg1, arg2, ...);
}
```

---

## SYSCALL_ENTRY Structure

```c
typedef struct SYSCALL_ENTRY {
    INT32 Ssn;            // System Service Number, or SYSCALL_SSN_INVALID (-1)
    PVOID SyscallAddress; // Gadget address in ntdll, or nullptr
} SYSCALL_ENTRY;
```

| Field | Success | Failure |
|---|---|---|
| `Ssn` | Valid SSN (>= 0) | `SYSCALL_SSN_INVALID` (-1) |
| `SyscallAddress` | Points to ntdll gadget | `nullptr` |

---

## Argument Limits

`System::Call` provides overloads for **0 to 14 arguments**, covering all known NT syscalls:

| Args | Stack Allocation (x64) | Example Syscalls |
|---|---|---|
| 0 | None | (none currently used) |
| 1 | None | `ZwClose`, `ZwDeleteFile` |
| 2 | None | `ZwTerminateProcess`, `ZwQueryAttributesFile` |
| 3 | None | `ZwWaitForSingleObject` |
| 4 | None | `ZwFreeVirtualMemory`, `ZwSetInformationObject` |
| 5 | `0x28` (shadow + 1) | `ZwCreateEvent`, `ZwQueryInformationFile` |
| 6 | `0x30` (shadow + 2) | `ZwAllocateVirtualMemory`, `ZwOpenFile` |
| 7-8 | `0x38-0x40` | — |
| 9 | `0x48` | `ZwReadFile`, `ZwWriteFile` |
| 10 | `0x50` | `ZwDeviceIoControlFile` |
| 11 | `0x58` | `ZwCreateFile`, `ZwQueryDirectoryFile`, `ZwCreateUserProcess` |
| 12-13 | `0x60-0x68` | — |
| 14 | `0x70` | `ZwCreateNamedPipeFile` |

---

[< Back to Windows Kernel README](README.md) | [Previous: PE Parsing](PE_PARSING.md) | [Next: NTDLL Wrappers >](NTDLL_WRAPPERS.md)
