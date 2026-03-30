#include "platform/socket/socket.h"
#include "core/types/primitives.h"
#include "platform/kernel/windows/windows_types.h"
#include "platform/console/logger.h"
#include "platform/kernel/windows/ntdll.h"
#include "core/memory/memory.h"

constexpr UINT32 IOCTL_AFD_BIND       = (0x00000012 << 12) | (0  << 2) | 3;
constexpr UINT32 IOCTL_AFD_CONNECT    = (0x00000012 << 12) | (1  << 2) | 3;
constexpr UINT32 IOCTL_AFD_SEND       = (0x00000012 << 12) | (7  << 2) | 3;
constexpr UINT32 IOCTL_AFD_RECV       = (0x00000012 << 12) | (5  << 2) | 3;

constexpr UINT32 AFD_SHARE_REUSE         = 0x1;

constexpr UINT32 OBJ_INHERIT    = 0x00000002;
constexpr UINT32 STATUS_PENDING = 0x00000103;
constexpr UINT32 STATUS_TIMEOUT = 0x00000102;
constexpr INT32  IPPROTO_TCP    = 6;

typedef struct _AfdBindData
{
	UINT32  ShareType;
	SockAddr Address;
} AfdBindData;

typedef struct _AfdBindData6
{
	UINT32   ShareType;
	SockAddr6 Address;
} AfdBindData6;

typedef struct _AfdConnectInfo
{
	SSIZE    UseSAN;
	SSIZE    Root;
	SSIZE    Unknown;
	SockAddr Address;
} AfdConnectInfo;

typedef struct _AfdConnectInfo6
{
	SSIZE     UseSAN;
	SSIZE     Root;
	SSIZE     Unknown;
	SockAddr6 Address;
} AfdConnectInfo6;

typedef struct _AfdWsaBuf
{
	UINT32 Length;
	PVOID  Buffer;
} AfdWsaBuf;

typedef struct _AfdSendRecvInfo
{
	PVOID  BufferArray;
	UINT32 BufferCount;
	UINT32 AfdFlags;
	UINT32 TdiFlags;
} AfdSendRecvInfo;

typedef struct _AfdSocketParams
{
	INT32  Reserved;
	UINT16 EaNameLength;
	UINT16 EaValueLength;
	CHAR   AfdOperation[16];
	UINT32 Flags;
	INT32  GroupId;
	INT32  AddressFamily;
	INT32  SocketType;
	INT32  Protocol;
	UINT32 ProviderInfoLength;
	WCHAR  ProviderInfo[8];
} AfdSocketParams;

// Wait for a pending AFD IOCTL to complete via the signaled event.
// On completion, reads IOSB.Status back into Status.
// Returns STATUS_TIMEOUT if timed out (Status is NOT updated in that case).
// Returns the wait status on success; if ZwWaitForSingleObject itself fails,
// propagates its failure NTSTATUS so callers see a non-timeout, non-success result.
[[nodiscard]] static Result<NTSTATUS, Error> AfdWait(PVOID SockEvent, PIO_STATUS_BLOCK IOSB, NTSTATUS *Status, LARGE_INTEGER *Timeout)
{
	auto waitResult = NTDLL::ZwWaitForSingleObject(SockEvent, 0, Timeout);
	if (!waitResult)
	{
		// ZwWaitForSingleObject failed — propagate its NTSTATUS so the
		// subsequent !NT_SUCCESS(Status) check in the caller catches it.
		return Result<NTSTATUS, Error>::Err(waitResult, Error::Socket_WaitFailed);
	}
	const auto& waitStatus = waitResult.Value();
	if (waitStatus != (NTSTATUS)STATUS_TIMEOUT)
		*Status = IOSB->Status;
	return Result<NTSTATUS, Error>::Ok(waitStatus);
}

