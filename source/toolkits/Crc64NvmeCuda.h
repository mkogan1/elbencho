#ifndef TOOLKITS_CRC64NVMECUDA_H_
#define TOOLKITS_CRC64NVMECUDA_H_

#include <cstddef>
#include <cstdint>
#include <cuda_runtime.h>

/**
 * CRC-64/NVME over a buffer in GPU memory, computed on the GPU so data that landed in VRAM
 * (GPU-direct RDMA delivery) can be verified against x-amz-checksum-crc64nvme without a copy
 * back to the host. Returns the same value as Crc64Nvme::calc(0, hostCopy, len).
 *
 * One instance per worker thread, created after cudaSetDevice() selected the worker's GPU.
 * Not thread-safe (each call uses the instance's own stream and scratch memory).
 */
class Crc64NvmeCuda
{
	public:
		/**
		 * @param maxLen largest buffer length calc() will be called with (sizes the scratch
		 * 		space for the per-chunk checksums).
		 * @throw std::runtime_error on CUDA errors.
		 */
		explicit Crc64NvmeCuda(size_t maxLen);
		~Crc64NvmeCuda();

		Crc64NvmeCuda(const Crc64NvmeCuda&) = delete;
		Crc64NvmeCuda& operator=(const Crc64NvmeCuda&) = delete;

		/**
		 * Blocking checksum of len bytes at devData (a device pointer).
		 *
		 * @throw std::runtime_error on CUDA errors or len > maxLen.
		 */
		uint64_t calc(const void* devData, size_t len);

		static constexpr unsigned CHUNK_LEN = 1024; // bytes checksummed per GPU thread

	private:
		cudaStream_t stream{nullptr};
		uint64_t* devTable{nullptr}; // slicing-by-8 table, 8*256 words
		uint64_t* devShiftPowers{nullptr}; // NUM_SHIFT_POWERS*64 words
		uint64_t* devChunkCrcs{nullptr}; // maxChunks words
		uint64_t* devResult{nullptr}; // 1 word
		size_t maxChunks{0};

		void freeAll();
};

#endif /* TOOLKITS_CRC64NVMECUDA_H_ */
