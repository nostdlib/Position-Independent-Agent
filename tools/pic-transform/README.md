# pic-transform

LLVM pass that eliminates data sections (`.rodata`, `.rdata`, `.data`, `.bss`) from compiled code by transforming string literals, floating-point constants, and constant arrays into stack-local allocations with immediate-value stores. Produces binaries with only a `.text` section.

Designed for position-independent shellcode — also useful anywhere pure-code binaries are required.

## What it does

| Input (normal compilation) | Output (after pic-transform) |
|---|---|
| `"Hello"` → `.rodata` section | `movabsq $0x6F6C6C6548, %rax` + stack store |
| `3.14159` → constant pool in `.rodata` | `movabsq $0x400921FB..., %rax` + `movq %rax, %xmm0` |
| `const int arr[] = {1,2,3}` → `.rodata` section | Packed immediate stores to stack |

## Usage

### As a standalone tool

```bash
# Compile to bitcode
clang++ -emit-llvm -c -O2 input.cpp -o input.bc

# Transform: eliminate all data sections
pic-transform input.bc -o output.bc

# Compile to final binary
clang++ output.bc -o output.exe
```

### As an LLVM pass plugin (Linux)

```bash
clang++ -fpass-plugin=./PICTransform.so -O2 input.cpp -o output
```

## Building

### Requirements

- LLVM 20+ development headers and libraries
- CMake 3.20+
- Ninja or Make

### Linux

```bash
# Install LLVM dev packages
sudo apt install llvm-22-dev

# Build
cmake -B build -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm
cmake --build build
```

### Windows

Download the `clang+llvm-22.x-x86_64-pc-windows-msvc.tar.xz` from [LLVM releases](https://github.com/llvm/llvm-project/releases) and extract it.

```powershell
cmake -B build -G Ninja -DLLVM_DIR="path/to/clang+llvm-22.x/lib/cmake/llvm" -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
```

### macOS

```bash
brew install llvm
cmake -B build -DLLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm
cmake --build build
```

## Using as a Git submodule

```bash
git submodule add https://github.com/mrzaxaryan/pic-transform.git
```

Then in your CMakeLists.txt:

```cmake
add_subdirectory(pic-transform)
```

Or download a prebuilt binary from [Releases](https://github.com/mrzaxaryan/pic-transform/releases).

## How it works

Operates on LLVM IR with three transformation passes:

1. **Global constant elimination** — Finds `@.str = private constant [N x i8] c"..."` globals, extracts raw bytes, replaces all uses with stack allocations initialized via word-packed immediate stores behind inline asm register barriers.

2. **Floating-point constant elimination** — Replaces `ConstantFP` operands (emitted as `.rodata` constant pool loads) with integer immediates bitcast to float, preventing constant pool generation.

3. **Remaining global diagnostics** — Warns about globals that could not be eliminated.

Register barriers (`asm sideeffect "", "=r,0"`) prevent the optimizer from recognizing immediate store patterns and coalescing them back into a `memcpy` from `.rodata`.

## License

AGPL-3.0 -- see [LICENSE](LICENSE).
