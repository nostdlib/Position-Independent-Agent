/**
 * @file dl.cc
 * @brief Position-independent dynamic symbol resolution for Android
 *
 * @details Implements FindLoadedLibrary() via /proc/self/maps parsing and
 * ResolveElfSymbol() via ELF dynamic symbol table walking.
 *
 * /proc/self/maps format (one line per mapping):
 *   address           perms offset  dev   inode   pathname
 *   7f8a3c000-7f8a40000 r-xp 00000000 fd:01 262175 /system/lib64/libart.so
 *
 * The first r-xp (executable) mapping of a library is typically its
 * base address (where the ELF header lives). We validate by checking
 * the ELF magic at the mapped address.
 *
 * For symbol resolution, we walk the ELF program headers to find
 * PT_DYNAMIC, extract DT_SYMTAB/DT_STRTAB/DT_HASH, then iterate
 * the symbol table looking for a name match.
 */

#include "platform/kernel/android/dl.h"
#include "platform/kernel/android/elf.h"
#include "platform/kernel/android/syscall.h"
#include "platform/kernel/android/system.h"
#include "core/memory/memory.h"

// =============================================================================
// /proc/self/maps parsing helpers
// =============================================================================

/// @brief Check if haystack contains needle (simple substring search)
static BOOL ContainsSubstr(const CHAR *haystack, USIZE haystackLen,
						   const CHAR *needle, USIZE needleLen)
{
	if (needleLen == 0 || needleLen > haystackLen)
		return false;

	for (USIZE i = 0; i <= haystackLen - needleLen; i++)
	{
		BOOL match = true;
		for (USIZE j = 0; j < needleLen; j++)
		{
			if (haystack[i + j] != needle[j])
			{
				match = false;
				break;
			}
		}
		if (match)
			return true;
	}
	return false;
}

/// @brief Parse a hex address from a string, advancing the pointer
/// @return Parsed address value
static USIZE ParseHex(const CHAR *&p)
{
	USIZE val = 0;
	while (true)
	{
		CHAR c = *p;
		if (c >= '0' && c <= '9')
			val = (val << 4) | (USIZE)(c - '0');
		else if (c >= 'a' && c <= 'f')
			val = (val << 4) | (USIZE)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			val = (val << 4) | (USIZE)(c - 'A' + 10);
		else
			break;
		p++;
	}
	return val;
}

/// @brief Validate ELF magic at a given address
static BOOL IsValidElf(PVOID addr)
{
	const UINT8 *p = (const UINT8 *)addr;
	return p[0] == ELF_MAGIC_0 && p[1] == ELF_MAGIC_1 &&
		   p[2] == ELF_MAGIC_2 && p[3] == ELF_MAGIC_3;
}

/// @brief String length (no libc)
static USIZE StrLen(const CHAR *s)
{
	USIZE len = 0;
	while (s[len] != '\0')
		len++;
	return len;
}

/// @brief String comparison (no libc)
static BOOL StrEq(const CHAR *a, const CHAR *b)
{
	while (*a && *b)
	{
		if (*a != *b)
			return false;
		a++;
		b++;
	}
	return *a == *b;
}

// =============================================================================
// FindLoadedLibrary — parse /proc/self/maps
// =============================================================================

PVOID FindLoadedLibrary(const CHAR *nameSubstr)
{
	auto mapsPath = "/proc/self/maps";

	// Open /proc/self/maps
#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_RISCV32)
	SSIZE fd = System::Call(SYS_OPENAT, (USIZE)AT_FDCWD, (USIZE)(const CHAR *)mapsPath, (USIZE)O_RDONLY);
#else
	SSIZE fd = System::Call(SYS_OPEN, (USIZE)(const CHAR *)mapsPath, (USIZE)O_RDONLY);
#endif

	if (fd < 0)
		return nullptr;

	USIZE needleLen = StrLen(nameSubstr);
	PVOID result = nullptr;

	// Read and parse line by line
	// Buffer large enough for several lines; /proc/self/maps lines are typically <200 chars
	CHAR buf[4096];
	USIZE bufLen = 0;

	while (true)
	{
		SSIZE n = System::Call(SYS_READ, (USIZE)fd, (USIZE)(buf + bufLen), sizeof(buf) - bufLen);
		if (n <= 0 && bufLen == 0)
			break;
		if (n > 0)
			bufLen += (USIZE)n;

		// Process complete lines
		USIZE lineStart = 0;
		for (USIZE i = 0; i < bufLen; i++)
		{
			if (buf[i] != '\n')
				continue;

			// Process line [lineStart..i)
			USIZE lineLen = i - lineStart;
			const CHAR *line = buf + lineStart;

			// Parse: "start-end perms ..."
			// Find the address range and permissions
			if (lineLen > 20)
			{
				const CHAR *p = line;
				USIZE startAddr = ParseHex(p);

				if (*p == '-')
				{
					p++;
					(VOID)ParseHex(p); // skip end address

					// Skip space, read perms (4 chars: rwxp)
					if (*p == ' ')
					{
						p++;
						BOOL readable = (p[0] == 'r');

						// Check if this line's path contains our target
						if (readable && ContainsSubstr(line, lineLen, nameSubstr, needleLen))
						{
							// Verify ELF magic at the start address
							PVOID addr = (PVOID)startAddr;
							if (IsValidElf(addr))
							{
								result = addr;
								goto done;
							}
						}
					}
				}
			}

			lineStart = i + 1;
		}

		// Move incomplete line to start of buffer
		if (lineStart < bufLen)
		{
			USIZE remaining = bufLen - lineStart;
			for (USIZE i = 0; i < remaining; i++)
				buf[i] = buf[lineStart + i];
			bufLen = remaining;
		}
		else
		{
			bufLen = 0;
		}

		if (n <= 0)
			break;
	}

done:
	System::Call(SYS_CLOSE, (USIZE)fd);
	return result;
}

