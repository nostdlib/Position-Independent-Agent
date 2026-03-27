#pragma once

#include "lib/runtime.h"
#include "tests.h"

class SizeReportTests
{
public:
	static BOOL RunAll()
	{
		BOOL allPassed = true;

		LOG_INFO("Running Size Report Tests...");

		RunTest(allPassed, &TestPrintSortedSizes, "Object sizes sorted large to small");

		if (allPassed) LOG_INFO("All Size Report tests passed!");
		else LOG_ERROR("Some Size Report tests failed!");

		return allPassed;
	}

private:
	struct SizeEntry
	{
		CHAR name[28];
		USIZE size;
	};

	static VOID CopyName(SizeEntry &entry, PCCHAR src, USIZE typeSize)
	{
		for (INT32 i = 0; i < 27; i++)
		{
			entry.name[i] = src[i];
			if (src[i] == '\0')
			{
				entry.size = typeSize;
				return;
			}
		}
		entry.name[27] = '\0';
		entry.size = typeSize;
	}

	static BOOL TestPrintSortedSizes()
	{
		constexpr INT32 MAX_ENTRIES = 40;
		SizeEntry entries[MAX_ENTRIES];
		INT32 count = 0;

		// ── CORE layer ──
		{ auto n = "Error";           CopyName(entries[count++], (PCCHAR)n, sizeof(Error)); }
		{ auto n = "double";          CopyName(entries[count++], (PCCHAR)n, sizeof(double)); }
		{ auto n = "IPAddress";       CopyName(entries[count++], (PCCHAR)n, sizeof(IPAddress)); }
		{ auto n = "Prng";            CopyName(entries[count++], (PCCHAR)n, sizeof(Prng)); }
		{ auto n = "BinaryReader";    CopyName(entries[count++], (PCCHAR)n, sizeof(BinaryReader)); }
		{ auto n = "BinaryWriter";    CopyName(entries[count++], (PCCHAR)n, sizeof(BinaryWriter)); }
		{ auto n = "StringFormatter::Arg"; CopyName(entries[count++], (PCCHAR)n, sizeof(StringFormatter::Argument)); }

		// ── PLATFORM layer ──
		{ auto n = "SockAddr";        CopyName(entries[count++], (PCCHAR)n, sizeof(SockAddr)); }
		{ auto n = "SockAddr6";       CopyName(entries[count++], (PCCHAR)n, sizeof(SockAddr6)); }
		{ auto n = "Socket";          CopyName(entries[count++], (PCCHAR)n, sizeof(Socket)); }
		{ auto n = "File";            CopyName(entries[count++], (PCCHAR)n, sizeof(File)); }
		{ auto n = "DirectoryEntry";  CopyName(entries[count++], (PCCHAR)n, sizeof(DirectoryEntry)); }
		{ auto n = "DirectoryIterator"; CopyName(entries[count++], (PCCHAR)n, sizeof(DirectoryIterator)); }
		{ auto n = "DateTime";        CopyName(entries[count++], (PCCHAR)n, sizeof(DateTime)); }
		{ auto n = "Random";          CopyName(entries[count++], (PCCHAR)n, sizeof(Random)); }

		// ── RUNTIME crypto ──
		{ auto n = "UInt128";         CopyName(entries[count++], (PCCHAR)n, sizeof(UInt128)); }
		{ auto n = "ECCPoint";        CopyName(entries[count++], (PCCHAR)n, sizeof(ECCPoint)); }
		{ auto n = "Poly1305";        CopyName(entries[count++], (PCCHAR)n, sizeof(Poly1305)); }
		{ auto n = "ChaCha20Poly1305"; CopyName(entries[count++], (PCCHAR)n, sizeof(ChaCha20Poly1305)); }
		{ auto n = "ChaCha20Encoder"; CopyName(entries[count++], (PCCHAR)n, sizeof(ChaCha20Encoder)); }
		{ auto n = "ECC";             CopyName(entries[count++], (PCCHAR)n, sizeof(ECC)); }
		{ auto n = "SHA256";          CopyName(entries[count++], (PCCHAR)n, sizeof(SHA256)); }
		{ auto n = "SHA384";          CopyName(entries[count++], (PCCHAR)n, sizeof(SHA384)); }
		{ auto n = "HMAC_SHA256";     CopyName(entries[count++], (PCCHAR)n, sizeof(HMAC_SHA256)); }
		{ auto n = "HMAC_SHA384";     CopyName(entries[count++], (PCCHAR)n, sizeof(HMAC_SHA384)); }

		// ── RUNTIME network / TLS ──
		{ auto n = "TlsBuffer";       CopyName(entries[count++], (PCCHAR)n, sizeof(TlsBuffer)); }
		{ auto n = "TlsHash";         CopyName(entries[count++], (PCCHAR)n, sizeof(TlsHash)); }
		{ auto n = "TlsState";        CopyName(entries[count++], (PCCHAR)n, sizeof(TlsState)); }
		{ auto n = "TlsCipher";       CopyName(entries[count++], (PCCHAR)n, sizeof(TlsCipher)); }
		{ auto n = "TlsClient";       CopyName(entries[count++], (PCCHAR)n, sizeof(TlsClient)); }
		{ auto n = "WebSocketFrame";  CopyName(entries[count++], (PCCHAR)n, sizeof(WebSocketFrame)); }
		{ auto n = "WebSocketMessage"; CopyName(entries[count++], (PCCHAR)n, sizeof(WebSocketMessage)); }
		{ auto n = "WebSocketClient"; CopyName(entries[count++], (PCCHAR)n, sizeof(WebSocketClient)); }
		{ auto n = "HttpClient";      CopyName(entries[count++], (PCCHAR)n, sizeof(HttpClient)); }

		// ── Common template instantiations ──
		{ auto n = "Span<UINT8>";                CopyName(entries[count++], (PCCHAR)n, sizeof(Span<UINT8>)); }
		{ auto n = "Result<VOID,Error>";         CopyName(entries[count++], (PCCHAR)n, sizeof(Result<VOID, Error>)); }

		// Bubble sort descending by size
		for (INT32 i = 0; i < count - 1; i++)
		{
			for (INT32 j = 0; j < count - i - 1; j++)
			{
				if (entries[j].size < entries[j + 1].size)
				{
					SizeEntry temp;
					Memory::Copy(&temp, &entries[j], sizeof(SizeEntry));
					Memory::Copy(&entries[j], &entries[j + 1], sizeof(SizeEntry));
					Memory::Copy(&entries[j + 1], &temp, sizeof(SizeEntry));
				}
			}
		}

		// Print header
		LOG_INFO("");
		LOG_INFO("  #   %-27s  %s", (PCCHAR)"Type", (PCCHAR)"Size");
		LOG_INFO("  --- %-27s  %s", (PCCHAR)"---------------------------", (PCCHAR)"--------");

		// Print sorted results
		for (INT32 i = 0; i < count; i++)
		{
			LOG_INFO("  %2d. %-27s  %u bytes", i + 1, entries[i].name, (UINT32)entries[i].size);
		}

		// Compute total bytes
		[[maybe_unused]] UINT32 totalBytes = 0;
		for (INT32 i = 0; i < count; i++)
			totalBytes += (UINT32)entries[i].size;

		LOG_INFO("");
		LOG_INFO("  Total types: %d, combined size: %u bytes", count, totalBytes);

		return true;
	}
};
