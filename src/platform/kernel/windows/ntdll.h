/**
 * @file ntdll.h
 * @brief NT Native API (ntdll.dll) Wrappers
 *
 * @details Provides position-independent wrappers around the Windows NT Native
 * API exported by ntdll.dll. These wrappers form the lowest-level OS interface
 * on Windows, sitting directly above the system call layer.
 *
 * On x86_64 and i386, Zw* functions resolve System Service Numbers (SSNs) at
 * runtime and execute indirect syscalls through gadgets found in ntdll, avoiding
 * direct calls into ntdll. On ARM64, where the kernel validates that the svc
 * instruction originates from within ntdll, wrappers resolve and call the ntdll
 * export address directly.
 *
 * Rtl* functions (runtime library routines) are called through resolved ntdll
 * export addresses on all architectures, as they execute entirely in user mode.
 *
 * All function addresses are resolved dynamically via DJB2 hash-based PEB
 * module lookup, eliminating static import table entries.
 *
 * @note All wrappers return Result<NTSTATUS, Error> and use
 * result::FromNTSTATUS for uniform error handling.
 *
 * @see Windows Driver Kit (WDK) — NT Native API Reference
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/
 * @see Using Nt and Zw Versions of the Native System Services Routines
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-nt-and-zw-versions-of-the-native-system-services-routines
 */

#pragma once

#include "core/types/primitives.h"
#include "platform/kernel/windows/windows_types.h"
#include "core/algorithms/djb2.h"
#include "core/types/error.h"
#include "core/types/result.h"

#define EVENT_ALL_ACCESS ((0x000F0000L) | (0x00100000L) | 0x3)
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#define OBJ_CASE_INSENSITIVE 0x00000040L

/**
 * @brief Specifies the type of event object to create.
 *
 * @details Used by ZwCreateEvent to determine the signaling behavior of the
 * event object.
 *
 * @see ZwCreateEvent
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwcreateevent
 */
typedef enum _EVENT_TYPE
{
	NotificationEvent,    ///< Manual-reset event: remains signaled until explicitly reset
	SynchronizationEvent  ///< Auto-reset event: automatically resets after releasing one waiting thread
} EVENT_TYPE,
	*PEVENT_TYPE;

/**
 * @brief Contains basic timestamp and attribute information for a file.
 *
 * @details Returned by ZwQueryInformationFile with FileBasicInformation class,
 * and by ZwQueryAttributesFile.
 *
 * @see ZwQueryInformationFile
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwqueryinformationfile
 */
typedef struct _FILE_BASIC_INFORMATION
{
	LARGE_INTEGER CreationTime;    ///< Time the file was created
	LARGE_INTEGER LastAccessTime;  ///< Time the file was last accessed
	LARGE_INTEGER LastWriteTime;   ///< Time the file was last written to
	LARGE_INTEGER ChangeTime;      ///< Time the file metadata was last changed
	UINT32 FileAttributes;         ///< File attribute flags (FILE_ATTRIBUTE_*)
} FILE_BASIC_INFORMATION, *PFILE_BASIC_INFORMATION;

/**
 * @brief Contains standard size and link information for a file.
 *
 * @details Returned by ZwQueryInformationFile with FileStandardInformation class.
 *
 * @see ZwQueryInformationFile
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwqueryinformationfile
 */
typedef struct _FILE_STANDARD_INFORMATION
{
	LARGE_INTEGER AllocationSize; ///< Number of bytes allocated for the file
	LARGE_INTEGER EndOfFile;      ///< Actual file size in bytes
	UINT32 NumberOfLinks;         ///< Number of hard links
	INT8 DeletePending;           ///< TRUE if file is marked for deletion
	INT8 Directory;               ///< TRUE if it is a directory
} FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;

/**
 * @brief Contains the current byte offset for a file.
 *
 * @details Used with ZwQueryInformationFile/ZwSetInformationFile with
 * FilePositionInformation class.
 *
 * @see ZwSetInformationFile
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwsetinformationfile
 */
typedef struct _FILE_POSITION_INFORMATION
{
	LARGE_INTEGER CurrentByteOffset; ///< Current file position in bytes from the beginning
} FILE_POSITION_INFORMATION, *PFILE_POSITION_INFORMATION;

/**
 * @brief Controls whether a file is marked for deletion when all handles are closed.
 *
 * @details Used with ZwSetInformationFile with FileDispositionInformation class.
 *
 * @see ZwSetInformationFile
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwsetinformationfile
 */
typedef struct _FILE_DISPOSITION_INFORMATION
{
	BOOL DeleteFile; ///< TRUE to mark the file for deletion on close
} FILE_DISPOSITION_INFORMATION, *PFILE_DISPOSITION_INFORMATION;

