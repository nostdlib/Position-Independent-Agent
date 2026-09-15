/**
 * @file primitives.h
 * @brief Primitive Type Definitions
 *
 * @details Defines fundamental types used throughout the Position-Independent Runtime.
 * These types provide:
 * - Fixed-width integer types (INT8, UINT8, INT16, UINT16, INT32, UINT32, INT64, UINT64)
 * - Character types (CHAR, WCHAR)
 * - Boolean types (BOOL)
 * - Size types (USIZE, SSIZE)
 * - Pointer types (PVOID, PCVOID)
 * - Variadic argument support (VA_LIST, VA_START, VA_ARG, VA_END)
 * - Platform-specific calling conventions (STDCALL)
 *
 * @note These types are Windows API compatible and designed for cross-platform use.
 *
 * @ingroup core
 *
 * @defgroup primitives Primitive Types
 * @ingroup core
 * @{
 */

#pragma once

#include "core/compiler/compiler.h"

// =============================================================================
// VOID AND POINTER TYPES
// =============================================================================


typedef void VOID, *PVOID, **PPVOID;
typedef const void *PCVOID, **PPCVOID;

// =============================================================================
// FIXED-WIDTH INTEGER TYPES
// =============================================================================

typedef signed char INT8, *PINT8;
typedef unsigned char UINT8, *PUINT8, **PPUINT8;

typedef signed short INT16, *PINT16;
typedef unsigned short UINT16, *PUINT16;

typedef signed int INT32, *PINT32;
typedef unsigned int UINT32, *PUINT32, **PPUINT32;

typedef signed long long INT64, *PINT64, **PPINT64;
typedef unsigned long long UINT64, *PUINT64, **PPUINT64;

// =============================================================================
// CHARACTER TYPES
// =============================================================================


typedef char CHAR, *PCHAR, **PPCHAR;
typedef unsigned char UCHAR, *PUCHAR;
typedef const CHAR *PCCHAR;

typedef wchar_t WCHAR, *PWCHAR, **PPWCHAR;
typedef const WCHAR *PCWCHAR;


typedef unsigned short CHAR16, *PCHAR16, **PPCHAR16;
typedef const CHAR16 *PCCHAR16;

// =============================================================================
// CHARACTER TYPE CONSTRAINT
// =============================================================================

/**
 * @brief Concept constraining character types to CHAR or WCHAR
 * @tparam TChar Type to check
 * @details Ensures template functions only work with supported character types.
 */
template <typename TChar>
concept TCHAR = __is_same_as(TChar, CHAR) || __is_same_as(TChar, WCHAR);

// =============================================================================
// BOOLEAN TYPE
// =============================================================================


typedef bool BOOL, *PBOOL, **PPBOOL;

// =============================================================================
// SIZE TYPES
// =============================================================================

/**
 * @brief Unsigned size type (pointer-sized)
 * @details 32-bit on 32-bit platforms, 64-bit on 64-bit platforms.
 * Used for sizes, counts, and array indexing.
 */
typedef __SIZE_TYPE__ USIZE, *PUSIZE;

/**
 * @brief Signed size type (pointer-sized)
 * @details 32-bit on 32-bit platforms, 64-bit on 64-bit platforms.
 * Used for signed offsets and differences.
 */
typedef __INTPTR_TYPE__ SSIZE, *PSSIZE;

// =============================================================================
// VARIADIC ARGUMENT SUPPORT
// =============================================================================

/** @brief Variadic argument list type */
typedef __builtin_va_list VA_LIST;

/**
 * @def VA_START
 * @brief Initialize variadic argument processing
 * @param ap VA_LIST variable to initialize
 * @param v Last named parameter before variadic arguments
 */
#define VA_START(ap, v) __builtin_va_start(ap, v)

/**
 * @def VA_ARG
 * @brief Retrieve next variadic argument
 * @param ap VA_LIST variable
 * @param t Type of argument to retrieve
 * @return Next argument of type t
 */
#define VA_ARG(ap, t) __builtin_va_arg(ap, t)

/**
 * @def VA_END
 * @brief End variadic argument processing
 * @param ap VA_LIST variable to clean up
 */
#define VA_END(ap) __builtin_va_end(ap)

// =============================================================================
// CALLING CONVENTIONS
// =============================================================================

/**
 * @def STDCALL
 * @brief Platform-specific standard calling convention
 * @details
 * - Windows i386: __stdcall (callee cleans stack)
 * - Windows x86_64: MS ABI (shadow space, specific register usage)
 * - Windows ARM: Default ARM ABI
 * - Linux: System V ABI (no special convention needed)
 */
#if defined(PLATFORM_WINDOWS_I386)
#define STDCALL __attribute__((stdcall))
#elif defined(PLATFORM_WINDOWS_X86_64)
#define STDCALL __attribute__((ms_abi))
#elif defined(PLATFORM_WINDOWS_ARMV7A)
#define STDCALL
#elif defined(PLATFORM_WINDOWS_AARCH64)
#define STDCALL
#elif defined(PLATFORM_LINUX)
#define STDCALL // Linux uses System V ABI, no special calling convention needed
#else
#define STDCALL // Default: no special calling convention
#endif

// =============================================================================
// FLOATING-POINT TYPES
// =============================================================================

/** @brief Double-precision floating-point (64-bit IEEE-754) */
typedef double DOUBLE, *PDOUBLE;

/** @} */ // end of primitives group
