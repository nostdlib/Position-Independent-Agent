#pragma once

#include "lib/runtime.h"
#include "platform/system/environment.h"
#include "tests.h"

// =============================================================================
// Transport Bench - WebSocket masking / TLS record emission micro-benchmarks.
// Opt-in (PIR_BENCH=1): logs measurements, asserts only mask round-trip
// correctness. Models the screenshot reply send path without any network.
// =============================================================================

class TransportBench
{
private:
	static constexpr USIZE BenchBytes = 1024 * 1024;
	static constexpr UINT32 RecordMax = 1024 * 16; // TLS max plaintext per record

	// Current Write() pattern: mask each 256-byte chunk into a stack buffer,
	// then hand it to the TLS layer (which copies it into its staging buffer).
	static VOID MaskChunked(const UINT8 *src, UINT8 *dst, USIZE size, const UINT8 *maskKey)
	{
		UINT8 chunk[256];
		USIZE offset = 0;
		while (offset < size)
		{
			USIZE chunkSize = (size - offset < sizeof(chunk)) ? size - offset : sizeof(chunk);
			for (USIZE i = 0; i < chunkSize; i++)
				chunk[i] = src[offset + i] ^ maskKey[(offset + i) & 3];
			Memory::Copy(dst + offset, chunk, chunkSize);
			offset += chunkSize;
		}
	}

	// Batched pattern: single pass, 4 bytes per iteration, straight to the output.
	static VOID MaskSinglePass(const UINT8 *src, UINT8 *dst, USIZE size, const UINT8 *maskKey)
	{
		USIZE i = 0;
		for (; i + 4 <= size; i += 4)
		{
			dst[i] = src[i] ^ maskKey[i & 3];
			dst[i + 1] = src[i + 1] ^ maskKey[(i + 1) & 3];
			dst[i + 2] = src[i + 2] ^ maskKey[(i + 2) & 3];
			dst[i + 3] = src[i + 3] ^ maskKey[(i + 3) & 3];
		}
		for (; i < size; i++)
			dst[i] = src[i] ^ maskKey[i & 3];
	}

	static BOOL TestMaskThroughput()
	{
		UINT8 *src = new UINT8[BenchBytes];
		UINT8 *dst = new UINT8[BenchBytes];
		if (!src || !dst)
		{
			delete[] src;
			delete[] dst;
			return false;
		}

		Prng prng(0x5C2EED01);
		prng.GetArray(Span<UINT8>(src, BenchBytes));

		UINT32 maskVal = 0x9E3779B9;
		const UINT8 *maskKey = (const UINT8 *)&maskVal;

		UINT64 chunked[5], single[5];
		for (UINT32 r = 0; r < 5; r++)
		{
			UINT64 t0 = DateTime::GetMonotonicNanoseconds();
			MaskChunked(src, dst, BenchBytes, maskKey);
			UINT64 t1 = DateTime::GetMonotonicNanoseconds();
			chunked[r] = t1 - t0;

			t0 = DateTime::GetMonotonicNanoseconds();
			MaskSinglePass(src, dst, BenchBytes, maskKey);
			t1 = DateTime::GetMonotonicNanoseconds();
			single[r] = t1 - t0;
		}

		// Correctness: masking twice with the same key restores the original
		BOOL ok = true;
		MaskSinglePass(src, dst, BenchBytes, maskKey);
		MaskSinglePass(dst, dst, BenchBytes, maskKey);
		ok = ok && Memory::Compare(dst, src, BenchBytes) == 0;
		MaskChunked(src, dst, BenchBytes, maskKey);
		MaskChunked(dst, dst, BenchBytes, maskKey);
		ok = ok && Memory::Compare(dst, src, BenchBytes) == 0;

		DOUBLE chunkedMs = (DOUBLE)Median(chunked, 5) / 1000000.0;
		DOUBLE singleMs = (DOUBLE)Median(single, 5) / 1000000.0;
		LOG_INFO("  mask 1 MiB chunked (256B stack): %.2f ms", chunkedMs);
		LOG_INFO("  mask 1 MiB single-pass:          %.2f ms", singleMs);

		delete[] src;
		delete[] dst;
		return ok;
	}

	static BOOL TestRecordModel()
	{
		// Records (and socket writes) per payload at the two chunk granularities
		USIZE sizes[3] = {64 * 1024, 256 * 1024, BenchBytes};
		for (UINT32 i = 0; i < 3; i++)
		{
			USIZE len = sizes[i];
			USIZE small = (len + 255) / 256;
			USIZE large = (len + RecordMax - 1) / RecordMax;
			LOG_INFO("  %u KiB payload: %u -> %u TLS records/socket writes (256B chunks vs 16KiB batches)",
			         (UINT32)(len / 1024), (UINT32)small, (UINT32)large);
		}
		return true;
	}

