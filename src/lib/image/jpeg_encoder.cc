/**
 * @file jpeg_encoder.cc
 * @brief Position-Independent JPEG Encoder Implementation
 *
 * @details Implements baseline JPEG encoding (ITU-T T.81) with the
 * Arai–Agui–Nakajima scaled DCT algorithm. Adapted from Tiny JPEG Encoder
 * by Sergio Gonzalez for position-independent execution.
 *
 * Internal float constants are embedded as UINT32 immediates via
 * __builtin_bit_cast to avoid .rdata section generation.
 *
 * @note Original DCT implementation by Thomas G. Lane (via NVIDIA SDK).
 *
 * @see ITU-T T.81 — JPEG standard
 *      https://www.w3.org/Graphics/JPEG/itu-t81.pdf
 * @see Arai, Agui, Nakajima — "A fast DCT-SQ scheme for images"
 *      Trans. IEICE E-71(11):1095, 1988
 */

#include "lib/image/jpeg_encoder.h"
#include "core/memory/memory.h"
#include "core/math/byteorder.h"

// ============================================================
//  Constants
// ============================================================

static constexpr INT32 BufferSize = 1024;

/// @brief Reinterpret a UINT32 bit pattern as IEEE-754 float
/// @details The register barrier prevents the compiler from constant-folding
/// the bit pattern into a float constant pool (.rdata), which would break
/// position-independence on i386 (no RIP-relative addressing).
static FORCE_INLINE float F32(UINT32 bits)
{
	__asm__ volatile("" : "+r"(bits));
	return __builtin_bit_cast(float, bits);
}

// ============================================================
//  Internal types
// ============================================================

/// @brief Context for the user-provided write callback
struct WriteContext
{
	PVOID context;
	JpegWriteFunc *func;
};

/// @brief Internal encoder state for a single encode operation
struct EncoderState
{
	UINT8 ehuffsize[4][257];
	UINT16 ehuffcode[4][256];
	const UINT8 *htBits[4];
	const UINT8 *htVals[4];

	UINT8 qtLuma[64];
	UINT8 qtChroma[64];

	WriteContext writeContext;

	USIZE outputBufferCount;
	UINT8 outputBuffer[BufferSize];
};

/// @brief Pre-processed quantization matrices (1/divisor for multiplication)
struct ProcessedQT
{
	float chroma[64];
	float luma[64];
};

// ============================================================
//  Wire-format JPEG segment headers (packed for exact layout)
// ============================================================

#pragma pack(push)
#pragma pack(1)

/// @brief JFIF APP0 header (ITU-T T.81 Annex B, JFIF 1.02)
struct JFIFHeader
{
	UINT16 SOI;
	UINT16 APP0;
	UINT16 jfifLen;
	UINT8 jfifId[5];
	UINT16 version;
	UINT8 units;
	UINT16 xDensity;
	UINT16 yDensity;
	UINT8 xThumb;
	UINT8 yThumb;
};

/// @brief COM (comment) segment
struct CommentSegment
{
	UINT16 com;
	UINT16 comLen;
	CHAR comStr[1];
};

/// @brief Component specification within SOF marker (ITU-T T.81 A.1.1)
struct ComponentSpec
{
	UINT8 componentId;
	UINT8 samplingFactors; ///< Upper 4 bits: horizontal, lower 4: vertical
	UINT8 qt;			   ///< Quantization table selector
};

/// @brief SOF0 frame header (ITU-T T.81 B.2.2)
struct FrameHeader
{
	UINT16 SOF;
	UINT16 len;
	UINT8 precision;
	UINT16 height;
	UINT16 width;
	UINT8 numComponents;
	ComponentSpec componentSpec[3];
};

/// @brief Component specification within SOS marker
struct ScanComponentSpec
{
	UINT8 componentId;
	UINT8 dcAc; ///< (DC table selector << 4) | AC table selector
};

/// @brief SOS scan header (ITU-T T.81 B.2.3)
struct ScanHeader
{
	UINT16 SOS;
	UINT16 len;
	UINT8 numComponents;
	ScanComponentSpec componentSpec[3];
	UINT8 first;
	UINT8 last;
	UINT8 ahAl;
};

#pragma pack(pop)

// ============================================================
//  Huffman table class indices
// ============================================================

enum HuffmanTableIndex : INT32
{
	LumaDC = 0,
	LumaAC = 1,
	ChromaDC = 2,
	ChromaAC = 3,
};

enum HuffmanTableClass : INT32
{
	DC = 0,
	AC = 1,
};

// ============================================================
//  Zig-zag order (ITU-T T.81 Figure A.6)
//  Stack-local to avoid .rodata generation.
// ============================================================

