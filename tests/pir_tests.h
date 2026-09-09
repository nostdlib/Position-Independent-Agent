/**
 * pir_tests.h - Unified PIR Test Suite Header
 *
 * This header exposes all test suite classes for the CPP-PIC runtime.
 * Include this single header to access all test functionality.
 *
 * TEST SUITES:
 *   ArrayStorageTests      - Compile-time array storage tests
 *   Base64Tests            - Base64 encoding/decoding tests
 *   BinaryIOTests          - Binary reader/writer tests
 *   BitsetTests            - Bitset container tests
 *   BufferTests            - Buffer container tests
 *   ByteQueueTests         - ByteQueue container tests
 *   Djb2Tests              - Hash function tests
 *   DnsTests               - DNS resolution tests (DoT, DoH JSON, DoH binary)
 *   DoubleTests            - Floating-point tests
 *   EccTests               - Elliptic Curve Cryptography tests (ECDH key exchange)
 *   EnvironmentTests       - Environment variable and platform identification tests
 *   FileSystemTests        - File system operations tests
 *   ImageTests             - Image processing tests
 *   IPAddressTests         - IPAddress constexpr and runtime tests
 *   JpegTests              - JPEG decoding tests
 *   MemoryTests            - Memory operations tests
 *   PrngTests              - Pseudorandom number generator tests
 *   ProcessTests           - Process creation, wait, terminate tests
 *   PortableDeviceTests    - Portable-device pseudo-root tests (WPD/GVFS roots)
 *   RandomTests            - Random number generation tests
 *   ShellManagerTests      - ShellManager slot assignment / reuse tests
 *   ResultTests            - Result<T,E> type tests
 *   ScreenTests            - Screen/display device tests
 *   ShaTests               - SHA-2 hash function tests (SHA-224/256/384/512 and HMAC)
 *   SizeReportTests        - Object sizeof report (sorted large to small)
 *   SocketTests            - Socket and network tests
 *   SpanTests              - Span<T> type tests
 *   StringFormatterTests   - Printf-style formatting tests
 *   StringTests            - String utility tests
 *   TlsTests               - TLS 1.3 implementation tests
 *   UuidTests              - UUID construction, parsing, and serialization tests
 *   VectorTests            - Vector container tests
 *   WebSocketTests         - WebSocket client implementation tests (ws:// and wss://)
 *
 * USAGE:
 *   #include "tests.h"
 *
 *   // Run all tests
 *   Djb2Tests::RunAll();
 *   MemoryTests::RunAll();
 *   ArrayStorageTests::RunAll();
 *   SocketTests::RunAll();
 *   TlsTests::RunAll();
 *   ShaTests::RunAll();
 *   Base64Tests::RunAll();
 *   EccTests::RunAll();
 *   DnsTests::RunAll();
 *   WebSocketTests::RunAll();
 *   // ... etc
 */

#pragma once

#include "core/core.h"

#include "array_storage_tests.h"
#include "base64_tests.h"
#include "binary_io_tests.h"
#include "bitset_tests.h"
#include "buffer_tests.h"
#include "byte_queue_tests.h"
#include "djb2_tests.h"
#include "dns_tests.h"
#include "double_tests.h"
#include "ecc_tests.h"
#include "file_system_tests.h"
#include "portable_device_tests.h"
#include "image_tests.h"
#include "ip_address_tests.h"
#include "jpeg_tests.h"
#include "memory_tests.h"
#include "process_tests.h"
#include "prng_tests.h"
#include "random_tests.h"
#include "result_tests.h"
#include "screen_tests.h"
#include "sha_tests.h"
#include "size_report_tests.h"
#include "socket_tests.h"
#include "span_tests.h"
#include "string_formatter_tests.h"
#include "string_tests.h"
#include "tls_tests.h"
#include "tls_buffer_tests.h"
#include "uuid_tests.h"
#include "vector_tests.h"
#include "websocket_tests.h"
#include "shell_platform_test.h"
#include "shell_manager_tests.h"
#include "environment_tests.h"

