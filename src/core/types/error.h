/**
 * @file error.h
 * @brief Unified Error Type with Chain for Result-Based Error Handling
 *
 * @details Provides an error representation used by the `Result<T, Error>`
 * type throughout the codebase. Each Error has a top-level (Code, Platform)
 * pair plus a fixed-capacity chain of inner (cause) errors, preserving
 * the full error propagation path from root cause to failure site.
 *
 * Design principles:
 * - Fixed-size: no heap allocation, stored directly in Result
 * - Full chain: up to MaxDepth errors preserved (outermost + inner causes)
 * - Platform-aware: factory methods tag errors with their OS origin for formatting
 * - Backward-compatible: Code and Platform fields remain the outermost error
 *
 * @see result.h — Result<T, Error> tagged union that stores Error on failure
 *
 * @ingroup core
 *
 * @defgroup error Error Type
 * @ingroup core
 * @{
 */

#pragma once

#include "core/types/primitives.h"

/**
 * @struct Error
 * @brief Unified error with a fixed-capacity cause chain
 *
 * @details The top-level Code/Platform identifies the outermost failure site.
 * The inner arrays hold cause errors from most-recent-inner to root-cause,
 * preserving the full propagation path.
 *
 * When Depth == 0, there is no chain (single error, equivalent to old behavior).
 * When Depth > 0, Inner[0..Depth-1] holds the cause chain.
 *
 * @par Example Usage:
 * @code
 * // Single runtime error:
 * return Result<void, Error>::Err(Error::Socket_CreateFailed_Open);
 *
 * // Chained: OS error wrapped by runtime code:
 * return Result<void, Error>::Err(osResult, Error::Socket_CreateFailed_Open);
 * // Result: Code=Socket_CreateFailed_Open, Inner[0]=NTSTATUS value
 *
 * // Multi-level chain:
 * return Result<void, Error>::Err(socketResult, Error::Tls_OpenFailed_Socket);
 * // Result: Code=Tls_OpenFailed_Socket, Inner[0]=Socket_*, Inner[1]=NTSTATUS
 * @endcode
 */
struct Error
{
	/**
	 * @enum ErrorCodes
	 * @brief PIR runtime failure points — one unique value per failure site
	 *
	 * @details OS error codes (NTSTATUS, errno, EFI_STATUS) are stored directly
	 * in Error.Code when Platform != Runtime; they are not listed here.
	 */
	enum ErrorCodes : UINT32
	{
		None = 0, // no error / empty slot

		// -------------------------
		// Socket errors (1–3, 5–7, 9–11, 13–15, 39)
		// -------------------------
		Socket_CreateFailed_Open = 1,		 // ZwCreateFile / socket() failed
		Socket_BindFailed_EventCreate = 2,	 // ZwCreateEvent failed (Windows only)
		Socket_BindFailed_Bind = 3,			 // AFD_BIND / bind() syscall failed
		Socket_OpenFailed_EventCreate = 5,	 // ZwCreateEvent failed (Windows only)
		Socket_OpenFailed_Connect = 6,		 // AFD_CONNECT / connect() syscall failed
		Socket_CloseFailed_Close = 7,		 // ZwClose / close() failed
		Socket_ReadFailed_EventCreate = 9,	 // ZwCreateEvent failed (Windows only)
		Socket_ReadFailed_Timeout = 10,		 // receive timed out
		Socket_ReadFailed_Recv = 11,		 // AFD_RECV / recv() syscall failed
		Socket_WriteFailed_EventCreate = 13, // ZwCreateEvent failed (Windows only)
		Socket_WriteFailed_Timeout = 14,	 // send timed out
		Socket_WriteFailed_Send = 15,		 // AFD_SEND / send() syscall failed
		Socket_WaitFailed = 39,				 // ZwWaitForSingleObject failed (Windows only)

		// -------------------------
		// TLS errors (16–22)
		// -------------------------
		Tls_OpenFailed_Socket = 16,	   // underlying socket Open() failed
		Tls_OpenFailed_Handshake = 17, // TLS handshake failed
		Tls_CloseFailed_Socket = 18,   // underlying socket Close() failed
		Tls_ReadFailed_NotReady = 19,  // connection not established
		Tls_ReadFailed_Receive = 20,   // ProcessReceive() failed
		Tls_WriteFailed_NotReady = 21, // connection not established
		Tls_WriteFailed_Send = 22,	   // SendPacket() failed