Result<VOID, Error> Socket::Bind(const SockAddr &socketAddress, INT32 shareType)
{
	LOG_DEBUG("Bind(handle: 0x%p, family: %d, shareType: %d)\n", handle, socketAddress.SinFamily, shareType);

	PVOID SockEvent = nullptr;
	auto evtResult = NTDLL::ZwCreateEvent(&SockEvent, EVENT_ALL_ACCESS, nullptr, SynchronizationEvent, false);
	if (!evtResult)
		return Result<VOID, Error>::Err(evtResult, Error::Socket_BindFailed_EventCreate);

	IO_STATUS_BLOCK IOSB;
	Memory::Zero(&IOSB, sizeof(IOSB));

	UINT8 OutputBlock[40];
	Memory::Zero(&OutputBlock, sizeof(OutputBlock));

	NTSTATUS Status;
	if (socketAddress.SinFamily == AF_INET6)
	{
		AfdBindData6 BindConfig;
		Memory::Zero(&BindConfig, sizeof(BindConfig));
		BindConfig.ShareType = shareType;
		BindConfig.Address   = (const SockAddr6 &)socketAddress;

		auto ioResult = NTDLL::ZwDeviceIoControlFile(handle, SockEvent, nullptr, nullptr, &IOSB,
		                                             IOCTL_AFD_BIND,
		                                             &BindConfig, sizeof(BindConfig),
		                                             &OutputBlock, sizeof(OutputBlock));
		if (!ioResult)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<VOID, Error>::Err(ioResult, Error::Socket_BindFailed_Bind);
		}
		Status = ioResult.Value();
	}
	else
	{
		AfdBindData BindConfig;
		Memory::Zero(&BindConfig, sizeof(BindConfig));
		BindConfig.ShareType = shareType;
		BindConfig.Address   = socketAddress;

		auto ioResult = NTDLL::ZwDeviceIoControlFile(handle, SockEvent, nullptr, nullptr, &IOSB,
		                                             IOCTL_AFD_BIND,
		                                             &BindConfig, sizeof(BindConfig),
		                                             &OutputBlock, sizeof(OutputBlock));
		if (!ioResult)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<VOID, Error>::Err(ioResult, Error::Socket_BindFailed_Bind);
		}
		Status = ioResult.Value();
	}

	if (Status == (NTSTATUS)STATUS_PENDING)
	{
		auto waitResult = AfdWait(SockEvent, &IOSB, &Status, nullptr);
		if (!waitResult)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<VOID, Error>::Err(waitResult, Error::Socket_BindFailed_Bind);
		}
	}

	(VOID)NTDLL::ZwClose(SockEvent);

	if (!NT_SUCCESS(Status))
	{
		return Result<VOID, Error>::Err(
			Error::Windows((UINT32)Status),
			Error::Socket_BindFailed_Bind);
	}

	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Socket::Open()
{
	LOG_DEBUG("Open(handle: 0x%p, port: %d)\n", handle, port);

	// AFD requires an explicit bind to a wildcard local address before connect
	union
	{
		SockAddr  addr4;
		SockAddr6 addr6;
	} bindBuffer;

	SocketAddressHelper::PrepareBindAddress(ip.IsIPv6(), 0, Span<UINT8>((UINT8 *)&bindBuffer, sizeof(bindBuffer)));

	auto bindResult = Bind((SockAddr &)bindBuffer, AFD_SHARE_REUSE);
	if (!bindResult)
		return bindResult; // propagates BindFailed_* with its NTSTATUS

	PVOID SockEvent = nullptr;
	auto evtResult = NTDLL::ZwCreateEvent(&SockEvent, EVENT_ALL_ACCESS, nullptr, SynchronizationEvent, false);
	if (!evtResult)
		return Result<VOID, Error>::Err(evtResult, Error::Socket_OpenFailed_EventCreate);

	IO_STATUS_BLOCK IOSB;
	Memory::Zero(&IOSB, sizeof(IOSB));

	union
	{
		SockAddr  addr4;
		SockAddr6 addr6;
	} addrBuffer;

	SocketAddressHelper::PrepareAddress(ip, port, Span<UINT8>((UINT8 *)&addrBuffer, sizeof(addrBuffer)));

	NTSTATUS Status;
	if (ip.IsIPv6())
	{
		AfdConnectInfo6 ConnectInfo;
		Memory::Zero(&ConnectInfo, sizeof(ConnectInfo));
		ConnectInfo.Address = addrBuffer.addr6;

		auto ioResult = NTDLL::ZwDeviceIoControlFile(handle, SockEvent, nullptr, nullptr, &IOSB,
		                                             IOCTL_AFD_CONNECT,
		                                             &ConnectInfo, sizeof(ConnectInfo),
		                                             nullptr, 0);
		if (!ioResult)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<VOID, Error>::Err(ioResult, Error::Socket_OpenFailed_Connect);
		}
		Status = ioResult.Value();
	}
	else
	{
		AfdConnectInfo ConnectInfo;
		Memory::Zero(&ConnectInfo, sizeof(ConnectInfo));
		ConnectInfo.Address = addrBuffer.addr4;

		auto ioResult = NTDLL::ZwDeviceIoControlFile(handle, SockEvent, nullptr, nullptr, &IOSB,
		                                             IOCTL_AFD_CONNECT,
		                                             &ConnectInfo, sizeof(ConnectInfo),
		                                             nullptr, 0);
		if (!ioResult)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<VOID, Error>::Err(ioResult, Error::Socket_OpenFailed_Connect);
		}
		Status = ioResult.Value();
	}

	if (Status == (NTSTATUS)STATUS_PENDING)
	{
		// 5-second connect timeout (100-ns units, negative = relative)
		LARGE_INTEGER ConnectTimeout;
		ConnectTimeout.QuadPart = -5LL * 1000 * 10000;

		auto waitResult = AfdWait(SockEvent, &IOSB, &Status, &ConnectTimeout);
		if (!waitResult)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<VOID, Error>::Err(waitResult, Error::Socket_OpenFailed_Connect);
		}
		if (waitResult.Value() == (NTSTATUS)STATUS_TIMEOUT)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<VOID, Error>::Err(
				Error::Windows((UINT32)STATUS_TIMEOUT),
				Error::Socket_OpenFailed_Connect);
		}
	}

	(VOID)NTDLL::ZwClose(SockEvent);

	if (!NT_SUCCESS(Status))
	{
		return Result<VOID, Error>::Err(
			Error::Windows((UINT32)Status),
			Error::Socket_OpenFailed_Connect);
	}

	LOG_DEBUG("Open: connected successfully\n");
	return Result<VOID, Error>::Ok();
}