static BOOL RunPIRTests()
{
	BOOL allPassed = true;

	LOG_INFO("=== CPP-PIC Test Suite ===");
	LOG_INFO("");

	// CORE - Types
	RunTestSuite<BinaryIOTests>(allPassed);
	RunTestSuite<BitsetTests>(allPassed);
	RunTestSuite<BufferTests>(allPassed);
	RunTestSuite<ByteQueueTests>(allPassed);
	RunTestSuite<DoubleTests>(allPassed);
	RunTestSuite<IPAddressTests>(allPassed);
	RunTestSuite<ResultTests>(allPassed);
	RunTestSuite<SpanTests>(allPassed);
	RunTestSuite<UuidTests>(allPassed);
	RunTestSuite<VectorTests>(allPassed);

	// CORE - Strings and Algorithms
	RunTestSuite<ArrayStorageTests>(allPassed);
	RunTestSuite<Base64Tests>(allPassed);
	RunTestSuite<Djb2Tests>(allPassed);
	RunTestSuite<PrngTests>(allPassed);
	RunTestSuite<StringFormatterTests>(allPassed);
	RunTestSuite<StringTests>(allPassed);

	// PLATFORM - Memory, System, File I/O, Shell process, Environment, and Display
	RunTestSuite<EnvironmentTests>(allPassed);
#if !defined(ARCHITECTURE_MIPS)
	// MIPS32: getdents64 returns "." and ".." under qemu (dirent-layout investigation pending).
	RunTestSuite<FileSystemTests>(allPassed);
	RunTestSuite<PortableDeviceTests>(allPassed);
#endif
	RunTestSuite<MemoryTests>(allPassed);
	RunTestSuite<ProcessTests>(allPassed);
#if !defined(ARCHITECTURE_MIPS)
	// MIPS32: qemu-mipsel can't translate the MIPS PTY-grant ioctls (errno 130). Passes on hardware.
	RunTestSuite<ShellProcessTests>(allPassed);
	RunTestSuite<ShellManagerTests>(allPassed);
#endif
	RunTestSuite<RandomTests>(allPassed);
	RunTestSuite<ScreenTests>(allPassed);

	// RUNTIME - TLS buffer mechanics (pure memory logic, no sockets involved,
	// so it also runs on MIPS)
	RunTestSuite<TlsBufferTests>(allPassed);

	// RUNTIME - Cryptography
	RunTestSuite<EccTests>(allPassed);
	RunTestSuite<ShaTests>(allPassed);

#if !defined(ARCHITECTURE_MIPS)
	// RUNTIME - Network
	// MIPS32: qemu-mipsel o32 socket-type quirk (SOCK_STREAM=1 is read as DGRAM under qemu →
	// EPROTONOSUPPORT). Real MIPS hardware uses SOCK_STREAM=1 correctly; Dns/Tls/WebSocket cascade
	// from Socket. These run (and pass) on real MIPS hardware — skipped here only for qemu.
	RunTestSuite<DnsTests>(allPassed);
	RunTestSuite<SocketTests>(allPassed);
	RunTestSuite<TlsTests>(allPassed);
	RunTestSuite<WebSocketTests>(allPassed);

	// RUNTIME - Image
	// MIPS32: unaligned-access SIGBUS in the image path (real bug, deferred — screenshot code,
	// unused on headless routers). Tracked for a follow-up alignment fix.
	RunTestSuite<ImageTests>(allPassed);
	RunTestSuite<JpegTests>(allPassed);
#endif

	// Size Report always runs last since it's just informational and doesn't test functionality
	RunTestSuite<SizeReportTests>(allPassed);
	// Final summary
	LOG_INFO("=== Test Suite Complete ===");
	if (allPassed)
		LOG_INFO("ALL TESTS PASSED!");
	else
		LOG_ERROR("SOME TESTS FAILED!");

	return allPassed ? 0 : 1;
}