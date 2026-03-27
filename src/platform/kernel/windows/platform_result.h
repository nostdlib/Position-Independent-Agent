/**
 * @file platform_result.h
 * @brief Windows NTSTATUS to Result conversion
 *
 * @details Provides the result::FromNTSTATUS<T>() helper that converts a Windows
 * NTSTATUS code into a Result<T, Error>. Success is determined by NT_SUCCESS
 * semantics (status >= 0). On failure, the NTSTATUS is wrapped in an
 * Error::Windows() value for uniform cross-platform error handling.
 */
#pragma once

#include "core/types/result.h"
#include "core/types/error.h"

namespace result
{

/// Windows NTSTATUS: success when status >= 0 (NT_SUCCESS semantics).
template <typename T>
[[nodiscard]] FORCE_INLINE Result<T, Error> FromNTSTATUS(INT32 status) noexcept
{
	if (status >= 0)
	{
		if constexpr (__is_same_as(T, VOID))
			return Result<T, Error>::Ok();
		else
			return Result<T, Error>::Ok((T)status);
	}
	return Result<T, Error>::Err(Error::Windows((UINT32)status));
}

} // namespace result
