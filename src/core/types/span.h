/**
 * @file span.h
 * @brief Non-Owning Contiguous Buffer Views (Span)
 *
 * @details Provides a non-owning, bounds-aware view over contiguous memory,
 * replacing raw `(T*, USIZE)` parameter pairs throughout the codebase.
 * Two forms exist:
 *
 * - **Span<T>** (dynamic extent): stores pointer + runtime size
 * - **Span<T, N>** (static extent): stores pointer only; size is a compile-time constant
 *
 * Key properties:
 * - Zero runtime cost: all methods are constexpr FORCE_INLINE
 * - Implicit Span<T> -> Span<const T> conversion (writable-to-readonly)
 * - Implicit Span<T, N> -> Span<T> conversion (static-to-dynamic)
 * - Compile-time slicing preserves static extent through the chain
 * - Stack-only: heap allocation is deleted
 *
 * Modeled after C++20 std::span but without STL dependencies, using
 * Clang builtins (__is_same_as, __is_const) for concept constraints.
 *
 * @see C++20 std::span — https://en.cppreference.com/w/cpp/container/span
 *
 * @ingroup core
 *
 * @defgroup span Span
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"

/// Sentinel value indicating a runtime-determined extent
constexpr USIZE DYNAMIC_EXTENT = static_cast<USIZE>(-1);

// Forward declaration
template <typename T, USIZE Extent = DYNAMIC_EXTENT>
class Span;

// =============================================================================
// PARTIAL SPECIALIZATION: Dynamic Extent (Span<T> / Span<T, DYNAMIC_EXTENT>)
// =============================================================================

/**
 * @brief Non-owning view over a contiguous sequence of elements (dynamic extent)
 *
 * @tparam T Element type (can be const-qualified for read-only views)
 *
 * @details Stores a pointer and a runtime size. This is the default form
 * used in all function signatures throughout the codebase.
 *
 * Span<T> implicitly converts to Span<const T>, enabling functions that accept
 * Span<const T> to receive Span<T> arguments without explicit casting.
 *
 * Span<T, N> (static extent) implicitly converts to Span<T> (dynamic extent).
 *
 * @par Example Usage:
 * @code
 * UINT8 buffer[64];
 * Span<UINT8> writable(buffer);           // From array (size deduced)
 * Span<const UINT8> readable = writable;  // Implicit const conversion
 *
 * Span<UINT8> sub = writable.Subspan(4, 16);      // Runtime slicing
 * Span<UINT8, 16> fixed = writable.Subspan<4, 16>(); // Compile-time slicing
 * @endcode
 */
template <typename T>
class Span<T, DYNAMIC_EXTENT>
{
private:
	T *m_data;
	USIZE m_size;

public:
	//=============================================================================
	// Constructors
	//=============================================================================

	constexpr FORCE_INLINE Span() : m_data(nullptr), m_size(0) {}

	constexpr FORCE_INLINE Span(T *data, USIZE size) : m_data(data), m_size(size) {}

	// Array constructor: for non-character arrays, size = N (full array).
	// For const char/wchar_t arrays (string literals), size = N-1 (exclude null terminator).
	template <USIZE N>
	constexpr FORCE_INLINE Span(T (&arr)[N]) : m_data(arr),
		m_size((__is_const(T) && (__is_same_as(__remove_const(T), char) || __is_same_as(__remove_const(T), wchar_t)) && N > 0) ? N - 1 : N) {}

	// Span<U> -> Span<const U> implicit conversion (dynamic to dynamic)
	template <typename U>
		requires(__is_same_as(T, const U))
	constexpr FORCE_INLINE Span(const Span<U> &other) : m_data(other.Data()), m_size(other.Size()) {}

	// Span<T, N> -> Span<T> implicit conversion (static to dynamic)
	template <typename U, USIZE N>
		requires(__is_same_as(T, U) && N != DYNAMIC_EXTENT)
	constexpr FORCE_INLINE Span(const Span<U, N> &other) : m_data(other.Data()), m_size(N) {}

	// Span<U, N> -> Span<const U> implicit conversion (static to dynamic + const)
	// !__is_const(U) prevents ambiguity when U is already const (line 62 handles that case)
	template <typename U, USIZE N>
		requires(__is_same_as(T, const U) && !__is_const(U) && N != DYNAMIC_EXTENT)
	constexpr FORCE_INLINE Span(const Span<U, N> &other) : m_data(other.Data()), m_size(N) {}