Result<VOID, Error> Socket::Close()
{
	LOG_DEBUG("Close(handle: 0x%p)\n", handle);

	auto closeResult = NTDLL::ZwClose(handle);
	handle = nullptr;

	if (!closeResult)
		return Result<VOID, Error>::Err(closeResult, Error::Socket_CloseFailed_Close);

	return Result<VOID, Error>::Ok();
}

Result<SSIZE, Error> Socket::Read(Span<CHAR> buffer)
{
	LOG_DEBUG("Read(handle: 0x%p, bufferSize: %d)\n", handle, (UINT32)buffer.Size());

	PVOID SockEvent = nullptr;
	auto evtResult = NTDLL::ZwCreateEvent(&SockEvent, EVENT_ALL_ACCESS, nullptr, SynchronizationEvent, false);
	if (!evtResult)
		return Result<SSIZE, Error>::Err(evtResult, Error::Socket_ReadFailed_EventCreate);

	AfdWsaBuf RecvBuffer;
	RecvBuffer.Length = (UINT32)buffer.Size();
	RecvBuffer.Buffer = (PVOID)buffer.Data();

	AfdSendRecvInfo RecvInfo;
	Memory::Zero(&RecvInfo, sizeof(RecvInfo));
	RecvInfo.BufferArray = &RecvBuffer;
	RecvInfo.BufferCount = 1;
	RecvInfo.TdiFlags    = 0x20;

	IO_STATUS_BLOCK IOSB;
	Memory::Zero(&IOSB, sizeof(IOSB));

	auto ioResult = NTDLL::ZwDeviceIoControlFile(handle, SockEvent, nullptr, nullptr, &IOSB,
	                                             IOCTL_AFD_RECV,
	                                             &RecvInfo, sizeof(RecvInfo),
	                                             nullptr, 0);
	if (!ioResult)
	{
		(VOID)NTDLL::ZwClose(SockEvent);
		return Result<SSIZE, Error>::Err(ioResult, Error::Socket_ReadFailed_Recv);
	}

	NTSTATUS Status = ioResult.Value();
	if (Status == (NTSTATUS)STATUS_PENDING)
	{
		// 5-minute receive timeout (100-ns units, negative = relative to now)
		LARGE_INTEGER Timeout;
		Timeout.QuadPart = 5 * 60 * 1000 * -10000LL;

		auto waitResult = AfdWait(SockEvent, &IOSB, &Status, &Timeout);
		if (!waitResult)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<SSIZE, Error>::Err(waitResult, Error::Socket_ReadFailed_Recv);
		}
		if (waitResult.Value() == (NTSTATUS)STATUS_TIMEOUT)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<SSIZE, Error>::Err(
				Error::Windows((UINT32)STATUS_TIMEOUT),
				Error::Socket_ReadFailed_Timeout);
		}
	}

	(VOID)NTDLL::ZwClose(SockEvent);

	if (!NT_SUCCESS(Status))
	{
		return Result<SSIZE, Error>::Err(
			Error::Windows((UINT32)Status),
			Error::Socket_ReadFailed_Recv);
	}

	return Result<SSIZE, Error>::Ok((SSIZE)IOSB.Information);
}