		// -------------------------
		// WebSocket errors (23–32)
		// -------------------------
		Ws_TransportFailed = 23,  // TLS/socket transport open failed
		Ws_DnsFailed = 24,		  // DNS resolution failed
		Ws_HandshakeFailed = 25,  // HTTP 101 upgrade handshake failed
		Ws_WriteFailed = 26,	  // frame write to transport failed
		Ws_NotConnected = 27,	  // operation attempted on closed connection
		Ws_AllocFailed = 28,	  // memory allocation failed
		Ws_ReceiveFailed = 29,	  // frame receive failed
		Ws_ConnectionClosed = 30, // server sent CLOSE frame
		Ws_InvalidFrame = 31,	  // received frame with invalid RSV bits or opcode
		Ws_FrameTooLarge = 32,	  // received frame exceeds size limit

		// -------------------------
		// DNS errors (33–38)
		// -------------------------
		Dns_ConnectFailed = 33,	 // TLS connection to DNS server failed
		Dns_QueryFailed = 34,	 // DNS query generation failed
		Dns_SendFailed = 35,	 // failed to send DNS query
		Dns_ResponseFailed = 36, // DNS server returned non-200 or bad content-length
		Dns_ParseFailed = 37,	 // failed to parse DNS binary response
		Dns_ResolveFailed = 38,	 // all DNS servers/fallbacks exhausted

		// -------------------------
		// HTTP errors (40–48)
		// -------------------------
		Http_OpenFailed = 40,				// TLS connection open failed
		Http_CloseFailed = 41,				// TLS connection close failed
		Http_ReadFailed = 42,				// TLS read failed
		Http_WriteFailed = 43,				// TLS write failed
		Http_SendGetFailed = 44,			// GET request write failed
		Http_SendPostFailed = 45,			// POST request write failed
		Http_ReadHeadersFailed_Read = 46,	// header read failed
		Http_ReadHeadersFailed_Status = 47, // unexpected HTTP status code
		Http_ParseUrlFailed = 48,			// URL format invalid

		// -------------------------
		// FileSystem errors (50–57)
		// -------------------------
		Fs_OpenFailed = 50,		   // file open syscall failed
		Fs_DeleteFailed = 51,	   // file delete syscall failed
		Fs_ReadFailed = 52,		   // file read syscall failed
		Fs_WriteFailed = 53,	   // file write syscall failed
		Fs_CreateDirFailed = 54,   // directory create syscall failed
		Fs_DeleteDirFailed = 55,   // directory delete syscall failed
		Fs_PathResolveFailed = 56, // path name resolution failed
		Fs_SeekFailed = 57,		   // file seek/offset syscall failed

		// -------------------------
		// Crypto errors (60–63)
		// -------------------------
		Ecc_InitFailed = 60,			 // curve not recognized or random gen failed
		Ecc_ExportKeyFailed = 61,		 // null buffer or insufficient size
		Ecc_SharedSecretFailed = 62,	 // invalid key format or point at infinity
		ChaCha20_DecodeFailed = 63,		 // Poly1305 authentication failed
		ChaCha20_GenerateKeyFailed = 64, // invalid nonce size in Poly1305 key generation
		ChaCha20_KeySetupFailed = 65,	 // invalid key size (must be 128 or 256 bits)

		// -------------------------
		// TlsCipher errors (70–73)
		// -------------------------
		TlsCipher_ComputePublicKeyFailed = 70, // ECC key generation failed
		TlsCipher_ComputePreKeyFailed = 71,	   // premaster key computation failed
		TlsCipher_ComputeKeyFailed = 72,	   // key derivation failed
		TlsCipher_DecodeFailed = 73,		   // record decryption failed
		TlsCipher_ComputeVerifyFailed = 86,	   // verify data computation failed

		// -------------------------
		// TlsBuffer errors (87)
		// -------------------------
		TlsBuffer_AllocationFailed = 87, // buffer growth allocation failed

		// -------------------------
		// TLS internal errors (74–84)
		// -------------------------
		Tls_SendPacketFailed = 74,		 // packet send to socket failed
		Tls_ClientHelloFailed = 75,		 // ClientHello send failed
		Tls_ServerHelloFailed = 76,		 // ServerHello processing failed
		Tls_ServerHelloDoneFailed = 77,	 // ServerHelloDone processing failed
		Tls_ServerFinishedFailed = 78,	 // ServerFinished processing failed
		Tls_VerifyFinishedFailed = 79,	 // Finished verification failed
		Tls_ClientExchangeFailed = 80,	 // ClientKeyExchange send failed
		Tls_ClientFinishedFailed = 81,	 // ClientFinished send failed
		Tls_ChangeCipherSpecFailed = 82, // ChangeCipherSpec send failed
		Tls_ProcessReceiveFailed = 83,	 // receive processing failed
		Tls_OnPacketFailed = 84,		 // packet handling failed
		Tls_ReadFailed_Channel = 85,	 // ReadChannel returned 0 bytes

