#include "platform/kernel/windows/ntdll.h"
#include "platform/platform.h"
#include "platform/kernel/windows/platform_result.h"
#include "platform/kernel/windows/peb.h"
#include "platform/kernel/windows/system.h"

#define ResolveNtdllExportAddress(functionName) ResolveExportAddressFromPebModule(Djb2::HashCompileTime(L"ntdll.dll"), Djb2::HashCompileTime(functionName))

template <typename... Args>
[[nodiscard]] static inline Result<NTSTATUS, Error> DispatchNt(UINT64 nameHash, Args... args)
{
#ifndef NO_SYSCALL
	SYSCALL_ENTRY entry = System::ResolveSyscallEntry(nameHash);
	if (entry.Ssn != SYSCALL_SSN_INVALID)
	{
		return result::FromNTSTATUS<NTSTATUS>(System::Call(entry, (USIZE)args...));
	}
#endif
	using Fn = NTSTATUS(STDCALL *)(Args...);
	auto fn = (Fn)ResolveExportAddressFromPebModule(Djb2::HashCompileTime(L"ntdll.dll"), nameHash);
	return result::FromNTSTATUS<NTSTATUS>(fn(args...));
}

Result<NTSTATUS, Error> NTDLL::ZwCreateEvent(PPVOID EventHandle, UINT32 DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, EVENT_TYPE EventType, INT8 InitialState)
{
	return DispatchNt(Djb2::HashCompileTime("ZwCreateEvent"), EventHandle, DesiredAccess, ObjectAttributes, EventType, InitialState);
}

Result<NTSTATUS, Error> NTDLL::ZwDeviceIoControlFile(PVOID FileHandle, PVOID Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, UINT32 IoControlCode, PVOID InputBuffer, UINT32 InputBufferLength, PVOID OutputBuffer, UINT32 OutputBufferLength)
{
	return DispatchNt(Djb2::HashCompileTime("ZwDeviceIoControlFile"), FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, IoControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength);
}

Result<NTSTATUS, Error> NTDLL::ZwWaitForSingleObject(PVOID Object, INT8 Alertable, PLARGE_INTEGER Timeout)
{
	return DispatchNt(Djb2::HashCompileTime("ZwWaitForSingleObject"), Object, Alertable, Timeout);
}

Result<NTSTATUS, Error> NTDLL::ZwClose(PVOID Handle)
{
	return DispatchNt(Djb2::HashCompileTime("ZwClose"), Handle);
}