/// @brief Initialize the zig-zag reordering table on the stack
static VOID InitZigZag(UINT8 zz[64])
{
	const UINT8 embedded[] = {
		0, 1, 5, 6, 14, 15, 27, 28,
		2, 4, 7, 13, 16, 26, 29, 42,
		3, 8, 12, 17, 25, 30, 41, 43,
		9, 11, 18, 24, 31, 40, 44, 53,
		10, 19, 23, 32, 39, 45, 52, 54,
		20, 22, 33, 38, 46, 51, 55, 60,
		21, 34, 37, 47, 50, 56, 59, 61,
		35, 36, 48, 49, 57, 58, 62, 63};
	Memory::Copy(zz, embedded, 64);
}

// ============================================================
//  Buffered output
// ============================================================

/**
 * @brief Write data to the output buffer, flushing when full
 *
 * @details Buffers output in chunks of BufferSize-1 bytes before flushing
 * to the user callback. Recursively handles writes larger than the buffer.
 *
 * @param state Encoder state with output buffer
 * @param data Data to write
 * @param numBytes Number of bytes to write
 */
static VOID WriteOutput(EncoderState *state, const PVOID data, USIZE numBytes)
{
	USIZE capped = numBytes;
	if (capped > BufferSize - 1 - state->outputBufferCount)
		capped = BufferSize - 1 - state->outputBufferCount;

	Memory::Copy(state->outputBuffer + state->outputBufferCount, data, capped);
	state->outputBufferCount += capped;

	if (state->outputBufferCount == (USIZE)(BufferSize - 1))
	{
		state->writeContext.func(state->writeContext.context, state->outputBuffer, (INT32)state->outputBufferCount);
		state->outputBufferCount = 0;
	}

	if (capped < numBytes)
	{
		WriteOutput(state, (UINT8 *)data + capped, numBytes - capped);
	}
}

// ============================================================
//  JPEG marker writing
// ============================================================

/**
 * @brief Write a DQT (Define Quantization Table) marker
 *
 * @param state Encoder state
 * @param matrix 64-byte quantization table in zig-zag order
 * @param id Table destination identifier (0 or 1)
 *
 * @see ITU-T T.81 B.2.4.1 — Quantization table-specification syntax
 */
static VOID WriteDQT(EncoderState *state, const UINT8 *matrix, UINT8 id)
{
	UINT16 marker = ByteOrder::Swap16(0xFFDB);
	WriteOutput(state, &marker, sizeof(UINT16));
	UINT16 len = ByteOrder::Swap16(0x0043); // 2 + 1 + 64 = 67
	WriteOutput(state, &len, sizeof(UINT16));
	UINT8 precisionAndId = id;
	WriteOutput(state, &precisionAndId, sizeof(UINT8));
	WriteOutput(state, (PVOID)matrix, 64);
}

/**
 * @brief Write a DHT (Define Huffman Table) marker
 *
 * @param state Encoder state
 * @param bits BITS array (16 entries: count of codes per length)
 * @param vals HUFFVAL array (symbol values)
 * @param htClass DC (0) or AC (1) table class
 * @param id Table destination identifier
 *
 * @see ITU-T T.81 B.2.4.2 — Huffman table-specification syntax
 */
static VOID WriteDHT(EncoderState *state, const UINT8 *bits, const UINT8 *vals,
					 HuffmanTableClass htClass, UINT8 id)
{
	INT32 numValues = 0;
	for (INT32 i = 0; i < 16; ++i)
		numValues += bits[i];

	UINT16 marker = ByteOrder::Swap16(0xFFC4);
	UINT16 len = ByteOrder::Swap16((UINT16)(2 + 1 + 16 + numValues));
	UINT8 tcTh = (UINT8)(((UINT8)htClass << 4) | id);

	WriteOutput(state, &marker, sizeof(UINT16));
	WriteOutput(state, &len, sizeof(UINT16));
	WriteOutput(state, &tcTh, sizeof(UINT8));
	WriteOutput(state, (PVOID)bits, 16);
	WriteOutput(state, (PVOID)vals, (USIZE)numValues);
}

// ============================================================
//  Huffman code generation (ITU-T T.81 Annex C)
// ============================================================

/**
 * @brief Generate code sizes from BITS specification
 *
 * @details Implements procedure specified in ITU-T T.81 Figure C.1.
 * Generates HUFFSIZE table from the BITS counts.
 *
 * @param huffsize Output array (at least 257 entries)
 * @param bits BITS array (16 entries)
 *
 * @see ITU-T T.81 C.2 — Generation of table of Huffman code sizes
 */
static VOID GenerateCodeLengths(UINT8 huffsize[/*257*/], const UINT8 *bits)
{
	INT32 k = 0;
	for (INT32 i = 0; i < 16; ++i)
	{
		for (INT32 j = 0; j < bits[i]; ++j)
			huffsize[k++] = (UINT8)(i + 1);
		huffsize[k] = 0;
	}
}

