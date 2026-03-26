# Build System: From C++23 to Raw Shellcode

This document traces the complete build pipeline that transforms ordinary C++23
source code into a raw, position-independent binary blob. The build system does
more heavy lifting than the application code itself -- it enforces invariants
that, if broken, produce silent runtime crashes rather than compiler errors.

**Prerequisites:** [01 - What Is PIC](01-what-is-pic.md), [02 - Project Overview](02-project-overview.md).

---

## 1. What Is CMake and Why Use It

CMake is a build system *generator* -- it produces Makefiles or Ninja files,
not binaries. This project targets 8 platforms x 7 architectures x debug/release
= 60+ configurations. CMake presets let you pick one and build with a single
command. The module structure:

```
CMakeLists.txt
  +-- cmake/Common.cmake            (orchestrator)
       +-- cmake/Options.cmake       (validates PIR_PLATFORM, PIR_ARCH)
       +-- cmake/Triples.cmake       (maps platform+arch to target triples)
       +-- cmake/CompilerFlags.cmake  (compiler and linker flags)
       +-- cmake/Sources.cmake        (collects .cc and .h files)
       +-- cmake/PICTransform.cmake   (acquires the LLVM pass)
       +-- cmake/PostBuild.cmake      (extraction, verification, base64)
  +-- cmake/Target.cmake             (loads platform module, creates target)
       +-- cmake/platforms/<Platform>.cmake
```

---

## 2. The Full Build Pipeline

```
 SOURCE CODE (.cc)
       |  [1] Compile (pic-transform runs here, PIR_BASE_FLAGS applied)
       v
 OBJECT FILES (.o)
       |  [2] Link (linker script, function order, LTO optimization)
       v
 NATIVE EXECUTABLE (ELF / PE / Mach-O)
       |  [3] OSABI patch (FreeBSD=9, Solaris=6)
       |  [4] ExtractBinary: llvm-objcopy --dump-section -> output.bin
       |  [5] Base64 encode -> output.b64.txt
       |  [6] VerifyPICMode: parse map file, verify PIC invariants
       v
 output.bin  <-- THIS IS THE SHELLCODE
```

Debug builds at `-O0`/`-Og` skip steps 4-6 (those opt levels generate data
sections that break PIC). You can still run the debug executable normally.

---

## 3. Compiler Flags: The Complete Table

Assembled in `cmake/CompilerFlags.cmake`. Every flag exists to prevent extra
sections, survive without a runtime, or produce correct freestanding code.

### Base Flags (All Builds)

| Flag | Why It Matters for PIC |
|------|------------------------|
| `-std=c++23` | Enables `consteval`, concepts, modern features used throughout |
| `-Werror -Wall -Wextra` | Catches accidental data-section generation early |
| `-nostdlib` | No libc -- no `printf`, `malloc`, `memcpy` |
| `-fno-ident` | Suppresses `.comment` section (compiler version string) |
| `-fno-exceptions` | Exception tables (`.eh_frame`, `.gcc_except_table`) are data sections |
| `-fno-rtti` | RTTI type-info tables are data sections |
| `-fno-builtin` | Builtins like `memcpy` may reference nonexistent external symbols |
| `-fno-stack-check` | Stack check routines call into the nonexistent runtime |
| `-fno-jump-tables` | Jump tables are `.rodata` with absolute addresses |
| `-ffunction-sections` | Each function in its own section; enables dead code elimination |
| `-mno-stack-arg-probe` | `__chkstk` is a runtime function that does not exist |
| `-mno-implicit-float` | Prevents SSE/NEON for non-FP ops that need `.rodata` constants |

### Release-Only Additions

