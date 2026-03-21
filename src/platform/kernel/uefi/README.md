[< Back to Platform](../../README.md) | [< Back to Project Root](../../../../README.md)

# UEFI Firmware Interface

Position-independent UEFI 2.10 interface for **x86_64** and **AArch64**. Fundamentally different from all other platforms — no syscalls, no kernel transitions, no process model. All functionality accessed through **firmware-provided function pointer tables** (protocols).

## No Syscalls: Protocol Function Tables

```
OS Kernel:   mov rax, <number>; syscall  →  kernel dispatch via SSDT/trap
UEFI:        call [protocol + offset]    →  direct firmware function call
```

UEFI applications run in **ring 0** (same privilege as the firmware). Function calls are ordinary indirect calls through vtable-like structures — no privilege transitions, no trap instructions.

## Calling Convention: Microsoft ABI on x86_64

**Critical difference from all other platforms:** x86_64 UEFI uses the **Microsoft x64 ABI**, not System V:

| Aspect | Microsoft (UEFI) | System V (Linux/BSD) |
|---|---|---|
| Integer args | `RCX, RDX, R8, R9` | `RDI, RSI, RDX, RCX, R8, R9` |
| Stack shadow | 32-byte shadow space | None |
| Callee-saved | `RBX, RBP, RDI, RSI, R12-R15` | `RBX, RBP, R12-R15` |

All UEFI function pointers are declared with `__attribute__((ms_abi))` (or `EFIAPI` macro). Calling a UEFI function with the wrong ABI silently corrupts the stack.

AArch64 UEFI uses standard AAPCS64 — no difference from Linux.

## Context Storage: MSR vs Thread Pointer

The `EFI_CONTEXT` structure (containing `ImageHandle`, `SystemTable`, and network state flags) must be accessible from any position-independent function. Each architecture stores it in a dedicated CPU register:

### x86_64: WRMSR to IA32_GS_BASE (Not WRGSBASE)

```asm
; Store context pointer in GS_BASE MSR
mov ecx, 0xC0000101      ; IA32_GS_BASE MSR number
mov eax, low32(ptr)       ; low 32 bits of pointer
mov edx, high32(ptr)      ; high 32 bits of pointer
wrmsr                     ; write to MSR

; Read it back
mov ecx, 0xC0000101
rdmsr                     ; EDX:EAX = 64-bit value
shl rdx, 32
or rax, rdx               ; combine into 64-bit pointer
```

**Why `WRMSR` instead of `WRGSBASE`?** `WRGSBASE`/`RDGSBASE` require `CR4.FSGSBASE`, which firmware may not enable. `WRMSR`/`RDMSR` are privileged instructions that always work in ring 0 — no CR4 configuration needed.

### AArch64: TPIDR_EL0

```asm
msr tpidr_el0, x0         ; store pointer
mrs x0, tpidr_el0         ; read pointer
```

The thread pointer register is freely available in UEFI since there's no OS claiming it. Simpler and faster than the MSR approach.

## Protocol Discovery

UEFI functionality is organized into **protocols** — structures containing function pointers, identified by 128-bit GUIDs. To use a protocol:

```
1. Construct the GUID on the stack (position-independent — can't live in .rdata)
2. BootServices→LocateProtocol(&guid, NULL, &interface)
3. Cast interface to protocol-specific struct
4. Call functions via pointer: interface→FunctionName(args...)
```

### Stack-Based GUID Construction

GUIDs cannot be stored as constants in `.rdata` (the binary has no data sections). They're built field-by-field at runtime:

```c
NOINLINE EFI_GUID MakeGuid() {
    EFI_GUID g;
    g.Data1 = 0x964E5B22;
    g.Data2 = 0x6459;
    g.Data3 = 0x11D2;
    g.Data4[0] = 0x8E; g.Data4[1] = 0x39;
    g.Data4[2] = 0x00; g.Data4[3] = 0xA0;
    g.Data4[4] = 0xC9; g.Data4[5] = 0x69;
    g.Data4[6] = 0x72; g.Data4[7] = 0x3B;
    return g;
}
```

The `NOINLINE` attribute is critical — without it, the compiler may constant-fold the GUID into a `.rdata` section, breaking position independence.

## Pseudo-Async Networking