/**
 * @brief Generate Huffman code values from code sizes
 *
 * @details Implements procedure specified in ITU-T T.81 Figure C.2.
 * Generates HUFFCODE table from HUFFSIZE.
 *
 * @param codes Output code array
 * @param huffsize Code size array (terminated by 0)
 *
 * @see ITU-T T.81 C.2 — Generation of table of Huffman codes
 */
static VOID GenerateCodes(UINT16 codes[], const UINT8 *huffsize)
{
	UINT16 code = 0;
	INT32 k = 0;
	UINT8 sz = huffsize[0];
	for (;;)
	{
		do
		{
			codes[k++] = code++;
		} while (huffsize[k] == sz);

		if (huffsize[k] == 0)
			return;

		do
		{
			code = (UINT16)(code << 1);
			++sz;
		} while (huffsize[k] != sz);
	}
}

/**
 * @brief Build extended Huffman tables for encoding
 *
 * @details Maps symbol values to their corresponding codes and sizes
 * for O(1) lookup during entropy coding.
 *
 * @param outEhuffsize Output: code size indexed by symbol value
 * @param outEhuffcode Output: code value indexed by symbol value
 * @param huffval Symbol value table
 * @param huffsize Code size table
 * @param huffcode Code value table
 * @param count Number of entries
 *
 * @see ITU-T T.81 C.2 — Ordering procedure for encoding
 */
static VOID BuildExtendedTable(UINT8 *outEhuffsize, UINT16 *outEhuffcode,
							   const UINT8 *huffval, const UINT8 *huffsize, const UINT16 *huffcode, INT32 count)
{
	for (INT32 k = 0; k < count; ++k)
	{
		UINT8 val = huffval[k];
		outEhuffcode[val] = huffcode[k];
		outEhuffsize[val] = huffsize[k];
	}
}

// ============================================================
//  Entropy coding helpers
// ============================================================

/**
 * @brief Compute variable-length integer encoding for a DCT coefficient
 *
 * @details Implements the SSSS category and additional bits encoding
 * per ITU-T T.81 Table F.1.
 *
 * @param value DCT coefficient value
 * @param out Output: out[0] = additional bits, out[1] = bit count (SSSS category)
 *
 * @see ITU-T T.81 F.1.2.1.1 — Structure of DC code table
 */
static VOID CalculateVLI(INT32 value, UINT16 out[2])
{
	INT32 absVal = value;
	if (value < 0)
	{
		absVal = -absVal;
		--value;
	}
	out[1] = 1;
	while (absVal >>= 1)
		++out[1];
	out[0] = (UINT16)(value & ((1 << out[1]) - 1));
}

/**
 * @brief Write bits to the output bitstream
 *
 * @details Maintains a 32-bit buffer, flushing complete bytes to output.
 * Inserts byte-stuffing (0x00 after 0xFF) per ITU-T T.81 B.1.1.5.
 *
 * @param state Encoder state
 * @param bitbuffer Current bit accumulator
 * @param location Current bit position in the accumulator
 * @param numBits Number of bits to write (1–16)
 * @param bits Bit values to write (right-aligned)
 *
 * @see ITU-T T.81 B.1.1.5 — Byte stuffing
 */
static VOID WriteBits(EncoderState *state, UINT32 *bitbuffer, UINT32 *location,
					  UINT16 numBits, UINT16 bits)
{
	UINT32 nloc = *location + numBits;
	*bitbuffer |= (UINT32)(bits << (32 - nloc));
	*location = nloc;
	while (*location >= 8)
	{
		UINT8 c = (UINT8)((*bitbuffer) >> 24);
		WriteOutput(state, &c, 1);
		if (c == 0xFF)
		{
			UINT8 zero = 0;
			WriteOutput(state, &zero, 1);
		}
		*bitbuffer <<= 8;
		*location -= 8;
	}
}

// ============================================================
//  DCT (Arai–Agui–Nakajima scaled algorithm)
// ============================================================

/**
 * @brief Forward 8x8 DCT using the Arai–Agui–Nakajima algorithm
 *
 * @details Performs a scaled 2D forward DCT on an 8x8 block of float samples.
 * The scaling factors are absorbed into the quantization step, so the output
 * requires division by the scaled quantization matrix rather than the standard one.
 *
 * @param data 64-element float array (8x8 block, row-major)
 *
 * @see Arai, Agui, Nakajima — Trans. IEICE E-71(11):1095, 1988
 * @see Pennebaker & Mitchell — JPEG: Still Image Data Compression Standard, Figure 4-8
 */
