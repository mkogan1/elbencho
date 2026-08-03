// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3TK_H_
#define TOOLKITS_S3TK_H_

#include "PathStore.h"
#include "workers/WorkerException.h"

#ifdef S3_SUPPORT
	#include <atomic>
	#include <cstring>
	#include <streambuf>

	#include <aws/core/Aws.h>
    #include <aws/core/client/ClientConfiguration.h>
    #include <aws/core/utils/HashingUtils.h>
	#include <aws/core/utils/memory/AWSMemory.h>
	#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
    #include <aws/core/VersionConfig.h>
	#include INCLUDE_AWS_S3(model/CompleteMultipartUploadRequest.h)

	#include "toolkits/RateLimiter.h"

    #ifdef S3_AWSCRT
        #include INCLUDE_AWS_S3(S3CrtClient.h)

        namespace S3 = Aws::S3Crt::Model;
        using S3Client = Aws::S3Crt::S3CrtClient;
        using S3Errors = Aws::S3Crt::S3CrtErrors;
        using S3ErrorType = Aws::S3Crt::S3CrtError;
        using S3ClientConfiguration = Aws::S3Crt::ClientConfiguration;
        using S3ChecksumAlgorithm = S3::ChecksumAlgorithm;
        namespace S3ChecksumAlgorithmMapper = S3::ChecksumAlgorithmMapper;
    #else
        #include INCLUDE_AWS_S3(S3Client.h)

        namespace S3 = Aws::S3::Model;
        using S3Client = Aws::S3::S3Client;
        using S3Errors = Aws::S3::S3Errors;
        using S3ErrorType = Aws::S3::S3Error;
        using S3ClientConfiguration = Aws::Client::ClientConfiguration;
        using S3ChecksumAlgorithm = S3::ChecksumAlgorithm;
        namespace S3ChecksumAlgorithmMapper = S3::ChecksumAlgorithmMapper;
    #endif // S3_AWSCRT

    /* calc a comparable integer value for a specific AWS SDK CPP version; we use multipliers
        (1,000,000 and 10,000) to ensure the patch/minor numbers don't overlap. */
    #define AWS_VER_CALC(maj, min, pat) ( (maj * 1000000) + (min * 10000) + (pat) )

    // Calculate the integer value of the CURRENTLY installed SDK
    #define AWS_CURRENT_VER_VAL AWS_VER_CALC( \
        AWS_SDK_VERSION_MAJOR, AWS_SDK_VERSION_MINOR, AWS_SDK_VERSION_PATCH)

    /* AWS SDK CPP version check macro: Returns true (1) if current version >= required version,
        e.g. "#if !AWS_SDK_AT_LEAST(1, 11, 708)" */
    #define AWS_SDK_AT_LEAST(req_maj, req_min, req_pat) \
        (AWS_CURRENT_VER_VAL >= AWS_VER_CALC(req_maj, req_min, req_pat))

	/**
	 * In-memory streambuf for S3 upload bodies. Optionally paces reads via RateLimiter in 64 KiB
	 * chunks so --limitwrite shapes wire traffic during a part, not only between parts.
	 */
	class S3PacedStreamBuf : public std::streambuf
	{
		public:
			static constexpr size_t PACE_CHUNK_SIZE = 64 * 1024;

			S3PacedStreamBuf(unsigned char* buf, uint64_t bufLen,
				RateLimiter* rateLimiter = nullptr,
				std::atomic_bool* isInterruptionRequested = nullptr) :
				bufBegin( (char*)buf),
				bufEnd( (char*)buf + bufLen),
				cursorPos( (char*)buf),
				dataSize(bufLen),
				rateLimiter(rateLimiter),
				isInterruptionRequested(isInterruptionRequested)
			{
				// empty get/put areas; underflow/xsgetn and xsputn/overflow handle data transfers
				setg(nullptr, nullptr, nullptr);
				setp(nullptr, nullptr);
			}

		protected:
			char* dataEndPtr() const
			{
				char* ptr = bufBegin + dataSize;
				return (ptr < bufEnd) ? ptr : bufEnd;
			}

			bool paceAndWait(size_t chunk)
			{
				IF_UNLIKELY(!rateLimiter || !chunk)
					return true;

				std::atomic_bool dummyInterrupt{false};
				std::atomic_bool& interruptFlag = isInterruptionRequested ?
					*isInterruptionRequested : dummyInterrupt;
				bool interrupted = false;

				rateLimiter->wait(chunk, interruptFlag, interrupted);
				return !interrupted;
			}

			std::streamsize xsgetn(char* dest, std::streamsize n) override
			{
				IF_UNLIKELY(n <= 0 || !dest)
					return 0;

				std::streamsize totalCopied = 0;

				// drain already-paced get area first
				if(gptr() && gptr() < egptr() )
				{
					const std::streamsize avail = egptr() - gptr();
					const std::streamsize take = (avail < n) ? avail : n;
					std::memcpy(dest, gptr(), (size_t)take);
					gbump( (int)take);
					totalCopied += take;
				}

				while(totalCopied < n && cursorPos < dataEndPtr() )
				{
					size_t remainingInBuf = (size_t)(dataEndPtr() - cursorPos);
					size_t remainingRequest = (size_t)(n - totalCopied);
					size_t chunk = remainingInBuf < remainingRequest ?
						remainingInBuf : remainingRequest;

					if(rateLimiter && chunk > PACE_CHUNK_SIZE)
						chunk = PACE_CHUNK_SIZE;

					IF_UNLIKELY(!paceAndWait(chunk) )
						return totalCopied; // short read; HTTP continue-handler also aborts

					std::memcpy(dest + totalCopied, cursorPos, chunk);
					cursorPos += chunk;
					totalCopied += (std::streamsize)chunk;
				}

				return totalCopied;
			}

			int_type underflow() override
			{
				if(gptr() && gptr() < egptr() )
					return traits_type::to_int_type(*gptr() );

				if(cursorPos >= dataEndPtr() )
					return traits_type::eof();

				// deliver one paced chunk via get area pointing into the source buffer
				size_t remainingInBuf = (size_t)(dataEndPtr() - cursorPos);
				size_t chunk = remainingInBuf;

				if(rateLimiter && chunk > PACE_CHUNK_SIZE)
					chunk = PACE_CHUNK_SIZE;

				IF_UNLIKELY(!paceAndWait(chunk) )
					return traits_type::eof();

				char* chunkEnd = cursorPos + chunk;
				setg(cursorPos, cursorPos, chunkEnd);
				cursorPos = chunkEnd;

				return traits_type::to_int_type(*gptr() );
			}

			std::streamsize xsputn(const char* src, std::streamsize n) override
			{
				IF_UNLIKELY(n <= 0 || !src)
					return 0;

				if(cursorPos >= bufEnd)
					return 0;

				const std::streamsize writable = bufEnd - cursorPos;
				const std::streamsize toWrite = (n < writable) ? n : writable;

				std::memcpy(cursorPos, src, (size_t)toWrite);
				cursorPos += toWrite;

				if(cursorPos > dataEndPtr() )
					dataSize = (size_t)(cursorPos - bufBegin);

				return toWrite;
			}

			int_type overflow(int_type ch = traits_type::eof() ) override
			{
				if(traits_type::eq_int_type(ch, traits_type::eof() ) )
					return traits_type::not_eof(ch);

				if(cursorPos >= bufEnd)
					return traits_type::eof();

				*cursorPos = traits_type::to_char_type(ch);
				cursorPos++;

				if(cursorPos > dataEndPtr() )
					dataSize = (size_t)(cursorPos - bufBegin);

				return ch;
			}

			pos_type seekoff(off_type off, std::ios_base::seekdir dir,
				std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override
			{
				if( !(which & (std::ios_base::in | std::ios_base::out) ) )
					return pos_type(off_type(-1) );

				char* newPos = nullptr;

				switch(dir)
				{
					case std::ios_base::beg:
						newPos = bufBegin + off;
						break;
					case std::ios_base::cur:
					{
						// prefer logical position: if get area active use gptr, else bufPos
						char* cur = (gptr() && gptr() >= eback() && gptr() <= egptr() ) ?
							gptr() : cursorPos;
						newPos = cur + off;
					} break;
					case std::ios_base::end:
						newPos = ( (which & std::ios_base::out) ? bufEnd : dataEndPtr() ) + off;
						break;
					default:
						return pos_type(off_type(-1) );
				}

				if(newPos < bufBegin || newPos > bufEnd)
					return pos_type(off_type(-1) );

				cursorPos = newPos;
				setg(nullptr, nullptr, nullptr);
				return pos_type(newPos - bufBegin);
			}

			pos_type seekpos(pos_type pos,
				std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override
			{
				return seekoff(off_type(pos), std::ios_base::beg, which);
			}

			std::streamsize showmanyc() override
			{
				char* endPtr = dataEndPtr();
				return (cursorPos < endPtr) ? (std::streamsize)(endPtr - cursorPos) : 0;
			}

		private:
			char* bufBegin;
			char* bufEnd;
			char* cursorPos; // current read/write cursor
			size_t dataSize; // initialized readable size, extended by writes
			RateLimiter* rateLimiter;
			std::atomic_bool* isInterruptionRequested;
	};

    /**
     * Aws::IOStream derived in-memory stream implementation for S3 object upload/download. The
     * actual in-memory part comes from the streambuf that gets provided to the constructor.
	 *
	 * Optional RateLimiter paces body reads during upload for mid-transfer --limitwrite shaping.
     */
    class S3MemoryStream : public Aws::IOStream
    {
        public:
            S3MemoryStream(unsigned char* buf, uint64_t bufLen,
				RateLimiter* rateLimiter = nullptr,
				std::atomic_bool* isInterruptionRequested = nullptr) :
                Aws::IOStream(&staticZeroStreamBuf),
				streamBuf(buf, bufLen, rateLimiter, isInterruptionRequested)
            {
                /* staticZeroStreamBuf was only because base class needs to be init'ed before our
                    streamBuf, so immediately replace with actual streamBuf now that it's ready */
                rdbuf(&streamBuf);
            }

            virtual ~S3MemoryStream() = default;

        private:
            static std::stringbuf staticZeroStreamBuf; /* only for first
                init of std::iostream because that needs to be done before streamBuf init, but
                will be replaced immediately after streamBuf is initialized */

			S3PacedStreamBuf streamBuf;
    };


#endif // S3_SUPPORT


class ProgArgs; // forward declaration


class S3Tk
{
	public:
		static void initS3Global(const ProgArgs* progArgs);
		static void uninitS3Global(const ProgArgs* progArgs);

#ifdef S3_SUPPORT
        static std::shared_ptr<S3Client> initS3Client(
            const ProgArgs* progArgs, size_t workerRank =
                std::chrono::system_clock::now().time_since_epoch().count(),
                std::atomic_bool* isInterruptionRequested = NULL,
                std::string* outS3EndpointStr = NULL);
        static Aws::String computeKeyMD5(const Aws::String& key);
        static void scanCustomTree(const ProgArgs* progArgs, std::shared_ptr<S3Client> s3Client,
            std::string bucketName, std::string objectPrefix, std::string outTreeFilePath);
        static void precreateMpuIDs(const ProgArgs* progArgs, std::shared_ptr<S3Client> s3Client,
            std::string bucketName, std::string objectPrefix, const PathList& pathList,
            StringVec& outMpuIDs);

#endif // S3_SUPPORT

	private:

#ifdef S3_SUPPORT
		static bool globalInitCalled; // to make uninit a no-op if init wasn't called

		static Aws::SDKOptions* s3SDKOptions; // needed for init and again for uninit later

    // inliners
    public:
        /**
         * Calculate checksum for S3 requests for which automatic checksum calculation is not
         * supported by the AWS SDK CPP. This applies IOStream-based requests like PutObjectRequest
         * and UploadPartRequest. For UploadPartRequest, the same checksum also needs to be given in
         * the completion request.
         *
         * @param request the s3 request to which the checksum will be added.
         * @param completedPart completion object for uploadPartRequest; NULL if this is not a
         *      UploadPartRequest.
         * @param s3ChecksumAlgorithm algorithm to add and calculate; this function will be a no-op
         *      if value is S3ChecksumAlgorithm::NOT_SET.
         * @param buf the request buffer for which to calculate the checksum.
         * @param bufLen length of buf in bytes.
         */
        template <typename REQUESTTYPE>
        static void addUploadPartRequestChecksum(REQUESTTYPE& request,
            S3::CompletedPart* completedPart, S3ChecksumAlgorithm s3ChecksumAlgorithm,
            unsigned char* buf, uint64_t bufLen)
        {
            if(s3ChecksumAlgorithm == S3ChecksumAlgorithm::NOT_SET)
                return; // nothing to do

			// unmetered stream: checksum must not sleep under --limitwrite
            S3MemoryStream memStream(buf, bufLen);

            switch(s3ChecksumAlgorithm)
            {
                case S3ChecksumAlgorithm::CRC32:
                {
                    Aws::String checksumStr = Aws::Utils::HashingUtils::Base64Encode(
                        Aws::Utils::HashingUtils::CalculateCRC32(memStream) );
                    request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
                    request.SetChecksumCRC32(checksumStr);

                    if(completedPart != NULL)
                        completedPart->SetChecksumCRC32(checksumStr);
                } break;
                case S3ChecksumAlgorithm::CRC32C:
                {
                    Aws::String checksumStr = Aws::Utils::HashingUtils::Base64Encode(
                        Aws::Utils::HashingUtils::CalculateCRC32C(memStream) );
                    request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
                    request.SetChecksumCRC32C(checksumStr);

                    if(completedPart != NULL)
                        completedPart->SetChecksumCRC32C(checksumStr);
                } break;
                case S3ChecksumAlgorithm::SHA1:
                {
                    Aws::String checksumStr = Aws::Utils::HashingUtils::Base64Encode(
                        Aws::Utils::HashingUtils::CalculateSHA1(memStream) );
                    request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
                    request.SetChecksumSHA1(checksumStr);
                    if(completedPart != NULL)
                        completedPart->SetChecksumSHA1(checksumStr);
                } break;
                case S3ChecksumAlgorithm::SHA256:
                {
                    Aws::String checksumStr = Aws::Utils::HashingUtils::Base64Encode(
                        Aws::Utils::HashingUtils::CalculateSHA256(memStream) );
                    request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
                    request.SetChecksumSHA256(checksumStr);
                    if(completedPart != NULL)
                        completedPart->SetChecksumSHA256(checksumStr);
                } break;

                default:
                    throw WorkerException(std::string(
                        "Invalid S3 request checksum algorithm value: ") +
                        std::to_string( (unsigned) s3ChecksumAlgorithm) );

            } // end of switch()
        }

#endif // S3_SUPPORT
};

#endif /* TOOLKITS_S3TK_H_ */