Result<NTSTATUS, Error> NTDLL::ZwCreateFile(PPVOID FileHandle, UINT32 DesiredAccess, PVOID ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, UINT32 FileAttributes, UINT32 ShareAccess, UINT32 CreateDisposition, UINT32 CreateOptions, PVOID EaBuffer, UINT32 EaLength)
{
	return DispatchNt(Djb2::HashCompileTime("ZwCreateFile"), FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

Result<NTSTATUS, Error> NTDLL::ZwAllocateVirtualMemory(PVOID ProcessHandle, PPVOID BaseAddress, USIZE ZeroBits, PUSIZE RegionSize, UINT32 AllocationType, UINT32 Protect)
{
	return DispatchNt(Djb2::HashCompileTime("ZwAllocateVirtualMemory"), ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect);
}

Result<NTSTATUS, Error> NTDLL::ZwFreeVirtualMemory(PVOID ProcessHandle, PPVOID BaseAddress, PUSIZE RegionSize, UINT32 FreeType)
{
	return DispatchNt(Djb2::HashCompileTime("ZwFreeVirtualMemory"), ProcessHandle, BaseAddress, RegionSize, FreeType);
}

Result<NTSTATUS, Error> NTDLL::ZwTerminateProcess(PVOID ProcessHandle, NTSTATUS ExitStatus)
{
	return DispatchNt(Djb2::HashCompileTime("ZwTerminateProcess"), ProcessHandle, ExitStatus);
}

Result<NTSTATUS, Error> NTDLL::ZwQueryInformationFile(PVOID FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, UINT32 Length, UINT32 FileInformationClass)
{
	return DispatchNt(Djb2::HashCompileTime("ZwQueryInformationFile"), FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
}

Result<NTSTATUS, Error> NTDLL::ZwReadFile(PVOID FileHandle, PVOID Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, UINT32 Length, PLARGE_INTEGER ByteOffset, PUINT32 Key)
{
	return DispatchNt(Djb2::HashCompileTime("ZwReadFile"), FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key);
}

Result<NTSTATUS, Error> NTDLL::ZwWriteFile(PVOID FileHandle, PVOID Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, UINT32 Length, PLARGE_INTEGER ByteOffset, PUINT32 Key)
{
	return DispatchNt(Djb2::HashCompileTime("ZwWriteFile"), FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key);
}

Result<NTSTATUS, Error> NTDLL::ZwSetInformationFile(PVOID FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, UINT32 Length, UINT32 FileInformationClass)
{
	return DispatchNt(Djb2::HashCompileTime("ZwSetInformationFile"), FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
}

Result<NTSTATUS, Error> NTDLL::ZwDeleteFile(POBJECT_ATTRIBUTES FileName)
{
	return DispatchNt(Djb2::HashCompileTime("ZwDeleteFile"), FileName);
}

Result<NTSTATUS, Error> NTDLL::ZwQueryAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes, PFILE_BASIC_INFORMATION FileInformation)
{
	return DispatchNt(Djb2::HashCompileTime("ZwQueryAttributesFile"), ObjectAttributes, FileInformation);
}

Result<NTSTATUS, Error> NTDLL::ZwOpenFile(PPVOID FileHandle, UINT32 DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, UINT32 ShareAccess, UINT32 OpenOptions)
{
	return DispatchNt(Djb2::HashCompileTime("ZwOpenFile"), FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions);
}

Result<VOID, Error> NTDLL::RtlDosPathNameToNtPathName_U(const WCHAR *DosName, UNICODE_STRING *NtName, WCHAR **FilePart, PRTL_RELATIVE_NAME_U RelativeName)
{
	BOOL result = ((BOOL(STDCALL *)(const WCHAR *DosName, UNICODE_STRING *NtName, WCHAR **FilePart, PRTL_RELATIVE_NAME_U RelativeName))ResolveNtdllExportAddress("RtlDosPathNameToNtPathName_U"))(DosName, NtName, FilePart, RelativeName);
	if (!result)
	{
		return Result<VOID, Error>::Err(Error(Error::Ntdll_RtlPathResolveFailed));
	}
	return Result<VOID, Error>::Ok();
}

VOID NTDLL::RtlFreeUnicodeString(PUNICODE_STRING UnicodeString)
{
	((VOID(STDCALL *)(PUNICODE_STRING UnicodeString))ResolveNtdllExportAddress("RtlFreeUnicodeString"))(UnicodeString);
}

Result<NTSTATUS, Error> NTDLL::ZwQueryVolumeInformationFile(PVOID FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation, UINT32 Length, UINT32 FsInformationClass)
{
	return DispatchNt(Djb2::HashCompileTime("ZwQueryVolumeInformationFile"), FileHandle, IoStatusBlock, FsInformation, Length, FsInformationClass);
}

Result<NTSTATUS, Error> NTDLL::ZwQueryInformationProcess(PVOID ProcessHandle, UINT32 ProcessInformationClass, PVOID ProcessInformation, UINT32 ProcessInformationLength, PUINT32 ReturnLength)
{
	return DispatchNt(Djb2::HashCompileTime("ZwQueryInformationProcess"), ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength);
}

Result<NTSTATUS, Error> NTDLL::ZwCreateNamedPipeFile(PPVOID FileHandle, UINT32 DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, UINT32 ShareAccess, UINT32 CreateDisposition, UINT32 CreateOptions, UINT32 NamedPipeType, UINT32 ReadMode, UINT32 CompletionMode, UINT32 MaximumInstances, UINT32 InboundQuota, UINT32 OutboundQuota, PLARGE_INTEGER DefaultTimeout)
{
	return DispatchNt(Djb2::HashCompileTime("ZwCreateNamedPipeFile"), FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, CreateDisposition, CreateOptions, NamedPipeType, ReadMode, CompletionMode, MaximumInstances, InboundQuota, OutboundQuota, DefaultTimeout);
}

Result<NTSTATUS, Error> NTDLL::ZwSetInformationObject(PVOID Handle, UINT32 ObjectInformationClass, PVOID ObjectInformation, UINT32 ObjectInformationLength)
{
	return DispatchNt(Djb2::HashCompileTime("ZwSetInformationObject"), Handle, ObjectInformationClass, ObjectInformation, ObjectInformationLength);
}

Result<NTSTATUS, Error> NTDLL::ZwCreateUserProcess(PPVOID ProcessHandle, PPVOID ThreadHandle, UINT32 ProcessDesiredAccess, UINT32 ThreadDesiredAccess, POBJECT_ATTRIBUTES ProcessObjectAttributes, POBJECT_ATTRIBUTES ThreadObjectAttributes, UINT32 ProcessFlags, UINT32 ThreadFlags, PVOID ProcessParameters, PVOID CreateInfo, PVOID AttributeList)
{
	return DispatchNt(Djb2::HashCompileTime("ZwCreateUserProcess"), ProcessHandle, ThreadHandle, ProcessDesiredAccess, ThreadDesiredAccess, ProcessObjectAttributes, ThreadObjectAttributes, ProcessFlags, ThreadFlags, ProcessParameters, CreateInfo, AttributeList);
}

Result<NTSTATUS, Error> NTDLL::RtlCreateProcessParametersEx(PVOID *ProcessParameters, PUNICODE_STRING ImagePathName, PUNICODE_STRING DllPath, PUNICODE_STRING CurrentDirectory, PUNICODE_STRING CommandLine, PVOID Environment, PUNICODE_STRING WindowTitle, PUNICODE_STRING DesktopInfo, PUNICODE_STRING ShellInfo, PUNICODE_STRING RuntimeData, UINT32 Flags)
{
	NTSTATUS status = ((NTSTATUS(STDCALL *)(PVOID * ProcessParameters, PUNICODE_STRING ImagePathName, PUNICODE_STRING DllPath, PUNICODE_STRING CurrentDirectory, PUNICODE_STRING CommandLine, PVOID Environment, PUNICODE_STRING WindowTitle, PUNICODE_STRING DesktopInfo, PUNICODE_STRING ShellInfo, PUNICODE_STRING RuntimeData, UINT32 Flags)) ResolveNtdllExportAddress("RtlCreateProcessParametersEx"))(ProcessParameters, ImagePathName, DllPath, CurrentDirectory, CommandLine, Environment, WindowTitle, DesktopInfo, ShellInfo, RuntimeData, Flags);
	return result::FromNTSTATUS<NTSTATUS>(status);
}

Result<NTSTATUS, Error> NTDLL::RtlDestroyProcessParameters(PVOID ProcessParameters)
{
	NTSTATUS status = ((NTSTATUS(STDCALL *)(PVOID ProcessParameters))ResolveNtdllExportAddress("RtlDestroyProcessParameters"))(ProcessParameters);
	return result::FromNTSTATUS<NTSTATUS>(status);
}

Result<NTSTATUS, Error> NTDLL::ZwQueryDirectoryFile(PVOID FileHandle, PVOID Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, UINT32 Length, UINT32 FileInformationClass, BOOL ReturnSingleEntry, PUNICODE_STRING FileName, BOOL RestartScan)
{
	return DispatchNt(Djb2::HashCompileTime("ZwQueryDirectoryFile"), FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, FileName, RestartScan);
}

Result<NTSTATUS, Error> NTDLL::LdrLoadDll(PWCHAR SearchPath, UINT32 *DllCharacteristics, PUNICODE_STRING DllName, PPVOID BaseAddress)
{
	NTSTATUS status = ((NTSTATUS(STDCALL *)(PWCHAR SearchPath, UINT32 *DllCharacteristics, PUNICODE_STRING DllName, PPVOID BaseAddress))ResolveNtdllExportAddress("LdrLoadDll"))(SearchPath, DllCharacteristics, DllName, BaseAddress);
	return result::FromNTSTATUS<NTSTATUS>(status);
}

Result<NTSTATUS, Error> NTDLL::ZwQuerySystemInformation(UINT32 SystemInformationClass, PVOID SystemInformation, UINT32 SystemInformationLength, PUINT32 ReturnLength)
{
	return DispatchNt(Djb2::HashCompileTime("ZwQuerySystemInformation"), SystemInformationClass, SystemInformation, SystemInformationLength, ReturnLength);
}
