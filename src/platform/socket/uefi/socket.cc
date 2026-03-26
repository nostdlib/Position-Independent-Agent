/**
 * socket.cc - UEFI Socket Implementation using TCP4/TCP6 Protocol
 */

#include "platform/socket/socket.h"
#include "core/memory/memory.h"
#include "platform/kernel/uefi/efi_context.h"
#include "platform/kernel/uefi/efi_tcp4_protocol.h"
#include "platform/kernel/uefi/efi_tcp6_protocol.h"
#include "platform/kernel/uefi/efi_service_binding.h"
#include "platform/kernel/uefi/efi_simple_network_protocol.h"
#include "platform/kernel/uefi/efi_ip4_config2_protocol.h"
#include "platform/console/logger.h"

// =============================================================================
// Internal Socket Context
// =============================================================================

struct UefiSocketContext
{
	EFI_HANDLE TcpHandle;
	EFI_SERVICE_BINDING_PROTOCOL *ServiceBinding;
	EFI_HANDLE ServiceHandle;
	BOOL IsConfigured;
	BOOL IsConnected;
	BOOL IsIPv6;
	union
	{
		EFI_TCP4_PROTOCOL *Tcp4;
		EFI_TCP6_PROTOCOL *Tcp6;
	};
};

// =============================================================================
// Helper Functions
// =============================================================================

static VOID EFIAPI EmptyNotify([[maybe_unused]] EFI_EVENT Event, [[maybe_unused]] PVOID Context)
{
}

[[nodiscard]] static Result<void, Error> InitializeNetworkInterface(EFI_CONTEXT &ctx)
{
	if (ctx.NetworkInitialized)
		return Result<void, Error>::Ok();

	LOG_DEBUG("Socket: InitializeNetworkInterface starting...");

	EFI_BOOT_SERVICES *bs = ctx.SystemTable->BootServices;
	// Initialize GUID field-by-field to avoid .rdata section on aarch64
	// {A19832B9-AC25-11D3-9A2D-0090273FC14D}
	EFI_GUID SnpGuid;
	SnpGuid.Data1 = 0xA19832B9;
	SnpGuid.Data2 = 0xAC25;
	SnpGuid.Data3 = 0x11D3;
	SnpGuid.Data4[0] = 0x9A;
	SnpGuid.Data4[1] = 0x2D;
	SnpGuid.Data4[2] = 0x00;
	SnpGuid.Data4[3] = 0x90;
	SnpGuid.Data4[4] = 0x27;
	SnpGuid.Data4[5] = 0x3F;
	SnpGuid.Data4[6] = 0xC1;
	SnpGuid.Data4[7] = 0x4D;
	USIZE HandleCount = 0;
	EFI_HANDLE *HandleBuffer = nullptr;

	EFI_STATUS lhbStatus = bs->LocateHandleBuffer(ByProtocol, &SnpGuid, nullptr, &HandleCount, &HandleBuffer);
	if (EFI_ERROR_CHECK(lhbStatus))
	{
		LOG_DEBUG("Socket: LocateHandleBuffer failed: 0x%lx", (UINT64)lhbStatus);
		return Result<void, Error>::Err(Error::Uefi((UINT32)lhbStatus), Error::Socket_OpenFailed_Connect);
	}
	if (HandleCount == 0)
	{
		LOG_DEBUG("Socket: no SNP handles found");
		return Result<void, Error>::Err(Error::Socket_OpenFailed_Connect);
	}

	LOG_DEBUG("Socket: Found %u SNP handles", (UINT32)HandleCount);

	for (USIZE i = 0; i < HandleCount; i++)
	{
		EFI_SIMPLE_NETWORK_PROTOCOL *Snp = nullptr;
		if (EFI_ERROR_CHECK(bs->OpenProtocol(HandleBuffer[i], &SnpGuid, (PVOID *)&Snp, ctx.ImageHandle, nullptr, EFI_OPEN_PROTOCOL_GET_PROTOCOL)) || Snp == nullptr)
			continue;

		if (Snp->Mode != nullptr)
		{
			if (Snp->Mode->State == EfiSimpleNetworkStopped)
			{
				LOG_DEBUG("Socket: SNP[%u] Starting...", (UINT32)i);
				Snp->Start(Snp);
			}
			if (Snp->Mode->State == EfiSimpleNetworkStarted)
			{
				LOG_DEBUG("Socket: SNP[%u] Initializing...", (UINT32)i);
				Snp->Initialize(Snp, 0, 0);
			}
			if (Snp->Mode->State == EfiSimpleNetworkInitialized)
			{
				LOG_DEBUG("Socket: SNP[%u] Initialized successfully", (UINT32)i);
				ctx.NetworkInitialized = true;
				break;
			}
		}
	}

	bs->FreePool(HandleBuffer);
	LOG_DEBUG("Socket: InitializeNetworkInterface done, success=%d", (INT32)ctx.NetworkInitialized);
	if (!ctx.NetworkInitialized)
		return Result<void, Error>::Err(Error::Socket_OpenFailed_Connect);
	return Result<void, Error>::Ok();
}

