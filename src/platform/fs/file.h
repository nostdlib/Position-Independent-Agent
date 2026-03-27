/**
 * @file file.h
 * @brief File I/O abstraction
 *
 * @details Provides a platform-independent RAII file handle wrapper with
 * factory-based creation via File::Open(). Supports read, write, seek, delete,
 * and existence checks. The File class is move-only (non-copyable) to prevent
 * double-close bugs, and stack-only to avoid heap allocation. Open mode flags
 * control read/write/append/create/truncate/binary behavior.
 */
#pragma once

#include "core/types/primitives.h"
#include "core/types/span.h"
#include "core/types/error.h"
#include "core/types/result.h"
#include "platform/fs/offset_origin.h"
class File
{
private:
	PVOID fileHandle; ///< Platform-specific file handle (HANDLE on Windows, fd cast to PVOID on POSIX)
	USIZE fileSize;   ///< Cached file size in bytes, set at open time

	/// Private constructor (trivial -- never fails)
	File(PVOID handle, USIZE size);

public:
	static constexpr INT32 ModeRead = 0x0001;     ///< Open for reading
	static constexpr INT32 ModeWrite = 0x0002;     ///< Open for writing
	static constexpr INT32 ModeAppend = 0x0004;    ///< Append to end of file
	static constexpr INT32 ModeCreate = 0x0008;    ///< Create file if it does not exist
	static constexpr INT32 ModeTruncate = 0x0010;  ///< Truncate existing file to zero length
	static constexpr INT32 ModeBinary = 0x0020;    ///< Open in binary mode (no newline translation)

	/**
	 * @brief Opens a file at the given path with the specified mode flags.
	 * @details On Windows, uses NtCreateFile via indirect syscall. On POSIX, uses
	 * the open() syscall with translated flags. On UEFI, uses EFI_FILE_PROTOCOL::Open.
	 * The returned File owns the handle and closes it on destruction (RAII).
	 * @param path Null-terminated wide string file path.
	 * @param flags Bitmask of ModeRead, ModeWrite, ModeAppend, ModeCreate, ModeTruncate, ModeBinary.
	 * @return File handle on success, or an Error on failure.
	 */
	[[nodiscard]] static Result<File, Error> Open(PCWCHAR path, INT32 flags = 0);

	/**
	 * @brief Deletes a file at the given path.
	 * @details On Windows, uses NtSetInformationFile with FileDispositionInformation.
	 * On POSIX, uses the unlink() syscall. On UEFI, uses EFI_FILE_PROTOCOL::Delete.
	 * @param path Null-terminated wide string file path.
	 * @return Void on success, or an Error on failure.
	 */
	[[nodiscard]] static Result<VOID, Error> Delete(PCWCHAR path);

	/**
	 * @brief Checks whether a file exists at the given path.
	 * @details On Windows, uses NtQueryAttributesFile. On POSIX, uses the stat() or
	 * access() syscall. On UEFI, attempts to open and immediately close the file.
	 * @param path Null-terminated wide string file path.
	 * @return Void on success (file exists), or an Error if the file does not exist.
	 */
	[[nodiscard]] static Result<VOID, Error> Exists(PCWCHAR path);

	/// Default constructor; initializes to an invalid handle with zero size.
	File() : fileHandle(InvalidFileHandle()), fileSize(0) {}

	/**
	 * @brief Returns the platform-specific invalid file handle sentinel.
	 * @details Windows uses nullptr. POSIX/UEFI uses (PVOID)(SSIZE)-1 because
	 * fd 0 is a valid descriptor (stdin). Implemented as FORCE_INLINE rather
	 * than constexpr because integer-to-pointer casts are not constant expressions.
	 * @return The invalid handle value for the current platform.
	 */
	static FORCE_INLINE PVOID InvalidFileHandle()
	{
#if defined(PLATFORM_LINUX) || defined(PLATFORM_ANDROID) || defined(PLATFORM_MACOS) || defined(PLATFORM_IOS) || defined(PLATFORM_SOLARIS) || defined(PLATFORM_FREEBSD)
		return (PVOID)(SSIZE)-1;
#else
		return nullptr;
#endif
	}
	~File() { Close(); }

	File(const File &) = delete;
	File &operator=(const File &) = delete;

	File(File &&other) noexcept;
	File &operator=(File &&other) noexcept;

	VOID *operator new(USIZE) = delete;
	VOID operator delete(VOID *) = delete;
	VOID *operator new(USIZE, PVOID ptr) noexcept { return ptr; }
	VOID operator delete(VOID *, PVOID) noexcept {}

	/**
	 * @brief Checks whether this file handle is valid (i.e., the file is open).
	 * @return TRUE if the handle is valid, FALSE otherwise.
	 */
	BOOL IsValid() const;

	/**
	 * @brief Closes the file handle and releases platform resources.
	 * @details Safe to call on an already-closed or default-constructed File.
	 */
	VOID Close();

	/**
	 * @brief Reads data from the file into the provided buffer.
	 * @details Reads up to buffer.Size() bytes from the current file position.
	 * On Windows, uses NtReadFile. On POSIX, uses the read() syscall.
	 * On UEFI, uses EFI_FILE_PROTOCOL::Read.
	 * @param buffer Writable span to receive the data.
	 * @return Number of bytes actually read, or an Error on failure.
	 */
	[[nodiscard]] Result<UINT32, Error> Read(Span<UINT8> buffer);

	/**
	 * @brief Writes data from the provided buffer to the file.
	 * @details Writes buffer.Size() bytes at the current file position.
	 * On Windows, uses NtWriteFile. On POSIX, uses the write() syscall.
	 * On UEFI, uses EFI_FILE_PROTOCOL::Write.
	 * @param buffer Read-only span containing the data to write.
	 * @return Number of bytes actually written, or an Error on failure.
	 */
	[[nodiscard]] Result<UINT32, Error> Write(Span<const UINT8> buffer);

	/**
	 * @brief Returns the cached file size in bytes.
	 * @return File size as set when the file was opened.
	 */
	constexpr USIZE GetSize() const { return fileSize; }

	/**
	 * @brief Gets the current file pointer position.
	 * @return Absolute byte offset from the beginning of the file, or an Error on failure.
	 */
	[[nodiscard]] Result<USIZE, Error> GetOffset() const;

	/**
	 * @brief Sets the file pointer to an absolute byte offset from the beginning.
	 * @param absoluteOffset Byte offset from the start of the file.
	 * @return Void on success, or an Error on failure.
	 */
	[[nodiscard]] Result<VOID, Error> SetOffset(USIZE absoluteOffset);

	/**
	 * @brief Moves the file pointer by a relative amount from the specified origin.
	 * @param relativeAmount Signed byte offset to move (positive = forward, negative = backward).
	 * @param origin Reference point for the seek (Start, Current, or End).
	 * @return Void on success, or an Error on failure.
	 */
	[[nodiscard]] Result<VOID, Error> MoveOffset(SSIZE relativeAmount, OffsetOrigin origin = OffsetOrigin::Current);
};
