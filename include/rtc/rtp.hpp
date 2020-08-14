#ifndef RTC_RTPL_H
#define RTC_RTPL_H

struct RTCP_ReportBlock {
    uint32_t ssrc;

    uint32_t fractionLostAndCumulitveNumberOfPacketsLost;
    uint32_t highestSeqNoReceived;
    uint32_t arrivalJitter;
    uint32_t lastReport;
    uint32_t delaySinceLastReport;

    void print() {
//        std::cout << " +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-\n";
//        std::cout << "|" << ssrc << "|\n";
//        std::cout << "|" << fractionLost << "|" << packetsLost << "|\n";
//        std::cout << "|" << ntohl(highestSeqNoReceived) << "|\n";
//        std::cout << "|" << ntohl(arrivalJitter) << "|\n";
//        std::cout << "|" << ntohl(lastReport) << "|\n";
//        std::cout << "|" << ntohl(delaySinceLastReport) << "|\n";
//        std::cout << " +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-" << std::endl;
        std::cout <<
            " ssrc:" << ntohl(ssrc) <<
//            " fractionLost: " << (uint16_t) fractionLost <<
//            " packetsLost:" << ntohs(packetsLost) <<
            "flcnpl: " << fractionLostAndCumulitveNumberOfPacketsLost <<
            " highestSeqNo:" << ntohl(highestSeqNoReceived) <<
            " jitter:" << ntohl(arrivalJitter) <<
            " lastSR:" << ntohl(lastReport) <<
            " lastSRDelay:" << ntohl(delaySinceLastReport) << std::endl;
    }
};

struct RTCP_HEADER {

};

struct RTCP_SR {

#if __BYTE_ORDER == __BIG_ENDIAN
    uint16_t version:2;
	uint16_t padding:1;
	uint16_t rc:5;
	uint16_t payloadType:8;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t reportCount:5;
    uint16_t padding:1;
    uint16_t version:2;
    uint16_t payloadType:8;
#endif

    uint16_t length;
    uint32_t ssrc;

    uint64_t ntpTimestamp;
    uint32_t rtpTimestamp;

    uint32_t packetCount;
    uint32_t octetCount;

    RTCP_ReportBlock reportBlocks;

    RTCP_ReportBlock* getReportBlock(int num) {
        return &reportBlocks+num;
    }

    unsigned int getTotalSize() {
        return offsetof(RTCP_SR, reportBlocks) + sizeof(RTCP_ReportBlock)*reportCount;
    }

    void print() {
        std::cout <<
              "version:" << (uint16_t) version <<
              " padding:" << (padding ? "T" : "F") <<
              " reportCount: " << (uint16_t) reportCount <<
              " payloadType:" << (uint16_t) payloadType <<
              " totalLength:" << ntohs(length) <<
              " SSRC:" << ntohl(ssrc) <<
              " NTP TS: " << ntpTimestamp << // TODO This needs to be convereted from network-endian
              " RTP TS: " << ntohl(rtpTimestamp) <<
              " packetCount: " << ntohl(packetCount) <<
              " octetCount: " << ntohl(octetCount)
        << std::endl;
            for (int i =0;i < reportCount; i++)
                getReportBlock(i)->print();

//        // 4 bytes
//        std::cout
//        << "|" << version << " |" << (padding ? "P" : " ")
//        << "|" << reportCount << "|" << payloadType << "|" << ntohs(length) << "|\n";
//
//        // 4 bytes
//        std::cout << "|" << ntohl(ssrc) << "|\n";
//
//        std::cout << "|MATH IS HARD|\n";
//        std::cout << "|MATH IS HARD|\n";
//
//        std::cout << "|" << packetCount << "|\n";
//        std::cout << "|" << senderCount << "|\n";
//
//        for (int i =0;i < reportCount; i++)
//            getReportBlock(i)->print();
//
//        std::cout << " +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-\n";
//        std::cout << std::endl;
    }
};

struct RTCP_RR {

#if __BYTE_ORDER == __BIG_ENDIAN
    uint16_t version:2;
	uint16_t padding:1;
	uint16_t rc:5;
	uint16_t payloadType:8;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t reportCount:5;
    uint16_t padding:1;
    uint16_t version:2;
    uint16_t payloadType:8;
#endif

    uint16_t length;
    uint32_t ssrc;

    RTCP_ReportBlock reportBlocks;

    RTCP_ReportBlock* getReportBlock(int num) {
        return &reportBlocks+num;
    }

    unsigned int getTotalSize() const {
        return offsetof(RTCP_RR, reportBlocks) + sizeof(RTCP_ReportBlock)*reportCount;
    }
    void print() {
        std::cout <<
                  "version:" << (uint16_t) version <<
                  " padding:" << (padding ? "T" : "F") <<
                  " reportCount: " << (uint16_t) reportCount <<
                  " payloadType:" << (uint16_t) payloadType <<
                  " totalLength:" << ntohs(length) <<
                  " SSRC:" << ntohl(ssrc)
                  << std::endl;
        for (int i =0;i < reportCount; i++)
            getReportBlock(i)->print();
    }

    void setLength() {
//        std::cout << "TOTAL SIZE" << getTotalSize() << std::endl;
//        PLOG_DEBUG << "TOTAL SIZE " << getTotalSize()/4 << std::endl;
//        this->length = htons((getTotalSize()/4)+1);
        this->length = htons((sizeof(RTCP_ReportBlock)/4)+1);
    }
};

struct RTP
{
#if __BYTE_ORDER == __BIG_ENDIAN
    uint16_t version:2;
	uint16_t padding:1;
	uint16_t extension:1;
	uint16_t csrcCount:4;
	uint16_t markerBit:1;
	uint16_t payloadType:7;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t csrCcount:4;
    uint16_t extension:1;
    uint16_t padding:1;
    uint16_t version:2;
    uint16_t payloadType:7;
    uint16_t markerBit:1;
#endif
    uint16_t seqNumber;
    uint32_t timestamp;
    uint32_t ssrc;
    uint32_t csrc[16];
};

struct RTPC_TWCC {
#if __BYTE_ORDER == __BIG_ENDIAN
    uint16_t version:2;
	uint16_t padding:1;
	uint16_t format:5;
	uint16_t payloadType:8;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t format:5;
    uint16_t padding:1;
    uint16_t version:2;
    uint16_t payloadType:8;
#endif

    uint16_t length;
    uint32_t senderSSRC;
    uint32_t mediaSSRC;
    uint16_t baseSeqNo;
    uint16_t packetCount;
    uint16_t refTime;
    uint16_t twccSSRC;
};

 struct RTCP_REMB
{

#if __BYTE_ORDER == __BIG_ENDIAN
    uint16_t version:2;
	uint16_t padding:1;
	uint16_t format:5;
	uint16_t payloadType:8;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    uint16_t format:5;
    uint16_t padding:1;
    uint16_t version:2;
    uint16_t payloadType:8;
#endif
    uint16_t length;

    uint32_t senderSSRC;
    uint32_t mediaSourceSSRC;

    /*! \brief Unique identifier ('R' 'E' 'M' 'B') */
    char id[4] = {'R', 'E', 'M', 'B'};
    /*! \brief Num SSRC, Br Exp, Br Mantissa (bit mask) */
    uint32_t bitrate;

    /*! \brief SSRC feedback (we expect at max three SSRCs in there) */
    uint32_t ssrc[3];

    int getTotalSize() {
        return offsetof(RTCP_REMB, ssrc) + 4;
    }
};
#endif //RTC_RTPL_H