[[nodiscard]] static Result<void, Error> InitializeDhcp(EFI_CONTEXT &ctx)
{
	if (ctx.DhcpConfigured)
		return Result<void, Error>::Ok();

	LOG_DEBUG("Socket: InitializeDhcp starting...");

	EFI_BOOT_SERVICES *bs = ctx.SystemTable->BootServices;
	// Initialize GUID field-by-field to avoid .rdata section on aarch64
	// {5B446ED1-E30B-4FAA-871A-3654ECA36080}
	EFI_GUID Ip4Config2Guid;
	Ip4Config2Guid.Data1 = 0x5B446ED1;
	Ip4Config2Guid.Data2 = 0xE30B;
	Ip4Config2Guid.Data3 = 0x4FAA;
	Ip4Config2Guid.Data4[0] = 0x87;
	Ip4Config2Guid.Data4[1] = 0x1A;
	Ip4Config2Guid.Data4[2] = 0x36;
	Ip4Config2Guid.Data4[3] = 0x54;
	Ip4Config2Guid.Data4[4] = 0xEC;
	Ip4Config2Guid.Data4[5] = 0xA3;
	Ip4Config2Guid.Data4[6] = 0x60;
	Ip4Config2Guid.Data4[7] = 0x80;
	USIZE HandleCount = 0;
	EFI_HANDLE *HandleBuffer = nullptr;

	EFI_STATUS lhbStatus = bs->LocateHandleBuffer(ByProtocol, &Ip4Config2Guid, nullptr, &HandleCount, &HandleBuffer);
	if (EFI_ERROR_CHECK(lhbStatus))
	{
		LOG_DEBUG("Socket: DHCP LocateHandleBuffer failed: 0x%lx", (UINT64)lhbStatus);
		return Result<void, Error>::Err(Error::Uefi((UINT32)lhbStatus), Error::Socket_OpenFailed_Connect);
	}
	if (HandleCount == 0)
	{
		LOG_DEBUG("Socket: no Ip4Config2 handles found");
		return Result<void, Error>::Err(Error::Socket_OpenFailed_Connect);
	}

	LOG_DEBUG("Socket: Found %u Ip4Config2 handles", (UINT32)HandleCount);

	for (USIZE i = 0; i < HandleCount; i++)
	{
		EFI_IP4_CONFIG2_PROTOCOL *Ip4Config2 = nullptr;
		if (EFI_ERROR_CHECK(bs->OpenProtocol(HandleBuffer[i], &Ip4Config2Guid, (PVOID *)&Ip4Config2, ctx.ImageHandle, nullptr, EFI_OPEN_PROTOCOL_GET_PROTOCOL)) || Ip4Config2 == nullptr)
			continue;

		// Check if gateway exists (DHCP already completed)
		USIZE DataSize = 0;
		EFI_STATUS Status = Ip4Config2->GetData(Ip4Config2, Ip4Config2DataTypeGateway, &DataSize, nullptr);
		if (Status == EFI_BUFFER_TOO_SMALL && DataSize >= sizeof(EFI_IPv4_ADDRESS))
		{
			LOG_DEBUG("Socket: DHCP already configured (gateway exists, size=%u)", (UINT32)DataSize);
			ctx.DhcpConfigured = true;
			break;
		}

		// Set DHCP policy
		LOG_DEBUG("Socket: Setting DHCP policy...");
		EFI_IP4_CONFIG2_POLICY Policy = Ip4Config2PolicyDhcp;
		Status = Ip4Config2->SetData(Ip4Config2, Ip4Config2DataTypePolicy, sizeof(Policy), &Policy);
		if (EFI_ERROR_CHECK(Status) && Status != EFI_ALREADY_STARTED)
		{
			LOG_DEBUG("Socket: SetData DHCP policy failed: 0x%lx", (UINT64)Status);
			continue;
		}

		// Wait for DHCP to complete - check for gateway assignment
		LOG_DEBUG("Socket: Waiting for DHCP (up to 5s)...");
		for (UINT32 retry = 0; retry < 50; retry++)
		{
			DataSize = 0;
			Status = Ip4Config2->GetData(Ip4Config2, Ip4Config2DataTypeGateway, &DataSize, nullptr);
			if (Status == EFI_BUFFER_TOO_SMALL && DataSize >= sizeof(EFI_IPv4_ADDRESS))
			{
				LOG_DEBUG("Socket: DHCP completed after %ums", retry * 100);
				ctx.DhcpConfigured = true;
				break;
			}
			bs->Stall(100000); // 100ms
		}

		if (!ctx.DhcpConfigured)
		{
			LOG_DEBUG("Socket: DHCP timeout after 5s, proceeding anyway");
			ctx.DhcpConfigured = true; // Allow TCP to try with whatever config exists
		}
		break;
	}

	bs->FreePool(HandleBuffer);

	// One-time delay for TCP stack readiness on first network init
	if (ctx.DhcpConfigured && !ctx.TcpStackReady)
	{
		LOG_DEBUG("Socket: First connection - waiting 500ms for TCP stack readiness...");
		bs->Stall(500000); // 500ms
		ctx.TcpStackReady = true;
	}

	LOG_DEBUG("Socket: InitializeDhcp done, success=%d", (INT32)ctx.DhcpConfigured);
	if (!ctx.DhcpConfigured)
		return Result<void, Error>::Err(Error::Socket_OpenFailed_Connect);
	return Result<void, Error>::Ok();
}

// Wait for async operation with Poll to drive network stack
template <typename TCP_PROTOCOL>
static EFI_STATUS WaitForCompletion(EFI_BOOT_SERVICES *bs, TCP_PROTOCOL *Tcp, volatile EFI_STATUS &TokenStatus, UINT64 TimeoutMs)
{
	// Check immediately - fast path
	Tcp->Poll(Tcp);
	if (TokenStatus != EFI_NOT_READY)
		return EFI_SUCCESS;

	// Poll loop with short stalls
	for (UINT64 i = 0; i < TimeoutMs; i++)
	{
		Tcp->Poll(Tcp);
		if (TokenStatus != EFI_NOT_READY)
			return EFI_SUCCESS;
		bs->Stall(1000); // 1ms
	}

	return EFI_TIMEOUT;
}

