/*
 * libdatachannel streamer example
 * Copyright (c) 2020 Filip Klembara (in2core)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RTCPSenderReporter_hpp
#define RTCPSenderReporter_hpp

#if RTC_ENABLE_MEDIA

#include "message.hpp"
#include "rtppacketizationconfig.hpp"

namespace rtc {

/// Class for sending RTCP SR
class RTCPSenderReportable {
    bool needsToReport = false;

    uint32_t packetCount = 0;
    uint32_t payloadOctets = 0;
    double timeOffset = 0;

    uint32_t _previousReportedTimestamp = 0;

    void addToReport(RTP * rtp, uint32_t rtpSize);
    message_ptr getSenderReport(uint32_t timestamp);
protected:
    /// Outgoing callback for sender reports
    synchronized_callback<message_ptr> senderReportOutgoingCallback;
public:
    static uint64_t secondsToNTP(double seconds);

    /// Timestamp of previous sender report
    const uint32_t & previousReportedTimestamp = _previousReportedTimestamp;

    /// RTP configuration
    const std::shared_ptr<RTPPacketizationConfig> rtpConfig;

    RTCPSenderReportable(std::shared_ptr<RTPPacketizationConfig> rtpConfig);

    /// Set `needsToReport` flag. Sender report will be sent before next RTP packet with same timestamp.
    void setNeedsToReport();

    /// Set offset to compute NTS for RTCP SR packets. Offset represents relation between real start time and timestamp of the stream in RTP packets
    /// @note `time_offset = rtpConfig->startTime_s - rtpConfig->timestampToSeconds(rtpConfig->timestamp)`
    void startRecording();

    /// Send RTCP SR with given timestamp
    /// @param timestamp timestamp of the RTCP SR
    void sendReport(uint32_t timestamp);
    
protected:
    /// Calls given block with function for statistics. Sends RTCP SR packet with current timestamp before `block` call if `needs_to_report` flag is true.
    /// @param block Block of code to run. This block has function for rtp stats recording.
    template <typename T>
    T withStatsRecording(std::function<T (std::function<void (message_ptr)>)> block) {
        if (needsToReport) {
            sendReport(rtpConfig->timestamp);
            needsToReport = false;
        }
        auto result = block([this](message_ptr _rtp) {
            auto rtp = reinterpret_cast<RTP *>(_rtp->data());
            this->addToReport(rtp, _rtp->size());
        });
        return result;
    }
};

} // namespace

#endif /* RTC_ENABLE_MEDIA */

#endif /* RTCPSenderReporter_hpp */
