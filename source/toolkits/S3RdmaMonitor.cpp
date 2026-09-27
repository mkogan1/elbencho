// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "Common.h"
#include "toolkits/S3RdmaMonitor.h"
#include "toolkits/S3RdmaTk.h"

#if defined(S3_SUPPORT) && defined(S3RDMA_SUPPORT)

#include <aws/core/monitoring/MonitoringFactory.h>
#include <aws/core/monitoring/MonitoringInterface.h>
#include <aws/core/utils/StringUtils.h>

#include <cstdlib>

#define S3RDMA_MONITOR_ALLOC_TAG	"S3RdmaMonitor"


namespace
{
	/* per-thread landing zone for the RDMA reply headers. sync S3 requests run the monitoring
		listener inline on the submitting thread, so this reliably belongs to the request which
		the worker just sent. */
	thread_local S3RdmaReply threadReply;

	/**
	 * Picks the RDMA reply headers off each http response.
	 *
	 * Note: This is a global listener, so it also sees requests of workers which don't use RDMA.
	 * Those responses simply carry no RDMA headers and thus reset the thread-local to "no reply".
	 */
	class S3RdmaMonitoring : public Aws::Monitoring::MonitoringInterface
	{
		public:
			void* OnRequestStarted(const Aws::String& serviceName, const Aws::String& requestName,
				const std::shared_ptr<const Aws::Http::HttpRequest>& request) const override
			{
				return NULL;
			}

			void OnRequestSucceeded(const Aws::String& serviceName, const Aws::String& requestName,
				const std::shared_ptr<const Aws::Http::HttpRequest>& request,
				const Aws::Client::HttpResponseOutcome& outcome,
				const Aws::Monitoring::CoreMetricsCollection& metricsFromCore,
				void* context) const override
			{
				grabReply(outcome);
			}

			void OnRequestFailed(const Aws::String& serviceName, const Aws::String& requestName,
				const std::shared_ptr<const Aws::Http::HttpRequest>& request,
				const Aws::Client::HttpResponseOutcome& outcome,
				const Aws::Monitoring::CoreMetricsCollection& metricsFromCore,
				void* context) const override
			{
				grabReply(outcome);
			}

			void OnRequestRetry(const Aws::String& serviceName, const Aws::String& requestName,
				const std::shared_ptr<const Aws::Http::HttpRequest>& request,
				void* context) const override {}

			void OnFinish(const Aws::String& serviceName, const Aws::String& requestName,
				const std::shared_ptr<const Aws::Http::HttpRequest>& request,
				void* context) const override {}

		private:
			static void grabReply(const Aws::Client::HttpResponseOutcome& outcome)
			{
				threadReply = S3RdmaReply(); // a retry must not inherit the previous attempt

				IF_UNLIKELY(!outcome.IsSuccess() || !outcome.GetResult() )
					return;

				const Aws::Http::HttpResponse* response = outcome.GetResult().get();

				if(!response->HasHeader(S3RDMA_HEADER_REPLY) )
					return;

				threadReply.hasReply = true;
				threadReply.statusCode =
					std::atol(response->GetHeader(S3RDMA_HEADER_REPLY).c_str() );

				if(response->HasHeader(S3RDMA_HEADER_BYTES) )
				{
					threadReply.hasNumBytes = true;
					threadReply.numBytes = Aws::Utils::StringUtils::ConvertToInt64(
						response->GetHeader(S3RDMA_HEADER_BYTES).c_str() );
				}
			}
	};

	class S3RdmaMonitoringFactory : public Aws::Monitoring::MonitoringFactory
	{
		public:
			Aws::UniquePtr<Aws::Monitoring::MonitoringInterface>
				CreateMonitoringInstance() const override
			{
				return Aws::MakeUnique<S3RdmaMonitoring>(S3RDMA_MONITOR_ALLOC_TAG);
			}
	};
};

/**
 * Add our monitoring listener to the given SDK options. Must be called before Aws::InitAPI().
 */
void S3RdmaMonitor::registerFactory(Aws::SDKOptions& sdkOptions)
{
	sdkOptions.monitoringOptions.customizedMonitoringFactory_create_fn.push_back(
		[]() { return Aws::MakeUnique<S3RdmaMonitoringFactory>(S3RDMA_MONITOR_ALLOC_TAG); } );
}

const S3RdmaReply& S3RdmaMonitor::getThreadReply()
{
	return threadReply;
}

void S3RdmaMonitor::resetThreadReply()
{
	threadReply = S3RdmaReply();
}

#endif // S3_SUPPORT && S3RDMA_SUPPORT
