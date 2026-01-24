#ifndef RTC_CHAIN_INTEROP_H
#define RTC_CHAIN_INTEROP_H

#if RTC_ENABLE_MEDIA
#include "common.hpp"
#include <map>
#include <deque>
#include <vector>
#include <chrono>
// ChainInterop will be used for storing info about TWCC packets
// RtpTwccHandler produces new entries inside ChainInterop as it is processing RTP packets.
// GCC algorithm consumes it.
namespace rtc {
	struct RTC_CPP_EXPORT BitrateStats {
		double txBitsPerSecond, rxBitsPerSecond, packetLoss;
		BitrateStats() : txBitsPerSecond(0), rxBitsPerSecond(0), packetLoss(0) {}
		BitrateStats(double tx, double rx, double loss) : txBitsPerSecond(tx), rxBitsPerSecond(rx), packetLoss(loss) {};
	};
	
	struct PacketInfo {
		bool isReceived;
		bool isSent;
		uint16_t numBytes;
		std::chrono::microseconds arrivalDuration;
		std::chrono::steady_clock::time_point departureTime;
		PacketInfo(uint16_t numBytes);
	};
	// WholeFrameInfo is an internal representation for a video frame
	class WholeFrameInfo {
		std::chrono::steady_clock::time_point time;
		uint16_t seqNumStart, seqNumEnd;
	
	public:
		WholeFrameInfo(std::chrono::steady_clock::time_point time, uint16_t seqNumStart, uint16_t seqNumEnd);
		std::chrono::steady_clock::time_point getTime() const;
		uint16_t getSeqNumStart() const;
		uint16_t getSeqNumEnd() const;
	};
	
	struct RTC_CPP_EXPORT ArrivalGroup {
		std::vector<PacketInfo> packets;
		std::chrono::microseconds arrivalTime;
		std::chrono::steady_clock::time_point departureTime;
	
		ArrivalGroup() = default;
		ArrivalGroup(const ArrivalGroup &other);
		ArrivalGroup(ArrivalGroup&& other) noexcept;
		ArrivalGroup& operator=(const ArrivalGroup& other);
		ArrivalGroup& operator=(ArrivalGroup&& other) noexcept;
		void add(PacketInfo packet);	
		void reset();
	};
	
	class RTC_CPP_EXPORT ChainInterop {
		using clock = std::chrono::steady_clock;
		std::map<uint16_t, PacketInfo> packetInfo;
		std::deque<WholeFrameInfo> wholeFrameInfo;
		// timeThreshold should be at least 1000ms.
		std::chrono::milliseconds timeThreshold;
		std::mutex mapMutex;
	
	public:
		ChainInterop(int thresholdMs);
		void addPackets(uint16_t baseSeqNum, const std::vector<uint16_t> &numBytes);
		void setSentInfo(const std::vector<uint16_t> &seqNums);
		void setSentInfo(const std::vector<uint16_t> &seqNums, const std::vector<clock::time_point> &sendTimes);
		size_t updateReceivedStatus(uint16_t baseSeqNum, const std::vector<bool> &statuses, const std::vector<std::chrono::microseconds> &arrivalTimesUs);
		BitrateStats getBitrateStats(uint16_t seqnum, uint16_t packetCount);
		void deleteOldFrames();
		size_t getNumberOfFrames() const;
		size_t getNumberOfFramesReceived() const;
		std::vector<ArrivalGroup> runArrivalGroupAccumulator(uint16_t seqnum, uint16_t numPackets);
	};
	
	std::chrono::microseconds interDepartureTimePkt(ArrivalGroup group, PacketInfo packet);
	std::chrono::microseconds interArrivalTimePkt(ArrivalGroup group, PacketInfo packet);
	std::chrono::microseconds interGroupDelayVariationPkt(ArrivalGroup group, PacketInfo packet);
} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTC_CHAIN_INTEROP_H */
