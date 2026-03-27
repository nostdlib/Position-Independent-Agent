/**
 * @file result.h
 * @brief Zero-Cost Result Type for Error Handling
 *
 * @details Tagged union `Result<T, E>` — either a success value (T) or error (E).
 * When T is void, a trivial sentinel replaces the value member so a single
 * template handles both cases without a separate specialization.
 *
 * E is stored directly — no heap allocation beyond sizeof(E).
 * Compile-time safety via `[[nodiscard]]` and `operator BOOL` ensures callers
 * check results without any runtime cost.
 *
 * Multi-arg and propagation Err() overloads chain errors using Error::Wrap(),
 * preserving the full error propagation path from root cause to failure site.
 *
 * @note Uses Clang builtin __is_trivially_destructible for zero-overhead destruction.
 *
 * @ingroup core
 *
 * @defgroup result Result Type
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"
#include "core/types/error.h"

/// Placement new/delete for constructing objects in pre-allocated storage
constexpr FORCE_INLINE PVOID operator new(USIZE, PVOID ptr) noexcept { return ptr; }
constexpr FORCE_INLINE VOID operator delete(PVOID, PVOID) noexcept {}

/// Trivial sentinel replacing T when T is void
struct VOID_TAG
{
};

// Helper to map void to VOID_TAG for storage, while keeping T as the user-facing type
template <typename T>
struct VOID_TO_TAG
{
	using Type = T;
};

// Specialization for void maps to VOID_TAG
template <>
struct VOID_TO_TAG<VOID>
{
	using Type = VOID_TAG;
};

// Class template for Result
template <typename T, typename E>
class [[nodiscard]] Result
{
	static constexpr BOOL IsVoid = __is_same_as(T, VOID);
	using STORED_TYPE = typename VOID_TO_TAG<T>::Type;

	union
	{
		STORED_TYPE m_value;
		E m_error;
	};
	BOOL m_isOk;

	constexpr VOID DestroyActive() noexcept
	{
		if constexpr (!__is_trivially_destructible(STORED_TYPE) || !__is_trivially_destructible(E))
		{
			if (m_isOk)
			{
				if constexpr (!__is_trivially_destructible(STORED_TYPE))
					m_value.~STORED_TYPE();
			}
			else
			{
				if constexpr (!__is_trivially_destructible(E))
					m_error.~E();
			}
		}
	}

public:
	using ValueType = T;
	using ErrorType = E;

	// =====================================================================
	// Ok factories
	// =====================================================================

	[[nodiscard]] static constexpr FORCE_INLINE Result Ok(STORED_TYPE value) noexcept
		requires(!IsVoid)
	{
		Result r;
		r.m_isOk = true;
		new (&r.m_value) STORED_TYPE(static_cast<STORED_TYPE &&>(value));
		return r;
	}

	[[nodiscard]] static constexpr FORCE_INLINE Result Ok() noexcept
		requires(IsVoid)
	{
		Result r;
		r.m_isOk = true;
		return r;
	}

	// =====================================================================
	// Err factories
	// =====================================================================

	/// Single error — stores E directly
	[[nodiscard]] static constexpr FORCE_INLINE Result Err(E error) noexcept
	{
		Result r;
		r.m_isOk = false;
		new (&r.m_error) E(static_cast<E &&>(error));
		return r;
	}

	/// 2-arg Err — chains first (inner/cause) error under last (outer/site) error.
	/// Both errors are preserved in the chain via Error::Wrap().
	template <typename First>
		requires(requires(First f) { E(f); })
	[[nodiscard]] static constexpr FORCE_INLINE Result Err(First first, E last) noexcept
	{
		return Err(E::Wrap(E(static_cast<First &&>(first)), (UINT32)last.Code, last.Platform));
	}

	/// Propagation Err — chains the underlying result's error under the new site code.
	/// Both the inner result's full error chain and the outer site code are preserved.
	template <typename OtherT>
	[[nodiscard]] static constexpr FORCE_INLINE Result Err(const Result<OtherT, E> &r, E outer) noexcept
	{
		return Err(E::Wrap(r.Error(), (UINT32)outer.Code, outer.Platform));
	}

	// =====================================================================
	// Destructor + move semantics
	// =====================================================================

	constexpr ~Result() noexcept { DestroyActive(); }

	constexpr Result(Result &&other) noexcept : m_isOk(other.m_isOk)
	{
		if (m_isOk)
			new (&m_value) STORED_TYPE(static_cast<STORED_TYPE &&>(other.m_value));
		else
			new (&m_error) E(static_cast<E &&>(other.m_error));
	}

	constexpr Result &operator=(Result &&other) noexcept
	{
		if (this != &other)
		{
			DestroyActive();
			m_isOk = other.m_isOk;
			if (m_isOk)
				new (&m_value) STORED_TYPE(static_cast<STORED_TYPE &&>(other.m_value));
			else
				new (&m_error) E(static_cast<E &&>(other.m_error));
		}
		return *this;
	}

	Result(const Result &) = delete;
	Result &operator=(const Result &) = delete;

	// =====================================================================
	// Value / error queries
	// =====================================================================

	[[nodiscard]] constexpr FORCE_INLINE BOOL IsOk() const noexcept { return m_isOk; }
	[[nodiscard]] constexpr FORCE_INLINE BOOL IsErr() const noexcept { return !m_isOk; }
	[[nodiscard]] constexpr FORCE_INLINE operator BOOL() const noexcept { return m_isOk; }

	[[nodiscard]] constexpr FORCE_INLINE STORED_TYPE &Value() noexcept
		requires(!IsVoid)
	{
		return m_value;
	}
	[[nodiscard]] constexpr FORCE_INLINE const STORED_TYPE &Value() const noexcept
		requires(!IsVoid)
	{
		return m_value;
	}

	/// Returns the stored error for inspection and %e formatting.
	[[nodiscard]] constexpr FORCE_INLINE const E &Error() const noexcept { return m_error; }

private:
	constexpr Result() noexcept {}
};

/// Compact specialization for Result<void, Error>.
///
/// Encodes success as Error.Code == Error::None, eliminating the discriminant
/// flag and union.  sizeof matches sizeof(Error) exactly.
///
/// The API surface is identical to the primary template instantiated with
/// T = void, E = Error.  No call-site changes are needed.
///
/// @note Error() on an Ok result returns Error{None, Runtime} — well-defined,
///       unlike the primary template where reading the inactive union member is UB.
template <>
class [[nodiscard]] Result<VOID, Error>
{
	struct Error m_error;

public:
	using ValueType = VOID;
	using ErrorType = struct Error;

	// =====================================================================
	// Ok factory
	// =====================================================================

	[[nodiscard]] static constexpr FORCE_INLINE Result Ok() noexcept
	{
		Result r;
		r.m_error = {};
		return r;
	}

	// =====================================================================
	// Err factories
	// =====================================================================

	/// Single error — stores Error directly
	[[nodiscard]] static constexpr FORCE_INLINE Result Err(struct Error error) noexcept
	{
		Result r;
		r.m_error = error;
		return r;
	}

	/// 2-arg Err — chains first (inner/cause) error under second (outer/site) error.
	/// Both errors are preserved in the chain via Error::Wrap().
	template <typename Second>
		requires(__is_constructible(struct Error, Second))
	[[nodiscard]] static constexpr FORCE_INLINE Result Err(struct Error first, Second second) noexcept
	{
		struct Error outer(second);
		return Err(Error::Wrap(first, (UINT32)outer.Code, outer.Platform));
	}

	/// Propagation Err — chains the underlying result's error under the new site code.
	/// Both the inner result's full error chain and the outer site code are preserved.
	template <typename OtherT>
	[[nodiscard]] static constexpr FORCE_INLINE Result Err(const Result<OtherT, struct Error> &r, struct Error outer) noexcept
	{
		return Err(Error::Wrap(r.Error(), (UINT32)outer.Code, outer.Platform));
	}

	// =====================================================================
	// Destructor + move semantics
	// =====================================================================

	constexpr ~Result() noexcept = default;

	constexpr Result(Result &&other) noexcept = default;
	constexpr Result &operator=(Result &&other) noexcept = default;

	Result(const Result &) = delete;
	Result &operator=(const Result &) = delete;

	// =====================================================================
	// Value / error queries
	// =====================================================================

	[[nodiscard]] constexpr FORCE_INLINE BOOL IsOk() const noexcept { return m_error.Code == Error::None; }
	[[nodiscard]] constexpr FORCE_INLINE BOOL IsErr() const noexcept { return m_error.Code != Error::None; }
	[[nodiscard]] constexpr FORCE_INLINE operator BOOL() const noexcept { return m_error.Code == Error::None; }

	/// Returns the stored error for inspection and %e formatting.
	[[nodiscard]] constexpr FORCE_INLINE const struct Error &Error() const noexcept { return m_error; }

private:
	constexpr Result() noexcept = default;
};

/** @} */ // end of result group