/**
 * @brief Callback routine invoked when an asynchronous I/O operation completes.
 *
 * @details Passed to asynchronous NT I/O functions (ZwReadFile, ZwWriteFile,
 * ZwDeviceIoControlFile) and called upon completion.
 */
typedef VOID(STDCALL *PIO_APC_ROUTINE)(
	PVOID ApcContext,
	PIO_STATUS_BLOCK IoStatusBlock,
	UINT32 Reserved);

typedef struct _RTLP_CURDIR_REF *PRTLP_CURDIR_REF;

/**
 * @brief Contains a relative NT path name and its containing directory handle.
 *
 * @details Populated by RtlDosPathNameToNtPathName_U when the input path can
 * be expressed relative to a parent directory.
 */
typedef struct _RTL_RELATIVE_NAME_U
{
	UNICODE_STRING RelativeName;     ///< Relative portion of the NT path
	PVOID ContainingDirectory;       ///< Handle to the containing directory
	PRTLP_CURDIR_REF CurDirRef;     ///< Reference to the current directory state
} RTL_RELATIVE_NAME_U, *PRTL_RELATIVE_NAME_U;

/**
 * @brief Contains device type and characteristic information for a file system volume.
 *
 * @details Returned by ZwQueryVolumeInformationFile with FileFsDeviceInformation class.
 *
 * @see ZwQueryVolumeInformationFile
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-zwqueryvolumeinformationfile
 */
typedef struct _FILE_FS_DEVICE_INFORMATION
{
	UINT32 DeviceType;      ///< Type of the device (e.g., FILE_DEVICE_DISK)
	UINT32 Characteristics; ///< Bitmask of device characteristics (FILE_DEVICE_* flags)
} FILE_FS_DEVICE_INFORMATION, *PFILE_FS_DEVICE_INFORMATION;

/**
 * @brief Contains detailed information about a file in a directory listing.
 *
 * @details Returned by ZwQueryDirectoryFile with FileBothDirectoryInformation
 * class. Includes both the long file name and the 8.3 short name.
 *
 * @see ZwQueryDirectoryFile
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwquerydirectoryfile
 */
typedef struct _FILE_BOTH_DIR_INFORMATION
{
	UINT32 NextEntryOffset;         ///< Byte offset to the next entry, or 0 for the last entry
	UINT32 FileIndex;               ///< File reference number for the directory entry
	LARGE_INTEGER CreationTime;     ///< Time the file was created
	LARGE_INTEGER LastAccessTime;   ///< Time the file was last accessed
	LARGE_INTEGER LastWriteTime;    ///< Time the file was last written to
	LARGE_INTEGER ChangeTime;       ///< Time the file metadata was last changed
	LARGE_INTEGER EndOfFile;        ///< Actual file size in bytes
	LARGE_INTEGER AllocationSize;   ///< Number of bytes allocated for the file
	UINT32 FileAttributes;          ///< File attribute flags (FILE_ATTRIBUTE_*)
	UINT32 FileNameLength;          ///< Length of FileName in bytes (not including null)
	UINT32 EaSize;                  ///< Size of extended attributes
	INT8 ShortNameLength;           ///< Length of ShortName in bytes
	WCHAR ShortName[12];            ///< 8.3 short file name
	WCHAR FileName[1];              ///< Variable-length long file name
} FILE_BOTH_DIR_INFORMATION, *PFILE_BOTH_DIR_INFORMATION;

/**
 * @brief Identifies the type of file information to query or set.
 *
 * @details Used as the FileInformationClass parameter in ZwQueryInformationFile,
 * ZwSetInformationFile, and ZwQueryDirectoryFile.
 *
 * @see ZwQueryInformationFile
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwqueryinformationfile
 */
typedef enum _FILE_INFORMATION_CLASS_DIR
{
	FileBothDirectoryInformation = 3,  ///< Query directory with both long and short names
	FileFsDeviceInformation = 4,       ///< Query file system device information
	FileStandardInformation = 5,       ///< Query standard file information (size, links)
	FileDispositionInformation = 13,   ///< Set file deletion disposition
	FilePositionInformation = 14,      ///< Query or set current byte offset
} FILE_INFORMATION_CLASS_DIR;

/**
 * @brief Wrappers for NT Native API functions exported by ntdll.dll.
 *
 * @details Provides position-independent access to the Windows NT Native API.
 * Zw* functions use indirect syscalls (x86_64/i386) or direct ntdll calls
 * (ARM64). Rtl* functions are called through resolved export addresses.
 *
 * All Zw* wrappers follow the pattern:
 * 1. Resolve SSN via ResolveSyscall()
 * 2. If SSN is valid, execute via System::Call (indirect syscall)
 * 3. Otherwise, fall back to direct ntdll function call
 * 4. Convert NTSTATUS to Result via result::FromNTSTATUS
 *
 * @see Using Nt and Zw Versions of the Native System Services Routines
 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-nt-and-zw-versions-of-the-native-system-services-routines
 */
