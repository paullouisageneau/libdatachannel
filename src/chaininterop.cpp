#if RTC_ENABLE_MEDIA

#include "chaininterop.hpp"
#include <cmath>
#include <utility>

namespace rtc {

PacketInfo::PacketInfo(uint16_t numBytes)
    : isReceived(false), isSent(false), numBytes(numBytes) {}

WholeFrameInfo::WholeFrameInfo(std::chrono::steady_clock::time_point time, uint16_t seqNumStart, uint16_t seqNumEnd) 
	: time(time), seqNumStart(seqNumStart), seqNumEnd(seqNumEnd) {}

std::chrono::steady_clock::time_point WholeFrameInfo::getTime() const { return time; }

uint16_t WholeFrameInfo::getSeqNumStart() const { return seqNumStart; }

uint16_t WholeFrameInfo::getSeqNumEnd() const { return seqNumEnd; }

ArrivalGroup::ArrivalGroup(const ArrivalGroup &other) : packets(other.packets),
										  arrivalTime(other.arrivalTime),
										  departureTime(other.departureTime) {}
ArrivalGroup::ArrivalGroup(ArrivalGroup&& other) noexcept : packets(std::move(other.packets)),
											  arrivalTime(std::move(other.arrivalTime)),
											  departureTime(std::move(other.departureTime)) {}
ArrivalGroup& ArrivalGroup::operator=(const ArrivalGroup& other) {
	if (this != &other) {
		packets = other.packets;
		arrivalTime = other.arrivalTime;
		departureTime = other.departureTime;
	}
	return *this;
}

ArrivalGroup& ArrivalGroup::operator=(ArrivalGroup&& other) noexcept {
	if (this != &other) {
		packets = std::move(other.packets);
		arrivalTime = std::move(other.arrivalTime);
		departureTime = std::move(other.departureTime);
		other.reset();
	}
	return *this;
}

void ArrivalGroup::add(PacketInfo packet) {
	packets.push_back(packet);
	arrivalTime = packet.arrivalDuration;
	departureTime = packet.departureTime;
}

void ArrivalGroup::reset() {
	packets.clear();
	arrivalTime = std::chrono::microseconds::zero();
	departureTime = std::chrono::steady_clock::time_point();
}

ChainInterop::ChainInterop(int thresholdMs) {
	timeThreshold = std::chrono::milliseconds(thresholdMs);
}

void ChainInterop::addPackets(uint16_t baseSeqNum, const std::vector<uint16_t> &numBytes) {
	std::unique_lock<std::mutex> guard(mapMutex);
	uint16_t numPackets = (uint16_t) numBytes.size();
	wholeFrameInfo.emplace_back(clock::now(), baseSeqNum, baseSeqNum + numPackets);
	for (const auto& bytes : numBytes) {
		packetInfo.emplace(std::make_pair(baseSeqNum, PacketInfo(bytes)));
		baseSeqNum++;
	}
}

void ChainInterop::setSentInfo(const std::vector<uint16_t> &seqNums) {
	std::unique_lock<std::mutex> guard(mapMutex);
	clock::time_point now = std::chrono::steady_clock::now();
	for (const auto &seqNum : seqNums) {
		packetInfo.at(seqNum).isSent = true;
		packetInfo.at(seqNum).departureTime = now;
	}
}

void ChainInterop::setSentInfo(const std::vector<uint16_t> &seqNums,
                               const std::vector<clock::time_point> &sendTimes) {
	std::unique_lock<std::mutex> guard(mapMutex);
	for (size_t i = 0; i < seqNums.size(); i++) {
		packetInfo.at(seqNums.at(i)).isSent = true;
		packetInfo.at(seqNums.at(i)).departureTime = sendTimes.at(i);
	}
}

size_t ChainInterop::updateReceivedStatus(
	uint16_t baseSeqNum,
	const std::vector<bool> &statuses,
	const std::vector<std::chrono::microseconds> &arrivalTimesUs) {

	uint16_t seqNum = baseSeqNum;
	size_t totalProcessedStatusCount = 0;

	if (!packetInfo.empty()) {
		size_t vectorIdx = 0;
		while (vectorIdx < statuses.size()) {
			auto it = packetInfo.find(seqNum);
			if (it != packetInfo.end()) {
				it->second.isReceived = statuses.at(vectorIdx);
				it->second.arrivalDuration = arrivalTimesUs.at(vectorIdx);
				totalProcessedStatusCount++;
			}
			vectorIdx++;
			seqNum++;
		}
	}
	return totalProcessedStatusCount;
}

BitrateStats ChainInterop::getBitrateStats(uint16_t seqnum, uint16_t packetCount) {
	if (packetInfo.empty())
		return BitrateStats();
	std::unique_lock<std::mutex> guard(mapMutex);
	const std::chrono::seconds oneSecond = std::chrono::seconds(1);
	clock::time_point timeNow = std::chrono::steady_clock::now();
	clock::time_point firstPacketTime = timeNow;

	size_t receivedBytes = 0, receivedPackets = 0, notReceivedBytes = 0;
	for (const auto &packet : packetInfo) {
		// Packets might be sent later if we use Pacer
		if (packet.second.isSent) {
			if (timeNow - packet.second.departureTime < oneSecond) {
				if (packet.second.departureTime < firstPacketTime) {
					firstPacketTime = packet.second.departureTime;
				}
				if (packet.second.isReceived) {
					receivedBytes += packet.second.numBytes;
				} else {
					notReceivedBytes += packet.second.numBytes;
				}
				
			}
		}
	}
	// Packet loss is calculated only for the reported packets
	// Due to seqnum wraparound I did not want to merge both for loops
	for (size_t i = 0; i < packetCount; i++)
	{
		auto packet = packetInfo.find(seqnum);
		if (packet != packetInfo.end() && packet->second.isReceived) {
			receivedPackets++;
		}
		seqnum++;
	}
	double elapsedSeconds =
	    (double)std::chrono::duration_cast<std::chrono::milliseconds>(timeNow - firstPacketTime).count() / 1000.0;
	double receivedBitrate = (double)receivedBytes * 8 / elapsedSeconds;
	double sentBitrate = (double)(receivedBytes + notReceivedBytes) * 8 / elapsedSeconds;
	if (!std::isfinite(receivedBitrate))
		receivedBitrate = 0;
	if (!std::isfinite(sentBitrate))
		sentBitrate = 0;
	double packetLoss = 1.0f - ((double)receivedPackets / packetCount);
	return BitrateStats(sentBitrate, receivedBitrate, packetLoss);
}

void ChainInterop::deleteOldFrames() {
	std::unique_lock<std::mutex> guard(mapMutex);
	clock::time_point now = std::chrono::steady_clock::now();

	while (!wholeFrameInfo.empty() && now - wholeFrameInfo.front().getTime() > timeThreshold) {
		uint16_t seqNum = wholeFrameInfo.front().getSeqNumStart();
		uint16_t seqNumEnd = wholeFrameInfo.front().getSeqNumEnd();
		// Due to wraparound seqNum might be greater than seqNumEnd
		while (seqNum != seqNumEnd) {
			packetInfo.erase(seqNum);
			seqNum++;
		}
		wholeFrameInfo.pop_front();
	}
}

size_t ChainInterop::getNumberOfFrames() const { return wholeFrameInfo.size(); }

size_t ChainInterop::getNumberOfFramesReceived() const {
	size_t nReceived = 0;
	bool frameReceived = true;
	for (const auto &frame : wholeFrameInfo) {
		uint16_t seqNum = frame.getSeqNumStart();
		uint16_t seqNumEnd = frame.getSeqNumEnd();
		while (seqNum < seqNumEnd) {
			frameReceived = frameReceived && packetInfo.at(seqNum).isReceived;
			seqNum++;
		}
		if (frameReceived) {
			nReceived++;
		}
		frameReceived = true;
	}
	
	return nReceived;
}

std::vector<ArrivalGroup> ChainInterop::runArrivalGroupAccumulator(uint16_t baseSeqnum, uint16_t numPackets) {
	const std::chrono::microseconds interDepartureThreshold(5000);
	const std::chrono::microseconds interArrivalThreshold(5000);
	const std::chrono::microseconds interGroupDelayVariationThreshold(0);

	bool init = false;
	ArrivalGroup group;
	std::vector<ArrivalGroup> groups;
	uint16_t seqnum = baseSeqnum;
	uint16_t lastAddedSeqnum = 0;
	std::unique_lock<std::mutex> guard(mapMutex);
	for (size_t i = 0; i < numPackets; i++, seqnum++) {
		auto packet = packetInfo.find(seqnum);
		if (packet == packetInfo.end()) {
			continue;
		}
		if (!(packet->second.isSent && packet->second.isReceived)) {
			continue;
		}
		if (!init) {
			group.add(packet->second);
			lastAddedSeqnum = seqnum;
			init = true;
			continue;
		}
		if (packet->second.arrivalDuration < group.arrivalTime) {
			// ignores out of order arrivals
			continue;
		}
		if (packet->second.departureTime >= group.departureTime) {
			if (interDepartureTimePkt(group, packet->second) <= interDepartureThreshold) {
				group.add(packet->second);
				lastAddedSeqnum = seqnum;
				continue;
			}
			if (interArrivalTimePkt(group, packet->second) <= interArrivalThreshold &&
			    interGroupDelayVariationPkt(group, packet->second) < interGroupDelayVariationThreshold) {
				group.add(packet->second);
				lastAddedSeqnum = seqnum;
				continue;
			}
			groups.push_back(std::move(group));
			group.reset();
			group.add(packet->second);
			lastAddedSeqnum = seqnum;
		}
	}
	if (lastAddedSeqnum == baseSeqnum + (numPackets - 1)) {
		groups.push_back(std::move(group));
	}
	return groups;
}

std::chrono::microseconds interDepartureTimePkt(ArrivalGroup group, PacketInfo packet) {
	if (group.packets.empty())
		return std::chrono::microseconds::zero();

	return std::chrono::duration_cast<std::chrono::microseconds>(
	    packet.departureTime - group.packets.back().departureTime);
}
std::chrono::microseconds interArrivalTimePkt(ArrivalGroup group, PacketInfo packet) {
	return packet.arrivalDuration - group.packets.back().arrivalDuration;
}
std::chrono::microseconds interGroupDelayVariationPkt(ArrivalGroup group, PacketInfo packet) {
	auto interDeparture = std::chrono::duration_cast<std::chrono::microseconds>(
	    packet.departureTime - group.departureTime);
	auto interArrival = packet.arrivalDuration - group.arrivalTime;
	return interArrival - interDeparture;
}

} // namespace rtc

#endif
