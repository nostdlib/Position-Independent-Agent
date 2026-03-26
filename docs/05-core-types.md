# Core Types: The Vocabulary of the Codebase

This document covers the custom type system that underpins the entire Position-Independent
Agent runtime. Every file in the project imports `primitives.h`, and nearly every function
signature involves `Result`, `Span`, or `Error`. Understanding these types is not optional
-- they are the vocabulary you will read on every page of this codebase.

If you have not yet read [01-what-is-pic.md](01-what-is-pic.md) and
[03-build-system.md](03-build-system.md), do so first. The "why" behind these types
flows directly from the Golden Rule: no data sections, no standard library, no runtime
support beyond what the code provides for itself.

---

## 1. Why Custom Types Instead of `<stdint.h>`

The obvious question: why define `INT32`, `UINT64`, and friends by hand when `<cstdint>`
already exists?

Because `<cstdint>` is part of libc. The project compiles with `-nostdlib` -- no standard
headers, no standard library, nothing. Including `<cstdint>` would pull in a header that
assumes a hosted environment, which is exactly what position-independent code cannot assume.

The solution is compiler builtins. Clang and GCC predefine types like `__SIZE_TYPE__` and
`__INTPTR_TYPE__` that are guaranteed correct for the target platform without any header
dependency. From `src/core/types/primitives.h`:

```cpp
typedef signed char      INT8,  *PINT8;
typedef unsigned char    UINT8, *PUINT8, **PPUINT8;
typedef signed short     INT16, *PINT16;
typedef unsigned short   UINT16, *PUINT16;
typedef signed int       INT32, *PINT32;
typedef unsigned int     UINT32, *PUINT32, **PPUINT32;
typedef signed long long INT64, *PINT64, **PPINT64;
typedef unsigned long long UINT64, *PUINT64, **PPUINT64;
```

These are plain C typedefs. They use fundamental types whose sizes are guaranteed by the
platform ABI (e.g., `int` is 32 bits on every target this project supports). No headers
required -- the compiler knows these types intrinsically.

The pointer companion types (`PINT32`, `PUINT8`, etc.) follow Windows SDK convention.
Whether you find that convenient or annoying is a matter of taste, but it does reduce
visual noise in function signatures that deal with output parameters.

For void and pointer types, the same pattern applies:

```cpp
typedef void VOID, *PVOID, **PPVOID;
typedef const void *PCVOID, **PPCVOID;
```

The point is self-sufficiency. The type system depends on nothing outside the compiler
itself. As discussed in [03-build-system.md](03-build-system.md), the build enforces
`-nostdlib -ffreestanding -fno-exceptions -fno-rtti`, so any accidental standard header
inclusion would fail at compile time.

---

## 2. USIZE: The Pointer-Sized Workhorse

```cpp
typedef __SIZE_TYPE__ USIZE, *PUSIZE;
typedef __INTPTR_TYPE__ SSIZE, *PSSIZE;
```

`USIZE` is the project's equivalent of `size_t`. It is an unsigned integer whose width
matches the pointer size of the target: 4 bytes on 32-bit platforms (addressing up to
4 GB), 8 bytes on 64-bit platforms (addressing up to 16 EB).

`__SIZE_TYPE__` is a compiler builtin. Clang defines it as the correct type for the
target triple without any header. On `x86_64-unknown-linux-gnu`, it expands to
`unsigned long`. On `i686-pc-windows-msvc`, it expands to `unsigned int`. The typedef
abstracts that away.

You will see `USIZE` in virtually every function in this codebase:

- Array indices and loop counters
- Byte counts passed to memory operations
- Buffer sizes in `Span<T>`
- The size parameter of `operator new`

Why not just use `UINT64` everywhere? Because on a 32-bit target, `UINT64` arithmetic
generates two-register operations for what should be single-register work. `USIZE` gives
you the natural word size, which is what memory operations process in one cycle. It is
the right type for "how big is this thing in memory."

`SSIZE` is the signed counterpart, built from `__INTPTR_TYPE__`. It exists for signed
offsets and pointer differences, where a negative value is meaningful.

---

## 3. Character Types and the TCHAR Concept

### CHAR, WCHAR, and the Platform Split

```cpp
typedef char    CHAR, *PCHAR, **PPCHAR;
typedef wchar_t WCHAR, *PWCHAR, **PPWCHAR;
```

`CHAR` is an 8-bit narrow character for UTF-8 strings. No surprises.