static VOID ForwardDCT(float *data)
{
	float tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
	float tmp10, tmp11, tmp12, tmp13;
	float z1, z2, z3, z4, z5, z11, z13;

	float c4 = F32(0x3F3504F3);	  // cos(4*pi/16) * sqrt(2) = 0.707106781
	float c6 = F32(0x3EC3EF15);	  // cos(6*pi/16) * sqrt(2) = 0.382683433
	float c2c6 = F32(0x3F0A8BD4); // cos(2*pi/16) - cos(6*pi/16) = 0.541196100
	float c2p6 = F32(0x3FA73D75); // cos(2*pi/16) + cos(6*pi/16) = 1.306562965

	// Pass 1: process rows
	float *dataptr = data;
	for (INT32 ctr = 7; ctr >= 0; ctr--)
	{
		tmp0 = dataptr[0] + dataptr[7];
		tmp7 = dataptr[0] - dataptr[7];
		tmp1 = dataptr[1] + dataptr[6];
		tmp6 = dataptr[1] - dataptr[6];
		tmp2 = dataptr[2] + dataptr[5];
		tmp5 = dataptr[2] - dataptr[5];
		tmp3 = dataptr[3] + dataptr[4];
		tmp4 = dataptr[3] - dataptr[4];

		tmp10 = tmp0 + tmp3;
		tmp13 = tmp0 - tmp3;
		tmp11 = tmp1 + tmp2;
		tmp12 = tmp1 - tmp2;

		dataptr[0] = tmp10 + tmp11;
		dataptr[4] = tmp10 - tmp11;

		z1 = (tmp12 + tmp13) * c4;
		dataptr[2] = tmp13 + z1;
		dataptr[6] = tmp13 - z1;

		tmp10 = tmp4 + tmp5;
		tmp11 = tmp5 + tmp6;
		tmp12 = tmp6 + tmp7;

		z5 = (tmp10 - tmp12) * c6;
		z2 = c2c6 * tmp10 + z5;
		z4 = c2p6 * tmp12 + z5;
		z3 = tmp11 * c4;

		z11 = tmp7 + z3;
		z13 = tmp7 - z3;

		dataptr[5] = z13 + z2;
		dataptr[3] = z13 - z2;
		dataptr[1] = z11 + z4;
		dataptr[7] = z11 - z4;

		dataptr += 8;
	}

	// Pass 2: process columns
	dataptr = data;
	for (INT32 ctr = 7; ctr >= 0; ctr--)
	{
		tmp0 = dataptr[8 * 0] + dataptr[8 * 7];
		tmp7 = dataptr[8 * 0] - dataptr[8 * 7];
		tmp1 = dataptr[8 * 1] + dataptr[8 * 6];
		tmp6 = dataptr[8 * 1] - dataptr[8 * 6];
		tmp2 = dataptr[8 * 2] + dataptr[8 * 5];
		tmp5 = dataptr[8 * 2] - dataptr[8 * 5];
		tmp3 = dataptr[8 * 3] + dataptr[8 * 4];
		tmp4 = dataptr[8 * 3] - dataptr[8 * 4];

		tmp10 = tmp0 + tmp3;
		tmp13 = tmp0 - tmp3;
		tmp11 = tmp1 + tmp2;
		tmp12 = tmp1 - tmp2;

		dataptr[8 * 0] = tmp10 + tmp11;
		dataptr[8 * 4] = tmp10 - tmp11;

		z1 = (tmp12 + tmp13) * c4;
		dataptr[8 * 2] = tmp13 + z1;
		dataptr[8 * 6] = tmp13 - z1;

		tmp10 = tmp4 + tmp5;
		tmp11 = tmp5 + tmp6;
		tmp12 = tmp6 + tmp7;

		z5 = (tmp10 - tmp12) * c6;
		z2 = c2c6 * tmp10 + z5;
		z4 = c2p6 * tmp12 + z5;
		z3 = tmp11 * c4;

		z11 = tmp7 + z3;
		z13 = tmp7 - z3;

		dataptr[8 * 5] = z13 + z2;
		dataptr[8 * 3] = z13 - z2;
		dataptr[8 * 1] = z11 + z4;
		dataptr[8 * 7] = z11 - z4;

		dataptr++;
	}
}

// ============================================================
//  MCU encoding
// ============================================================

/**
 * @brief Encode and write a single 8x8 Minimum Coded Unit
 *
 * @details Applies forward DCT, quantizes coefficients using the scaled
 * quantization matrix, then entropy-codes the DC (differential) and AC
 * (run-length) coefficients using Huffman tables.
 *
 * @param state Encoder state
 * @param mcu 64-element float array of sample values (level-shifted by -128)
 * @param qt Pre-processed quantization matrix (1/divisor values)
 * @param huffDcLen DC Huffman code sizes
 * @param huffDcCode DC Huffman code values
 * @param huffAcLen AC Huffman code sizes
 * @param huffAcCode AC Huffman code values
 * @param pred Previous DC coefficient (updated on return)
 * @param bitbuffer Bit accumulator (updated on return)
 * @param location Bit position in accumulator (updated on return)
 *
 * @see ITU-T T.81 F.1.2 — Huffman encoding procedures for DC/AC coefficients
 */