	/// @name Element Access
	/// @{

	/** @brief Get pointer to the underlying data */
	constexpr FORCE_INLINE T *Data() const { return m_data; }
	/** @brief Get the number of elements */
	constexpr FORCE_INLINE USIZE Size() const { return m_size; }
	/** @brief Get the size in bytes (Size() * sizeof(T)) */
	constexpr FORCE_INLINE USIZE SizeBytes() const { return m_size * sizeof(T); }
	/** @brief Check if the span is empty */
	constexpr FORCE_INLINE BOOL IsEmpty() const { return m_size == 0; }
	/** @brief Access element by index (no bounds checking) */
	constexpr FORCE_INLINE T &operator[](USIZE index) const { return m_data[index]; }

	/// @}
	/// @name Runtime Slicing
	/// @{

	/** @brief Get a subspan from offset to end */
	constexpr FORCE_INLINE Span Subspan(USIZE offset) const { return Span(m_data + offset, m_size - offset); }
	/** @brief Get a subspan of count elements starting at offset */
	constexpr FORCE_INLINE Span Subspan(USIZE offset, USIZE count) const { return Span(m_data + offset, count); }
	/** @brief Get the first count elements */
	constexpr FORCE_INLINE Span First(USIZE count) const { return Span(m_data, count); }
	/** @brief Get the last count elements */
	constexpr FORCE_INLINE Span Last(USIZE count) const { return Span(m_data + m_size - count, count); }

	/// @}
	/// @name Compile-Time Slicing
	/// @{

	/** @brief Get the first Count elements as a static-extent Span */
	template <USIZE Count>
	constexpr FORCE_INLINE Span<T, Count> First() const { return Span<T, Count>(m_data); }

	/** @brief Get the last Count elements as a static-extent Span */
	template <USIZE Count>
	constexpr FORCE_INLINE Span<T, Count> Last() const { return Span<T, Count>(m_data + m_size - Count); }

	/** @brief Get Count elements starting at Offset as a static-extent Span */
	template <USIZE Offset, USIZE Count>
	constexpr FORCE_INLINE Span<T, Count> Subspan() const { return Span<T, Count>(m_data + Offset); }

	/// @}
	/// @name Iterators
	/// @{

	/** @brief Iterator to the first element */
	constexpr FORCE_INLINE T *begin() const { return m_data; }
	/** @brief Iterator past the last element */
	constexpr FORCE_INLINE T *end() const { return m_data + m_size; }

	/// @}

	// Stack-only: prevent heap allocation
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;
	// Placement new/delete required by Result<Span<T>, Error>
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}
};

// =============================================================================
// PRIMARY TEMPLATE: Static Extent (Span<T, N>)
// =============================================================================

/**
 * @brief Non-owning view over a contiguous sequence of elements (static extent)
 *
 * @tparam T Element type
 * @tparam Extent Compile-time element count
 *
 * @details Stores ONLY a pointer — the size is a compile-time constant baked
 * into the type. This eliminates the m_size member entirely, halving the
 * Span's footprint and enabling the compiler to propagate the known size
 * through all downstream optimizations (loop unrolling, dead-store elim, etc.).
 *
 * Implicitly converts to Span<T> (dynamic extent) when passed to functions.
 *
 * Compile-time slicing (First<N>(), Last<N>(), Subspan<O,N>(), Subspan<O>())
 * returns Span<T, N> so the static extent is preserved through the chain,
 * preventing unnecessary size loads/stores in the generated code.
 *
 * @par Example Usage:
 * @code
 * UINT8 digest[32];
 * Span<UINT8, 32> fixed(digest);           // Pointer only — Size() == 32 is free
 * Span<UINT8, 16> lo = fixed.First<16>();  // Static slice — no m_size at all
 * Span<UINT8, 16> hi = fixed.Last<16>();
 * Span<UINT8, 8>  mid = fixed.Subspan<4, 8>();
 * Span<UINT8, 28> tail = fixed.Subspan<4>(); // Count = 32-4 deduced from type
 * Span<UINT8> dynamic = fixed;             // Implicit static-to-dynamic conversion
 * @endcode
 */
template <typename T, USIZE Extent>
class Span
{
private:
	T *m_data;

public:
	//=============================================================================
	// Constructors
	//=============================================================================
	constexpr FORCE_INLINE Span() : m_data(nullptr) {}

	constexpr FORCE_INLINE Span(T (&arr)[Extent]) : m_data(arr) {}