		// -------------------------
		// Process errors (90–92)
		// -------------------------
		Process_CreateFailed = 90,	  // process creation failed
		Process_WaitFailed = 91,	  // waiting for process failed
		Process_TerminateFailed = 92, // process termination failed
		Process_NotSupported = 93,	  // process not supported on this platform

		Pipe_ReadFailed = 112,	    // pipe read failed
		Pipe_WriteFailed = 113,	    // pipe write failed
		Pipe_CreateFailed = 114,    // pipe creation failed
		Pipe_NotSupported = 115,    // pipe not supported on this platform
		// -------------------------
		// Misc errors (95–101)
		// -------------------------
		Base64_DecodeFailed = 95,			// Base64 decoding failed
		String_ParseIntFailed = 96,			// integer parsing failed
		String_ParseFloatFailed = 97,		// float parsing failed
		IpAddress_ToStringFailed = 98,		// buffer too small for IP string
		IpAddress_ParseFailed = 105,		// IP address string parsing failed
		Uuid_ToStringFailed = 106,			// buffer too small for UUID string
		Uuid_FromStringFailed = 107,		// UUID string parsing failed
		Kernel32_CreateProcessFailed = 99,	// CreateProcessW failed
		Kernel32_SetHandleInfoFailed = 100, // SetHandleInformation failed
		Ntdll_RtlPathResolveFailed = 101,	// RtlDosPathNameToNtPathName_U failed
		Kernel32_CreatePipeFailed = 108,	// CreatePipe failed
		Kernel32_PeekNamedPipeFailed = 109, // PeekNamedPipe failed

		// -------------------------
		// Factory creation errors (102–104)
		// -------------------------
		Tls_CreateFailed = 102,	 // Socket::Create() failed in TlsClient::Create()
		Http_CreateFailed = 103, // URL parse / DNS / TLS create failed in HttpClient::Create()
		Ws_CreateFailed = 104,	 // URL parse / DNS / TLS create failed in WebSocketClient::Create()

		// -------------------------
		// JPEG encoder errors (110)
		// -------------------------
		Jpeg_InvalidParams = 110, // invalid image dimensions or component count

		// -------------------------
		// Image processing errors (111)
		// -------------------------
		Image_AllocationFailed = 111, // memory allocation failed during contour finding

		// -------------------------
		// Screen errors (120–122)
		// -------------------------
		Screen_GetDevicesFailed = 120, // display enumeration failed
		Screen_CaptureFailed = 121,	   // screen capture failed
		Screen_AllocFailed = 122,	   // memory allocation failed

		// -------------------------
		// PTY errors (130–132)
		// -------------------------
		Pty_CreateFailed = 130,	  // PTY creation failed
		Pty_ReadFailed = 131,	  // PTY read failed
		Pty_WriteFailed = 132,	  // PTY write failed
		Pty_NotSupported = 133,	  // PTY not supported on this platform

		// -------------------------
		// ShellProcess errors (135–137)
		// -------------------------
		ShellProcess_CreateFailed = 135, // shell process creation failed
		ShellProcess_NotSupported = 136, // shell not supported on this platform
		ShellProcess_ReadFailed = 137,	  // shell read failed
	};

	/**
	 * @enum PlatformKind
	 * @brief Identifies which OS layer produced the error code
	 *
	 * @details When Platform != Runtime, Code holds the raw OS error value
	 * rather than an ErrorCodes enumerator. The platform tag drives
	 * formatting in %e (hex for Windows/UEFI, decimal for Posix).
	 */
	enum class PlatformKind : UINT8
	{
		Runtime = 0, ///< PIR runtime layer — Code is an ErrorCodes enumerator
		Windows = 1, ///< NTSTATUS — Code holds the raw NTSTATUS value
		Posix = 2,	 ///< errno — Code holds errno as a positive UINT32
		Uefi = 3,	 ///< EFI_STATUS — Code holds the raw EFI_STATUS value
	};

	/// Maximum number of inner (cause) errors stored in the chain
	static constexpr USIZE MaxInnerDepth = 4;

	/// Maximum total depth (outermost + inner causes)
	static constexpr USIZE MaxDepth = 1 + MaxInnerDepth;

	ErrorCodes Code;	   ///< Outermost error code (ErrorCodes enumerator or raw OS code)
	PlatformKind Platform; ///< OS layer that produced the outermost code
	UINT8 Depth;		   ///< Number of inner cause entries used (0 = single error)