UEFI has no `poll()`, `select()`, or signal-based async. Network operations (TCP connect/send/receive) are asynchronous at the protocol level but must be polled manually:

```c
// Issue async operation
EFI_STATUS status = tcp->Connect(tcp, &connectToken);

// Busy-poll for completion
while (connectToken.CompletionToken.Status == EFI_NOT_READY) {
    tcp->Poll(tcp);           // kick the firmware's TCP state machine
    bs->Stall(1000);          // sleep 1 millisecond
    elapsed += 1;
    if (elapsed > timeout_ms)
        return EFI_TIMEOUT;
}
```

The `Poll()` call is essential — without it, the firmware's TCP stack doesn't process incoming packets or advance connection state.

### Network Initialization Sequence

UEFI networking requires explicit multi-step initialization:

```
1. LocateProtocol(SNP_GUID) → Simple Network Protocol
2. snp→Start()              → enable NIC hardware
3. snp→Initialize(0, 0)     → initialize NIC buffers
4. Verify snp→Mode→State == EfiSimpleNetworkInitialized
5. LocateProtocol(IP4_CONFIG2_GUID) → IPv4 configuration
6. ip4Config→SetData(Policy = Dhcp)  → start DHCP
7. Loop: ip4Config→GetData(InterfaceInfo) until success (50 retries × 100ms)
8. Stall(500000)             → 500ms settle time for TCP stack readiness
9. LocateProtocol(TCP_SERVICE_BINDING_GUID)
10. serviceBinding→CreateChild(&tcpHandle)
11. HandleProtocol(tcpHandle, TCP_PROTOCOL_GUID) → usable TCP interface
```

State flags (`NetworkInitialized`, `DhcpConfigured`, `TcpStackReady`) in `EFI_CONTEXT` prevent redundant initialization.

## Boot Services vs Runtime Services

**Boot Services** (18+ functions): Available until `ExitBootServices()` is called. Includes memory allocation, protocol discovery, event management, and image loading. The OS calls `ExitBootServices()` during boot to take control of hardware.

**Runtime Services** (11+ functions): Survive after `ExitBootServices()`. Includes real-time clock, NVRAM variables, system reset, and virtual address mapping. These are the only firmware functions available to a running OS.

For this runtime (which runs as a UEFI application, not an OS), both are available.

## Status Code Model

UEFI uses `EFI_STATUS` (machine-word-sized) with the **high bit** indicating error:

```c
#define EFI_ERROR_MASK  ((USIZE)1 << (sizeof(USIZE) * 8 - 1))  // bit 63 on 64-bit
#define EFI_SUCCESS     0
#define EFI_NOT_FOUND   (EFI_ERROR_MASK | 14)
#define EFI_TIMEOUT     (EFI_ERROR_MASK | 18)
```

This is fundamentally different from POSIX (negative errno) and Windows (NTSTATUS negative bit 31).

## Key Protocol Interfaces

| Protocol | GUID | Purpose |
|---|---|---|
| **Simple File System** | `964E5B22-...` | Filesystem volume access |
| **File** | `09576E92-...` | File/directory operations |
| **TCP4** | `00720665-...` | IPv4 TCP connections |
| **TCP6** | `46E44855-...` | IPv6 TCP connections |
| **IP4 Config2** | `5B446ED1-...` | DHCP / static IP configuration |
| **Simple Network** | `A19832B9-...` | Raw NIC access (Transmit/Receive) |
| **Graphics Output** | `9042A9DE-...` | Framebuffer and display modes |
| **Service Binding** | `937FE521-...` | Per-connection child handle management |

## Comparison with OS Platforms

| Aspect | OS Platforms | UEFI |
|---|---|---|
| **Invocation** | Trap instruction | Direct function call |
| **Privilege** | User mode → kernel | Ring 0 (same level) |
| **Discovery** | Fixed syscall numbers | GUID-based protocol lookup |
| **Calling convention** | Platform ABI | Microsoft x64 (on x86_64) |
| **Error model** | Negative return / carry flag | High-bit status code |
| **Async I/O** | poll/ppoll/select | Manual busy-poll loop |
| **Networking** | Socket syscalls | Protocol-based TCP stack |
| **Memory** | mmap/VirtualAlloc | AllocatePages / AllocatePool |