| Flag | Purpose |
|------|---------|
| `-fvisibility=hidden` | All symbols non-exportable; prevents GOT/PLT generation |
| `-fno-threadsafe-statics` | Guard variables use `__cxa_guard_*` from the nonexistent runtime |
| `-fno-math-errno` | No runtime to set `errno` in |
| `-flto=full` | Full link-time optimization (see [Section 5](#5-link-time-optimization-lto)) |
| `-fomit-frame-pointer` | Frees RBP as a general-purpose register |

### MIPS64-Specific

MIPS defaults to `-mabicalls` (GOT-relative code via `$gp`). Incompatible with
PIC shellcode since there is no dynamic linker to populate the GOT:

```cmake
list(APPEND PIR_BASE_FLAGS -mno-abicalls -fno-pic -G0)
```

`-G0` disables small data sections (`.sdata`/`.sbss`) whose GP-relative offsets
overflow at `-O0`.

---

## 4. The Red Zone Problem

The "red zone" is 128 bytes below RSP on x86_64. Leaf functions can use it
without adjusting RSP -- a performance optimization from the System V ABI.

```
         +-----------------+
         |  caller frame   |
         +-----------------+  <-- RSP
         |   RED ZONE      |  128 bytes usable WITHOUT adjusting RSP
         +-----------------+  <-- RSP - 128
```

Signal handlers and interrupts push data *at RSP*, clobbering the red zone.
Shellcode may execute in signal handler contexts or foreign stacks where the
red zone is unsafe. `-mno-red-zone` disables the optimization on every x86_64
platform (Windows, Linux, macOS, FreeBSD, Solaris, UEFI). ARM64 has no red
zone concept, so the flag is unnecessary there.

---

## 5. Link-Time Optimization (LTO)

Release builds use `-flto=full`. The compiler emits LLVM bitcode instead of
machine code; the linker sees all code at once and optimizes across translation
unit boundaries. Dead code vanishes, cross-file inlining kicks in, the binary
shrinks.

**The constant pool problem:** LTO can generate unnamed data blocks placed in
the code section. On macOS, these can land *before* `entry_point`, so offset 0
of the shellcode is data, not code. Two workarounds:

1. **macOS:** `entry_point.cc` is compiled with `-fno-lto`. Apple's linker
   places non-LTO sections before LTO sections, guaranteeing offset 0 is code.
2. **Linker scripts:** On i386/RISC-V/MIPS64, LTO emits `.rodata.cst*` entries.
   Linker scripts merge `.rodata` into `.text` so they stay in the extracted
   binary.

`-fno-jump-tables` is repeated in `PIR_BASE_LINK_FLAGS` because with LTO,
machine code generation happens at link time -- the flag must reach the backend:

```cmake
set(PIR_BASE_LINK_FLAGS -nostdlib -fno-jump-tables)
```

---

## 6. Function Order Files

Shellcode execution starts at byte 0. If `entry_point` is not there, you crash.
Function order files force the linker to place `entry_point` first. The Linux
version (`cmake/data/function.order.linux`) is one line:

```
entry_point
```

macOS prefixes an underscore per Mach-O convention: `_entry_point`.

| Platform | Linker Flag |
|----------|-------------|
| Linux / FreeBSD / Solaris | `--symbol-ordering-file=<path>` |
| Windows / UEFI | `/ORDER:@<path>` |
| macOS / iOS | `-order_file,<path>` |

---

## 7. Linker Scripts

Used on architectures where LTO generates `.rodata` entries that would be lost
when `ExtractBinary` extracts only `.text`.

**i386** (`cmake/data/linker.i386.ld`) -- merge `.rodata` into `.text`:

```ld
SECTIONS
{
    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
}
```

`.text` inputs are listed first so `--symbol-ordering-file` can place
`entry_point` at offset 0. Everything not listed is discarded.

**MIPS64** -- merges everything (including `.data`, `.bss`, `.got`):

```ld
SECTIONS
{
    . = 0x120000000;
    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*)
        *(.got)
    }
}
```

LLD synthesizes a 16-byte mandatory MIPS GOT header even with `-mno-abicalls`.
Merging it into `.text` prevents a PIC verification failure. The base address
`0x120000000` is required because `qemu-mips64el-static` cannot map VA 0.

**Solaris** -- controls ELF program headers:

```ld
PHDRS
{
    text PT_LOAD;
}
SECTIONS
{
    . = 0x400000;
    .text : { *(.text .text.*) *(.got.plt) } :text
}
```

The `PHDRS` block prevents `PT_PHDR` emission. The Solaris kernel rejects
static binaries with `PT_PHDR` but no `PT_INTERP`:
`if (uphdr && !intphdr) goto bad; // ENOEXEC`.

---

## 8. The pic-transform Build Step

A custom LLVM pass that finds string literals, float constants, and arrays that
would land in `.rodata` and converts them to stack-based immediate stores.
`"Hello"` becomes instructions that push `0x6F6C6C6548` onto the stack.

**Plugin mode** (preferred) -- loaded into the compiler via `-fpass-plugin=`:

```cmake
target_compile_options(${PIR_TRIPLE} PRIVATE
    "SHELL:-fpass-plugin=${PIC_TRANSFORM_PLUGIN}")
```

**Standalone mode** (fallback) -- 3-step compile pipeline:

```
clang++ -emit-llvm -c source.cc -o source.o.bc
pic-transform source.o.bc -o source.o.transformed.bc
clang++ -c source.o.transformed.bc -o source.o
```

`PICTransform.cmake` finds the tool on PATH first, then builds it from
`tools/pic-transform/` if LLVM dev files are available. Plugin is preferred
over standalone to avoid the 3-step overhead. Without this pass, most C++ code
generates data sections and breaks position-independence.

---

## 9. PIC Verification

`VerifyPICMode.cmake` parses the linker map file and checks three things:

**Check 1 -- entry_point at offset 0.** Format-specific regex matching:
PE checks `0001:00000000 ... .text$entry_point`, ELF checks `.text.entry_point`
immediately after `.text`, Mach-O checks `_entry_point` as first symbol. On
macOS, it also compares the first symbol's VMA against `__TEXT,__text` start
VMA to catch unnamed LTO data preceding `_entry_point`.

**Check 2 -- no forbidden sections.** Scans for `.rdata`, `.rodata`, `.data`,
`.bss`, `.got`, `.plt` with non-zero size.

**Check 3 -- macOS section audit.** Checks for `__DATA,__data`, `__DATA,__bss`,
`__DATA_CONST,__got`, `__DATA,__la_symbol_ptr`, and *any* `__TEXT` subsection
that is not `__text`. If anything is found:

```
[pir:verify] Forbidden data sections break position-independence!
Found: .rodata, .got
```

This turns subtle runtime crashes into loud build-time errors.

---

## 10. ExtractBinary: From ELF to Raw Shellcode

The compiler produces a full executable with headers and metadata. We only need
the raw machine code. `ExtractBinary.cmake` runs `llvm-objcopy`:

```
llvm-objcopy --dump-section=.text=output.bin input.elf output.tmp
```

For Mach-O, the section name is `__TEXT,__text`. The explicit output path
prevents objcopy from modifying the input in-place -- Mach-O round-tripping
can corrupt code signatures, causing SIGKILL on ARM64. The script also
generates a disassembly (`output.txt`) and extracted strings
(`output.strings.txt`), then reports the extraction ratio.

After extraction, `Base64Encode.cmake` produces `output.b64.txt` for text
transport, handling GNU/BSD `base64` flag incompatibilities.

---

## 11. OSABI Patching

ELF byte 7 (`EI_OSABI`) identifies the target OS. LLD always writes 0
(`ELFOSABI_NONE`), which works for Linux but not others:

| Platform | Required Value | Without It |
|----------|---------------|------------|
| FreeBSD | 9 (`ELFOSABI_FREEBSD`) | `ENOEXEC` |
| Solaris | 6 (`ELFOSABI_SOLARIS`) | "Exec format error" |

`PatchElfOSABI.cmake` overwrites this single byte using `printf` + `dd`:

```bash
printf '\011' | dd of=output.elf bs=1 seek=7 count=1 conv=notrunc
```

One byte. Without it the kernel refuses to load the binary.

---

## 12. macOS ARM64 Special Handling

The most complex platform configuration. Every workaround was discovered
through painful debugging.

**No static binaries.** Apple's ARM64 kernel requires dyld. A static binary
gets SIGKILL. Solution: omit `-static` on ARM64; the linker adds
`LC_LOAD_DYLINKER`, dyld starts, sees no libraries to load, jumps to
`_entry_point`.

**Hidden visibility is mandatory.** Without it, weak symbols (template
instantiations) generate `__TEXT,__stubs` + `__DATA,__la_symbol_ptr`. Stubs
load pointers from `__DATA` (not mapped by PIC loader) and crash.

**LTO entry point fix.** LTO constant pools can land before `_entry_point`.
Apple's linker places non-LTO sections first, so `entry_point.cc` is compiled
with `-fno-lto`.

**4KB page alignment for ADRP.** ARM64 `ADRP` computes page-relative addresses.
Without alignment, `__text` starts at a non-page-aligned VMA. After extraction
and loading at a page-aligned address, every `ADRP` computes the wrong page:

```cmake
pir_add_link_flags(-sectalign,__TEXT,__text,1000)
```

**dyld_stub_binder.** The linker adds this undefined symbol for dynamic
executables. `-nostdlib` means `libSystem` is not linked. Fix:
`-undefined,dynamic_lookup`. The symbol is never called because hidden
visibility eliminates all lazy-binding stubs.

```
 PROBLEM                          WORKAROUND
 +-------------------------------+--------------------------------------+
 | Kernel requires dyld          | No -static; LC_LOAD_DYLINKER added  |
 | Weak symbols -> stubs -> crash| -fvisibility=hidden on all builds   |
 | LTO data before entry_point   | entry_point.cc compiled -fno-lto    |
 | ADRP page alignment broken    | -sectalign,__TEXT,__text,1000       |
 | dyld_stub_binder undefined    | -undefined,dynamic_lookup           |
 +-------------------------------+--------------------------------------+
```

---

**See also:** [ARCHITECTURE-MAP.md](ARCHITECTURE-MAP.md) for a visual map of
how all components connect.