	static BOOL TestAeadRecordOverhead()
	{
		ChaCha20Encoder encoder;
		UINT8 localKey[POLY1305_KEYLEN];
		UINT8 remoteKey[POLY1305_KEYLEN];
		UCHAR localIv[TLS_CHACHA20_IV_LENGTH];
		UCHAR remoteIv[TLS_CHACHA20_IV_LENGTH];

		Prng prng(0xC0FFEE42);
		prng.GetArray(Span<UINT8>(localKey, sizeof(localKey)));
		prng.GetArray(Span<UINT8>(remoteKey, sizeof(remoteKey)));
		prng.GetArray(Span<UINT8>((PUINT8)localIv, sizeof(localIv)));
		prng.GetArray(Span<UINT8>((PUINT8)remoteIv, sizeof(remoteIv)));

		auto init = encoder.Initialize(Span<const UINT8, POLY1305_KEYLEN>(localKey),
		                               Span<const UINT8, POLY1305_KEYLEN>(remoteKey),
		                               localIv, remoteIv);
		if (!init)
		{
			LOG_ERROR("  failed to initialize encoder: %e", init.Error());
			return false;
		}

		UINT8 *payload = new UINT8[BenchBytes];
		if (!payload)
			return false;
		prng.GetArray(Span<UINT8>(payload, BenchBytes));

		TlsBuffer out;
		UINT64 small[5], large[5];
		for (UINT32 r = 0; r < 5; r++)
		{
			(VOID)out.SetSize(0);
			UINT64 t0 = DateTime::GetMonotonicNanoseconds();
			for (USIZE offset = 0; offset < BenchBytes; offset += 256)
			{
				UCHAR aad[5] = {0x17, 0x03, 0x03, 0x01, 0x00};
				encoder.Encode(out, Span<const CHAR>((PCHAR)payload + offset, 256), Span<const UCHAR>(aad, sizeof(aad)));
			}
			UINT64 t1 = DateTime::GetMonotonicNanoseconds();
			small[r] = t1 - t0;

			(VOID)out.SetSize(0);
			t0 = DateTime::GetMonotonicNanoseconds();
			for (USIZE offset = 0; offset + RecordMax <= BenchBytes; offset += RecordMax)
			{
				UCHAR aad[5] = {0x17, 0x03, 0x03, 0x40, 0x00};
				encoder.Encode(out, Span<const CHAR>((PCHAR)payload + offset, RecordMax), Span<const UCHAR>(aad, sizeof(aad)));
			}
			t1 = DateTime::GetMonotonicNanoseconds();
			large[r] = t1 - t0;
		}

		UINT64 smallMedian = Median(small, 5);
		UINT64 largeMedian = Median(large, 5);
		DOUBLE smallMs = (DOUBLE)smallMedian / 1000000.0;
		DOUBLE largeMs = (DOUBLE)largeMedian / 1000000.0;
		DOUBLE perRecord = ((DOUBLE)smallMedian - (DOUBLE)largeMedian) / (DOUBLE)(BenchBytes / 256 - BenchBytes / RecordMax);
		LOG_INFO("  AEAD 1 MiB in 256B records:  %.2f ms (%u records)", smallMs, (UINT32)(BenchBytes / 256));
		LOG_INFO("  AEAD 1 MiB in 16KiB records: %.2f ms (%u records)", largeMs, (UINT32)(BenchBytes / RecordMax));
		LOG_INFO("  per-record AEAD overhead:    ~%.0f ns", perRecord);

		delete[] payload;
		return true;
	}

	static BOOL TestEchoRoundTrip()
	{
		const CHAR url[] = "wss://echo.websocket.org/";
		auto createResult = WebSocketClient::Create(Span<const CHAR>(url, sizeof(url) - 1));
		if (!createResult)
		{
			LOG_INFO("  SKIP: no egress to echo.websocket.org (%e)", createResult.Error());
			return true;
		}
		WebSocketClient &ws = createResult.Value();

		// Discard the server greeting so the first timed read is our own echo
		auto greeting = ws.Read();
		if (greeting)
			LOG_INFO("  discarded greeting (%u bytes)", (UINT32)greeting.Value().Length);

		constexpr UINT32 frameSize = 64 * 1024;
		PCHAR frame = new CHAR[frameSize];
		if (!frame)
			return false;
		Prng prng(0xEC10BEEF);
		prng.GetArray(Span<UINT8>((PUINT8)frame, frameSize));

		BOOL ok = true;
		UINT64 times[5];
		UINT64 writeNs[5];
		for (UINT32 r = 0; r < 5 && ok; r++)
		{
			UINT64 t0 = DateTime::GetMonotonicNanoseconds();
			auto writeResult = ws.Write(Span<const CHAR>(frame, frameSize), WebSocketOpcode::Binary);
			UINT64 tWrite = DateTime::GetMonotonicNanoseconds();
			auto readResult = ws.Read();
			UINT64 t1 = DateTime::GetMonotonicNanoseconds();
			times[r] = t1 - t0;
			writeNs[r] = tWrite - t0;

			if (!writeResult || !readResult || readResult.Value().Length != frameSize ||
			    Memory::Compare(readResult.Value().Data, frame, frameSize) != 0)
			{
				LOG_ERROR("  64 KiB echo round %u failed", r);
				ok = false;
			}
		}

		if (ok)
		{
			LOG_INFO("  64 KiB send (write only, median of 5): %.2f ms", (DOUBLE)Median(writeNs, 5) / 1000000.0);
			LOG_INFO("  64 KiB echo round trip (wss, median of 5): %.2f ms", (DOUBLE)Median(times, 5) / 1000000.0);
		}

		delete[] frame;
		return ok;
	}

public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Transport Bench...");
		LOG_INFO("  Test: mask throughput (chunked vs single-pass)");
		RunTest(allPassed, &TestMaskThroughput, "mask round-trip correctness");
		LOG_INFO("  Test: TLS record count model");
		RunTest(allPassed, &TestRecordModel, "record count model");
		LOG_INFO("  Test: AEAD per-record overhead");
		RunTest(allPassed, &TestAeadRecordOverhead, "AEAD record overhead");
		LOG_INFO("  Test: 64 KiB echo round trip");
		RunTest(allPassed, &TestEchoRoundTrip, "echo round trip");

		if (allPassed)
			LOG_INFO("Transport bench complete!");
		else
			LOG_ERROR("Some transport bench tests failed!");

		return allPassed;
	}
};
