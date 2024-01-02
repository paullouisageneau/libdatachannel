#ifndef PACER_H
#define PACER_H

#if RTC_ENABLE_MEDIA

#include <deque>
#include <chrono>
#include "mediahandler.hpp"

namespace rtc {

class RTC_CPP_EXPORT PacerAlgorithm {
public:
	virtual unsigned int getBudget() = 0;
	virtual unsigned int getPace() = 0;
	virtual void setPace(unsigned int pace) = 0;
	virtual void setBudget(unsigned int budget) = 0;
	virtual void resetBudget() = 0;
};

class RTC_CPP_EXPORT Metronome final : public MediaHandler {
    std::deque<message_ptr> mSendQueue;
	size_t mQueueSizeInBytes;
	size_t mMaxQueueSizeInBytes;
    std::mutex mSendQueueMutex;

	std::chrono::milliseconds mThreadDelay;
	std::function<void(message_vector&)> mProcessPacketsCallback; // For bookkeeping of sent packets
	std::shared_ptr<PacerAlgorithm> mPacerAlgorithm;

public:
	Metronome(size_t maxQueueSizeInBytes, std::shared_ptr<PacerAlgorithm> pacerAlgorithm,
	          std::function<void(message_vector &)> processPacketsCallback);
	
	void outgoing(message_vector &messages, const message_callback &send) override;
	void senderProcess(const message_callback &send);

};

} // namespace rtc

#endif /* RTC_ENABLE_MEDIA */

#endif /* PACER_H */
