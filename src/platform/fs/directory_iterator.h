/**
 * @file directory_iterator.h
 * @brief Directory iteration
 *
 * @details Provides an RAII iterator for enumerating directory entries across
 * platforms. Created via the DirectoryIterator::Create() factory method, the
 * iterator is move-only and stack-only. On Windows it uses FindFirstFile/
 * FindNextFile or drive bitmask enumeration. On Linux, macOS, and Solaris it
 * buffers entries via getdents64/getdirentries64 syscalls.
 */
#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"
#include "core/types/error.h"
#include "core/types/result.h"
#include "platform/fs/directory_entry.h"
class DirectoryIterator
{
private:
	PVOID handle;                ///< Platform handle to the directory (or drive bitmask on Windows)
	DirectoryEntry currentEntry; ///< Most recently read directory entry
	BOOL isFirst;                ///< TRUE before the first call to Next()
#ifdef PLATFORM_WINDOWS
	BOOL isBitMaskMode = false;  ///< TRUE when enumerating logical drives via bitmask on Windows
#endif

#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_SOLARIS) || defined(PLATFORM_FREEBSD)
	CHAR buffer[1024]; ///< Kernel entry buffer for getdents64/getdirentries64
	INT32 bytesRead;       ///< Number of bytes returned by the last syscall
	INT32 bufferPosition;  ///< Current byte position within the buffer
#endif

	/// Private constructor for factory use only.
	DirectoryIterator();

public:
	~DirectoryIterator() { Close(); }

	DirectoryIterator(const DirectoryIterator &) = delete;
	DirectoryIterator &operator=(const DirectoryIterator &) = delete;

	DirectoryIterator(DirectoryIterator &&other) noexcept;
	DirectoryIterator &operator=(DirectoryIterator &&other) noexcept;

	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	/**
	 * @brief Closes the directory handle and releases platform resources.
	 * @details Safe to call on an already-closed or default-constructed iterator.
	 */
	VOID Close();

	/**
	 * @brief Creates and initializes a directory iterator for the given path.
	 * @param path Null-terminated wide string directory path.
	 * @return Initialized iterator on success, or an Error on failure.
	 */
	[[nodiscard]] static Result<DirectoryIterator, Error> Create(PCWCHAR path);

	/**
	 * @brief Advances the iterator to the next directory entry.
	 * @return VOID on success (entry available via Get()), or an Error when no more entries remain or a syscall fails.
	 */
	[[nodiscard]] Result<VOID, Error> Next();

	/**
	 * @brief Returns a reference to the current directory entry.
	 * @return The most recently read DirectoryEntry.
	 */
	const DirectoryEntry &Get() const { return currentEntry; }

	/**
	 * @brief Checks whether the iterator holds a valid directory handle.
	 * @return TRUE if the iterator is valid, FALSE otherwise.
	 */
	BOOL IsValid() const;
};
