#include "toolkits/Crc64Nvme.h"

#include <cstring>

namespace
{
	// reflected form of the NVME polynomial
	constexpr uint64_t POLY_REFLECTED = 0x9A6C9329AC4BC9B5ULL;

	/* slicing-by-8: table[k][b] is the crc contribution of byte b when it sits k bytes before the
		end of an 8-byte word, so one word costs eight table lookups and no per-byte dependency */
	struct Table
	{
		uint64_t t[8][256];

		Table()
		{
			for(unsigned i = 0; i < 256; i++)
			{
				uint64_t c = i;
				for(int k = 0; k < 8; k++)
					c = (c & 1) ? (c >> 1) ^ POLY_REFLECTED : (c >> 1);
				t[0][i] = c;
			}

			for(unsigned i = 0; i < 256; i++)
				for(int k = 1; k < 8; k++)
					t[k][i] = t[0][t[k-1][i] & 0xff] ^ (t[k-1][i] >> 8);
		}
	};

	const Table& table()
	{
		static const Table instance;
		return instance;
	}

	// GF(2) 64x64 matrix as 64 column images; times() applies it to a register value
	inline uint64_t matTimes(const uint64_t* m, uint64_t v)
	{
		uint64_t r = 0;

		for(unsigned j = 0; v; j++, v >>= 1)
			if(v & 1)
				r ^= m[j];

		return r;
	}

	inline void matSquare(uint64_t* out, const uint64_t* m)
	{
		for(unsigned j = 0; j < 64; j++)
			out[j] = matTimes(m, m[j]);
	}

	// operator for 2^k zero bytes, k < 64 (2^63 bytes is more than any object)
	struct ShiftPowers
	{
		uint64_t p[Crc64Nvme::MAX_SHIFT_POWERS][64];

		ShiftPowers()
		{
			const Table& tbl = table();

			for(unsigned j = 0; j < 64; j++)
			{ // one zero byte through the register: what the byte loop in calc() does
				uint64_t v = 1ULL << j;
				p[0][j] = tbl.t[0][v & 0xff] ^ (v >> 8);
			}

			for(unsigned k = 1; k < Crc64Nvme::MAX_SHIFT_POWERS; k++)
				matSquare(p[k], p[k-1]);
		}
	};

	const ShiftPowers& shiftPowersInstance()
	{
		static const ShiftPowers instance;
		return instance;
	}
}

uint64_t Crc64Nvme::calc(uint64_t crc, const void* data, size_t len)
{
	const Table& tbl = table();
	const unsigned char* p = static_cast<const unsigned char*>(data);

	crc = ~crc;

	while(len >= 8)
	{
		uint64_t w;
		std::memcpy(&w, p, 8); // little-endian hosts only, as elbencho's other checksums assume
		w ^= crc;

		crc = tbl.t[7][w & 0xff] ^
			tbl.t[6][(w >> 8) & 0xff] ^
			tbl.t[5][(w >> 16) & 0xff] ^
			tbl.t[4][(w >> 24) & 0xff] ^
			tbl.t[3][(w >> 32) & 0xff] ^
			tbl.t[2][(w >> 40) & 0xff] ^
			tbl.t[1][(w >> 48) & 0xff] ^
			tbl.t[0][w >> 56];

		p += 8;
		len -= 8;
	}

	while(len--)
		crc = tbl.t[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);

	return ~crc;
}

/**
 * The init/xorout inversions cancel out, so this works directly on final crc values:
 * crc(A||B) = shift(crc(A), len(B)) ^ crc(B), exactly as zlib's crc32_combine does.
 */
uint64_t Crc64Nvme::combine(uint64_t crcA, uint64_t crcB, uint64_t lenB)
{
	const ShiftPowers& sp = shiftPowersInstance();

	for(unsigned k = 0; lenB; k++, lenB >>= 1)
		if(lenB & 1)
			crcA = matTimes(sp.p[k], crcA);

	return crcA ^ crcB;
}

const uint64_t* Crc64Nvme::sliceTable()
{
	return &table().t[0][0];
}

void Crc64Nvme::shiftPowers(uint64_t* out, unsigned count)
{
	const ShiftPowers& sp = shiftPowersInstance();

	if(count > MAX_SHIFT_POWERS)
		count = MAX_SHIFT_POWERS;

	std::memcpy(out, sp.p, count * 64 * sizeof(uint64_t) );
}

std::string Crc64Nvme::toBase64(uint64_t crc)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	unsigned char bytes[8];
	for(int i = 7; i >= 0; i--)
	{
		bytes[i] = crc & 0xff;
		crc >>= 8;
	}

	// 8 bytes -> 2 full groups of 3 plus 2 remaining bytes -> 12 chars with one '=' pad
	std::string out;
	out.reserve(12);

	for(int i = 0; i < 6; i += 3)
	{
		uint32_t v = (bytes[i] << 16) | (bytes[i+1] << 8) | bytes[i+2];
		out += alphabet[(v >> 18) & 63];
		out += alphabet[(v >> 12) & 63];
		out += alphabet[(v >> 6) & 63];
		out += alphabet[v & 63];
	}

	uint32_t v = (bytes[6] << 16) | (bytes[7] << 8);
	out += alphabet[(v >> 18) & 63];
	out += alphabet[(v >> 12) & 63];
	out += alphabet[(v >> 6) & 63];
	out += '=';

	return out;
}