static VOID EncodeMCU(EncoderState *state, float *mcu, float *qt,
					  UINT8 *huffDcLen, UINT16 *huffDcCode,
					  UINT8 *huffAcLen, UINT16 *huffAcCode,
					  INT32 *pred, UINT32 *bitbuffer, UINT32 *location)
{
	UINT8 zigZag[64];
	InitZigZag(zigZag);

	INT32 du[64];
	float dctMcu[64];
	Memory::Copy(dctMcu, mcu, 64 * sizeof(float));

	ForwardDCT(dctMcu);

	float half = F32(0x3F000000); // 0.5f
	float bias = F32(0x44800000); // 1024.0f
	for (INT32 i = 0; i < 64; ++i)
	{
		float fval = dctMcu[i] * qt[i];
		// Floor via truncation with bias to handle negative values
		fval = fval + bias + half;
		INT32 ival = (INT32)fval;
		fval = F32(__builtin_bit_cast(UINT32, (float)ival));
		fval -= bias;
		du[zigZag[i]] = (INT32)fval;
	}

	UINT16 vli[2];

	// DC coefficient (differential encoding)
	INT32 diff = du[0] - *pred;
	*pred = du[0];
	if (diff != 0)
	{
		CalculateVLI(diff, vli);
		WriteBits(state, bitbuffer, location, huffDcLen[vli[1]], huffDcCode[vli[1]]);
		WriteBits(state, bitbuffer, location, vli[1], vli[0]);
	}
	else
	{
		WriteBits(state, bitbuffer, location, huffDcLen[0], huffDcCode[0]);
	}

	// AC coefficients (run-length encoding)
	INT32 lastNonZero = 0;
	for (INT32 i = 63; i > 0; --i)
	{
		if (du[i] != 0)
		{
			lastNonZero = i;
			break;
		}
	}

	for (INT32 i = 1; i <= lastNonZero; ++i)
	{
		INT32 zeroCount = 0;
		while (du[i] == 0)
		{
			++zeroCount;
			++i;
			if (zeroCount == 16)
			{
				// ZRL: 16 consecutive zeros
				WriteBits(state, bitbuffer, location, huffAcLen[0xF0], huffAcCode[0xF0]);
				zeroCount = 0;
			}
		}
		CalculateVLI(du[i], vli);

		UINT16 sym = (UINT16)((UINT16)zeroCount << 4) | vli[1];
		WriteBits(state, bitbuffer, location, huffAcLen[sym], huffAcCode[sym]);
		WriteBits(state, bitbuffer, location, vli[1], vli[0]);
	}

	if (lastNonZero != 63)
	{
		// EOB marker
		WriteBits(state, bitbuffer, location, huffAcLen[0], huffAcCode[0]);
	}
}

// ============================================================
//  Main encoding loop
// ============================================================

/**
 * @brief Encode all MCU blocks and write compressed scan data
 *
 * @param state Encoder state (Huffman tables and QT must be initialized)
 * @param srcData Raw pixel data
 * @param width Image width
 * @param height Image height
 * @param srcNumComponents Bytes per pixel (3 or 4)
 */
