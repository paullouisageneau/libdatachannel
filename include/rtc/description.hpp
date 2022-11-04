/**
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 * Copyright (c) 2020 Staz Modrzynski
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef RTC_DESCRIPTION_H
#define RTC_DESCRIPTION_H

#include "candidate.hpp"
#include "common.hpp"

#include <iostream>
#include <map>
#include <vector>

namespace rtc {

const string DEFAULT_OPUS_AUDIO_PROFILE =
    "minptime=10;maxaveragebitrate=96000;stereo=1;sprop-stereo=1;useinbandfec=1";

// Use Constrained Baseline profile Level 3.1 (necessary for Firefox)
// https://developer.mozilla.org/en-US/docs/Web/Media/Formats/WebRTC_codecs#Supported_video_codecs
// TODO: Should be 42E0 but 42C0 appears to be more compatible. Investigate this.
const string DEFAULT_H264_VIDEO_PROFILE =
    "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1";

class RTC_CPP_EXPORT Description {
public:
	enum class Type { Unspec, Offer, Answer, Pranswer, Rollback };
	enum class Role { ActPass, Passive, Active };

	enum class Direction {
		SendOnly = RTC_DIRECTION_SENDONLY,
		RecvOnly = RTC_DIRECTION_RECVONLY,
		SendRecv = RTC_DIRECTION_SENDRECV,
		Inactive = RTC_DIRECTION_INACTIVE,
		Unknown = RTC_DIRECTION_UNKNOWN
	};

	Description(const string &sdp, Type type = Type::Unspec, Role role = Role::ActPass);
	Description(const string &sdp, string typeString);

	Type type() const;
	string typeString() const;
	Role role() const;
	string bundleMid() const;
	std::vector<string> iceOptions() const;
	optional<string> iceUfrag() const;
	optional<string> icePwd() const;
	optional<string> fingerprint() const;
	bool ended() const;

	void hintType(Type type);
	void setFingerprint(string fingerprint);
	void addIceOption(string option);
	void removeIceOption(const string &option);

	std::vector<string> attributes() const;
	void addAttribute(string attr);
	void removeAttribute(const string &attr);

	std::vector<Candidate> candidates() const;
	std::vector<Candidate> extractCandidates();
	bool hasCandidate(const Candidate &candidate) const;
	void addCandidate(Candidate candidate);
	void addCandidates(std::vector<Candidate> candidates);
	void endCandidates();

	operator string() const;
	string generateSdp(string_view eol = "\r\n") const;
	string generateApplicationSdp(string_view eol = "\r\n") const;

	class RTC_CPP_EXPORT Entry {
	public:
		virtual ~Entry() = default;

		virtual string type() const { return mType; }
		virtual string description() const { return mDescription; }
		virtual string mid() const { return mMid; }

		Direction direction() const { return mDirection; }
		void setDirection(Direction dir);

		bool isRemoved() const { return mIsRemoved; }
		void markRemoved();

		std::vector<string> attributes() const;
		void addAttribute(string attr);
		void removeAttribute(const string &attr);

		struct RTC_CPP_EXPORT ExtMap {
			static int parseId(string_view description);

			ExtMap(string_view description);

			void setDescription(string_view description);

			int id;
			string uri;
			string attributes;
			Direction direction = Direction::Unknown;
		};

		std::vector<int> extIds();
		ExtMap *extMap(int id);
		void addExtMap(ExtMap map);
		void removeExtMap(int id);

		operator string() const;
		string generateSdp(string_view eol = "\r\n", string_view addr = "0.0.0.0",
		                   uint16_t port = 9) const;

		virtual void parseSdpLine(string_view line);

		// For backward compatibility, do not use
		[[deprecated]] std::vector<string>::iterator beginAttributes();
		[[deprecated]] std::vector<string>::iterator endAttributes();
		[[deprecated]] std::vector<string>::iterator
		removeAttribute(std::vector<string>::iterator iterator);
		[[deprecated]] std::map<int, ExtMap>::iterator beginExtMaps();
		[[deprecated]] std::map<int, ExtMap>::iterator endExtMaps();
		[[deprecated]] std::map<int, ExtMap>::iterator
		removeExtMap(std::map<int, ExtMap>::iterator iterator);

	protected:
		Entry(const string &mline, string mid, Direction dir = Direction::Unknown);
		virtual string generateSdpLines(string_view eol) const;

		std::vector<string> mAttributes;
		std::map<int, ExtMap> mExtMaps;

	private:
		string mType;
		string mDescription;
		string mMid;
		Direction mDirection;
		bool mIsRemoved;
	};

	struct RTC_CPP_EXPORT Application : public Entry {
	public:
		Application(string mid = "data");
		Application(const string &mline, string mid);
		virtual ~Application() = default;

		string description() const override;
		Application reciprocate() const;

		void setSctpPort(uint16_t port) { mSctpPort = port; }
		void hintSctpPort(uint16_t port) { mSctpPort = mSctpPort.value_or(port); }
		void setMaxMessageSize(size_t size) { mMaxMessageSize = size; }

		optional<uint16_t> sctpPort() const { return mSctpPort; }
		optional<size_t> maxMessageSize() const { return mMaxMessageSize; }

		virtual void parseSdpLine(string_view line) override;

	private:
		virtual string generateSdpLines(string_view eol) const override;

		optional<uint16_t> mSctpPort;
		optional<size_t> mMaxMessageSize;
	};

	// Media (non-data)
	class RTC_CPP_EXPORT Media : public Entry {
	public:
		Media(const string &sdp);
		Media(const string &mline, string mid, Direction dir = Direction::SendOnly);
		virtual ~Media() = default;

		string description() const override;
		Media reciprocate() const;

		void addSSRC(uint32_t ssrc, optional<string> name, optional<string> msid = nullopt,
		             optional<string> trackId = nullopt);
		void removeSSRC(uint32_t ssrc);
		void replaceSSRC(uint32_t old, uint32_t ssrc, optional<string> name,
		                 optional<string> msid = nullopt, optional<string> trackID = nullopt);
		bool hasSSRC(uint32_t ssrc) const;
		std::vector<uint32_t> getSSRCs() const;
		std::optional<std::string> getCNameForSsrc(uint32_t ssrc) const;

		int bitrate() const;
		void setBitrate(int bitrate);

		struct RTC_CPP_EXPORT RtpMap {
			static int parsePayloadType(string_view description);

			explicit RtpMap(int payloadType);
			RtpMap(string_view description);

			void setDescription(string_view description);

			void addFeedback(string fb);
			void removeFeedback(const string &str);
			void addParameter(string p);
			void removeParameter(const string &str);

			int payloadType;
			string format;
			int clockRate;
			string encParams;

			std::vector<string> rtcpFbs;
			std::vector<string> fmtps;

			// For backward compatibility, do not use
			[[deprecated]] void addFB(string fb) { addFeedback(std::move(fb)); }
			[[deprecated]] void removeFB(const string &str) { removeFeedback(str); }
			[[deprecated]] void addAttribute(string attr) { addParameter(std::move(attr)); }
		};

		bool hasPayloadType(int payloadType) const;
		std::vector<int> payloadTypes() const;
		RtpMap *rtpMap(int payloadType);
		void addRtpMap(RtpMap map);
		void removeRtpMap(int payloadType);
		void removeFormat(const string &format);

		void addRtxCodec(int payloadType, int origPayloadType, unsigned int clockRate);

		virtual void parseSdpLine(string_view line) override;

		// For backward compatibility, do not use
		using RTPMap = RtpMap;
		[[deprecated]] int getBitrate() const { return bitrate(); }
		[[deprecated]] inline void addRTPMap(RtpMap map) { addRtpMap(std::move(map)); }
		[[deprecated]] inline void addRTXCodec(int pt, int origpt, unsigned int clk) {
			addRtxCodec(pt, origpt, clk);
		}
		[[deprecated]] std::map<int, RtpMap>::iterator beginMaps();
		[[deprecated]] std::map<int, RtpMap>::iterator endMaps();
		[[deprecated]] std::map<int, RtpMap>::iterator
		removeMap(std::map<int, RtpMap>::iterator iterator);

	private:
		virtual string generateSdpLines(string_view eol) const override;

		int mBas = -1;

		std::map<int, RtpMap> mRtpMaps;
		std::vector<uint32_t> mSsrcs;
		std::map<uint32_t, string> mCNameMap;
	};

	class RTC_CPP_EXPORT Audio : public Media {
	public:
		Audio(string mid = "audio", Direction dir = Direction::SendOnly);

		void addAudioCodec(int payloadType, string codec, optional<string> profile = std::nullopt);

		void addOpusCodec(int payloadType, optional<string> profile = DEFAULT_OPUS_AUDIO_PROFILE);
	};

	class RTC_CPP_EXPORT Video : public Media {
	public:
		Video(string mid = "video", Direction dir = Direction::SendOnly);

		void addVideoCodec(int payloadType, string codec, optional<string> profile = std::nullopt);

		void addH264Codec(int payloadType, optional<string> profile = DEFAULT_H264_VIDEO_PROFILE);
		void addVP8Codec(int payloadType);
		void addVP9Codec(int payloadType);
	};

	bool hasApplication() const;
	bool hasAudioOrVideo() const;
	bool hasMid(string_view mid) const;

	int addMedia(Media media);
	int addMedia(Application application);
	int addApplication(string mid = "data");
	int addVideo(string mid = "video", Direction dir = Direction::SendOnly);
	int addAudio(string mid = "audio", Direction dir = Direction::SendOnly);
	void clearMedia();

	variant<Media *, Application *> media(unsigned int index);
	variant<const Media *, const Application *> media(unsigned int index) const;
	unsigned int mediaCount() const;

	const Application *application() const;
	Application *application();

	static Type stringToType(const string &typeString);
	static string typeToString(Type type);

private:
	optional<Candidate> defaultCandidate() const;
	shared_ptr<Entry> createEntry(string mline, string mid, Direction dir);
	void removeApplication();

	Type mType;

	// Session-level attributes
	Role mRole;
	string mUsername;
	string mSessionId;
	std::vector<string> mIceOptions;
	optional<string> mIceUfrag, mIcePwd;
	optional<string> mFingerprint;
	std::vector<string> mAttributes; // other attributes

	// Entries
	std::vector<shared_ptr<Entry>> mEntries;
	shared_ptr<Application> mApplication;

	// Candidates
	std::vector<Candidate> mCandidates;
	bool mEnded = false;
};

} // namespace rtc

RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, const rtc::Description &description);
RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, rtc::Description::Type type);
RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, rtc::Description::Role role);
RTC_CPP_EXPORT std::ostream &operator<<(std::ostream &out, const rtc::Description::Direction &direction);

#endif
