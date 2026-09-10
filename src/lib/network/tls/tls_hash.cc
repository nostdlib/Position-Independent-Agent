#include "lib/network/tls/tls_hash.h"
#include "platform/console/logger.h"
#include "lib/crypto/sha2.h"

VOID TlsHash::Reset()
{
	cache.Clear();
}


VOID TlsHash::Append(Span<const CHAR> buffer)
{
	cache.Append(buffer);
}

VOID TlsHash::GetHash(Span<CHAR> out)
{
	if (out.Size() == SHA256_DIGEST_SIZE)
	{
		LOG_DEBUG("Computing SHA256 hash with size: %d bytes", (INT32)out.Size());
		SHA256 ctx;
		if (cache.GetSize() > 0)
		{
			LOG_DEBUG("SHA256 hash cache size: %d bytes", cache.GetSize());
			ctx.Update(Span<const UINT8>((UINT8 *)cache.GetBuffer(), cache.GetSize()));
		}
		ctx.Final(Span<UINT8, SHA256_DIGEST_SIZE>((UINT8 *)out.Data()));
	}
	else
	{
		LOG_DEBUG("Computing SHA384 hash with size: %d bytes", (INT32)out.Size());
		SHA384 ctx;
		if (cache.GetSize() > 0)
			ctx.Update(Span<const UINT8>((UINT8 *)cache.GetBuffer(), cache.GetSize()));
		ctx.Final(Span<UINT8, SHA384_DIGEST_SIZE>((UINT8 *)out.Data()));
	}
}