`WCHAR` is where it gets interesting. It wraps `wchar_t`, whose size is
platform-dependent:

| Platform       | `sizeof(WCHAR)` | Encoding   |
|----------------|------------------|------------|
| Windows / UEFI | 2 bytes          | UTF-16     |
| Linux / macOS  | 4 bytes          | UTF-32     |

This is a historical divergence. Windows committed to 2-byte wide characters in the
early 1990s (UCS-2, later extended to UTF-16 with surrogate pairs). The Unix world went
with 4-byte `wchar_t` to represent the full Unicode range in a single code unit (UTF-32).

For code that only talks to the local OS API, `WCHAR` is correct on both platforms. The
Windows kernel expects UTF-16 strings, and `WCHAR` is 2 bytes there. Linux syscalls use
byte strings (UTF-8), and when wide characters are needed locally, 4-byte `WCHAR` works
fine.

### CHAR16: Wire Protocol Safety

```cpp
typedef unsigned short CHAR16, *PCHAR16, **PPCHAR16;
```

The problem arises when data crosses a network boundary or is serialized to a fixed
format. If you write a `WCHAR` string on Linux (4 bytes per character) and read it on
Windows (2 bytes per character), you get garbage.

`CHAR16` is always `unsigned short` -- always 2 bytes, on every platform. It exists
specifically for wire protocols, file formats, and any situation where the byte layout
must be identical regardless of where the code runs. Think of it as "the type you use
when the other end of the connection is not necessarily the same OS."

### The TCHAR Concept

```cpp
template <typename TChar>
concept TCHAR = __is_same_as(TChar, CHAR) || __is_same_as(TChar, WCHAR);
```

This is a C++20 concept that constrains template parameters to either `CHAR` or `WCHAR`.
It serves as a compile-time gate: any template function constrained with `TCHAR` will
refuse to instantiate with `int`, `float`, `CHAR16`, or anything else.

Without this concept, passing the wrong type to a string function would produce the
typical wall of template error messages. With `TCHAR`, you get a single clear diagnostic:
"constraint not satisfied." The concept uses `__is_same_as`, a Clang builtin, instead
of `std::is_same_v` -- again, because `<type_traits>` is a standard library header and
not available under `-nostdlib`.

Every generic string function in `src/core/strings/` is constrained with `TCHAR`. See
[the memory and strings chapter](../book-notes/06-memory-and-strings.md) for how these
functions work.

---

## 4. Result<T, E>: Error Handling Without Exceptions

### Why Not Exceptions

C++ exceptions (`try`/`catch`) require runtime support: stack unwinding tables
(`__gxx_personality_v0`), type information (`typeid`, RTTI), and `.eh_frame` sections.
All of these create data sections that violate the Golden Rule. The build disables them
entirely with `-fno-exceptions -fno-rtti`. Even if they were available, exceptions are
unpredictable in code that must control its own memory layout.

`Result<T, E>` is the alternative. It is a tagged union -- it holds either a success
value of type `T` or an error of type `E`, never both. Callers must check before
accessing. This is the same pattern as Rust's `Result<T, E>`.

### The Union Memory Layout

From `src/core/types/result.h`:

```cpp
template <typename T, typename E>
class [[nodiscard]] Result
{
    static constexpr BOOL IsVoid = __is_same_as(T, void);
    using STORED_TYPE = typename VOID_TO_TAG<T>::Type;

    union
    {
        STORED_TYPE m_value;
        E m_error;
    };
    BOOL m_isOk;
    // ...
};
```

The `union` is the key. Because `m_value` and `m_error` share the same memory, the
total size of a `Result<T, E>` is:

```
sizeof(Result<T, E>) = max(sizeof(T), sizeof(E)) + sizeof(BOOL)
```

Without the union, you would need `sizeof(T) + sizeof(E) + sizeof(BOOL)`. For a
`Result<UINT64, Error>` where `Error` is ~30 bytes, the union saves 8 bytes per
instance. On the stack, in a tight loop, that matters.

The `m_isOk` flag is the discriminator. It tells you which union member is active.
Reading the wrong member is undefined behavior -- the API enforces correct access
through `IsOk()` / `IsErr()` checks.

### The void Specialization

When `T` is `void` (a function that returns nothing on success), there is nothing to
store in the value slot. The `VOID_TAG` struct handles this:

```cpp
struct VOID_TAG {};

template <>
struct VOID_TO_TAG<void>
{
    using Type = VOID_TAG;
};
```