// =============================================================================
// Socket Constructor
// =============================================================================

Result<Socket, Error> Socket::Create(const IPAddress &ipAddress, UINT16 portNum)
{
	LOG_DEBUG("Socket: Create starting for port %u...", (UINT32)portNum);

	EFI_CONTEXT *ctx = GetEfiContext();
	if (ctx == nullptr || ctx->SystemTable == nullptr)
	{
		LOG_DEBUG("Socket: Create failed - no EFI context");
		return Result<Socket, Error>::Err(Error::Socket_CreateFailed_Open);
	}

	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;
	UefiSocketContext *sockCtx = nullptr;

	LOG_DEBUG("Socket: Allocating socket context...");
	EFI_STATUS allocStatus = bs->AllocatePool(EfiLoaderData, sizeof(UefiSocketContext), (PVOID *)&sockCtx);
	if (EFI_ERROR_CHECK(allocStatus))
	{
		LOG_DEBUG("Socket: AllocatePool failed: 0x%lx", (UINT64)allocStatus);
		return Result<Socket, Error>::Err(Error::Uefi((UINT32)allocStatus), Error::Socket_CreateFailed_Open);
	}
	if (sockCtx == nullptr)
	{
		LOG_DEBUG("Socket: AllocatePool returned null");
		return Result<Socket, Error>::Err(Error::Socket_CreateFailed_Open);
	}

	Memory::Zero(sockCtx, sizeof(UefiSocketContext));
	sockCtx->IsIPv6 = ipAddress.IsIPv6();

	// Initialize GUIDs field-by-field to avoid .rdata section
	EFI_GUID ServiceBindingGuid;
	EFI_GUID ProtocolGuid;
	if (sockCtx->IsIPv6)
	{
		// EFI_TCP6_SERVICE_BINDING_PROTOCOL_GUID {EC20EB79-6C1A-4664-9A0D-D2E4CC16D664}
		ServiceBindingGuid.Data1 = 0xEC20EB79;
		ServiceBindingGuid.Data2 = 0x6C1A;
		ServiceBindingGuid.Data3 = 0x4664;
		ServiceBindingGuid.Data4[0] = 0x9A;
		ServiceBindingGuid.Data4[1] = 0x0D;
		ServiceBindingGuid.Data4[2] = 0xD2;
		ServiceBindingGuid.Data4[3] = 0xE4;
		ServiceBindingGuid.Data4[4] = 0xCC;
		ServiceBindingGuid.Data4[5] = 0x16;
		ServiceBindingGuid.Data4[6] = 0xD6;
		ServiceBindingGuid.Data4[7] = 0x64;
		// EFI_TCP6_PROTOCOL_GUID {46E44855-BD60-4AB7-AB0D-A6790824A3F0}
		ProtocolGuid.Data1 = 0x46E44855;
		ProtocolGuid.Data2 = 0xBD60;
		ProtocolGuid.Data3 = 0x4AB7;
		ProtocolGuid.Data4[0] = 0xAB;
		ProtocolGuid.Data4[1] = 0x0D;
		ProtocolGuid.Data4[2] = 0xA6;
		ProtocolGuid.Data4[3] = 0x79;
		ProtocolGuid.Data4[4] = 0x08;
		ProtocolGuid.Data4[5] = 0x24;
		ProtocolGuid.Data4[6] = 0xA3;
		ProtocolGuid.Data4[7] = 0xF0;
	}
	else
	{
		// EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID {00720665-67EB-4A99-BAF7-D3C33A1C7CC9}
		ServiceBindingGuid.Data1 = 0x00720665;
		ServiceBindingGuid.Data2 = 0x67EB;
		ServiceBindingGuid.Data3 = 0x4A99;
		ServiceBindingGuid.Data4[0] = 0xBA;
		ServiceBindingGuid.Data4[1] = 0xF7;
		ServiceBindingGuid.Data4[2] = 0xD3;
		ServiceBindingGuid.Data4[3] = 0xC3;
		ServiceBindingGuid.Data4[4] = 0x3A;
		ServiceBindingGuid.Data4[5] = 0x1C;
		ServiceBindingGuid.Data4[6] = 0x7C;
		ServiceBindingGuid.Data4[7] = 0xC9;
		// EFI_TCP4_PROTOCOL_GUID {65530BC7-A359-410F-B010-5AADC7EC2B62}
		ProtocolGuid.Data1 = 0x65530BC7;
		ProtocolGuid.Data2 = 0xA359;
		ProtocolGuid.Data3 = 0x410F;
		ProtocolGuid.Data4[0] = 0xB0;
		ProtocolGuid.Data4[1] = 0x10;
		ProtocolGuid.Data4[2] = 0x5A;
		ProtocolGuid.Data4[3] = 0xAD;
		ProtocolGuid.Data4[4] = 0xC7;
		ProtocolGuid.Data4[5] = 0xEC;
		ProtocolGuid.Data4[6] = 0x2B;
		ProtocolGuid.Data4[7] = 0x62;
	}

	LOG_DEBUG("Socket: LocateHandleBuffer for TCP%d...", sockCtx->IsIPv6 ? 6 : 4);
	USIZE HandleCount = 0;
	EFI_HANDLE *HandleBuffer = nullptr;
	EFI_STATUS lhbStatus = bs->LocateHandleBuffer(ByProtocol, &ServiceBindingGuid, nullptr, &HandleCount, &HandleBuffer);
	if (EFI_ERROR_CHECK(lhbStatus))
	{
		LOG_DEBUG("Socket: LocateHandleBuffer failed: 0x%lx", (UINT64)lhbStatus);
		if (HandleBuffer != nullptr)
			bs->FreePool(HandleBuffer);
		bs->FreePool(sockCtx);
		return Result<Socket, Error>::Err(Error::Uefi((UINT32)lhbStatus), Error::Socket_CreateFailed_Open);
	}
	if (HandleCount == 0)
	{
		LOG_DEBUG("Socket: no TCP service binding handles found");
		if (HandleBuffer != nullptr)
			bs->FreePool(HandleBuffer);
		bs->FreePool(sockCtx);
		return Result<Socket, Error>::Err(Error::Socket_CreateFailed_Open);
	}

	LOG_DEBUG("Socket: Found %u TCP service binding handles", (UINT32)HandleCount);
	sockCtx->ServiceHandle = HandleBuffer[0];
	EFI_STATUS Status = bs->OpenProtocol(sockCtx->ServiceHandle, &ServiceBindingGuid, (PVOID *)&sockCtx->ServiceBinding, ctx->ImageHandle, nullptr, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	bs->FreePool(HandleBuffer);

	if (EFI_ERROR_CHECK(Status))
	{
		LOG_DEBUG("Socket: OpenProtocol ServiceBinding failed: 0x%lx", (UINT64)Status);
		bs->FreePool(sockCtx);
		return Result<Socket, Error>::Err(
			Error::Uefi((UINT32)Status),
			Error::Socket_CreateFailed_Open);
	}

	LOG_DEBUG("Socket: CreateChild...");
	sockCtx->TcpHandle = nullptr;
	EFI_STATUS ccStatus = sockCtx->ServiceBinding->CreateChild(sockCtx->ServiceBinding, &sockCtx->TcpHandle);
	if (EFI_ERROR_CHECK(ccStatus))
	{
		LOG_DEBUG("Socket: CreateChild failed: 0x%lx", (UINT64)ccStatus);
		bs->CloseProtocol(sockCtx->ServiceHandle, &ServiceBindingGuid, ctx->ImageHandle, nullptr);
		bs->FreePool(sockCtx);
		return Result<Socket, Error>::Err(Error::Uefi((UINT32)ccStatus), Error::Socket_CreateFailed_Open);
	}

	LOG_DEBUG("Socket: OpenProtocol TCP interface...");
	PVOID TcpInterface = nullptr;
	EFI_STATUS opStatus = bs->OpenProtocol(sockCtx->TcpHandle, &ProtocolGuid, &TcpInterface, ctx->ImageHandle, nullptr, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
	if (EFI_ERROR_CHECK(opStatus))
	{
		LOG_DEBUG("Socket: OpenProtocol TCP interface failed: 0x%lx", (UINT64)opStatus);
		sockCtx->ServiceBinding->DestroyChild(sockCtx->ServiceBinding, sockCtx->TcpHandle);
		bs->CloseProtocol(sockCtx->ServiceHandle, &ServiceBindingGuid, ctx->ImageHandle, nullptr);
		bs->FreePool(sockCtx);
		return Result<Socket, Error>::Err(Error::Uefi((UINT32)opStatus), Error::Socket_CreateFailed_Open);
	}

	if (sockCtx->IsIPv6)
		sockCtx->Tcp6 = (EFI_TCP6_PROTOCOL *)TcpInterface;
	else
		sockCtx->Tcp4 = (EFI_TCP4_PROTOCOL *)TcpInterface;

	Socket sock(ipAddress, portNum);
	sock.handle = sockCtx;
	LOG_DEBUG("Socket: Create completed successfully");
	return Result<Socket, Error>::Ok(static_cast<Socket &&>(sock));
}

// =============================================================================
// Open (Connect)
// =============================================================================

Result<void, Error> Socket::Open()
{
	LOG_DEBUG("Socket: Open() starting...");

	UefiSocketContext *sockCtx = (UefiSocketContext *)handle;
	if (sockCtx->IsConnected)
	{
		LOG_DEBUG("Socket: Open() - already connected");
		return Result<void, Error>::Ok();
	}

	EFI_CONTEXT *ctx = GetEfiContext();
	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;

	auto netResult = InitializeNetworkInterface(*ctx);
	if (!netResult)
		return Result<void, Error>::Err(netResult, Error::Socket_OpenFailed_Connect);

	auto dhcpResult = InitializeDhcp(*ctx);
	if (!dhcpResult)
		return Result<void, Error>::Err(dhcpResult, Error::Socket_OpenFailed_Connect);

	LOG_DEBUG("Socket: Creating connect event...");
	EFI_EVENT ConnectEvent;
	EFI_STATUS efiStatus = bs->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, EmptyNotify, nullptr, &ConnectEvent);
	if (EFI_ERROR_CHECK(efiStatus))
	{
		LOG_DEBUG("Socket: CreateEvent failed");
		return Result<void, Error>::Err(
			Error::Uefi((UINT32)efiStatus),
			Error::Socket_OpenFailed_EventCreate);
	}

	EFI_STATUS Status;
	BOOL success = false;

	if (sockCtx->IsIPv6)
	{
		LOG_DEBUG("Socket: Configuring TCP6...");
		EFI_TCP6_CONFIG_DATA ConfigData;
		Memory::Zero(&ConfigData, sizeof(ConfigData));
		ConfigData.HopLimit = 64;
		ConfigData.AccessPoint.ActiveFlag = true;
		ConfigData.AccessPoint.RemotePort = port;
		const UINT8 *ipv6Addr = ip.ToIPv6();
		if (ipv6Addr)
			Memory::Copy(ConfigData.AccessPoint.RemoteAddress.Addr, ipv6Addr, 16);

		efiStatus = sockCtx->Tcp6->Configure(sockCtx->Tcp6, &ConfigData);
		if (EFI_ERROR_CHECK(efiStatus))
		{
			LOG_DEBUG("Socket: TCP6 Configure failed");
			bs->CloseEvent(ConnectEvent);
			return Result<void, Error>::Err(
				Error::Uefi((UINT32)efiStatus),
				Error::Socket_OpenFailed_Connect);
		}
		sockCtx->IsConfigured = true;
		LOG_DEBUG("Socket: TCP6 configured, connecting...");

		EFI_TCP6_CONNECTION_TOKEN ConnectToken;
		Memory::Zero(&ConnectToken, sizeof(ConnectToken));
		ConnectToken.CompletionToken.Event = ConnectEvent;
		ConnectToken.CompletionToken.Status = EFI_NOT_READY;

		Status = sockCtx->Tcp6->Connect(sockCtx->Tcp6, &ConnectToken);
		if (!EFI_ERROR_CHECK(Status) || Status == EFI_NOT_READY)
		{
			Status = WaitForCompletion(bs, sockCtx->Tcp6, ConnectToken.CompletionToken.Status, 5000);
			success = !EFI_ERROR_CHECK(Status) && !EFI_ERROR_CHECK(ConnectToken.CompletionToken.Status);
		}
		else
		{
			LOG_DEBUG("Socket: TCP6 Connect() call failed: 0x%lx", (UINT64)Status);
		}

		if (!success)
		{
			LOG_DEBUG("Socket: TCP6 connection failed, unconfiguring");
			sockCtx->Tcp6->Configure(sockCtx->Tcp6, nullptr);
			sockCtx->IsConfigured = false;
		}
	}
	else
	{
		LOG_DEBUG("Socket: Configuring TCP4...");
		EFI_TCP4_CONFIG_DATA ConfigData;
		Memory::Zero(&ConfigData, sizeof(ConfigData));
		ConfigData.TimeToLive = 64;
		ConfigData.AccessPoint.ActiveFlag = true;
		ConfigData.AccessPoint.UseDefaultAddress = true;
		ConfigData.AccessPoint.RemotePort = port;

		UINT32 ipv4Addr = ip.ToIPv4();
		ConfigData.AccessPoint.RemoteAddress.Addr[0] = (UINT8)(ipv4Addr & 0xFF);
		ConfigData.AccessPoint.RemoteAddress.Addr[1] = (UINT8)((ipv4Addr >> 8) & 0xFF);
		ConfigData.AccessPoint.RemoteAddress.Addr[2] = (UINT8)((ipv4Addr >> 16) & 0xFF);
		ConfigData.AccessPoint.RemoteAddress.Addr[3] = (UINT8)((ipv4Addr >> 24) & 0xFF);

		LOG_DEBUG("Socket: TCP4 remote %u.%u.%u.%u:%u",
				  ConfigData.AccessPoint.RemoteAddress.Addr[0],
				  ConfigData.AccessPoint.RemoteAddress.Addr[1],
				  ConfigData.AccessPoint.RemoteAddress.Addr[2],
				  ConfigData.AccessPoint.RemoteAddress.Addr[3],
				  (UINT32)port);

		Status = sockCtx->Tcp4->Configure(sockCtx->Tcp4, &ConfigData);
		if (EFI_ERROR_CHECK(Status))
		{
			LOG_DEBUG("Socket: TCP4 Configure failed: 0x%lx", (UINT64)Status);
			bs->CloseEvent(ConnectEvent);
			return Result<void, Error>::Err(
				Error::Uefi((UINT32)Status),
				Error::Socket_OpenFailed_Connect);
		}
		sockCtx->IsConfigured = true;
		LOG_DEBUG("Socket: TCP4 configured, connecting...");

		EFI_TCP4_CONNECTION_TOKEN ConnectToken;
		Memory::Zero(&ConnectToken, sizeof(ConnectToken));
		ConnectToken.CompletionToken.Event = ConnectEvent;
		ConnectToken.CompletionToken.Status = EFI_NOT_READY;

		Status = sockCtx->Tcp4->Connect(sockCtx->Tcp4, &ConnectToken);
		if (!EFI_ERROR_CHECK(Status) || Status == EFI_NOT_READY)
		{
			Status = WaitForCompletion(bs, sockCtx->Tcp4, ConnectToken.CompletionToken.Status, 5000);
			success = !EFI_ERROR_CHECK(Status) && !EFI_ERROR_CHECK(ConnectToken.CompletionToken.Status);
		}
		else
		{
			LOG_DEBUG("Socket: TCP4 Connect() call failed: 0x%lx", (UINT64)Status);
		}

		if (!success)
		{
			LOG_DEBUG("Socket: TCP4 connection failed, unconfiguring");
			sockCtx->Tcp4->Configure(sockCtx->Tcp4, nullptr);
			sockCtx->IsConfigured = false;
		}
	}

	bs->CloseEvent(ConnectEvent);
	sockCtx->IsConnected = success;
	LOG_DEBUG("Socket: Open() done, connected=%d", (INT32)success);

	if (!success)
	{
		return Result<void, Error>::Err(
			Error::Socket_OpenFailed_Connect);
	}
	return Result<void, Error>::Ok();
}

// =============================================================================
// Close
// =============================================================================

Result<void, Error> Socket::Close()
{
	LOG_DEBUG("Socket: Close() starting...");

	UefiSocketContext *sockCtx = (UefiSocketContext *)handle;
	EFI_CONTEXT *ctx = GetEfiContext();
	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;

	if (sockCtx->IsIPv6)
	{
		// Cancel any pending I/O
		LOG_DEBUG("Socket: TCP6 Cancel pending I/O...");
		sockCtx->Tcp6->Cancel(sockCtx->Tcp6, nullptr);

		if (sockCtx->IsConnected)
		{
			LOG_DEBUG("Socket: TCP6 closing connection...");
			EFI_EVENT CloseEvent;
			if (!EFI_ERROR_CHECK(bs->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, EmptyNotify, nullptr, &CloseEvent)))
			{
				EFI_TCP6_CLOSE_TOKEN CloseToken;
				Memory::Zero(&CloseToken, sizeof(CloseToken));
				CloseToken.CompletionToken.Event = CloseEvent;
				CloseToken.CompletionToken.Status = EFI_NOT_READY;
				CloseToken.AbortOnClose = true; // Force abort to avoid waiting for remote ACK

				EFI_STATUS Status = sockCtx->Tcp6->Close(sockCtx->Tcp6, &CloseToken);
				if (!EFI_ERROR_CHECK(Status) || Status == EFI_NOT_READY)
					WaitForCompletion(bs, sockCtx->Tcp6, CloseToken.CompletionToken.Status, 100);

				bs->CloseEvent(CloseEvent);
			}
		}

		if (sockCtx->IsConfigured)
		{
			LOG_DEBUG("Socket: TCP6 unconfiguring...");
			[[maybe_unused]] EFI_STATUS cfgStatus = sockCtx->Tcp6->Configure(sockCtx->Tcp6, nullptr);
			LOG_DEBUG("Socket: TCP6 Configure(nullptr) returned 0x%lx", (UINT64)cfgStatus);
		}
	}
	else
	{
		// Cancel any pending I/O
		LOG_DEBUG("Socket: TCP4 Cancel pending I/O...");
		sockCtx->Tcp4->Cancel(sockCtx->Tcp4, nullptr);

		if (sockCtx->IsConnected)
		{
			LOG_DEBUG("Socket: TCP4 closing connection...");
			EFI_EVENT CloseEvent;
			if (!EFI_ERROR_CHECK(bs->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, EmptyNotify, nullptr, &CloseEvent)))
			{
				EFI_TCP4_CLOSE_TOKEN CloseToken;
				Memory::Zero(&CloseToken, sizeof(CloseToken));
				CloseToken.CompletionToken.Event = CloseEvent;
				CloseToken.CompletionToken.Status = EFI_NOT_READY;
				CloseToken.AbortOnClose = true; // Force abort to avoid waiting for remote ACK

				EFI_STATUS Status = sockCtx->Tcp4->Close(sockCtx->Tcp4, &CloseToken);
				if (!EFI_ERROR_CHECK(Status) || Status == EFI_NOT_READY)
					WaitForCompletion(bs, sockCtx->Tcp4, CloseToken.CompletionToken.Status, 100);

				bs->CloseEvent(CloseEvent);
			}
		}

		if (sockCtx->IsConfigured)
		{
			LOG_DEBUG("Socket: TCP4 unconfiguring...");
			[[maybe_unused]] EFI_STATUS cfgStatus = sockCtx->Tcp4->Configure(sockCtx->Tcp4, nullptr);
			LOG_DEBUG("Socket: TCP4 Configure(nullptr) returned 0x%lx", (UINT64)cfgStatus);
		}
	}

	LOG_DEBUG("Socket: CloseProtocol on TcpHandle...");
	// Initialize GUIDs field-by-field to avoid .rdata section
	EFI_GUID ProtocolGuid;
	EFI_GUID ServiceBindingGuid;
	if (sockCtx->IsIPv6)
	{
		// EFI_TCP6_PROTOCOL_GUID {46E44855-BD60-4AB7-AB0D-A6790824A3F0}
		ProtocolGuid.Data1 = 0x46E44855;
		ProtocolGuid.Data2 = 0xBD60;
		ProtocolGuid.Data3 = 0x4AB7;
		ProtocolGuid.Data4[0] = 0xAB;
		ProtocolGuid.Data4[1] = 0x0D;
		ProtocolGuid.Data4[2] = 0xA6;
		ProtocolGuid.Data4[3] = 0x79;
		ProtocolGuid.Data4[4] = 0x08;
		ProtocolGuid.Data4[5] = 0x24;
		ProtocolGuid.Data4[6] = 0xA3;
		ProtocolGuid.Data4[7] = 0xF0;
		// EFI_TCP6_SERVICE_BINDING_PROTOCOL_GUID {EC20EB79-6C1A-4664-9A0D-D2E4CC16D664}
		ServiceBindingGuid.Data1 = 0xEC20EB79;
		ServiceBindingGuid.Data2 = 0x6C1A;
		ServiceBindingGuid.Data3 = 0x4664;
		ServiceBindingGuid.Data4[0] = 0x9A;
		ServiceBindingGuid.Data4[1] = 0x0D;
		ServiceBindingGuid.Data4[2] = 0xD2;
		ServiceBindingGuid.Data4[3] = 0xE4;
		ServiceBindingGuid.Data4[4] = 0xCC;
		ServiceBindingGuid.Data4[5] = 0x16;
		ServiceBindingGuid.Data4[6] = 0xD6;
		ServiceBindingGuid.Data4[7] = 0x64;
	}
	else
	{
		// EFI_TCP4_PROTOCOL_GUID {65530BC7-A359-410F-B010-5AADC7EC2B62}
		ProtocolGuid.Data1 = 0x65530BC7;
		ProtocolGuid.Data2 = 0xA359;
		ProtocolGuid.Data3 = 0x410F;
		ProtocolGuid.Data4[0] = 0xB0;
		ProtocolGuid.Data4[1] = 0x10;
		ProtocolGuid.Data4[2] = 0x5A;
		ProtocolGuid.Data4[3] = 0xAD;
		ProtocolGuid.Data4[4] = 0xC7;
		ProtocolGuid.Data4[5] = 0xEC;
		ProtocolGuid.Data4[6] = 0x2B;
		ProtocolGuid.Data4[7] = 0x62;
		// EFI_TCP4_SERVICE_BINDING_PROTOCOL_GUID {00720665-67EB-4A99-BAF7-D3C33A1C7CC9}
		ServiceBindingGuid.Data1 = 0x00720665;
		ServiceBindingGuid.Data2 = 0x67EB;
		ServiceBindingGuid.Data3 = 0x4A99;
		ServiceBindingGuid.Data4[0] = 0xBA;
		ServiceBindingGuid.Data4[1] = 0xF7;
		ServiceBindingGuid.Data4[2] = 0xD3;
		ServiceBindingGuid.Data4[3] = 0xC3;
		ServiceBindingGuid.Data4[4] = 0x3A;
		ServiceBindingGuid.Data4[5] = 0x1C;
		ServiceBindingGuid.Data4[6] = 0x7C;
		ServiceBindingGuid.Data4[7] = 0xC9;
	}
	[[maybe_unused]] EFI_STATUS closeStatus = bs->CloseProtocol(sockCtx->TcpHandle, &ProtocolGuid, ctx->ImageHandle, nullptr);
	LOG_DEBUG("Socket: CloseProtocol returned 0x%lx", (UINT64)closeStatus);

	LOG_DEBUG("Socket: DestroyChild...");
	[[maybe_unused]] EFI_STATUS destroyStatus = sockCtx->ServiceBinding->DestroyChild(sockCtx->ServiceBinding, sockCtx->TcpHandle);
	LOG_DEBUG("Socket: DestroyChild returned 0x%lx", (UINT64)destroyStatus);

	LOG_DEBUG("Socket: CloseProtocol on ServiceHandle...");
	bs->CloseProtocol(sockCtx->ServiceHandle, &ServiceBindingGuid, ctx->ImageHandle, nullptr);

	LOG_DEBUG("Socket: FreePool...");
	bs->FreePool(sockCtx);
	handle = nullptr;
	LOG_DEBUG("Socket: Close() completed");
	return Result<void, Error>::Ok();
}

