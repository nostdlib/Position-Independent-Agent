#include "core/types/ip_address.h"
#include "core/memory/memory.h"
#include "core/string/string.h"

Result<IPAddress, Error> IPAddress::FromString(Span<const CHAR> ipString)
{
	if (ipString.Size() == 0)
	{
		return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
	}

	// Check if it's IPv6 (contains ':')
	BOOL hasColon = false;
	for (USIZE k = 0; k < ipString.Size(); k++)
	{
		if (ipString[k] == ':')
		{
			hasColon = true;
			break;
		}
	}

	if (hasColon)
	{
		// Parse IPv6 address
		UINT8 ipv6[16];
		Memory::Zero(ipv6, 16);

		// IPv6 parsing logic
		UINT32 groupIndex = 0;
		UINT32 doubleColonPos = 0xFFFFFFFF;
		BOOL foundDoubleColon = false;
		USIZE pos = 0;
		CHAR hexBuffer[5];
		UINT32 hexIndex = 0;

		while (pos < ipString.Size() && groupIndex < 8)
		{
			if (ipString[pos] == ':')
			{
				if (pos + 1 < ipString.Size() && ipString[pos + 1] == ':' && !foundDoubleColon)
				{
					// Flush accumulated hex digits before :: as a separate group
					if (hexIndex > 0 && groupIndex < 8)
					{
						UINT32 value = StringUtils::ParseHex(Span<const CHAR>(hexBuffer, hexIndex));
						ipv6[groupIndex * 2] = (UINT8)(value >> 8);
						ipv6[groupIndex * 2 + 1] = (UINT8)(value & 0xFF);
						groupIndex++;
						hexIndex = 0;
					}
					// Handle double colon
					foundDoubleColon = true;
					doubleColonPos = groupIndex;
					pos += 2;
					if (pos >= ipString.Size())
						break;
					continue;
				}
				else if (hexIndex > 0)
				{
					// Process accumulated hex digits
					UINT32 value = StringUtils::ParseHex(Span<const CHAR>(hexBuffer, hexIndex));
					ipv6[groupIndex * 2] = (UINT8)(value >> 8);
					ipv6[groupIndex * 2 + 1] = (UINT8)(value & 0xFF);
					groupIndex++;
					hexIndex = 0;
					pos++;
				}
				else
				{
					pos++;
				}
			}
			else if ((ipString[pos] >= '0' && ipString[pos] <= '9') ||
					 (ipString[pos] >= 'a' && ipString[pos] <= 'f') ||
					 (ipString[pos] >= 'A' && ipString[pos] <= 'F'))
			{
				if (hexIndex < 4)
				{
					hexBuffer[hexIndex++] = ipString[pos];
				}
				pos++;
			}
			else
			{
				// Invalid character
				return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
			}
		}

		// Process final group if any
		if (hexIndex > 0 && groupIndex < 8)
		{
			UINT32 value = StringUtils::ParseHex(Span<const CHAR>(hexBuffer, hexIndex));
			ipv6[groupIndex * 2] = (UINT8)(value >> 8);
			ipv6[groupIndex * 2 + 1] = (UINT8)(value & 0xFF);
			groupIndex++;
		}

		// Handle double colon expansion
		if (foundDoubleColon && groupIndex < 8)
		{
			UINT32 tailGroups = groupIndex - doubleColonPos;

			// Move tail groups to the end (iterate backward to handle overlapping ranges)
			if (tailGroups > 0)
			{
				for (UINT32 i = tailGroups; i > 0; i--)
				{
					UINT32 srcIdx = (groupIndex - tailGroups + (i - 1)) * 2;
					UINT32 dstIdx = (8 - tailGroups + (i - 1)) * 2;
					ipv6[dstIdx] = ipv6[srcIdx];
					ipv6[dstIdx + 1] = ipv6[srcIdx + 1];
					if (srcIdx != dstIdx)
					{
						ipv6[srcIdx] = 0;
						ipv6[srcIdx + 1] = 0;
					}
				}
			}
		}

		return Result<IPAddress, Error>::Ok(IPAddress(ipv6));
	}
	else
	{
		// Parse IPv4 address
		CHAR currentOctet[8];
		UINT32 currentOctetIndex = 0;
		UINT32 completedOctetCount = 0;
		UINT8 octets[4];
		UINT32 addr = 0;

		Memory::Zero(currentOctet, sizeof(currentOctet));

		for (USIZE pos = 0; pos <= ipString.Size(); pos++)
		{
			BOOL endOfOctet = false;
			BOOL endOfString = (pos == ipString.Size());

			if (endOfString)
			{
				endOfOctet = true;
			}
			else if (ipString[pos] == '.')
			{
				endOfOctet = true;
			}
			else
			{
				if (ipString[pos] >= '0' && ipString[pos] <= '9')
				{
					if (currentOctetIndex > 2)
					{
						return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
					}
					currentOctet[currentOctetIndex] = ipString[pos];
					currentOctetIndex++;
				}
				else
				{
					return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
				}
			}

			if (endOfOctet)
			{
				if (currentOctetIndex == 0)
				{
					return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
				}

				auto octetResult = StringUtils::ParseInt64(Span<const CHAR>(currentOctet, currentOctetIndex));
				if (!octetResult)
				{
					return Result<IPAddress, Error>::Err(octetResult, Error::IpAddress_ParseFailed);
				}
				auto& octet = octetResult.Value();
				if (octet > 255)
				{
					return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
				}

				if (completedOctetCount >= 4)
				{
					return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
				}

				octets[completedOctetCount] = (UINT8)octet;
				completedOctetCount++;

				if (endOfString)
				{
					break;
				}

				Memory::Zero(currentOctet, sizeof(currentOctet));
				currentOctetIndex = 0;
			}
		}

		if (completedOctetCount != 4)
		{
			return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
		}

		Memory::Copy((PVOID)&addr, octets, 4);
		return Result<IPAddress, Error>::Ok(IPAddress(addr));
	}
}

