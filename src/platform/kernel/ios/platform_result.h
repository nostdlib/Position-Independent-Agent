/**
 * @file platform_result.h
 * @brief iOS syscall result to Result<T, Error> conversion.
 *
 * @details iOS uses the same XNU BSD syscall return convention as macOS.
 * Provides FromiOS() as an alias for the macOS conversion logic. After the
 * carry-flag negation in the System::Call wrappers, negative return values
 * indicate errors; this function maps them to Result::Err with a POSIX error
 * code.
 */
#pragma once

#include "core/types/result.h"
#include "core/types/error.h"

namespace result
{

/// iOS syscall: success when result >= 0, failure stores -result as errno.
template <typename T>
[[nodiscard]] FORCE_INLINE Result<T, Error> FromiOS(SSIZE result) noexcept
{
	if (result >= 0)
	{
		if constexpr (__is_same_as(T, VOID))
			return Result<T, Error>::Ok();
		else
			return Result<T, Error>::Ok((T)result);
	}
	return Result<T, Error>::Err(Error::Posix((UINT32)(-result)));
}

} // namespace result
