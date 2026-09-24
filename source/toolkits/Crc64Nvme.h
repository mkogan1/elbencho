#ifndef TOOLKITS_CRC64NVME_H_
#define TOOLKITS_CRC64NVME_H_

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * CRC-64/NVME (the "Rocksoft" polynomial 0xAD93D23594C93659, reflected, init and xorout all
 * ones), the checksum S3 exposes as x-amz-checksum-crc64nvme. Check value: crc of "123456789"
 * is 0xAE8B14860A799888.
 */
class Crc64Nvme
{
	public:
		/** @param crc previous return value to continue an incremental checksum, 0 to start. */
		static uint64_t calc(uint64_t crc, const void* data, size_t len);

		/**
		 * crc of the concatenation A||B from crc(A), crc(B) and len(B), without touching the
		 * data. This is what makes independently checksummed pieces (stripes, chunks, GPU
		 * blocks) foldable into one value.
		 */
		static uint64_t combine(uint64_t crcA, uint64_t crcB, uint64_t lenB);

		/** The S3 header encoding: base64 of the 8 checksum bytes in big-endian order. */
		static std::string toBase64(uint64_t crc);

		// building blocks shared with the CUDA implementation

		/** Slicing-by-8 lookup table, 8*256 entries: t[k*256 + b]. */
		static const uint64_t* sliceTable();

		/**
		 * GF(2) operators that advance the crc register over 2^k zero bytes, k < count, as
		 * out[k*64 + j] = image of register bit j. Applying the operators for the set bits of n
		 * advances over n zero bytes; xor with crc(B) then gives combine(crc(A), crc(B), n).
		 */
		static void shiftPowers(uint64_t* out, unsigned count);

		static constexpr unsigned MAX_SHIFT_POWERS = 64;
};

#endif /* TOOLKITS_CRC64NVME_H_ */
