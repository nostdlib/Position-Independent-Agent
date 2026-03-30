#include "lib/network/tls/tls_buffer.h"
#include "core/memory/memory.h"
#include "platform/console/logger.h"

/// @brief Append data to the TLS buffer
/// @param data The span of data to append to the buffer
/// @return The offset at which the data was appended

INT32 TlsBuffer::Append(Span<const CHAR> data)
{
	auto r = CheckSize((INT32)data.Size());
	if (!r)
		return -1;
	Memory::Copy(buffer + size, data.Data(), data.Size());
	size += (INT32)data.Size();
	return size - (INT32)data.Size();
}

/// @brief Append a value of any type to the TLS buffer
/// @param count The number of bytes to append
/// @return The offset at which the bytes were appended
INT32 TlsBuffer::AppendSize(INT32 count)
{
	auto r = CheckSize(count);
	if (!r)
		return -1;
	size += count;
	return size - count;
}

/// @brief Set the size of the TLS buffer
/// @param size The new size of the buffer
/// @return Result indicating success or failure
Result<VOID, Error> TlsBuffer::SetSize(INT32 newSize)
{
	size = 0;
	auto r = CheckSize(newSize);
	if (!r)
		return Result<VOID, Error>::Err(r.Error());
	size = newSize;
	return Result<VOID, Error>::Ok();
}

/// @brief Clean up the TLS buffer by freeing memory if owned and resetting size and capacity
/// @return void
VOID TlsBuffer::Clear()
{
	if (buffer && ownsMemory)
	{
		delete[] buffer;
	}
	buffer = nullptr;
	size = 0;
	capacity = 0;
	readPos = 0;
}

/// @brief Ensure there is enough capacity in the TLS buffer to append additional data
/// @param appendSize The size of the data to be appended
/// @return Result indicating success or failure
Result<VOID, Error> TlsBuffer::CheckSize(INT32 appendSize)
{
	// Capacity check
	if (size + appendSize <= capacity)
	{
		LOG_DEBUG("Buffer size is sufficient: %d + %d <= %d", size, appendSize, capacity);
		return Result<VOID, Error>::Ok();
	}

	PCHAR oldBuffer = buffer;
	INT32 newLen = (size + appendSize) * 4;
	if (newLen < 256)
	{
		newLen = 256;
	}

	// Allocate new buffer and copy existing data if necessary
	PCHAR newBuffer = (PCHAR) new CHAR[newLen];
	// Validate allocation
	if (!newBuffer)
	{
		return Result<VOID, Error>::Err(Error::TlsBuffer_AllocationFailed);
	}
	if (size > 0)
	{
		LOG_DEBUG("Resizing buffer from %d to %d bytes", capacity, newLen);
		Memory::Copy(newBuffer, oldBuffer, size);
	}

	// Clean up old buffer if owned memory
	if (oldBuffer && ownsMemory)
	{
		delete[] oldBuffer;
		oldBuffer = nullptr;
	}
	buffer = newBuffer;
	capacity = newLen;
	ownsMemory = true;
	return Result<VOID, Error>::Ok();
}

/// @brief Remove consumed bytes from the front and shift remaining data down
/// @param bytes Number of bytes to consume from the front of the buffer
/// @return void
VOID TlsBuffer::Consume(INT32 bytes)
{
	if (bytes <= 0)
		return;
	if (bytes >= size)
	{
		size = 0;
		readPos = 0;
		return;
	}
	Memory::Move(buffer, buffer + bytes, size - bytes);
	size -= bytes;
	readPos = 0;
}

/// @brief Read a block of data from the TLS buffer
/// @param buf The span to receive the data read from the buffer
/// @return void
VOID TlsBuffer::Read(Span<CHAR> buf)
{
	INT32 available = size - readPos;
	INT32 count = (INT32)buf.Size();
	// Adjust count if it exceeds available data
	if (count > available)
		count = available;
	if (count > 0)
		Memory::Copy(buf.Data(), buffer + readPos, count);
	readPos += count;
}

/// @brief Read a 24-bit big-endian unsigned integer from the TLS buffer
/// @return The 24-bit value read from the buffer
UINT32 TlsBuffer::ReadU24BE()
{
	// Ensure there are at least 3 bytes available to read (24 bits)
	if (readPos + 3 > size)
	{
		readPos = size;
		return 0;
	}
	UINT8 b0 = (UINT8)buffer[readPos];
	UINT8 b1 = (UINT8)buffer[readPos + 1];
	UINT8 b2 = (UINT8)buffer[readPos + 2];
	readPos += 3;
	return ((UINT32)b0 << 16) | ((UINT32)b1 << 8) | (UINT32)b2;
}