// =============================================================================
// Bind (not used on UEFI - TCP protocol handles addressing via Configure)
// =============================================================================

Result<void, Error> Socket::Bind([[maybe_unused]] const SockAddr &socketAddress, [[maybe_unused]] INT32 shareType)
{
	return Result<void, Error>::Err(Error::Socket_BindFailed_Bind);
}

// =============================================================================
// Read
// =============================================================================

Result<SSIZE, Error> Socket::Read(Span<CHAR> buffer)
{
	PVOID bufferPtr = (PVOID)buffer.Data();
	UINT32 bufferLength = (UINT32)buffer.Size();

	LOG_DEBUG("Socket: Read(%u bytes) starting...", bufferLength);

	UefiSocketContext *sockCtx = (UefiSocketContext *)handle;
	if (!sockCtx->IsConnected)
	{
		return Result<SSIZE, Error>::Err(Error::Socket_ReadFailed_Recv);
	}

	EFI_CONTEXT *ctx = GetEfiContext();
	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;

	EFI_EVENT RxEvent;
	EFI_STATUS efiStatus = bs->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, EmptyNotify, nullptr, &RxEvent);
	if (EFI_ERROR_CHECK(efiStatus))
	{
		return Result<SSIZE, Error>::Err(
			Error::Uefi((UINT32)efiStatus),
			Error::Socket_ReadFailed_EventCreate);
	}

	SSIZE bytesRead = -1;

	if (sockCtx->IsIPv6)
	{
		EFI_TCP6_RECEIVE_DATA RxData;
		Memory::Zero(&RxData, sizeof(RxData));
		RxData.DataLength = bufferLength;
		RxData.FragmentCount = 1;
		RxData.FragmentTable[0].FragmentLength = bufferLength;
		RxData.FragmentTable[0].FragmentBuffer = bufferPtr;

		EFI_TCP6_IO_TOKEN RxToken;
		Memory::Zero(&RxToken, sizeof(RxToken));
		RxToken.CompletionToken.Event = RxEvent;
		RxToken.CompletionToken.Status = EFI_NOT_READY;
		RxToken.Packet.RxData = &RxData;

		EFI_STATUS Status = sockCtx->Tcp6->Receive(sockCtx->Tcp6, &RxToken);
		if (!EFI_ERROR_CHECK(Status) || Status == EFI_NOT_READY)
		{
			if (!EFI_ERROR_CHECK(WaitForCompletion(bs, sockCtx->Tcp6, RxToken.CompletionToken.Status, 60000)) &&
			    !EFI_ERROR_CHECK(RxToken.CompletionToken.Status))
				bytesRead = (SSIZE)RxData.DataLength;
		}
		else
		{
			LOG_DEBUG("Socket: TCP6 Receive() call failed: 0x%lx", (UINT64)Status);
		}
	}
	else
	{
		EFI_TCP4_RECEIVE_DATA RxData;
		Memory::Zero(&RxData, sizeof(RxData));
		RxData.DataLength = bufferLength;
		RxData.FragmentCount = 1;
		RxData.FragmentTable[0].FragmentLength = bufferLength;
		RxData.FragmentTable[0].FragmentBuffer = bufferPtr;

		EFI_TCP4_IO_TOKEN RxToken;
		Memory::Zero(&RxToken, sizeof(RxToken));
		RxToken.CompletionToken.Event = RxEvent;
		RxToken.CompletionToken.Status = EFI_NOT_READY;
		RxToken.Packet.RxData = &RxData;

		EFI_STATUS Status = sockCtx->Tcp4->Receive(sockCtx->Tcp4, &RxToken);
		if (!EFI_ERROR_CHECK(Status) || Status == EFI_NOT_READY)
		{
			if (!EFI_ERROR_CHECK(WaitForCompletion(bs, sockCtx->Tcp4, RxToken.CompletionToken.Status, 60000)) &&
			    !EFI_ERROR_CHECK(RxToken.CompletionToken.Status))
				bytesRead = (SSIZE)RxData.DataLength;
		}
		else
		{
			LOG_DEBUG("Socket: TCP4 Receive() call failed: 0x%lx", (UINT64)Status);
		}
	}

	bs->CloseEvent(RxEvent);

	if (bytesRead < 0)
	{
		return Result<SSIZE, Error>::Err(
			Error::Socket_ReadFailed_Recv);
	}

	LOG_DEBUG("Socket: Read() done, bytesRead=%d", (INT32)bytesRead);
	return Result<SSIZE, Error>::Ok(bytesRead);
}