class NTDLL
{
public:
	/**
	 * @brief Converts a DOS/Win32 path name to an NT path name.
	 *
	 * @details Translates a user-mode DOS-style path (e.g., "C:\\file.txt") into
	 * the equivalent NT object namespace path (e.g., "\\??\\C:\\file.txt"). The
	 * returned UNICODE_STRING is allocated by the runtime library and must be
	 * freed with RtlFreeUnicodeString.
	 *
	 * @param DosName The DOS/Win32 path to convert.
	 * @param NtName Receives the translated NT path as a UNICODE_STRING.
	 * @param FilePart Optional. Receives a pointer to the file name portion within NtName.
	 * @param RelativeName Optional. Receives the relative name and containing directory handle.
	 *
	 * @return Result<VOID, Error> Ok() on success, Err(Ntdll_RtlPathResolveFailed) on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see RtlDosPathNameToNtPathName_U_WithStatus (documented variant)
	 *      https://learn.microsoft.com/en-us/windows/win32/devnotes/rtldospathnametontpathname_u_withstatus
	 */
	[[nodiscard]] static Result<VOID, Error> RtlDosPathNameToNtPathName_U(const WCHAR *DosName, UNICODE_STRING *NtName, WCHAR **FilePart, PRTL_RELATIVE_NAME_U RelativeName);

	/**
	 * @brief Frees a UNICODE_STRING buffer allocated by the runtime library.
	 *
	 * @details Releases the memory allocated for a UNICODE_STRING previously
	 * returned by Rtl* functions such as RtlDosPathNameToNtPathName_U.
	 * After this call, the UNICODE_STRING fields are zeroed.
	 *
	 * @param UnicodeString Pointer to the UNICODE_STRING to free.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see RtlFreeUnicodeString
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-rtlfreeunicodestring
	 */
	static VOID RtlFreeUnicodeString(PUNICODE_STRING UnicodeString);