But `Result<void, Error>` gets an even more aggressive optimization -- a full template
specialization that eliminates the union and discriminator entirely:

```cpp
template <>
class [[nodiscard]] Result<void, Error>
{
    struct Error m_error;

public:
    [[nodiscard]] static constexpr FORCE_INLINE Result Ok() noexcept
    {
        Result r;
        r.m_error = {};   // Error with Code == None means success
        return r;
    }

    [[nodiscard]] constexpr FORCE_INLINE BOOL IsOk() const noexcept
    {
        return m_error.Code == Error::None;
    }
    // ...
};
```

Success is encoded as `Error::None` (code zero). No union, no discriminator flag.
`sizeof(Result<void, Error>)` equals `sizeof(Error)` exactly. This matters because
`Result<void, Error>` is the most common return type in the codebase -- every function
that either succeeds or fails without producing a value uses it.

### Usage Patterns

**Creating results:**

```cpp
// Success with a value
return Result<UINT32, Error>::Ok(42);

// Success with no value
return Result<void, Error>::Ok();

// Failure with a single error
return Result<void, Error>::Err(Error::Socket_CreateFailed_Open);
```

**Checking and accessing:**

```cpp
auto result = Socket::Create();
if (!result.IsOk()) {
    return Result<void, Error>::Err(result, Error::Tls_OpenFailed_Socket);
}
auto socket = result.Value();
```

The `[[nodiscard]]` attribute on the class means the compiler warns if a caller ignores
a returned `Result`. You cannot silently discard an error.

**Error propagation (chaining):**

The 2-arg and propagation `Err()` overloads chain errors together. When function A fails
because function B failed, B's entire error chain is preserved inside A's error:

```cpp
// In Socket::Open():
auto createResult = Socket::Create();
if (createResult.IsErr())
    return Result<void, Error>::Err(createResult, Error::Socket_OpenFailed_Connect);
```

This wraps `createResult`'s error under the new `Socket_OpenFailed_Connect` code, using
`Error::Wrap()` internally. The result is a chain: the outer code tells you what
operation failed at this level, the inner code tells you why. More on this in the Error
section below.

**Move-only semantics:**

```cpp
Result(const Result &) = delete;
Result &operator=(const Result &) = delete;
```

Results cannot be copied, only moved. This prevents accidental duplication of error state
and makes ownership transfer explicit.

---

## 5. Span<T>: Pointer-Plus-Length, Done Right

### The Problem with Raw Pointers

The classic C pattern for passing a buffer is `void foo(const char* data, size_t len)`.
This has two problems:

1. The pointer and length are separate arguments. You can swap them, forget one, or
   pass a length from the wrong buffer.
2. There is no type-level guarantee that the two parameters are related.

`Span<T>` bundles them into a single object. From `src/core/types/span.h`:

```cpp
template <typename T>
class Span<T, DYNAMIC_EXTENT>
{
private:
    T *m_data;
    USIZE m_size;
    // ...
};
```

A function that takes `Span<const UINT8>` instead of `(const UINT8*, USIZE)` gets a
single parameter where the pointer and length are guaranteed to refer to the same buffer.

### String Literal Handling

One of the more subtle design choices in `Span` is the array constructor:

```cpp
template <USIZE N>
constexpr FORCE_INLINE Span(T (&arr)[N]) : m_data(arr),
    m_size((__is_const(T) &&
            (__is_same_as(__remove_const(T), char) ||
             __is_same_as(__remove_const(T), wchar_t)) &&
            N > 0) ? N - 1 : N) {}
```

When you write `Span<const CHAR> s = "hello"`, the string literal `"hello"` is a
`const char[6]` (5 characters plus null terminator). The constructor detects that `T`
is `const char` and automatically subtracts 1, giving you `Size() == 5`.

For non-character arrays or non-const character arrays, it uses the full array size `N`.
This is the right default: a `UINT8 buffer[64]` should produce a span of 64 elements,
not 63. The null-terminator exclusion only applies to the specific case of const character
array references, which in practice means string literals.

This uses Clang builtins `__is_const` and `__is_same_as` instead of standard type traits,
keeping the header free of `<type_traits>` dependencies.

### Static Extent: Compile-Time Size

```cpp
template <typename T, USIZE Extent>
class Span
{
private:
    T *m_data;       // No m_size member at all
    // ...
public:
    constexpr FORCE_INLINE USIZE Size() const { return Extent; }
};
```

