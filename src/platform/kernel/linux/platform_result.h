/**
 * @file platform_result.h
 * @brief Linux syscall result to Result<T, Error> conversion.
 *
 * @details Provides the FromLinux() helper that converts raw Linux syscall return
 * values into the PIR Result<T, Error> type. Linux syscalls return negative errno
 * on failure; this function maps success (>= 0) to Result::Ok and failure to
 * Result::Err with a POSIX error code.
 */
#pragma once

#include "core/types/result.h"
#include "core/types/error.h"

namespace result
{

/// Linux syscall: success when result >= 0, failure stores -result as errno.
template <typename T>
[[nodiscard]] FORCE_INLINE Result<T, Error> FromLinux(SSIZE result) noexcept
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