	/**
	 * @brief Creates or opens a kernel event object.
	 *
	 * @details Creates a new event object of the specified type (notification or
	 * synchronization). Notification events remain signaled until explicitly
	 * reset; synchronization events auto-reset after releasing one thread.
	 *
	 * @param EventHandle Receives the handle to the created event object.
	 * @param DesiredAccess Access mask (typically EVENT_ALL_ACCESS).
	 * @param ObjectAttributes Optional attributes including name and security descriptor.
	 * @param EventType NotificationEvent (manual-reset) or SynchronizationEvent (auto-reset).
	 * @param InitialState TRUE if the event should be created in the signaled state.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows XP
	 * Minimum supported server: Windows Server 2003
	 *
	 * @see ZwCreateEvent
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwcreateevent
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwCreateEvent(PPVOID EventHandle, UINT32 DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, EVENT_TYPE EventType, INT8 InitialState);

	/**
	 * @brief Sends a device I/O control code to the driver associated with a file handle.
	 *
	 * @details Builds descriptors for the supplied input/output buffers and
	 * passes the untyped data to the device driver associated with the file
	 * handle. Used extensively for AFD (Auxiliary Function Driver) socket
	 * operations on Windows.
	 *
	 * @param FileHandle Handle to the file or device object.
	 * @param Event Optional event to signal upon completion.
	 * @param ApcRoutine Optional APC routine called upon completion.
	 * @param ApcContext Context passed to the APC routine.
	 * @param IoStatusBlock Receives the final status and byte count.
	 * @param IoControlCode Device-specific IOCTL code.
	 * @param InputBuffer Input data for the IOCTL operation.
	 * @param InputBufferLength Size of InputBuffer in bytes.
	 * @param OutputBuffer Buffer to receive output data.
	 * @param OutputBufferLength Size of OutputBuffer in bytes.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS (including STATUS_PENDING), Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwDeviceIoControlFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwdeviceiocontrolfile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwDeviceIoControlFile(PVOID FileHandle, PVOID Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, UINT32 IoControlCode, PVOID InputBuffer, UINT32 InputBufferLength, PVOID OutputBuffer, UINT32 OutputBufferLength);

	/**
	 * @brief Waits until the specified object is in the signaled state.
	 *
	 * @details Blocks the calling thread until the specified object (event,
	 * mutex, semaphore, process, thread, etc.) enters the signaled state,
	 * or until the optional timeout expires.
	 *
	 * @param Object Handle to the object to wait on.
	 * @param Alertable TRUE if the wait should be alertable (can be interrupted by APCs).
	 * @param Timeout Optional maximum wait time in 100-nanosecond intervals (negative = relative).
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS (including STATUS_TIMEOUT), Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps only]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwWaitForSingleObject
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwwaitforsingleobject
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwWaitForSingleObject(PVOID Object, INT8 Alertable, PLARGE_INTEGER Timeout);

	/**
	 * @brief Closes an open object handle.
	 *
	 * @details Releases the reference held by the handle and invalidates it.
	 * Applicable to any kernel object type (file, event, process, thread,
	 * section, key, etc.).
	 *
	 * @param Handle The object handle to close.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwClose
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwclose
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwClose(PVOID Handle);

	/**
	 * @brief Creates a new file or directory, or opens an existing one.
	 *
	 * @details Creates or opens a file, device, directory, or volume and returns
	 * a handle. This is the primary NT Native API for file creation and
	 * provides full control over access rights, sharing, disposition, and
	 * creation options.
	 *
	 * @param FileHandle Receives the handle to the created/opened file object.
	 * @param DesiredAccess Access mask specifying requested access rights.
	 * @param ObjectAttributes Pointer to OBJECT_ATTRIBUTES with the NT path and attributes.
	 * @param IoStatusBlock Receives the final status and information (e.g., FILE_CREATED).
	 * @param AllocationSize Optional initial allocation size for a new file.
	 * @param FileAttributes Attributes for a new file (e.g., FILE_ATTRIBUTE_NORMAL).
	 * @param ShareAccess Sharing mode (FILE_SHARE_READ, FILE_SHARE_WRITE, FILE_SHARE_DELETE).
	 * @param CreateDisposition Action to take (FILE_CREATE, FILE_OPEN, FILE_OPEN_IF, etc.).
	 * @param CreateOptions Flags controlling file behavior (FILE_SYNCHRONOUS_IO_NONALERT, etc.).
	 * @param EaBuffer Optional extended attributes buffer.
	 * @param EaLength Length of EaBuffer in bytes.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps | UWP apps]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwCreateFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwcreatefile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwCreateFile(PPVOID FileHandle, UINT32 DesiredAccess, PVOID ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, UINT32 FileAttributes, UINT32 ShareAccess, UINT32 CreateDisposition, UINT32 CreateOptions, PVOID EaBuffer, UINT32 EaLength);

	/**
	 * @brief Allocates virtual memory in the address space of a process.
	 *
	 * @details Reserves, commits, or both reserves and commits a region of
	 * virtual memory within the specified process. The base address and
	 * region size are rounded to page boundaries.
	 *
	 * @param ProcessHandle Handle to the target process (NtCurrentProcess() for self).
	 * @param BaseAddress In/out: requested base address; receives the actual allocated address.
	 * @param ZeroBits Number of high-order zero bits required in the base address.
	 * @param RegionSize In/out: requested size; receives the actual allocated size.
	 * @param AllocationType Type of allocation (MEM_COMMIT, MEM_RESERVE, or both).
	 * @param Protect Memory protection (PAGE_READWRITE, PAGE_EXECUTE_READ, etc.).
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows XP
	 * Minimum supported server: Windows Server 2003
	 *
	 * @see ZwAllocateVirtualMemory
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwallocatevirtualmemory
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwAllocateVirtualMemory(PVOID ProcessHandle, PPVOID BaseAddress, USIZE ZeroBits, PUSIZE RegionSize, UINT32 AllocationType, UINT32 Protect);

	/**
	 * @brief Releases or decommits virtual memory in the address space of a process.
	 *
	 * @details Decommits committed pages, releases an entire reserved region,
	 * or both. When releasing (MEM_RELEASE), BaseAddress must be the base
	 * returned by ZwAllocateVirtualMemory and RegionSize must be 0.
	 *
	 * @param ProcessHandle Handle to the target process (NtCurrentProcess() for self).
	 * @param BaseAddress In/out: base address of the region to free.
	 * @param RegionSize In/out: size of the region (0 for MEM_RELEASE of entire region).
	 * @param FreeType Type of free operation (MEM_DECOMMIT or MEM_RELEASE).
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows XP
	 * Minimum supported server: Windows Server 2003
	 *
	 * @see ZwFreeVirtualMemory
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwfreevirtualmemory
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwFreeVirtualMemory(PVOID ProcessHandle, PPVOID BaseAddress, PUSIZE RegionSize, UINT32 FreeType);

	/**
	 * @brief Terminates the specified process and all of its threads.
	 *
	 * @details Forces the target process to exit with the specified status code.
	 * All threads in the process are terminated. Use NtCurrentProcess()
	 * to terminate the calling process.
	 *
	 * @param ProcessHandle Handle to the process to terminate (NtCurrentProcess() for self).
	 * @param ExitStatus Exit code to report for the process.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Target platform: Universal
	 * Minimum supported client: Windows XP
	 * Minimum supported server: Windows Server 2003
	 *
	 * @see ZwTerminateProcess
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-zwterminateprocess
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwTerminateProcess(PVOID ProcessHandle, NTSTATUS ExitStatus);

	/**
	 * @brief Retrieves information about a file object.
	 *
	 * @details Queries various types of information for a file, directory,
	 * or volume specified by the file handle. The information class determines
	 * the structure written to the output buffer.
	 *
	 * @param FileHandle Handle to the file object.
	 * @param IoStatusBlock Receives the final status and bytes returned.
	 * @param FileInformation Output buffer for the requested information structure.
	 * @param Length Size of FileInformation buffer in bytes.
	 * @param FileInformationClass Identifies which information structure to return.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps | UWP apps]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwQueryInformationFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwqueryinformationfile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwQueryInformationFile(PVOID FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, UINT32 Length, UINT32 FileInformationClass);

	/**
	 * @brief Reads data from an open file.
	 *
	 * @details Reads a specified number of bytes from the given file starting
	 * at the specified byte offset. Supports both synchronous and asynchronous
	 * operation via the Event and ApcRoutine parameters.
	 *
	 * @param FileHandle Handle to the file to read from.
	 * @param Event Optional event to signal upon completion.
	 * @param ApcRoutine Optional APC routine called upon completion.
	 * @param ApcContext Context passed to the APC routine.
	 * @param IoStatusBlock Receives the final status and number of bytes read.
	 * @param Buffer Output buffer to receive the data.
	 * @param Length Number of bytes to read.
	 * @param ByteOffset Optional file offset to start reading from.
	 * @param Key Optional key for byte-range lock.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps | UWP apps]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwReadFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwreadfile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwReadFile(PVOID FileHandle, PVOID Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, UINT32 Length, PLARGE_INTEGER ByteOffset, PUINT32 Key);

	/**
	 * @brief Writes data to an open file.
	 *
	 * @details Writes a specified number of bytes to the given file starting
	 * at the specified byte offset. Supports both synchronous and asynchronous
	 * operation via the Event and ApcRoutine parameters.
	 *
	 * @param FileHandle Handle to the file to write to.
	 * @param Event Optional event to signal upon completion.
	 * @param ApcRoutine Optional APC routine called upon completion.
	 * @param ApcContext Context passed to the APC routine.
	 * @param IoStatusBlock Receives the final status and number of bytes written.
	 * @param Buffer Input buffer containing the data to write.
	 * @param Length Number of bytes to write.
	 * @param ByteOffset Optional file offset to start writing at.
	 * @param Key Optional key for byte-range lock.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps | UWP apps]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwWriteFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwwritefile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwWriteFile(PVOID FileHandle, PVOID Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, UINT32 Length, PLARGE_INTEGER ByteOffset, PUINT32 Key);

	/**
	 * @brief Sets information for a file object.
	 *
	 * @details Changes various types of information associated with a file
	 * object. The information class determines which structure is expected
	 * in the input buffer (e.g., position, disposition, end-of-file).
	 *
	 * @param FileHandle Handle to the file object.
	 * @param IoStatusBlock Receives the final status.
	 * @param FileInformation Buffer containing the information to set.
	 * @param Length Size of FileInformation buffer in bytes.
	 * @param FileInformationClass Identifies which information structure is provided.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps | UWP apps]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwSetInformationFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwsetinformationfile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwSetInformationFile(PVOID FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, UINT32 Length, UINT32 FileInformationClass);

	/**
	 * @brief Deletes a file by its object attributes.
	 *
	 * @details Deletes the specified file. The file is identified by its
	 * OBJECT_ATTRIBUTES (NT path), not by a handle. The file must not be
	 * currently opened by any process.
	 *
	 * @param FileName Pointer to OBJECT_ATTRIBUTES describing the file to delete.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows XP [desktop apps | UWP apps]
	 * Minimum supported server: Windows Server 2003
	 *
	 * @see ZwDeleteFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwdeletefile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwDeleteFile(POBJECT_ATTRIBUTES FileName);

	/**
	 * @brief Retrieves basic attribute information for a file without opening it.
	 *
	 * @details Queries the basic attributes (timestamps, file attributes) of a
	 * file identified by its OBJECT_ATTRIBUTES. More efficient than opening
	 * the file with ZwCreateFile and then querying, as no handle is created.
	 *
	 * @param ObjectAttributes Pointer to OBJECT_ATTRIBUTES describing the target file.
	 * @param FileInformation Receives the FILE_BASIC_INFORMATION for the file.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see NtQueryAttributesFile
	 *      https://learn.microsoft.com/en-us/windows/win32/devnotes/ntqueryattributesfile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwQueryAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes, PFILE_BASIC_INFORMATION FileInformation);

	/**
	 * @brief Opens an existing file, device, directory, or volume.
	 *
	 * @details Opens an existing file object and returns a handle. Unlike
	 * ZwCreateFile, this function cannot create new files and has a simpler
	 * parameter set. Equivalent to ZwCreateFile with FILE_OPEN disposition.
	 *
	 * @param FileHandle Receives the handle to the opened file object.
	 * @param DesiredAccess Access mask specifying requested access rights.
	 * @param ObjectAttributes Pointer to OBJECT_ATTRIBUTES with the NT path and attributes.
	 * @param IoStatusBlock Receives the final status and information.
	 * @param ShareAccess Sharing mode (FILE_SHARE_READ, FILE_SHARE_WRITE, FILE_SHARE_DELETE).
	 * @param OpenOptions Flags controlling open behavior (FILE_SYNCHRONOUS_IO_NONALERT, etc.).
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional [desktop apps | UWP apps]
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwOpenFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwopenfile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwOpenFile(PPVOID FileHandle, UINT32 DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, UINT32 ShareAccess, UINT32 OpenOptions);

	/**
	 * @brief Retrieves volume information for the file system of a given file.
	 *
	 * @details Queries file system information for the volume containing the
	 * specified file. The information class determines the structure returned
	 * (e.g., device type, size, label).
	 *
	 * @param FileHandle Handle to any file on the volume.
	 * @param IoStatusBlock Receives the final status and bytes returned.
	 * @param FsInformation Output buffer for the volume information structure.
	 * @param Length Size of FsInformation buffer in bytes.
	 * @param FsInformationClass Identifies which volume information structure to return.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows XP [desktop apps | UWP apps]
	 * Minimum supported server: Windows Server 2003
	 *
	 * @see ZwQueryVolumeInformationFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-zwqueryvolumeinformationfile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwQueryVolumeInformationFile(PVOID FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation, UINT32 Length, UINT32 FsInformationClass);

	/**
	 * @brief Retrieves information about a process.
	 *
	 * @details Queries various classes of information about a process identified
	 * by its handle. The information class determines what is returned (e.g.,
	 * basic info, PEB address, image file name).
	 *
	 * @param ProcessHandle Handle to the process to query (NtCurrentProcess() for self).
	 * @param ProcessInformationClass Identifies which information class to retrieve.
	 * @param ProcessInformation Output buffer for the requested information.
	 * @param ProcessInformationLength Size of ProcessInformation buffer in bytes.
	 * @param ReturnLength Optional. Receives the actual number of bytes written.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows XP [desktop apps | UWP apps]
	 * Minimum supported server: Windows Server 2003
	 *
	 * @see ZwQueryInformationProcess
	 *      https://learn.microsoft.com/en-us/windows/win32/procthread/zwqueryinformationprocess
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwQueryInformationProcess(PVOID ProcessHandle, UINT32 ProcessInformationClass, PVOID ProcessInformation, UINT32 ProcessInformationLength, PUINT32 ReturnLength);

	/**
	 * @brief Returns a pseudo-handle to the current process.
	 *
	 * @details Returns the constant (PVOID)-1, which is interpreted by the
	 * kernel as a reference to the calling process. This pseudo-handle does
	 * not need to be closed.
	 *
	 * @return Pseudo-handle representing the current process.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwCurrentProcess
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/zwcurrentprocess
	 */
	static FORCE_INLINE PVOID NtCurrentProcess() { return (PVOID)(USIZE)-1L; }

