// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "Common.h"
#include "Logger.h"
#include "ProgException.h"
#include "toolkits/S3RdmaTk.h"

#ifdef S3RDMA_SUPPORT

S3RdmaClient::S3RdmaClient()
{
	ops.put = putCallback;
	ops.get = getCallback;

	client = new cuObjClient(ops, CUOBJ_PROTO_RDMA_DC_V1);

	connected = client->isConnected();

	if(!connected)
	{
		delete client;
		client = NULL;

		throw ProgException("cuObject client connection failed. "
			"Check that the RDMA devices are available and that cufile.json is configured.");
	}

	LOGGER(Log_NORMAL, "S3 RDMA fabric connected (cuObject)." << std::endl);
}

/**
 * The shared process-wide client. Registration of each worker's buffer is serialized on
 * registerMutex; transfers on different registered buffers can run concurrently.
 */
S3RdmaClient& S3RdmaClient::getInstance()
{
	static S3RdmaClient instance;
	return instance;
}

S3RdmaClient::~S3RdmaClient()
{
	delete client;
}

/**
 * Register a buffer for RDMA access. Counterpart is deregisterBuf().
 *
 * @throw ProgException on registration error.
 */
void S3RdmaClient::registerBuf(void* ptr, size_t len)
{
	if(!ptr)
		throw ProgException("cuObject RDMA buffer registration failed. "
			"Pointer is null. Size: " + std::to_string(len) );

	if(len >= CUOBJ_MAX_MEMORY_REG_SIZE)
		throw ProgException("cuObject RDMA buffer registration size exceeds limit. "
			"Size: " + std::to_string(len) + "; "
			"Limit: " + std::to_string(CUOBJ_MAX_MEMORY_REG_SIZE) );

	const std::lock_guard<std::mutex> lock(registerMutex);

	cuObjErr_t registerRes = client->cuMemObjGetDescriptor(ptr, len);

	if(registerRes != CU_OBJ_SUCCESS)
		throw ProgException("cuObject RDMA buffer registration failed. "
			"Size: " + std::to_string(len) );
}

/**
 * Counterpart to registerBuf(). Errors are only logged, not thrown, because this typically runs
 * during cleanup.
 */
void S3RdmaClient::deregisterBuf(void* ptr)
{
	if(!ptr)
		return;

	const std::lock_guard<std::mutex> lock(registerMutex);

	cuObjErr_t deregisterRes = client->cuMemObjPutDescriptor(ptr);

	if(deregisterRes != CU_OBJ_SUCCESS)
		ErrLogger(Log_NORMAL) << "cuObject RDMA buffer deregistration failed." << std::endl;
}

/**
 * Max number of bytes which the cuObject lib transfers in a single callback for the given
 * registered buffer.
 */
size_t S3RdmaClient::getMaxRequestSize(void* ptr)
{
	const std::lock_guard<std::mutex> lock(registerMutex);

	ssize_t maxSize = client->cuMemObjGetMaxRequestCallbackSize(ptr);

	return (maxSize < 0) ? 0 : (size_t)maxSize;
}

std::string S3RdmaClient::getMemoryTypeStr(const void* ptr)
{
	switch(cuObjClient::getMemoryType(ptr) )
	{
		case CUOBJ_MEMORY_SYSTEM:		return "system";
		case CUOBJ_MEMORY_CUDA_MANAGED:	return "cuda_managed";
		case CUOBJ_MEMORY_CUDA_DEVICE:	return "cuda_device";
		default:						return "unknown";
	}
}

/**
 * Upload len bytes from the registered buffer ptr via RDMA. ioFunc sends the corresponding S3
 * request.
 *
 * @return number of bytes transferred or negative on error.
 */
ssize_t S3RdmaClient::put(const S3RdmaIOFunc& ioFunc, void* ptr, size_t len)
{
	return client->cuObjPut( (void*)&ioFunc, ptr, len);
}

/**
 * Download len bytes into the registered buffer ptr via RDMA. ioFunc sends the corresponding S3
 * request.
 *
 * @return number of bytes transferred or negative on error.
 */
ssize_t S3RdmaClient::get(const S3RdmaIOFunc& ioFunc, void* ptr, size_t len)
{
	return client->cuObjGet( (void*)&ioFunc, ptr, len);
}

ssize_t S3RdmaClient::putCallback(const void* handle, const char* ptr, size_t size, loff_t offset,
	const cufileRDMAInfo_t* rdmaInfo)
{
	return runCallback(handle, size, offset, rdmaInfo);
}

ssize_t S3RdmaClient::getCallback(const void* handle, char* ptr, size_t size, loff_t offset,
	const cufileRDMAInfo_t* rdmaInfo)
{
	return runCallback(handle, size, offset, rdmaInfo);
}

/**
 * Shared body of the put/get callbacks: resolve the user context back to the S3RdmaIOFunc and run
 * it with the RDMA descriptor string.
 *
 * Note: This is called by the cuObject lib, so it must not let exceptions escape.
 */
ssize_t S3RdmaClient::runCallback(const void* handle, size_t size, loff_t offset,
	const cufileRDMAInfo_t* rdmaInfo)
{
	const S3RdmaIOFunc* ioFunc = (const S3RdmaIOFunc*)cuObjClient::getCtx(handle);

	IF_UNLIKELY(!ioFunc || !rdmaInfo || !rdmaInfo->desc_str)
		return -1;

	try
	{
		return (*ioFunc)(rdmaInfo->desc_str, size, offset);
	}
	catch(const std::exception& e)
	{
		ErrLogger(Log_NORMAL) << "S3 RDMA callback failed: " << e.what() << std::endl;
		return -1;
	}
}

#else // !S3RDMA_SUPPORT

S3RdmaClient& S3RdmaClient::getInstance()
{
	throw ProgException("This executable was built without S3 RDMA (cuObject) support.");
}

#endif // S3RDMA_SUPPORT
