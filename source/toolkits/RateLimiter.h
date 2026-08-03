// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_RATELIMITER_H_
#define TOOLKITS_RATELIMITER_H_

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include "Common.h"

/**
 * Schedule-based rate limiter (leaky-bucket style).
 *
 * Paces average throughput to limitPerSec regardless of whether individual operations are larger
 * than the limit (e.g. a 16MiB block with a 3MiB/s limit waits ~5.3s between ops).
 * Does not allow catch-up bursts after slow I/O.
 *
 * Thread-safe: concurrent wait() callers (e.g. async S3 part uploads) reserve sequential schedule
 * slots under a mutex and sleep outside the lock.
 */
class RateLimiter
{
	private:
		uint64_t limitPerSec{0}; // limit in bytes (or other rate-limited units) per second
		std::chrono::steady_clock::time_point nextAllowedT; // earliest time next op may start
		std::mutex mutex; // protects nextAllowedT schedule updates

		/**
		 * Duration that nextSize consumes at limitPerSec.
		 * Uses ceil so we never undershoot the wait for fractional nanoseconds.
		 */
		static std::chrono::nanoseconds durationForSize(uint64_t nextSize, uint64_t limitPerSec)
		{
			// ceil(nextSize * 1e9 / limitPerSec) via 128-bit intermediate to avoid overflow
			unsigned __int128 product =
				(unsigned __int128)nextSize * (unsigned __int128)1000000000ULL;
			uint64_t nanos = (uint64_t)(product / limitPerSec);
			if(product % limitPerSec)
				nanos++;

			return std::chrono::nanoseconds(nanos);
		}

		/**
		 * Sleep until wakeT in short slices, checking for friendly interrupt.
		 *
		 * @return true if interrupted before wakeT, false if slept until wakeT (or already due).
		 */
		static bool interruptibleSleepUntil(std::chrono::steady_clock::time_point wakeT,
			std::atomic_bool& isInterruptionRequested)
		{
			const auto slice = std::chrono::milliseconds(50);

			for( ; ; )
			{
				if(isInterruptionRequested.load(std::memory_order_relaxed) )
					return true;

				const auto nowT = std::chrono::steady_clock::now();
				if(nowT >= wakeT)
					return false;

				const auto remaining = wakeT - nowT;
				std::this_thread::sleep_for(remaining < slice ? remaining : slice);
			}
		}

	// inliners
	public:

	void initStart(uint64_t limitPerSec)
	{
		std::lock_guard<std::mutex> lock(mutex);
		this->limitPerSec = limitPerSec;
		this->nextAllowedT = std::chrono::steady_clock::now();
	}

	/**
	 * Wait (sleep) if needed so that the average rate stays at limitPerSec, then reserve nextSize.
	 *
	 * Reserves the schedule slot under lock, then sleeps outside the lock until the slot start.
	 *
	 * @nextSize size of the next operation in rate-limited units (e.g. bytes)
	 * @isInterruptionRequested set by coordinator for friendly shutdown during long waits
	 * @outInterrupted set true if sleep was aborted due to interrupt; caller must abort
	 *
	 * @return true if we had to wait, false if we could proceed immediately (or on interrupt).
	 */
	bool wait(size_t nextSize, std::atomic_bool& isInterruptionRequested, bool& outInterrupted)
	{
		outInterrupted = false;

		IF_UNLIKELY(!limitPerSec || !nextSize)
			return false;

		std::chrono::steady_clock::time_point wakeT;

		{
			std::lock_guard<std::mutex> lock(mutex);

			IF_UNLIKELY(!limitPerSec)
				return false;

			auto nowT = std::chrono::steady_clock::now();

			// no catch-up burst after slow I/O
			if(nextAllowedT < nowT)
				nextAllowedT = nowT;

			wakeT = nextAllowedT;
			nextAllowedT += durationForSize(nextSize, limitPerSec);
		}

		auto nowT = std::chrono::steady_clock::now();
		if(nowT >= wakeT)
			return false;

		outInterrupted = interruptibleSleepUntil(wakeT, isInterruptionRequested);
		return true;
	}

};

#endif /* TOOLKITS_RATELIMITER_H_ */
