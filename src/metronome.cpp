#if RTC_ENABLE_MEDIA

#include "metronome.hpp"
#include "impl/threadpool.hpp"
#include "rtp.hpp"

namespace rtc {

using ThreadPool = rtc::impl::ThreadPool;

Metronome::Metronome(size_t maxQueueSizeInBytes, std::shared_ptr<PacerAlgorithm> pacer,
                     std::function<void(message_vector &)> processPacketsCallback)
    : mMaxQueueSizeInBytes(maxQueueSizeInBytes), mPacerAlgorithm(pacer),
      mProcessPacketsCallback(processPacketsCallback), mThreadDelay(5), mQueueSizeInBytes(0) {}

void Metronome::outgoing(message_vector &messages, const message_callback &send) {
	message_vector others;
	std::unique_lock<std::mutex> lock(mSendQueueMutex);
	for (auto &message : messages) {
		if (message->type != Message::Binary) {
			others.push_back(std::move(message));
			continue;
		}
		mQueueSizeInBytes += message->size();
		mSendQueue.push_back(std::move(message));
	}
	while (mQueueSizeInBytes > mMaxQueueSizeInBytes) {
		size_t msg_size = mSendQueue.front()->size();
		// I am not dropping a packet that is just over the limit.
		if (mMaxQueueSizeInBytes > mQueueSizeInBytes - msg_size)
			break;
		mQueueSizeInBytes -= msg_size;
		mSendQueue.pop_front();
	}
	messages.swap(others);
	ThreadPool::Instance().schedule(std::chrono::milliseconds(0), [this, &send]() { senderProcess(send); });
}

void Metronome::senderProcess(const message_callback &send) {
	if (!mSendQueue.empty()) {
		unsigned int budget = mPacerAlgorithm->getBudget();
		message_vector outgoing;
		{
			std::unique_lock<std::mutex> lock(mSendQueueMutex);
			while (!mSendQueue.empty() && budget >= mSendQueue.front()->size()) {
				size_t msg_size = mSendQueue.front()->size(); 
				budget -= msg_size;
				mQueueSizeInBytes -= msg_size;
				outgoing.push_back(std::move(mSendQueue.front()));
				mSendQueue.pop_front();
			}
		}
		mPacerAlgorithm->setBudget(budget);
		for (const auto &message : outgoing) {
			send(message);
		}
		if (mProcessPacketsCallback) {
			mProcessPacketsCallback(outgoing);
		}
		if (!mSendQueue.empty()) {
			ThreadPool::Instance().schedule(mThreadDelay, [this, &send]() { senderProcess(send); });
		}
	}
}

}

#endif