`Span<T, N>` stores only a pointer. The size `N` is a template parameter baked into the
type itself. `sizeof(Span<UINT8, 32>)` is just `sizeof(UINT8*)` -- 8 bytes on a 64-bit
platform, versus 16 bytes for the dynamic-extent version.

More importantly, the compiler knows the size at compile time and can optimize
accordingly: loop unrolling, bounds elimination, dead-store removal. When you slice a
static-extent span with a compile-time operation, the result preserves the static extent:

```cpp
UINT8 digest[32];
Span<UINT8, 32> full(digest);
Span<UINT8, 16> lo = full.First<16>();   // Static extent preserved
Span<UINT8, 16> hi = full.Last<16>();    // Still compile-time sized
Span<UINT8, 8>  mid = full.Subspan<4, 8>(); // Static slice
```

Static-extent slicing methods use `static_assert` to catch out-of-bounds slices at
compile time:

```cpp
template <USIZE Count>
constexpr FORCE_INLINE Span<T, Count> First() const
{
    static_assert(Count <= Extent, "First<Count>: Count exceeds static extent");
    return Span<T, Count>(m_data);
}
```

### Implicit Conversions

`Span` supports three implicit conversions that make the type practical:

1. **Writable to read-only:** `Span<T>` converts to `Span<const T>`
2. **Static to dynamic:** `Span<T, N>` converts to `Span<T>`
3. **Static to dynamic + const:** `Span<T, N>` converts to `Span<const T>`

A function accepting `Span<const UINT8>` can receive a `Span<UINT8>`, a
`Span<UINT8, 32>`, or a `Span<const UINT8, 32>` without any explicit cast. The
conversions go in only one direction -- you cannot implicitly gain mutability or
conjure a static extent from a dynamic one.

---

## 6. Error: Structured Failure with Chaining

### Structure

From `src/core/types/error.h`:

```cpp
struct Error
{
    enum ErrorCodes : UINT32 { None = 0, Socket_CreateFailed_Open = 1, /* ... */ };
    enum class PlatformKind : UINT8 { Runtime = 0, Windows = 1, Posix = 2, Uefi = 3 };

    static constexpr USIZE MaxInnerDepth = 4;

    ErrorCodes   Code;
    PlatformKind Platform;
    UINT8        Depth;
    UINT32       InnerCodes[MaxInnerDepth];
    PlatformKind InnerPlatforms[MaxInnerDepth];
};
```

Every `Error` has two dimensions:

- **Code**: what failed. Either a `ErrorCodes` enumerator (for runtime-layer failures)
  or a raw OS error value (NTSTATUS, errno, EFI_STATUS).
- **Platform**: which layer produced the code. `Runtime` means it is one of the named
  error codes. `Windows`, `Posix`, or `Uefi` means the code is a raw OS value.

This dual tagging is necessary because the same numeric value means different things on
different platforms. Error code `0xC0000034` is `STATUS_OBJECT_NAME_NOT_FOUND` on
Windows but meaningless on Linux. The `PlatformKind` tag tells formatting code whether
to display the value in hex (Windows/UEFI convention) or decimal (POSIX convention).

### Error Chaining via Wrap()

The most powerful feature of `Error` is its cause chain. When an operation fails because
a lower-level operation failed, `Error::Wrap()` preserves the entire history:

```cpp
static constexpr Error Wrap(const Error &inner, UINT32 outerCode,
                            PlatformKind outerPlatform = PlatformKind::Runtime)
{
    Error result;
    result.Code = (ErrorCodes)outerCode;
    result.Platform = outerPlatform;
    result.Depth = 0;

    // Inner[0] = the inner error's top-level code
    if (result.Depth < MaxInnerDepth)
    {
        result.InnerCodes[result.Depth] = (UINT32)inner.Code;
        result.InnerPlatforms[result.Depth] = inner.Platform;
        result.Depth++;
    }

    // Copy inner's own chain beneath
    for (USIZE i = 0; i < inner.Depth && result.Depth < MaxInnerDepth; i++)
    {
        result.InnerCodes[result.Depth] = inner.InnerCodes[i];
        result.InnerPlatforms[result.Depth] = inner.InnerPlatforms[i];
        result.Depth++;
    }

    return result;
}
```

Consider a WebSocket connection attempt that fails because TLS failed because the
socket failed because DNS failed. The resulting error chain looks like:

```
Code     = Ws_HandshakeFailed      (Runtime)
Inner[0] = Tls_OpenFailed_Socket   (Runtime)
Inner[1] = Socket_OpenFailed_Connect (Runtime)
Inner[2] = 0xC0000034              (Windows)    <- raw NTSTATUS
```