static VOID EncodeImageData(EncoderState *state, const UINT8 *srcData,
							INT32 width, INT32 height, INT32 srcNumComponents)
{
	UINT8 zigZag[64];
	InitZigZag(zigZag);

	// Build scaled quantization matrices (1/divisor for multiplication)
	// scalefactor[0] = 1, scalefactor[k] = cos(k*PI/16) * sqrt(2) for k=1..7
	// Actual divisor = 8 * scalefactor[row] * scalefactor[col] * QT[zigzag[i]]
	float aanScales[8];
	aanScales[0] = F32(0x3F800000); // 1.0f
	aanScales[1] = F32(0x3FB18A86); // 1.387039845f
	aanScales[2] = F32(0x3FA73D75); // 1.306562965f
	aanScales[3] = F32(0x3F968317); // 1.175875602f
	aanScales[4] = F32(0x3F800000); // 1.0f
	aanScales[5] = F32(0x3F49234E); // 0.785694958f
	aanScales[6] = F32(0x3F0A8BD4); // 0.541196100f
	aanScales[7] = F32(0x3E8D42AF); // 0.275899379f

	ProcessedQT pqt;
	float one = F32(0x3F800000);   // 1.0f
	float eight = F32(0x41000000); // 8.0f
	for (INT32 y = 0; y < 8; y++)
	{
		for (INT32 x = 0; x < 8; x++)
		{
			INT32 i = y * 8 + x;
			pqt.luma[i] = one / (eight * aanScales[x] * aanScales[y] * state->qtLuma[zigZag[i]]);
			pqt.chroma[i] = one / (eight * aanScales[x] * aanScales[y] * state->qtChroma[zigZag[i]]);
		}
	}

	// Write JFIF header
	{
		JFIFHeader header;
		header.SOI = ByteOrder::Swap16(0xFFD8);
		header.APP0 = ByteOrder::Swap16(0xFFE0);
		UINT16 jfifLen = sizeof(JFIFHeader) - 4; // exclude SOI & APP0 markers
		header.jfifLen = ByteOrder::Swap16(jfifLen);
		const CHAR jfifId[] = "JFIF\0";
		Memory::Copy(header.jfifId, (PCVOID)jfifId, sizeof(jfifId));
		header.version = ByteOrder::Swap16(0x0102);
		header.units = 0x01;						// dots-per-inch
		UINT16 density = ByteOrder::Swap16(0x0060); // 96 DPI
		header.xDensity = density;
		header.yDensity = density;
		header.xThumb = 0;
		header.yThumb = 0;
		WriteOutput(state, &header, sizeof(JFIFHeader));
	}

	// Write empty comment segment
	{
		CommentSegment com;
		com.com = ByteOrder::Swap16(0xFFFE);
		com.comLen = ByteOrder::Swap16(2);
		WriteOutput(state, &com, sizeof(CommentSegment));
	}

	// Write quantization tables
	WriteDQT(state, state->qtLuma, 0x00);
	WriteDQT(state, state->qtChroma, 0x01);

	// Write SOF0 frame header
	{
		FrameHeader header;
		header.SOF = ByteOrder::Swap16(0xFFC0);
		header.len = ByteOrder::Swap16(8 + 3 * 3);
		header.precision = 8;
		header.width = ByteOrder::Swap16((UINT16)width);
		header.height = ByteOrder::Swap16((UINT16)height);
		header.numComponents = 3;
		UINT8 qtSelectors[3];
		qtSelectors[0] = 0x00; // Luma uses QT 0
		qtSelectors[1] = 0x01; // Chroma uses QT 1
		qtSelectors[2] = 0x01; // Chroma uses QT 1
		for (INT32 i = 0; i < 3; ++i)
		{
			header.componentSpec[i].componentId = (UINT8)(i + 1);
			header.componentSpec[i].samplingFactors = 0x11;
			header.componentSpec[i].qt = qtSelectors[i];
		}
		WriteOutput(state, &header, sizeof(FrameHeader));
	}

	// Write Huffman tables
	WriteDHT(state, state->htBits[LumaDC], state->htVals[LumaDC], DC, 0);
	WriteDHT(state, state->htBits[LumaAC], state->htVals[LumaAC], AC, 0);
	WriteDHT(state, state->htBits[ChromaDC], state->htVals[ChromaDC], DC, 1);
	WriteDHT(state, state->htBits[ChromaAC], state->htVals[ChromaAC], AC, 1);

	// Write SOS scan header
	{
		ScanHeader header;
		header.SOS = ByteOrder::Swap16(0xFFDA);
		header.len = ByteOrder::Swap16((UINT16)(6 + sizeof(ScanComponentSpec) * 3));
		header.numComponents = 3;
		UINT8 htSelectors[3];
		htSelectors[0] = 0x00; // Luma DC uses HT 0
		htSelectors[1] = 0x11; // Luma AC uses HT 1
		htSelectors[2] = 0x11; // Chroma AC uses HT 1
		for (INT32 i = 0; i < 3; ++i)
		{
			header.componentSpec[i].componentId = (UINT8)(i + 1);
			header.componentSpec[i].dcAc = htSelectors[i];
		}
		header.first = 0;
		header.last = 63;
		header.ahAl = 0;
		WriteOutput(state, &header, sizeof(ScanHeader));
	}

	// Encode scan data: iterate over 8x8 blocks
	float duY[64];
	float duCb[64];
	float duCr[64];

	INT32 predY = 0;
	INT32 predCb = 0;
	INT32 predCr = 0;

	UINT32 bitbuffer = 0;
	UINT32 bitLocation = 0;

	// RGB-to-YCbCr conversion constants
	float kR = F32(0x3E991687);	  // 0.299f
	float kG = F32(0x3F1645A2);	  // 0.587f
	float kB = F32(0x3DE978D5);	  // 0.114f
	float kCbR = F32(0xBE2CBFB1); // -0.1687f
	float kCbG = F32(0x3EA9A027); // 0.3313f (negative in formula)
	float kCbB = F32(0x3F000000); // 0.5f
	float kCrR = F32(0x3F000000); // 0.5f
	float kCrG = F32(0x3ED65FD9); // 0.4187f (negative in formula)
	float kCrB = F32(0x3DA6809D); // 0.0813f (negative in formula)
	float f128 = F32(0x43000000); // 128.0f

	for (INT32 y = 0; y < height; y += 8)
	{
		for (INT32 x = 0; x < width; x += 8)
		{
			for (INT32 offY = 0; offY < 8; ++offY)
			{
				for (INT32 offX = 0; offX < 8; ++offX)
				{
					INT32 blockIndex = offY * 8 + offX;

					INT32 col = x + offX;
					INT32 row = y + offY;
					INT32 srcIndex = (row * width + col) * srcNumComponents;

					// Clamp to image bounds for partial blocks at edges
					if (row >= height)
						srcIndex -= (width * (row - height + 1)) * srcNumComponents;
					if (col >= width)
						srcIndex -= (col - width + 1) * srcNumComponents;

					UINT8 b = srcData[srcIndex + 2];
					UINT8 g = srcData[srcIndex + 1];
					UINT8 r = srcData[srcIndex + 0];

					float rf = (float)(INT32)r;
					float gf = (float)(INT32)g;
					float bf = (float)(INT32)b;

					duY[blockIndex] = kR * rf + kG * gf + kB * bf - f128;
					duCb[blockIndex] = kCbR * rf - kCbG * gf + kCbB * bf;
					duCr[blockIndex] = kCrR * rf - kCrG * gf - kCrB * bf;
				}
			}

			EncodeMCU(state, duY, pqt.luma,
					  state->ehuffsize[LumaDC], state->ehuffcode[LumaDC],
					  state->ehuffsize[LumaAC], state->ehuffcode[LumaAC],
					  &predY, &bitbuffer, &bitLocation);
			EncodeMCU(state, duCb, pqt.chroma,
					  state->ehuffsize[ChromaDC], state->ehuffcode[ChromaDC],
					  state->ehuffsize[ChromaAC], state->ehuffcode[ChromaAC],
					  &predCb, &bitbuffer, &bitLocation);
			EncodeMCU(state, duCr, pqt.chroma,
					  state->ehuffsize[ChromaDC], state->ehuffcode[ChromaDC],
					  state->ehuffsize[ChromaAC], state->ehuffcode[ChromaAC],
					  &predCr, &bitbuffer, &bitLocation);
		}
	}

	// Flush remaining bits (pad to byte boundary)
	if (bitLocation > 0 && bitLocation < 8)
		WriteBits(state, &bitbuffer, &bitLocation, (UINT16)(8 - bitLocation), 0);

	// Write EOI marker
	UINT16 eoi = ByteOrder::Swap16(0xFFD9);
	WriteOutput(state, &eoi, sizeof(UINT16));

	// Flush remaining output buffer
	if (state->outputBufferCount > 0)
	{
		state->writeContext.func(state->writeContext.context, state->outputBuffer, (INT32)state->outputBufferCount);
		state->outputBufferCount = 0;
	}
}