Result<UINT32, Error> Socket::Write(Span<const CHAR> buffer)
{
	LOG_DEBUG("Write(handle: 0x%p, length: %d)\n", handle, (UINT32)buffer.Size());

	PVOID SockEvent = nullptr;
	auto evtResult = NTDLL::ZwCreateEvent(&SockEvent, EVENT_ALL_ACCESS, nullptr, SynchronizationEvent, false);
	if (!evtResult)
		return Result<UINT32, Error>::Err(evtResult, Error::Socket_WriteFailed_EventCreate);

	AfdSendRecvInfo SendInfo;
	Memory::Zero(&SendInfo, sizeof(SendInfo));
	SendInfo.BufferCount = 1;

	AfdWsaBuf      SendBuffer;
	IO_STATUS_BLOCK IOSB;
	UINT32          totalSent = 0;

	do
	{
		Memory::Zero(&IOSB, sizeof(IOSB));
		SendBuffer.Buffer    = (PVOID)((PCHAR)buffer.Data() + totalSent);
		SendBuffer.Length    = (UINT32)buffer.Size() - totalSent;
		SendInfo.BufferArray = &SendBuffer;

		auto ioResult = NTDLL::ZwDeviceIoControlFile(handle, SockEvent, nullptr, nullptr, &IOSB,
		                                             IOCTL_AFD_SEND,
		                                             &SendInfo, sizeof(SendInfo),
		                                             nullptr, 0);
		if (!ioResult)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<UINT32, Error>::Err(ioResult, Error::Socket_WriteFailed_Send);
		}

		NTSTATUS Status = ioResult.Value();
		if (Status == (NTSTATUS)STATUS_PENDING)
		{
			// 1-minute send timeout
			LARGE_INTEGER Timeout;
			Timeout.QuadPart = 1 * 60 * 1000 * -10000LL;

			auto waitResult = AfdWait(SockEvent, &IOSB, &Status, &Timeout);
			if (!waitResult)
			{
				(VOID)NTDLL::ZwClose(SockEvent);
				return Result<UINT32, Error>::Err(waitResult, Error::Socket_WriteFailed_Send);
			}
			if (waitResult.Value() == (NTSTATUS)STATUS_TIMEOUT)
			{
				(VOID)NTDLL::ZwClose(SockEvent);
				return Result<UINT32, Error>::Err(
					Error::Windows((UINT32)STATUS_TIMEOUT),
					Error::Socket_WriteFailed_Timeout);
			}
		}

		if (!NT_SUCCESS(Status))
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<UINT32, Error>::Err(
				Error::Windows((UINT32)Status),
				Error::Socket_WriteFailed_Send);
		}

		totalSent += (UINT32)IOSB.Information;
		if (IOSB.Information == 0)
		{
			(VOID)NTDLL::ZwClose(SockEvent);
			return Result<UINT32, Error>::Err(Error::Socket_WriteFailed_Send);
		}
	} while (totalSent < (UINT32)buffer.Size());

	(VOID)NTDLL::ZwClose(SockEvent);
	LOG_DEBUG("Write: sent %d bytes\n", totalSent);
	return Result<UINT32, Error>::Ok(totalSent);
}

Result<Socket, Error> Socket::Create(const IPAddress &ipAddress, UINT16 port)
{
	Socket sock(ipAddress, port);

	LOG_DEBUG("Create(sock: 0x%p)\n", &sock);

	AfdSocketParams EaBuffer;
	Memory::Zero(&EaBuffer, sizeof(EaBuffer));
	EaBuffer.EaNameLength  = 0x0F1E;
	EaBuffer.EaValueLength = 0x001E;

	auto afdOpSource = "AfdOpenPacketXX";
	Memory::Copy(EaBuffer.AfdOperation, afdOpSource, 16);
	EaBuffer.AfdOperation[15] = '\0';

	EaBuffer.AddressFamily = SocketAddressHelper::GetAddressFamily(sock.ip);
	EaBuffer.SocketType    = SOCK_STREAM;
	EaBuffer.Protocol      = IPPROTO_TCP;

	UNICODE_STRING AfdName;
	const WCHAR afdNameSource[]  = L"\\Device\\Afd\\Endpoint";
	AfdName.Buffer      = (PWCHAR)afdNameSource;
	AfdName.Length      = (sizeof(afdNameSource) / sizeof(WCHAR) - 1) * sizeof(WCHAR);
	AfdName.MaximumLength = sizeof(afdNameSource);

	OBJECT_ATTRIBUTES   Object;
	IO_STATUS_BLOCK     IOSB;
	Memory::Zero(&IOSB, sizeof(IOSB));
	InitializeObjectAttributes(&Object, &AfdName, OBJ_CASE_INSENSITIVE | OBJ_INHERIT, 0, 0);

	auto createResult = NTDLL::ZwCreateFile(&sock.handle,
	                                        GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
	                                        &Object,
	                                        &IOSB,
	                                        nullptr,
	                                        0,
	                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
	                                        FILE_OPEN_IF,
	                                        0,
	                                        &EaBuffer,
	                                        sizeof(EaBuffer));
	if (!createResult)
	{
		LOG_DEBUG("Create: ZwCreateFile failed: errors=%e\n", createResult.Error());
		return Result<Socket, Error>::Err(createResult, Error::Socket_CreateFailed_Open);
	}

	LOG_DEBUG("Create: handle: 0x%p\n", sock.handle);
	return Result<Socket, Error>::Ok(static_cast<Socket &&>(sock));
}
