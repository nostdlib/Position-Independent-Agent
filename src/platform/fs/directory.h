/**
 * @file directory.h
 * @brief Directory operations
 *
 * @details Provides static methods for creating and deleting directories
 * across platforms. All operations return Result<VOID, Error> for uniform
 * error handling via the PIR Result type.
 */
#pragma once

#include "core/types/primitives.h"
#include "core/types/error.h"
#include "core/types/result.h"
class Directory
{
public:
	/**
	 * @brief Creates a directory at the given path.
	 * @details On Windows, uses NtCreateFile with FILE_DIRECTORY_FILE. On POSIX,
	 * uses the mkdir() syscall. On UEFI, uses EFI_FILE_PROTOCOL::Open with
	 * EFI_FILE_MODE_CREATE and the directory attribute.
	 * @param path Null-terminated wide string directory path.
	 * @return Void on success, or an Error on failure.
	 */
	[[nodiscard]] static Result<VOID, Error> Create(PCWCHAR path);

	/**
	 * @brief Deletes a directory at the given path.
	 * @details On Windows, uses NtSetInformationFile with FileDispositionInformation.
	 * On POSIX, uses the rmdir() syscall. On UEFI, opens the directory and calls
	 * EFI_FILE_PROTOCOL::Delete. The directory must be empty.
	 * @param path Null-terminated wide string directory path.
	 * @return Void on success, or an Error on failure.
	 */
	[[nodiscard]] static Result<VOID, Error> Delete(PCWCHAR path);
};