// ============================================================
//  Public API
// ============================================================

[[nodiscard]] Result<VOID, Error> JpegEncoder::Encode(
	JpegWriteFunc *func,
	PVOID context,
	INT32 quality,
	INT32 width,
	INT32 height,
	INT32 numComponents,
	Span<const UINT8> srcData)
{
	if ((numComponents != 3 && numComponents != 4) || width <= 0 || height <= 0 ||
		width > 0xFFFF || height > 0xFFFF)
	{
		return Result<VOID, Error>::Err(Error::Jpeg_InvalidParams);
	}

	if (quality < 1)
		quality = 1;
	if (quality > 100)
		quality = 100;

	// Standard luminance quantization table (ITU-T T.81 Annex K.1)
	UINT8 defaultQtLuma[64];
	{
		const UINT8 embedded[] = {
			16, 11, 10, 16, 24, 40, 51, 61,
			12, 12, 14, 19, 26, 58, 60, 55,
			14, 13, 16, 24, 40, 57, 69, 56,
			14, 17, 22, 29, 51, 87, 80, 62,
			18, 22, 37, 56, 68, 109, 103, 77,
			24, 35, 55, 64, 81, 104, 113, 92,
			49, 64, 78, 87, 103, 121, 120, 101,
			72, 92, 95, 98, 112, 100, 103, 99};
		Memory::Copy(defaultQtLuma, embedded, 64);
	}

	// Standard chrominance quantization table (ITU-T T.81 Annex K.1)
	UINT8 defaultQtChroma[64];
	{
		const UINT8 embedded[] = {
			16, 18, 24, 47, 99, 99, 99, 99,
			18, 21, 26, 66, 99, 99, 99, 99,
			24, 26, 56, 99, 99, 99, 99, 99,
			47, 66, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99,
			99, 99, 99, 99, 99, 99, 99, 99};
		Memory::Copy(defaultQtChroma, embedded, 64);
	}

	EncoderState state;
	Memory::Zero(&state, sizeof(EncoderState));

	// Scale quality factor (IJG formula)
	UINT32 qtFactor = (quality < 50) ? (5000 / (UINT32)quality) : (UINT32)(200 - quality * 2);

	// Scale quantization tables
	for (USIZE i = 0; i < 64; i++)
	{
		INT32 temp = (INT32)((defaultQtLuma[i] * qtFactor + 50) / 100);
		if (temp <= 0)
			temp = 1;
		if (temp > 255)
			temp = 255;
		state.qtLuma[i] = (UINT8)temp;
	}
	for (USIZE i = 0; i < 64; i++)
	{
		INT32 temp = (INT32)((defaultQtChroma[i] * qtFactor + 50) / 100);
		if (temp <= 0)
			temp = 1;
		if (temp > 255)
			temp = 255;
		state.qtChroma[i] = (UINT8)temp;
	}

	state.writeContext.context = context;
	state.writeContext.func = func;

	// Standard Huffman tables (ITU-T T.81 Annex K.3)

	// Chrominance AC values (K.3.3.2, Table K.6)
	const UINT8 htChromaAcVals[] = {
		0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
		0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
		0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
		0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
		0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
		0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
		0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
		0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
		0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
		0xF9, 0xFA};

	// Standard Huffman BITS tables (ITU-T T.81 Annex K.3)

	// Luminance DC BITS (K.3.3.1, Table K.3)
	UINT8 lumaDcBits[16];
	{
		const UINT8 e[] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
		Memory::Copy(lumaDcBits, e, 16);
	}

	// Luminance AC BITS (K.3.3.2, Table K.5)
	UINT8 lumaAcBits[16];
	{
		const UINT8 e[] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D};
		Memory::Copy(lumaAcBits, e, 16);
	}

	// Chrominance DC BITS (K.3.3.1, Table K.4)
	UINT8 chromaDcBits[16];
	{
		const UINT8 e[] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
		Memory::Copy(chromaDcBits, e, 16);
	}

	// Chrominance AC BITS (K.3.3.2, Table K.6)
	UINT8 chromaAcBits[16];
	{
		const UINT8 e[] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
		Memory::Copy(chromaAcBits, e, 16);
	}

	state.htBits[LumaDC] = lumaDcBits;
	state.htBits[LumaAC] = lumaAcBits;
	state.htBits[ChromaDC] = chromaDcBits;
	state.htBits[ChromaAC] = chromaAcBits;

	// Standard Huffman VALS tables (ITU-T T.81 Annex K.3)

	// DC values (K.3.3.1, Tables K.3 & K.4)
	UINT8 dcVals[12];
	{
		const UINT8 e[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
		Memory::Copy(dcVals, e, 12);
	}

	// Luminance AC values (K.3.3.2, Table K.5)
	UINT8 lumaAcVals[162];
	{
		const UINT8 e[] = {
			0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
			0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
			0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
			0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
			0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
			0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
			0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
			0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
			0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
			0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
			0xF9, 0xFA};
		Memory::Copy(lumaAcVals, e, 162);
	}

	// Chrominance AC values (K.3.3.2, Table K.6)
	UINT8 chromaAcVals[162];
	Memory::Copy(chromaAcVals, htChromaAcVals, 162);

	state.htVals[LumaDC] = dcVals;
	state.htVals[LumaAC] = lumaAcVals;
	state.htVals[ChromaDC] = dcVals;
	state.htVals[ChromaAC] = chromaAcVals;

	// Build extended Huffman tables
	INT32 tableLengths[4] = {0, 0, 0, 0};
	for (INT32 i = 0; i < 4; ++i)
		for (INT32 k = 0; k < 16; ++k)
			tableLengths[i] += state.htBits[i][k];

	UINT8 huffsize[4][257];
	UINT16 huffcode[4][256];
	for (INT32 i = 0; i < 4; ++i)
	{
		GenerateCodeLengths(huffsize[i], state.htBits[i]);
		GenerateCodes(huffcode[i], huffsize[i]);
	}
	for (INT32 i = 0; i < 4; ++i)
	{
		BuildExtendedTable(state.ehuffsize[i], state.ehuffcode[i],
						   state.htVals[i], huffsize[i], huffcode[i], tableLengths[i]);
	}

	EncodeImageData(&state, srcData.Data(), width, height, numComponents);

	return Result<VOID, Error>::Ok();
}