// =============================================================================
// Write
// =============================================================================

Result<UINT32, Error> Socket::Write(Span<const CHAR> buffer)
{
	UINT32 bufferLength = (UINT32)buffer.Size();

	LOG_DEBUG("Socket: Write(%u bytes) starting...", bufferLength);

	UefiSocketContext *sockCtx = (UefiSocketContext *)handle;
	if (!sockCtx->IsConnected)
	{
		LOG_DEBUG("Socket: Write() not connected");
		return Result<UINT32, Error>::Err(Error::Socket_WriteFailed_Send);
	}

	EFI_CONTEXT *ctx = GetEfiContext();
	EFI_BOOT_SERVICES *bs = ctx->SystemTable->BootServices;

	EFI_EVENT TxEvent;
	EFI_STATUS efiStatus = bs->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, EmptyNotify, nullptr, &TxEvent);
	if (EFI_ERROR_CHECK(efiStatus))
	{
		LOG_DEBUG("Socket: Write() CreateEvent failed");
		return Result<UINT32, Error>::Err(
			Error::Uefi((UINT32)efiStatus),
			Error::Socket_WriteFailed_EventCreate);
	}

	UINT32 totalSent = 0;

	while (totalSent < bufferLength)
	{
		PVOID chunkPtr = (PVOID)((const CHAR *)buffer.Data() + totalSent);
		UINT32 chunkLen = bufferLength - totalSent;
		BOOL chunkSent = false;

		if (sockCtx->IsIPv6)
		{
			EFI_TCP6_TRANSMIT_DATA TxData;
			Memory::Zero(&TxData, sizeof(TxData));
			TxData.Push = true;
			TxData.DataLength = chunkLen;
			TxData.FragmentCount = 1;
			TxData.FragmentTable[0].FragmentLength = chunkLen;
			TxData.FragmentTable[0].FragmentBuffer = chunkPtr;

			EFI_TCP6_IO_TOKEN TxToken;
			Memory::Zero(&TxToken, sizeof(TxToken));
			TxToken.CompletionToken.Event = TxEvent;
			TxToken.CompletionToken.Status = EFI_NOT_READY;
			TxToken.Packet.TxData = &TxData;

			EFI_STATUS Status = sockCtx->Tcp6->Transmit(sockCtx->Tcp6, &TxToken);
			if (!EFI_ERROR_CHECK(Status) || Status == EFI_NOT_READY)
			{
				if (!EFI_ERROR_CHECK(WaitForCompletion(bs, sockCtx->Tcp6, TxToken.CompletionToken.Status, 30000)) && !EFI_ERROR_CHECK(TxToken.CompletionToken.Status))
					chunkSent = true;
			}
			else
			{
				LOG_DEBUG("Socket: TCP6 Transmit() call failed: 0x%lx", (UINT64)Status);
			}
		}
		else
		{
			EFI_TCP4_TRANSMIT_DATA TxData;
			Memory::Zero(&TxData, sizeof(TxData));
			TxData.Push = true;
			TxData.DataLength = chunkLen;
			TxData.FragmentCount = 1;
			TxData.FragmentTable[0].FragmentLength = chunkLen;
			TxData.FragmentTable[0].FragmentBuffer = chunkPtr;

			EFI_TCP4_IO_TOKEN TxToken;
			Memory::Zero(&TxToken, sizeof(TxToken));
			TxToken.CompletionToken.Event = TxEvent;
			TxToken.CompletionToken.Status = EFI_NOT_READY;
			TxToken.Packet.TxData = &TxData;

			EFI_STATUS Status = sockCtx->Tcp4->Transmit(sockCtx->Tcp4, &TxToken);
			if (!EFI_ERROR_CHECK(Status) || Status == EFI_NOT_READY)
			{
				if (!EFI_ERROR_CHECK(WaitForCompletion(bs, sockCtx->Tcp4, TxToken.CompletionToken.Status, 30000)) && !EFI_ERROR_CHECK(TxToken.CompletionToken.Status))
					chunkSent = true;
			}
			else
			{
				LOG_DEBUG("Socket: TCP4 Transmit() call failed: 0x%lx", (UINT64)Status);
			}
		}

		if (!chunkSent)
		{
			LOG_DEBUG("Socket: Write() failed after %u bytes", totalSent);
			bs->CloseEvent(TxEvent);
			return Result<UINT32, Error>::Err(
				Error::Socket_WriteFailed_Send);
		}

		totalSent += chunkLen;
	}

	bs->CloseEvent(TxEvent);
	LOG_DEBUG("Socket: Write() done, bytesSent=%u", totalSent);
	return Result<UINT32, Error>::Ok(totalSent);
}