	/// Inner cause codes, ordered most-recent-inner [0] to root-cause [Depth-1]
	UINT32 InnerCodes[MaxInnerDepth];
	/// Platform tags corresponding to each InnerCodes entry
	PlatformKind InnerPlatforms[MaxInnerDepth];

	/**
	 * @brief Construct a single error with no cause chain
	 * @param code Error code value
	 * @param platform Platform origin (defaults to Runtime)
	 */
	constexpr Error(UINT32 code = 0, PlatformKind platform = PlatformKind::Runtime)
		: Code((ErrorCodes)code), Platform(platform), Depth(0), InnerCodes{}, InnerPlatforms{}
	{
	}

	/// @name Platform Factory Methods
	/// @{

	/**
	 * @brief Create a Windows NTSTATUS error
	 * @param ntstatus Raw NTSTATUS value
	 * @return Error tagged with PlatformKind::Windows
	 *
	 * @see https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-25e5c5b64e7c
	 */
	[[nodiscard]] static constexpr Error Windows(UINT32 ntstatus) { return Error(ntstatus, PlatformKind::Windows); }

	/**
	 * @brief Create a POSIX errno error
	 * @param errnoVal errno value (stored as positive UINT32)
	 * @return Error tagged with PlatformKind::Posix
	 *
	 * @see https://pubs.opengroup.org/onlinepubs/9699919799/functions/errno.html
	 */
	[[nodiscard]] static constexpr Error Posix(UINT32 errnoVal) { return Error(errnoVal, PlatformKind::Posix); }

	/**
	 * @brief Create a UEFI EFI_STATUS error
	 * @param efiStatus Raw EFI_STATUS value
	 * @return Error tagged with PlatformKind::Uefi
	 *
	 * @see UEFI Specification — Appendix D (Status Codes)
	 *      https://uefi.org/specs/UEFI/2.10/Apx_D_Status_Codes.html
	 */
	[[nodiscard]] static constexpr Error Uefi(UINT32 efiStatus) { return Error(efiStatus, PlatformKind::Uefi); }

	/// @}

	/// @name Chain Operations
	/// @{

	/**
	 * @brief Create a new Error that wraps an inner error under a new outer code
	 *
	 * @details The resulting Error has outerCode as its top-level Code, with the
	 * inner error's full chain preserved beneath it. If the combined depth exceeds
	 * MaxInnerDepth, the deepest (oldest) cause errors are truncated.
	 *
	 * @param inner The cause error (becomes part of the inner chain)
	 * @param outerCode The new outermost error code
	 * @param outerPlatform Platform of the outer code (defaults to Runtime)
	 * @return Error with outerCode on top and inner's chain beneath
	 */
	[[nodiscard]] static constexpr Error Wrap(const Error &inner, UINT32 outerCode,
											  PlatformKind outerPlatform = PlatformKind::Runtime)
	{
		Error result;
		result.Code = (ErrorCodes)outerCode;
		result.Platform = outerPlatform;
		result.Depth = 0;

		// Inner[0] = the inner error's top-level code
		if (result.Depth < MaxInnerDepth)
		{
			result.InnerCodes[result.Depth] = (UINT32)inner.Code;
			result.InnerPlatforms[result.Depth] = inner.Platform;
			result.Depth++;
		}

		// Copy inner's own chain beneath
		for (USIZE i = 0; i < inner.Depth && result.Depth < MaxInnerDepth; i++)
		{
			result.InnerCodes[result.Depth] = inner.InnerCodes[i];
			result.InnerPlatforms[result.Depth] = inner.InnerPlatforms[i];
			result.Depth++;
		}

		return result;
	}

	/**
	 * @brief Get the total number of errors in the chain (outermost + inner causes)
	 * @return 1 + Depth
	 */
	[[nodiscard]] constexpr USIZE TotalDepth() const { return 1 + Depth; }

	/**
	 * @brief Get the innermost (root cause) error code
	 * @return Root cause code if chain exists, otherwise this error's Code
	 */
	[[nodiscard]] constexpr UINT32 RootCode() const
	{
		return Depth > 0 ? InnerCodes[Depth - 1] : (UINT32)Code;
	}

	/**
	 * @brief Get the innermost (root cause) error platform
	 * @return Root cause platform if chain exists, otherwise this error's Platform
	 */
	[[nodiscard]] constexpr PlatformKind RootPlatform() const
	{
		return Depth > 0 ? InnerPlatforms[Depth - 1] : Platform;
	}

	/// @}
};

/** @} */ // end of error group