Result<IPAddress, Error> IPAddress::FromString(PCCHAR ipString)
{
	if (ipString == nullptr)
	{
		return Result<IPAddress, Error>::Err(Error::IpAddress_ParseFailed);
	}
	return FromString(Span<const CHAR>(ipString, StringUtils::Length(ipString)));
}


Result<VOID, Error> IPAddress::ToString(Span<CHAR> buffer) const
{
	if (buffer.Size() == 0)
	{
		return Result<VOID, Error>::Err(Error::IpAddress_ToStringFailed);
	}

	if (version == IPVersion::IPv4)
	{
		// Convert IPv4 to string
		if (buffer.Size() < 16) // Minimum size for "255.255.255.255\0"
		{
			return Result<VOID, Error>::Err(Error::IpAddress_ToStringFailed);
		}

		UINT8 octets[4];
		Memory::Copy(octets, &address.ipv4, 4);

		UINT32 offset = 0;
		for (UINT32 i = 0; i < 4; i++)
		{
			if (i > 0)
			{
				buffer[offset++] = '.';
			}
			CHAR temp[4];
			USIZE len = StringUtils::WriteDecimal(Span<CHAR>(temp), octets[i]);
			Memory::Copy(&buffer[offset], temp, len);
			offset += (UINT32)len;
		}
		buffer[offset] = '\0';
		return Result<VOID, Error>::Ok();
	}
	else if (version == IPVersion::IPv6)
	{
		// Convert IPv6 to string (simplified format)
		if (buffer.Size() < 40) // Minimum size for full IPv6 address
		{
			return Result<VOID, Error>::Err(Error::IpAddress_ToStringFailed);
		}

		UINT32 offset = 0;
		for (UINT32 i = 0; i < 8; i++)
		{
			if (i > 0)
			{
				buffer[offset++] = ':';
			}
			UINT16 group = ((UINT16)address.ipv6[i * 2] << 8) | address.ipv6[i * 2 + 1];

			// Convert to hex
			CHAR hexStr[5];
			USIZE hexLen = StringUtils::WriteHex(Span<CHAR>(hexStr), group);
			Memory::Copy(&buffer[offset], hexStr, hexLen);
			offset += (UINT32)hexLen;
		}
		buffer[offset] = '\0';
		return Result<VOID, Error>::Ok();
	}

	return Result<VOID, Error>::Err(Error::IpAddress_ToStringFailed);
}
