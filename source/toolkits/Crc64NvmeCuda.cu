#include "toolkits/Crc64NvmeCuda.h"

#include <stdexcept>
#include <string>
#include <vector>

#include "toolkits/Crc64Nvme.h"

namespace
{
	constexpr unsigned CHUNK_LEN = Crc64NvmeCuda::CHUNK_LEN;
	constexpr unsigned THREADS_PER_BLOCK = 256;
	constexpr unsigned TABLE_WORDS = 8 * 256;
	constexpr unsigned NUM_SHIFT_POWERS = 48; // shifts of up to 2^48 bytes between chunks

	inline void check(cudaError_t res, const char* what)
	{
		if(res != cudaSuccess)
			throw std::runtime_error(std::string("CUDA CRC64NVME: ") + what + ": " +
				cudaGetErrorString(res) );
	}

	__device__ __forceinline__ uint64_t matTimes(const uint64_t* m, uint64_t v)
	{
		uint64_t r = 0;

		for(unsigned j = 0; v; j++, v >>= 1)
			if(v & 1)
				r ^= m[j];

		return r;
	}

	/**
	 * Stage 1: one thread per CHUNK_LEN bytes computes the plain crc of its chunk (init and
	 * xorout applied, i.e. the value Crc64Nvme::calc(0, chunk, n) would return).
	 */
	__global__ void chunkCrcKernel(const unsigned char* __restrict__ data, size_t len,
		const uint64_t* __restrict__ table, uint64_t* __restrict__ chunkCrcs, unsigned numChunks)
	{
		__shared__ uint64_t tbl[TABLE_WORDS];

		for(unsigned i = threadIdx.x; i < TABLE_WORDS; i += blockDim.x)
			tbl[i] = table[i];

		__syncthreads();

		const unsigned c = blockIdx.x * blockDim.x + threadIdx.x;
		if(c >= numChunks)
			return;

		const size_t ofs = (size_t)c * CHUNK_LEN;
		size_t n = len - ofs;
		if(n > CHUNK_LEN)
			n = CHUNK_LEN;

		const unsigned char* p = data + ofs;
		uint64_t crc = ~0ULL;

		if( ( (uintptr_t)p & 7) == 0)
		{ // slicing-by-8 over whole words (cudaMalloc buffers are aligned, so this is the norm)
			const uint64_t* w = (const uint64_t*)p;

			for(; n >= 8; n -= 8, w++)
			{
				const uint64_t x = *w ^ crc;

				crc = tbl[7*256 + (x & 0xff)] ^
					tbl[6*256 + ( (x >> 8) & 0xff)] ^
					tbl[5*256 + ( (x >> 16) & 0xff)] ^
					tbl[4*256 + ( (x >> 24) & 0xff)] ^
					tbl[3*256 + ( (x >> 32) & 0xff)] ^
					tbl[2*256 + ( (x >> 40) & 0xff)] ^
					tbl[1*256 + ( (x >> 48) & 0xff)] ^
					tbl[0*256 + (x >> 56)];
			}

			p = (const unsigned char*)w;
		}

		for(; n; n--, p++)
			crc = tbl[(crc ^ *p) & 0xff] ^ (crc >> 8);

		chunkCrcs[c] = ~crc;
	}

