// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3RDMATK_H_
#define TOOLKITS_S3RDMATK_H_

#include "toolkits/S3RdmaProtocol.h"

#if defined(S3_SUPPORT) && defined(CUOBJ_SUPPORT)

#include <cstdint>
#include <memory>
#include <string>

#include "toolkits/CuObjClientTk.h"

class ProgArgs;

/**
 * Per-call context for an RDMA PUT/GET control-plane request.
 */
struct S3RdmaClientCtx
{
	std::string bucket;
	std::string object;
	std::string uploadID; // empty for single-shot (non-multipart)
	uint32_t partNumber = 0; // 1..10000 when uploadID is set
	std::string etag; // populated on success
	/* CRC-64/NVME as S3 encodes it (base64 of 8 big-endian bytes). rdmaPut sends it as
		x-amz-checksum-crc64nvme when set, so the server verifies the RDMA-read payload and stores
		it; rdmaGet asks for the stored value (x-amz-checksum-mode: ENABLED) when checksumMode is
		set and returns it here (empty if the object has none). */
	std::string checksumCrc64nvme;
	bool checksumMode = false;
	/* set by the caller when the GET covers the whole object: rdmaGet then sends no Range header,
		which is what lets the server return the stored full-object checksum */
	bool wholeObject = false;
};

/**
 * S3 RDMA control plane: issues the body-less, RDMA-token-carrying GET/PUT that
 * negotiates the out-of-band transfer, using the AWS SDK's low-level HTTP client
 * and a manual SigV4 signer. Endpoint, region, credentials and addressing style
 * are resolved from ProgArgs the same way as S3Tk::initS3Client(), so a worker's
 * control plane targets the same endpoint as its regular S3 client.
 */
class S3RdmaControlPlane
{
	public:
		S3RdmaControlPlane(const ProgArgs* progArgs, size_t workerRank);
		~S3RdmaControlPlane();

		bool isValid() const { return valid; }

		/**
		 * Issue the signed control-plane PUT carrying the RDMA token.
		 * @return bytes transferred (>0) on RDMA success, RDMA_NOT_SUPPORTED if the
		 *     server declined, or RDMA_ERROR on transport failure.
		 */
		ssize_t rdmaPut(S3RdmaClientCtx& ctx, const char* token, uint64_t size);

		/**
		 * Issue the signed control-plane GET carrying the RDMA token. When @offset
		 * is non-zero a byte-range request is made (server replies 206).
		 * @return bytes transferred (>0), RDMA_NOT_SUPPORTED if declined, or
		 *     RDMA_ERROR on failure.
		 */
		ssize_t rdmaGet(S3RdmaClientCtx& ctx, const char* token, uint64_t size,
			uint64_t offset);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
		bool valid = false;
};

/**
 * Mint a token, run rdmaPut, release the token, with one transient retry. The
 * buffer must already be registered via SharedCuObjClient::registerBuffer().
 *
 * @return >0 bytes transferred (success), RDMA_NOT_SUPPORTED (server declined),
 *     or RDMA_ERROR (failure). There is no HTTP fallback.
 */
ssize_t rdmaPutWithRetry(SharedCuObjClient& rdma, S3RdmaControlPlane& cp, S3RdmaClientCtx& ctx,
	void* buf, size_t size);

ssize_t rdmaGetWithRetry(SharedCuObjClient& rdma, S3RdmaControlPlane& cp, S3RdmaClientCtx& ctx,
	void* buf, size_t size, size_t offset);

#endif // S3_SUPPORT && CUOBJ_SUPPORT

#endif // TOOLKITS_S3RDMATK_H_