This is essentially a stack trace, but for errors instead of function calls. The outer
code tells you what the user-facing operation was. The inner chain tells you exactly
which layer broke and what the OS reported.

The chain has a fixed capacity of `MaxInnerDepth = 4` inner entries. If the error chain
is deeper than that, the oldest (deepest) causes are truncated. In practice, 4 levels
covers the full depth of the networking stack (WebSocket -> TLS -> Socket -> OS), which
is the deepest call chain in the project.

Helper methods `RootCode()` and `RootPlatform()` jump straight to the deepest known
cause without manual chain traversal.

### Platform Factory Methods

```cpp
static constexpr Error Windows(UINT32 ntstatus)  { return Error(ntstatus, PlatformKind::Windows); }
static constexpr Error Posix(UINT32 errnoVal)     { return Error(errnoVal, PlatformKind::Posix); }
static constexpr Error Uefi(UINT32 efiStatus)     { return Error(efiStatus, PlatformKind::Uefi); }
```

These factory methods exist so you never accidentally store an NTSTATUS as a POSIX errno
or vice versa. The platform tag is set at construction time and travels with the error
through the chain.

---

## 7. Why `operator new` Is Deleted

You will find this pattern on `Span`, and the same principle applies to other core types:

```cpp
VOID *operator new(USIZE) = delete;
VOID *operator new[](USIZE) = delete;
VOID operator delete(VOID *) = delete;
VOID operator delete[](VOID *) = delete;
```

This prevents heap allocation. `new Span<UINT8>(...)` will not compile. Every instance
must live on the stack.

The reasoning has two parts:

1. **No allocator available.** Heap allocation requires `malloc`/`free` or equivalent,
   which are libc functions. Under `-nostdlib`, there is no default heap allocator.
   The project does have its own allocator for specific use cases, but core types are
   not meant to use it.

2. **Automatic lifetime management.** Stack objects are destroyed when the function
   returns. There is no possibility of a memory leak, a use-after-free, or a double-free.
   The destructor runs deterministically at scope exit. For types like `Result` that
   manage a union with non-trivial members, this is important -- `DestroyActive()` is
   called exactly once, at exactly the right time.

The one exception is placement new, which *is* allowed:

```cpp
VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
VOID operator delete(VOID *, PVOID) noexcept {}
```

Placement new does not allocate memory -- it constructs an object at an address you
already own. `Result` uses this internally to construct the value or error in the
union's storage. This is the standard C++ technique for initializing union members,
and it requires no allocator.

---

## 8. Putting It All Together

A typical function signature in this codebase reads like this:

```cpp
Result<Span<const UINT8>, Error> Socket::Read(Span<UINT8> buffer);
```

Every type in that signature was covered in this document:

- `Result<..., Error>`: the function either succeeds with a value or fails with a
  chained error. The caller must check. The return value cannot be ignored.
- `Span<const UINT8>`: the success value is a read-only view into the buffer, with
  a pointer and length bundled together.
- `Error`: on failure, contains the error code, platform tag, and full cause chain.
- `Span<UINT8>`: the input buffer is a mutable view. The function knows both the
  pointer and how much space is available.
- `UINT8`: a fixed-width unsigned byte, defined without standard headers.

No exceptions. No heap allocation. No standard library headers. Every type is
self-contained, stack-allocated, and optimized for the constraints of
position-independent code.

---

## Further Reading

- [01-what-is-pic.md](01-what-is-pic.md) -- The Golden Rule and why these constraints exist
- [03-build-system.md](03-build-system.md) -- Compiler flags that enforce the constraints
- [book-notes/06-memory-and-strings.md](../book-notes/06-memory-and-strings.md) -- How
  `TCHAR`-constrained string functions use these types
- [book-notes/04-entry-point-and-bootstrap.md](../book-notes/04-entry-point-and-bootstrap.md) --
  How the runtime initializes before these types are used

### Source Files

| File | What It Defines |
|------|-----------------|
| `src/core/types/primitives.h` | All primitive types, TCHAR concept, USIZE, calling conventions |
| `src/core/types/result.h` | `Result<T, E>` tagged union, void specialization, placement new |
| `src/core/types/span.h` | `Span<T>` (dynamic), `Span<T, N>` (static), slicing, conversions |
| `src/core/types/error.h` | `Error` struct, error codes, platform tags, `Wrap()` chaining |
