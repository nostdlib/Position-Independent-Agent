# poly-transform

Polymorphic instruction transform tool for position-independent shellcode. Generates random per-build instruction subsets and verifies compiled binaries against them.

## Concept

Every build selects a **random subset of N instructions** (default: 10) from the architecture's full instruction set. Two builds of the same source code produce binaries with completely different instruction vocabularies, making static signature detection effectively impossible.

The instruction set is seeded from the build timestamp via FNV-1a (same mechanism as the DJB2 compile-time seeding in `core/algorithms/djb2.h`), so it's deterministic per build but unique across builds.

## Valid Combinations

| Architecture | Instructions used | Valid random-10 sets |
|---|---|---|
| x86_64 | ~154 | ~4.8 billion |
| i386 | ~132 | ~3.2 billion |
| AArch64 | ~107 | ~1.9 billion |
| ARMv7 | ~145 | ~2.5 billion |
| RISC-V 64 | ~123 | ~580 million |
| RISC-V 32 | ~84 | ~33 million |

## Usage

### Generate a random instruction set

```bash
poly-transform generate --arch x86_64 --seed 0xDEADBEEF --count 10
```

### Analyze instruction usage in a binary

```bash
# First generate disassembly
llvm-objdump -d --no-addresses --no-show-raw-insn output.elf > output.disasm

# Then analyze
poly-transform analyze --arch x86_64 --disasm output.disasm
```

### Verify binary against an instruction set

```bash
poly-transform verify --arch x86_64 --seed 0xDEADBEEF --disasm output.disasm
```

### Print valid combination counts

```bash
poly-transform combos --arch x86_64
poly-transform combos  # all architectures
```

## Instruction Categories

Instructions are grouped into categories. During generation, a minimum number of instructions is selected from each category to ensure Turing completeness:

| Category | x86_64 | AArch64 | RISC-V |
|---|---|---|---|
| System (mandatory) | `syscall` | `svc` | `ecall` |
| Data Movement (pick 2) | `mov`, `lea`, `push`, `pop`, ... | `mov`, `ldr`, `str`, ... | `lw`, `sw`, `mv`, `li`, ... |
| Arithmetic (pick 2) | `add`, `sub`, `imul`, `mul`, ... | `add`, `sub`, `mul`, ... | `add`, `addi`, `sub`, ... |
| Logic (pick 1) | `xor`, `and`, `or` | `and`, `orr`, `eor`, ... | `xor`, `and`, `or`, ... |
| Compare (pick 1) | `cmp`, `test`, `bt` | `cmp`, `cmn`, `tst` | `slt`, `slti`, ... |
| Branch (pick 1) | `jcc`, `jmp` | `b.cond`, `cbz`, ... | `beq`, `bne`, ... |
| Control (pick 1) | `call`, `ret` | `bl`, `ret`, ... | `jal`, `jalr`, ... |

Remaining budget (count minus mandatory minus minimum picks) is distributed randomly across all categories, including optional ones (shift, float, conditional).

## Building

No external dependencies — plain C++17:

```bash
cmake -B build
cmake --build build
```

## Integration

The tool integrates into the main build system via `cmake/PolyTransform.cmake`. It runs as a post-build analysis step (informational — does not fail the build). The rewrite engine for actually transforming instructions is a future enhancement.

## Architecture

```
Phase 1 (current): Analysis & Verification
  ┌─────────┐    ┌──────────────┐    ┌────────────────┐
  │ Compile  │ -> │ llvm-objdump │ -> │ poly-transform │ -> report
  │ & link   │    │ (disassemble)│    │ (analyze)      │
  └─────────┘    └──────────────┘    └────────────────┘

Phase 2 (future): Binary Rewriting
  ┌─────────┐    ┌────────────────┐    ┌──────────────┐
  │ Compile  │ -> │ poly-transform │ -> │ output.bin   │
  │ & link   │    │ (rewrite)      │    │ (morphed)    │
  └─────────┘    └────────────────┘    └──────────────┘
```

## License

Same as the parent project.