// =============================================================================
// ResolveElfSymbol — parse ELF dynamic symbol table
// =============================================================================

PVOID ResolveElfSymbol(PVOID base, const CHAR *symbolName)
{
	if (base == nullptr || !IsValidElf(base))
		return nullptr;

	const ElfEhdr *ehdr = (const ElfEhdr *)base;

	// Validate ELF class matches architecture
#if defined(ARCHITECTURE_AARCH64) || defined(ARCHITECTURE_X86_64) || \
	defined(ARCHITECTURE_RISCV64) || defined(ARCHITECTURE_MIPS64)
	if (ehdr->Ident[4] != ELFCLASS64)
		return nullptr;
#else
	if (ehdr->Ident[4] != ELFCLASS32)
		return nullptr;
#endif

	// Find PT_DYNAMIC and the first PT_LOAD (for base address calculation)
	const ElfPhdr *phdr = (const ElfPhdr *)((UINT8 *)base + ehdr->PhOff);
	const ElfDyn *dynamic = nullptr;
	USIZE dynamicCount = 0;
	USIZE loadBias = 0;
	BOOL foundLoad = false;

	for (UINT16 i = 0; i < ehdr->PhNum; i++)
	{
		if (phdr[i].Type == PT_DYNAMIC)
		{
			dynamic = (const ElfDyn *)((UINT8 *)base + phdr[i].Offset);
			dynamicCount = phdr[i].MemSz / sizeof(ElfDyn);
		}
		else if (phdr[i].Type == PT_LOAD && !foundLoad)
		{
			// Load bias = actual base - expected VAddr of first PT_LOAD
			loadBias = (USIZE)base - (USIZE)phdr[i].VAddr;
			foundLoad = true;
		}
	}

	if (dynamic == nullptr)
		return nullptr;

	// Extract DT_SYMTAB, DT_STRTAB, DT_HASH from PT_DYNAMIC
	const ElfSym *symtab = nullptr;
	const CHAR *strtab = nullptr;
	USIZE strtabSize = 0;
	const ElfHashTable *hashTable = nullptr;
	USIZE symCount = 0;

	for (USIZE i = 0; i < dynamicCount; i++)
	{
		if (dynamic[i].Tag == (decltype(dynamic[i].Tag))DT_NULL)
			break;

		switch ((USIZE)dynamic[i].Tag)
		{
		case DT_SYMTAB:
			symtab = (const ElfSym *)(loadBias + (USIZE)dynamic[i].Val);
			break;
		case DT_STRTAB:
			strtab = (const CHAR *)(loadBias + (USIZE)dynamic[i].Val);
			break;
		case DT_STRSZ:
			strtabSize = (USIZE)dynamic[i].Val;
			break;
		case DT_HASH:
			hashTable = (const ElfHashTable *)(loadBias + (USIZE)dynamic[i].Val);
			break;
		}
	}

	if (symtab == nullptr || strtab == nullptr)
		return nullptr;

	// Determine symbol count from DT_HASH if available
	if (hashTable != nullptr)
		symCount = hashTable->NChain;

	// Fallback: bounded linear scan if no hash table
	if (symCount == 0)
		symCount = 8192; // reasonable upper bound

	// Walk symbol table looking for matching name
	for (USIZE i = 0; i < symCount; i++)
	{
		const ElfSym &sym = symtab[i];

		// Skip undefined symbols
		if (sym.ShNdx == SHN_UNDEF)
			continue;

		// Only consider global/weak function symbols
		UINT8 binding = sym.Info >> 4;
		if (binding != STB_GLOBAL && binding != STB_WEAK)
			continue;

		// Compare name
		USIZE nameOffset = sym.Name;
		if (nameOffset >= strtabSize && strtabSize > 0)
			continue;

		const CHAR *name = strtab + nameOffset;
		if (StrEq(name, symbolName))
			return (PVOID)(loadBias + (USIZE)sym.Value);
	}

	return nullptr;
}