	/// Construct from a raw pointer when the extent is known at compile time.
	/// Marked explicit to prevent accidental implicit construction from T*.
	/// Template parameter makes this a function template so the non-template
	/// array-reference constructor above wins when the argument is an array,
	/// while this constructor wins when the argument is a plain pointer.
	template <typename = VOID>
	constexpr FORCE_INLINE explicit Span(T *ptr) : m_data(ptr) {}

	// Span<U, N> -> Span<const U, N> implicit conversion
	template <typename U>
		requires(__is_same_as(T, const U))
	constexpr FORCE_INLINE Span(const Span<U, Extent> &other) : m_data(other.Data()) {}

	/// @name Element Access
	/// @{

	/** @brief Get pointer to the underlying data */
	constexpr FORCE_INLINE T *Data() const { return m_data; }
	/** @brief Get the number of elements (compile-time constant) */
	constexpr FORCE_INLINE USIZE Size() const { return Extent; }
	/** @brief Get the size in bytes (Extent * sizeof(T), compile-time constant) */
	constexpr FORCE_INLINE USIZE SizeBytes() const { return Extent * sizeof(T); }
	/** @brief Check if the span is empty (compile-time constant) */
	constexpr FORCE_INLINE BOOL IsEmpty() const { return Extent == 0; }
	/** @brief Access element by index (no bounds checking) */
	constexpr FORCE_INLINE T &operator[](USIZE index) const { return m_data[index]; }

	/// @}
	/// @name Runtime Slicing
	/// @{

	/** @brief Get a subspan from offset to end (returns dynamic extent) */
	constexpr FORCE_INLINE Span<T> Subspan(USIZE offset) const { return Span<T>(m_data + offset, Extent - offset); }
	/** @brief Get a subspan of count elements starting at offset (returns dynamic extent) */
	constexpr FORCE_INLINE Span<T> Subspan(USIZE offset, USIZE count) const { return Span<T>(m_data + offset, count); }
	/** @brief Get the first count elements (returns dynamic extent) */
	constexpr FORCE_INLINE Span<T> First(USIZE count) const { return Span<T>(m_data, count); }
	/** @brief Get the last count elements (returns dynamic extent) */
	constexpr FORCE_INLINE Span<T> Last(USIZE count) const { return Span<T>(m_data + Extent - count, count); }

	/// @}
	/// @name Compile-Time Slicing
	/// @{

	/** @brief Get the first Count elements as a static-extent Span */
	template <USIZE Count>
	constexpr FORCE_INLINE Span<T, Count> First() const
	{
		static_assert(Count <= Extent, "First<Count>: Count exceeds static extent");
		return Span<T, Count>(m_data);
	}

	/** @brief Get the last Count elements as a static-extent Span */
	template <USIZE Count>
	constexpr FORCE_INLINE Span<T, Count> Last() const
	{
		static_assert(Count <= Extent, "Last<Count>: Count exceeds static extent");
		return Span<T, Count>(m_data + Extent - Count);
	}

	/** @brief Get Count elements starting at Offset as a static-extent Span */
	template <USIZE Offset, USIZE Count>
	constexpr FORCE_INLINE Span<T, Count> Subspan() const
	{
		static_assert(Offset <= Extent, "Subspan<Offset, Count>: Offset exceeds static extent");
		static_assert(Count <= Extent - Offset, "Subspan<Offset, Count>: Count exceeds remaining extent");
		return Span<T, Count>(m_data + Offset);
	}

	/** @brief Get elements from Offset to end; count (Extent - Offset) deduced from type */
	template <USIZE Offset>
	constexpr FORCE_INLINE Span<T, Extent - Offset> Subspan() const
	{
		static_assert(Offset <= Extent, "Subspan<Offset>: Offset exceeds static extent");
		return Span<T, Extent - Offset>(m_data + Offset);
	}

	/// @}
	/// @name Iterators
	/// @{

	/** @brief Iterator to the first element */
	constexpr FORCE_INLINE T *begin() const { return m_data; }
	/** @brief Iterator past the last element */
	constexpr FORCE_INLINE T *end() const { return m_data + Extent; }

	/// @}

	// Stack-only: prevent heap allocation
	VOID *operator new(USIZE) = delete;
	VOID *operator new[](USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID operator delete[](VOID *) = delete;
	// Placement new/delete required by Result<Span<T, N>, Error>
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}
};

/** @} */ // end of span group