	/**
	 * @brief Returns a pseudo-handle to the current thread.
	 *
	 * @details Returns the constant (PVOID)-2, which is interpreted by the
	 * kernel as a reference to the calling thread. This pseudo-handle does
	 * not need to be closed.
	 *
	 * @return Pseudo-handle representing the current thread.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see ZwCurrentThread
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/zwcurrentthread
	 */
	static FORCE_INLINE PVOID NtCurrentThread() { return (PVOID)(USIZE)-2L; }

	/**
	 * @brief Creates a named pipe file object.
	 *
	 * @details Creates the server end of a named pipe and returns a handle.
	 * Named pipes provide inter-process communication with support for
	 * byte-stream and message-mode data transfer.
	 *
	 * @param FileHandle Receives the handle to the named pipe.
	 * @param DesiredAccess Access mask specifying requested access rights.
	 * @param ObjectAttributes Pointer to OBJECT_ATTRIBUTES with the pipe's NT path.
	 * @param IoStatusBlock Receives the final status.
	 * @param ShareAccess Sharing mode for the pipe.
	 * @param CreateDisposition Creation disposition (FILE_CREATE, FILE_OPEN, etc.).
	 * @param CreateOptions Flags controlling pipe behavior.
	 * @param NamedPipeType Pipe type: byte stream (0) or message mode (1).
	 * @param ReadMode Read mode: byte stream (0) or message mode (1).
	 * @param CompletionMode Completion mode: blocking (0) or non-blocking (1).
	 * @param MaximumInstances Maximum number of pipe instances.
	 * @param InboundQuota Size of the inbound buffer in bytes.
	 * @param OutboundQuota Size of the outbound buffer in bytes.
	 * @param DefaultTimeout Default timeout in 100-nanosecond intervals (negative = relative).
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see NtCreateNamedPipeFile
	 *      https://learn.microsoft.com/en-us/windows/win32/devnotes/nt-create-named-pipe-file
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwCreateNamedPipeFile(PPVOID FileHandle, UINT32 DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, UINT32 ShareAccess, UINT32 CreateDisposition, UINT32 CreateOptions, UINT32 NamedPipeType, UINT32 ReadMode, UINT32 CompletionMode, UINT32 MaximumInstances, UINT32 InboundQuota, UINT32 OutboundQuota, PLARGE_INTEGER DefaultTimeout);

	/**
	 * @brief Sets information for a kernel object.
	 *
	 * @details Changes properties of a kernel object identified by its handle.
	 * The information class determines which property is being set (e.g.,
	 * handle flags, object name).
	 *
	 * @param Handle Handle to the object.
	 * @param ObjectInformationClass Identifies which object property to set.
	 * @param ObjectInformation Buffer containing the information to set.
	 * @param ObjectInformationLength Size of ObjectInformation buffer in bytes.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @note This function is undocumented by Microsoft. Behavior may vary across
	 * Windows versions.
	 *
	 * @see SetHandleInformation (closest documented Win32 equivalent)
	 *      https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-sethandleinformation
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwSetInformationObject(PVOID Handle, UINT32 ObjectInformationClass, PVOID ObjectInformation, UINT32 ObjectInformationLength);

	/**
	 * @brief Creates a new user-mode process with an initial thread.
	 *
	 * @details The modern NT Native API for process creation, replacing the
	 * older NtCreateProcessEx + NtCreateThread combination. Creates both
	 * the process and its initial thread atomically. Available since
	 * Windows Vista.
	 *
	 * @param ProcessHandle Receives the handle to the new process.
	 * @param ThreadHandle Receives the handle to the initial thread.
	 * @param ProcessDesiredAccess Access mask for the process handle.
	 * @param ThreadDesiredAccess Access mask for the thread handle.
	 * @param ProcessObjectAttributes Optional attributes for the process object.
	 * @param ThreadObjectAttributes Optional attributes for the thread object.
	 * @param ProcessFlags Flags controlling process creation behavior.
	 * @param ThreadFlags Flags controlling thread creation behavior.
	 * @param ProcessParameters RTL_USER_PROCESS_PARAMETERS for the new process.
	 * @param CreateInfo Process creation information structure.
	 * @param AttributeList Process/thread attribute list.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows Vista
	 * Minimum supported server: Windows Server 2008
	 *
	 * @note This function is undocumented by Microsoft. Behavior may vary across
	 * Windows versions.
	 *
	 * @see CreateProcessW (documented Win32 equivalent)
	 *      https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwCreateUserProcess(PPVOID ProcessHandle, PPVOID ThreadHandle, UINT32 ProcessDesiredAccess, UINT32 ThreadDesiredAccess, POBJECT_ATTRIBUTES ProcessObjectAttributes, POBJECT_ATTRIBUTES ThreadObjectAttributes, UINT32 ProcessFlags, UINT32 ThreadFlags, PVOID ProcessParameters, PVOID CreateInfo, PVOID AttributeList);

	/**
	 * @brief Creates a process parameters block for a new process.
	 *
	 * @details Allocates and initializes an RTL_USER_PROCESS_PARAMETERS
	 * structure containing the image path, command line, environment,
	 * and other startup information for ZwCreateUserProcess.
	 *
	 * @param ProcessParameters Receives a pointer to the allocated parameters block.
	 * @param ImagePathName NT path to the executable image.
	 * @param DllPath Optional DLL search path.
	 * @param CurrentDirectory Optional current directory for the new process.
	 * @param CommandLine Optional command line string.
	 * @param Environment Optional environment block.
	 * @param WindowTitle Optional window title.
	 * @param DesktopInfo Optional desktop information.
	 * @param ShellInfo Optional shell information.
	 * @param RuntimeData Optional runtime data.
	 * @param Flags Flags controlling parameter creation (e.g., RTL_USER_PROC_PARAMS_NORMALIZED).
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows Vista
	 * Minimum supported server: Windows Server 2008
	 *
	 * @note This function is undocumented by Microsoft. Behavior may vary across
	 * Windows versions.
	 *
	 * @see RTL_USER_PROCESS_PARAMETERS structure
	 *      https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-rtl_user_process_parameters
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> RtlCreateProcessParametersEx(PVOID *ProcessParameters, PUNICODE_STRING ImagePathName, PUNICODE_STRING DllPath, PUNICODE_STRING CurrentDirectory, PUNICODE_STRING CommandLine, PVOID Environment, PUNICODE_STRING WindowTitle, PUNICODE_STRING DesktopInfo, PUNICODE_STRING ShellInfo, PUNICODE_STRING RuntimeData, UINT32 Flags);

	/**
	 * @brief Frees a process parameters block created by RtlCreateProcessParametersEx.
	 *
	 * @details Releases the memory allocated for an RTL_USER_PROCESS_PARAMETERS
	 * structure. Must be called to avoid memory leaks after process creation.
	 *
	 * @param ProcessParameters Pointer to the parameters block to free.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @note This function is undocumented by Microsoft. Behavior may vary across
	 * Windows versions.
	 *
	 * @see RTL_USER_PROCESS_PARAMETERS structure
	 *      https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-rtl_user_process_parameters
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> RtlDestroyProcessParameters(PVOID ProcessParameters);

	/**
	 * @brief Queries directory entries for files matching an optional pattern.
	 *
	 * @details Enumerates files and subdirectories within a directory opened
	 * with FILE_LIST_DIRECTORY access. Returns one or more entries per call
	 * depending on buffer size and ReturnSingleEntry. Use RestartScan to
	 * begin enumeration from the start.
	 *
	 * @param FileHandle Handle to the directory to enumerate.
	 * @param Event Optional event to signal upon completion.
	 * @param ApcRoutine Optional APC routine called upon completion.
	 * @param ApcContext Context passed to the APC routine.
	 * @param IoStatusBlock Receives the final status and bytes returned.
	 * @param FileInformation Output buffer for the directory entry structures.
	 * @param Length Size of FileInformation buffer in bytes.
	 * @param FileInformationClass Identifies the directory information structure type.
	 * @param ReturnSingleEntry TRUE to return only one entry per call.
	 * @param FileName Optional wildcard pattern to filter entries.
	 * @param RestartScan TRUE to restart enumeration from the beginning.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows XP [desktop apps | UWP apps]
	 * Minimum supported server: Windows Server 2003
	 *
	 * @see ZwQueryDirectoryFile
	 *      https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-zwquerydirectoryfile
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwQueryDirectoryFile(PVOID FileHandle, PVOID Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, UINT32 Length, UINT32 FileInformationClass, BOOL ReturnSingleEntry, PUNICODE_STRING FileName, BOOL RestartScan);

	/**
	 * @brief Loads a DLL into the address space of the calling process.
	 *
	 * @details Maps the specified DLL into the process address space and
	 * resolves its imports. If the DLL is already loaded, increments its
	 * reference count and returns the existing base address.
	 *
	 * @param SearchPath Optional DLL search path, or NULL for default search order.
	 * @param DllCharacteristics Optional pointer to DLL characteristics flags, or NULL.
	 * @param DllName Pointer to UNICODE_STRING containing the DLL name.
	 * @param BaseAddress Receives the base address of the loaded DLL.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @note This function is undocumented by Microsoft.
	 *
	 * @see LoadLibraryW (documented Win32 equivalent)
	 *      https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryw
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> LdrLoadDll(PWCHAR SearchPath, UINT32 *DllCharacteristics, PUNICODE_STRING DllName, PPVOID BaseAddress);

	/**
	 * @brief Retrieves system information of the specified class.
	 *
	 * @details Queries a wide range of system information depending on the
	 * information class. Used here primarily for SystemFirmwareTableInformation
	 * (class 76) to retrieve raw SMBIOS firmware tables.
	 *
	 * @param SystemInformationClass Identifies the type of information to query.
	 * @param SystemInformation Output buffer to receive the requested information.
	 * @param SystemInformationLength Size of the SystemInformation buffer in bytes.
	 * @param ReturnLength Optional pointer to receive the actual size of information returned.
	 *
	 * @return Result<NTSTATUS, Error> Ok(status) on NT_SUCCESS, Err on failure.
	 *
	 * @par Requirements
	 * Minimum supported client: Windows 2000 Professional
	 * Minimum supported server: Windows 2000 Server
	 *
	 * @see NtQuerySystemInformation
	 *      https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation
	 */
	[[nodiscard]] static Result<NTSTATUS, Error> ZwQuerySystemInformation(UINT32 SystemInformationClass, PVOID SystemInformation, UINT32 SystemInformationLength, PUINT32 ReturnLength);
};