	/**
	 * Stage 2: fold the chunk crcs into one. crc(A||B) = shift(crc(A), len(B)) ^ crc(B), and
	 * shift is linear, so the whole-buffer crc is the xor over chunks of each chunk crc advanced
	 * by the number of bytes that follow it. Each thread advances its own chunk, then a warp
	 * xor-reduction and one atomic per warp accumulate the result.
	 */
	__global__ void combineKernel(const uint64_t* __restrict__ chunkCrcs, unsigned numChunks,
		size_t len, const uint64_t* __restrict__ shiftPowers, uint64_t* __restrict__ result)
	{
		__shared__ uint64_t sp[NUM_SHIFT_POWERS * 64];

		for(unsigned i = threadIdx.x; i < NUM_SHIFT_POWERS * 64; i += blockDim.x)
			sp[i] = shiftPowers[i];

		__syncthreads();

		const unsigned c = blockIdx.x * blockDim.x + threadIdx.x;
		uint64_t v = 0;

		if(c < numChunks)
		{
			size_t end = (size_t)(c + 1) * CHUNK_LEN;
			if(end > len)
				end = len;

			uint64_t trailing = len - end;
			v = chunkCrcs[c];

			for(unsigned k = 0; trailing; k++, trailing >>= 1)
				if(trailing & 1)
					v = matTimes(&sp[k * 64], v);
		}

		for(int o = 16; o > 0; o >>= 1)
			v ^= __shfl_xor_sync(0xffffffff, v, o);

		if( (threadIdx.x & 31) == 0)
			atomicXor( (unsigned long long*)result, (unsigned long long)v);
	}
}

Crc64NvmeCuda::Crc64NvmeCuda(size_t maxLen)
{
	maxChunks = (maxLen + CHUNK_LEN - 1) / CHUNK_LEN;
	if(!maxChunks)
		maxChunks = 1;

	try
	{
		check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "stream create");

		check(cudaMalloc(&devTable, TABLE_WORDS * sizeof(uint64_t) ), "table alloc");
		check(cudaMemcpy(devTable, Crc64Nvme::sliceTable(), TABLE_WORDS * sizeof(uint64_t),
			cudaMemcpyHostToDevice), "table copy");

		std::vector<uint64_t> powers(NUM_SHIFT_POWERS * 64);
		Crc64Nvme::shiftPowers(powers.data(), NUM_SHIFT_POWERS);

		check(cudaMalloc(&devShiftPowers, powers.size() * sizeof(uint64_t) ),
			"shift powers alloc");
		check(cudaMemcpy(devShiftPowers, powers.data(), powers.size() * sizeof(uint64_t),
			cudaMemcpyHostToDevice), "shift powers copy");

		check(cudaMalloc(&devChunkCrcs, maxChunks * sizeof(uint64_t) ), "chunk crcs alloc");
		check(cudaMalloc(&devResult, sizeof(uint64_t) ), "result alloc");
	}
	catch(...)
	{
		freeAll();
		throw;
	}
}

Crc64NvmeCuda::~Crc64NvmeCuda()
{
	freeAll();
}

void Crc64NvmeCuda::freeAll()
{
	cudaFree(devResult);
	cudaFree(devChunkCrcs);
	cudaFree(devShiftPowers);
	cudaFree(devTable);

	if(stream)
		cudaStreamDestroy(stream);

	devResult = devChunkCrcs = devShiftPowers = devTable = nullptr;
	stream = nullptr;
}

uint64_t Crc64NvmeCuda::calc(const void* devData, size_t len)
{
	if(!len)
		return 0;

	const size_t numChunks = (len + CHUNK_LEN - 1) / CHUNK_LEN;

	if(numChunks > maxChunks)
		throw std::runtime_error("CUDA CRC64NVME: length exceeds the size this instance was "
			"created for: " + std::to_string(len) );

	const unsigned numBlocks = (numChunks + THREADS_PER_BLOCK - 1) / THREADS_PER_BLOCK;

	check(cudaMemsetAsync(devResult, 0, sizeof(uint64_t), stream), "result reset");

	chunkCrcKernel<<<numBlocks, THREADS_PER_BLOCK, 0, stream>>>(
		(const unsigned char*)devData, len, devTable, devChunkCrcs, numChunks);
	combineKernel<<<numBlocks, THREADS_PER_BLOCK, 0, stream>>>(
		devChunkCrcs, numChunks, len, devShiftPowers, devResult);

	check(cudaGetLastError(), "kernel launch");

	uint64_t result;
	check(cudaMemcpyAsync(&result, devResult, sizeof(uint64_t), cudaMemcpyDeviceToHost, stream),
		"result copy");
	check(cudaStreamSynchronize(stream), "stream sync");

	return result;
}
