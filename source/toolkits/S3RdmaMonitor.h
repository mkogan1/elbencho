// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3RDMAMONITOR_H_
#define TOOLKITS_S3RDMAMONITOR_H_

#include <cstdint>

#if defined(S3_SUPPORT) && defined(S3RDMA_SUPPORT)

#include <aws/core/Aws.h>

/**
 * The "x-amz-rdma-reply" / "x-amz-rdma-bytes-transferred" response headers of the last S3 request
 * which was sent by this thread.
 *
 * The AWS SDK result classes only expose the headers which are modeled for the respective S3 API
 * call, so the RDMA reply headers are not reachable through the outcome. They are picked up
 * through a global monitoring listener instead (see registerFactory() ) and stashed in a
 * thread-local, because the listener runs inline on the thread which sent the request.
 */
struct S3RdmaReply
{
	bool hasReply{false}; // whether the server sent an "x-amz-rdma-reply" header at all
	long statusCode{0}; // value of "x-amz-rdma-reply"; 2xx is success, 501 is "declined"
	bool hasNumBytes{false}; // whether the server sent an "x-amz-rdma-bytes-transferred" header
	uint64_t numBytes{0}; // value of "x-amz-rdma-bytes-transferred"

	/**
	 * @return true if the server confirmed that it moved the data via RDMA.
	 */
	bool isRdmaSuccess() const
		{ return hasReply && (statusCode >= 200) && (statusCode < 300); }

	/**
	 * @return true if the server understood the protocol but refused to use RDMA for this request.
	 */
	bool isRdmaDeclined() const
		{ return hasReply && !isRdmaSuccess(); }
};

namespace S3RdmaMonitor
{
	void registerFactory(Aws::SDKOptions& sdkOptions);

	const S3RdmaReply& getThreadReply();
	void resetThreadReply();
};

#endif // S3_SUPPORT && S3RDMA_SUPPORT

#endif /* TOOLKITS_S3RDMAMONITOR_H_ */
