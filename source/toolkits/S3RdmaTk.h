// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3RDMATK_H_
#define TOOLKITS_S3RDMATK_H_

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <sys/types.h>

/* Headers of the "S3 over RDMA" protocol extension, as implemented by NVIDIA's aws-c-s3 fork and
	by the NooBaa and Ceph RGW servers.
	Spec: https://github.com/KiranModukuri/aws-c-s3/blob/nvidia_rdma/RDMA_PROTOCOL_SPEC.md
	See also: noobaa-core/docs/design/S3-over-RDMA.md */

/* Identifies the RDMA library which produced the token. Servers which don't know the value must
	reject the token, so this effectively version-gates the descriptor format. */
#define S3RDMA_HEADER_AGENT			"x-amz-rdma-agent"
#define S3RDMA_AGENT_CUOBJ			"cuobj"
// Carries the RDMA memory descriptor (buffer address, size, keys) to the S3 server.
#define S3RDMA_HEADER_TOKEN			"x-amz-rdma-token"
/* Response headers. The reply is a status code: 2xx means the server moved the data via RDMA,
	501 means it understood the request but declined. An absent reply header means the server
	doesn't implement the protocol at all. */
#define S3RDMA_HEADER_REPLY			"x-amz-rdma-reply"
#define S3RDMA_HEADER_BYTES			"x-amz-rdma-bytes-transferred"

/**
 * Callback which issues the actual S3 request while the cuObject lib holds the registered memory
 * ready for RDMA.
 *
 * @descStr the RDMA memory descriptor string to send as S3RDMA_HEADER_TOKEN header value.
 * @size number of bytes of this chunk.
 * @offset offset of this chunk within the overall transfer (0 for single callback transfers).
 * @return number of bytes transferred or -1 on error.
 */
typedef std::function<ssize_t(const char* descStr, size_t size, loff_t offset)> S3RdmaIOFunc;

#ifdef S3RDMA_SUPPORT

#include <cuobjclient.h>

/**
 * Process-wide cuObject client for out-of-band RDMA transfer of S3 object data.
 *
 * Buffer registration is serialized on an internal mutex. put()/get() run concurrently
 * from the worker threads, each using its own registered buffer.
 *
 * The S3 control path (auth, headers, metadata) still goes through the normal AWS SDK HTTP
 * request; only the payload bytes move via RDMA between our registered buffer and the server.
 *
 * Usage: getInstance(), registerBuf() the buffer that will actually be transferred (host,
 * or GPU when --gpuids/--cuda is set), then use put()/get() with a callback which sends
 * the S3 request including the descriptor string header.
 */
class S3RdmaClient
{
	public:
		~S3RdmaClient();

		static S3RdmaClient& getInstance();

		void registerBuf(void* ptr, size_t len);
		void deregisterBuf(void* ptr);

		ssize_t put(const S3RdmaIOFunc& ioFunc, void* ptr, size_t len);
		ssize_t get(const S3RdmaIOFunc& ioFunc, void* ptr, size_t len);

		size_t getMaxRequestSize(void* ptr);
		std::string getMemoryTypeStr(const void* ptr);

		bool isConnected() const { return connected; }

	private:
		S3RdmaClient();

		cuObjClient* client{NULL}; // raw ptr because cuObjClient has no move/copy semantics
		CUObjOps_t ops;
		bool connected{false};
		std::mutex registerMutex; // serializes register/deregister on the shared client

		// trampolines handed to the cuObject lib; they resolve the ctx back to the S3RdmaIOFunc
		static ssize_t putCallback(const void* handle, const char* ptr, size_t size, loff_t offset,
			const cufileRDMAInfo_t* rdmaInfo);
		static ssize_t getCallback(const void* handle, char* ptr, size_t size, loff_t offset,
			const cufileRDMAInfo_t* rdmaInfo);
		static ssize_t runCallback(const void* handle, size_t size, loff_t offset,
			const cufileRDMAInfo_t* rdmaInfo);
};

#else // !S3RDMA_SUPPORT

/**
 * Stub for builds without S3RDMA_SUPPORT, so that call sites don't need #ifdefs around the
 * declaration of the client. Any attempt to actually use it throws, but ProgArgs rejects
 * "--s3rdma" at startup in this case, so it can never be reached.
 */
class S3RdmaClient
{
	public:
		static S3RdmaClient& getInstance();

		void registerBuf(void* ptr, size_t len) {}
		void deregisterBuf(void* ptr) {}

		ssize_t put(const S3RdmaIOFunc& ioFunc, void* ptr, size_t len) { return -1; }
		ssize_t get(const S3RdmaIOFunc& ioFunc, void* ptr, size_t len) { return -1; }

		size_t getMaxRequestSize(void* ptr) { return 0; }
		std::string getMemoryTypeStr(const void* ptr) { return "n/a"; }

		bool isConnected() const { return false; }
};

#endif // S3RDMA_SUPPORT

#endif /* TOOLKITS_S3RDMATK_H_ */
