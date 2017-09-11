#define MS_CLASS "RTC::Consumer"
// #define MS_LOG_DEV

#include "RTC/Consumer.hpp"
#include "Logger.hpp"
#include "MediaSoupError.hpp"
#include "Utils.hpp"
#include "RTC/RTCP/FeedbackRtpNack.hpp"
#include "RTC/RTCP/SenderReport.hpp"
#include <vector>

namespace RTC
{
	/* Static. */

	static std::vector<RTC::RtpPacket*> RtpRetransmissionContainer(18);

	/* Instance methods. */

	Consumer::Consumer(
	  Channel::Notifier* notifier, uint32_t consumerId, RTC::Media::Kind kind, uint32_t sourceProducerId)
	  : consumerId(consumerId), kind(kind), sourceProducerId(sourceProducerId), notifier(notifier)
	{
		MS_TRACE();

		// Initialize sequence number.
		this->seqNum = static_cast<uint16_t>(Utils::Crypto::GetRandomUInt(0x00FF, 0xFFFF));

		// Set the RTCP report generation interval.
		if (this->kind == RTC::Media::Kind::AUDIO)
			this->maxRtcpInterval = RTC::RTCP::MaxAudioIntervalMs;
		else
			this->maxRtcpInterval = RTC::RTCP::MaxVideoIntervalMs;
	}

	Consumer::~Consumer()
	{
		MS_TRACE();

		delete this->rtpStream;
	}

	void Consumer::Destroy()
	{
		MS_TRACE();

		for (auto& listener : this->listeners)
		{
			listener->OnConsumerClosed(this);
		}

		this->notifier->Emit(this->consumerId, "close");

		delete this;
	}

	Json::Value Consumer::ToJson() const
	{
		MS_TRACE();

		static const Json::StaticString JsonStringConsumerId{ "consumerId" };
		static const Json::StaticString JsonStringKind{ "kind" };
		static const Json::StaticString JsonStringSourceProducerId{ "sourceProducerId" };
		static const Json::StaticString JsonStringRtpParameters{ "rtpParameters" };
		static const Json::StaticString JsonStringRtpStream{ "rtpStream" };
		static const Json::StaticString JsonStringEnabled{ "enabled" };
		static const Json::StaticString JsonStringPaused{ "paused" };
		static const Json::StaticString JsonStringSourcePaused{ "sourcePaused" };
		static const Json::StaticString JsonStringPreferredProfile{ "preferredProfile" };
		static const Json::StaticString JsonStringEffectiveProfile{ "effectiveProfile" };

		Json::Value json(Json::objectValue);

		json[JsonStringConsumerId] = Json::UInt{ this->consumerId };

		json[JsonStringKind] = RTC::Media::GetJsonString(this->kind);

		json[JsonStringSourceProducerId] = Json::UInt{ this->sourceProducerId };

		if (this->transport != nullptr)
			json[JsonStringRtpParameters] = this->rtpParameters.ToJson();

		if (this->rtpStream != nullptr)
			json[JsonStringRtpStream] = this->rtpStream->ToJson();

		json[JsonStringPaused] = this->paused;

		json[JsonStringSourcePaused] = this->sourcePaused;

		json[JsonStringPreferredProfile] =
		  RTC::RtpEncodingParameters::profile2String[this->preferredProfile];

		json[JsonStringEffectiveProfile] =
		  RTC::RtpEncodingParameters::profile2String[this->effectiveProfile];

		return json;
	}

	void Consumer::HandleRequest(Channel::Request* request)
	{
		MS_TRACE();

		switch (request->methodId)
		{
			case Channel::Request::MethodId::CONSUMER_DUMP:
			{
				auto json = ToJson();

				request->Accept(json);

				break;
			}

			default:
			{
				MS_ERROR("unknown method");

				request->Reject("unknown method");
			}
		}
	}

	/**
	 * A Transport has been assigned, and hence sending RTP parameters.
	 */
	void Consumer::Enable(RTC::Transport* transport, RTC::RtpParameters& rtpParameters)
	{
		MS_TRACE();

		// Must have a single encoding.
		if (rtpParameters.encodings.empty())
			MS_THROW_ERROR("invalid empty rtpParameters.encodings");
		else if (rtpParameters.encodings[0].ssrc == 0)
			MS_THROW_ERROR("missing rtpParameters.encodings[0].ssrc");

		if (IsEnabled())
			Disable();

		this->transport     = transport;
		this->rtpParameters = rtpParameters;

		FillSupportedCodecPayloadTypes();

		// Create RtpStreamSend instance.
		CreateRtpStream(this->rtpParameters.encodings[0]);

		MS_DEBUG_DEV("Consumer enabled [consumerId:%" PRIu32 "]", this->consumerId);
	}

	void Consumer::Pause()
	{
		MS_TRACE();

		if (this->paused)
			return;

		this->paused = true;

		MS_DEBUG_DEV("Consumer paused [consumerId:%" PRIu32 "]", this->consumerId);

		if (IsEnabled() && !this->sourcePaused)
			this->rtpStream->ClearRetransmissionBuffer();
	}

	void Consumer::Resume()
	{
		MS_TRACE();

		if (!this->paused)
			return;

		this->paused = false;

		MS_DEBUG_DEV("Consumer resumed [consumerId:%" PRIu32 "]", this->consumerId);

		if (IsEnabled() && !this->sourcePaused)
		{
			RequestFullFrame();
		}
	}

	void Consumer::SourcePause()
	{
		MS_TRACE();

		if (this->sourcePaused)
			return;

		this->sourcePaused = true;

		MS_DEBUG_DEV("Consumer source paused [consumerId:%" PRIu32 "]", this->consumerId);

		this->notifier->Emit(this->consumerId, "sourcepaused");

		if (IsEnabled() && !this->paused)
			this->rtpStream->ClearRetransmissionBuffer();
	}

	void Consumer::SourceResume()
	{
		MS_TRACE();

		if (!this->sourcePaused)
			return;

		this->sourcePaused = false;

		MS_DEBUG_DEV("Consumer source resumed [consumerId:%" PRIu32 "]", this->consumerId);

		this->notifier->Emit(this->consumerId, "sourceresumed");

		if (IsEnabled() && !this->paused)
			RequestFullFrame();
	}

	void Consumer::SourceRtpParametersUpdated()
	{
		MS_TRACE();

		if (!IsEnabled())
			return;

		this->syncRequired = true;
		this->rtpStream->ClearRetransmissionBuffer();
	}

	void Consumer::AddProfile(const RTC::RtpEncodingParameters::Profile profile)
	{
		// If this is the first enabled profile, remove the NONE entry from the set.
		if (this->profiles.size() == 1 &&
				(*(this->profiles.begin()) == RTC::RtpEncodingParameters::Profile::NONE))
			this->profiles.clear();

		// Insert profile.
		this->profiles.insert(profile);

		MS_DEBUG_TAG(rtp, "profile added: %s", RTC::RtpEncodingParameters::profile2String[profile].c_str());;

		RecalculateEffectiveProfile();
	}

	void Consumer::RemoveProfile(const RTC::RtpEncodingParameters::Profile profile)
	{
		// Remove profile.
		this->profiles.erase(profile);

		MS_DEBUG_TAG(rtp, "profile removed: %s", RTC::RtpEncodingParameters::profile2String[this->effectiveProfile].c_str());;

		RecalculateEffectiveProfile();
	}

	void Consumer::SetPreferredProfile(const RTC::RtpEncodingParameters::Profile profile)
	{
		MS_TRACE();

		if (this->preferredProfile == profile)
			return;

		this->preferredProfile = profile;

		RecalculateEffectiveProfile();
	}

	/**
	 * Called when the Transport assigned to this Consumer has been closed, so this
	 * Consumer becomes unhandled.
	 */
	void Consumer::Disable()
	{
		MS_TRACE();

		this->transport = nullptr;

		this->supportedCodecPayloadTypes.clear();

		if (this->rtpStream != nullptr)
		{
			delete this->rtpStream;
			this->rtpStream = nullptr;
		}

		// Reset RTCP and RTP counter stuff.
		this->lastRtcpSentTime = 0;
		this->transmittedCounter.Reset();
		this->retransmittedCounter.Reset();
	}

	void Consumer::SendRtpPacket(RTC::RtpPacket* packet, RTC::RtpEncodingParameters::Profile profile)
	{
		MS_TRACE();

		if (!IsEnabled())
			return;

		// If paused don't forward RTP.
		if (IsPaused())
			return;

		// Map the payload type.
		auto payloadType = packet->GetPayloadType();

		// NOTE: This may happen if this Consumer supports just some codecs of those
		// in the corresponding Producer.
		if (this->supportedCodecPayloadTypes.find(payloadType) == this->supportedCodecPayloadTypes.end())
		{
			MS_DEBUG_DEV("payload type not supported [payloadType:%" PRIu8 "]", payloadType);

			return;
		}

		// If the packet belongs to different profile than the one being sent, drop it.
		// NOTE: This is specific to simulcast with no temporal layers.
		if (profile != this->effectiveProfile)
			return;

		// Check whether sequence number and timestamp sync is required.
		if (this->syncRequired)
		{
			// TODO
			MS_ERROR("------------ syncRequired IS TRUE !!!!!");

			// TODO
			MS_ERROR(
			  "--- [profile:%s, effectiveProfile:%s, ssrc:%" PRIu32 ", packet->seq:%" PRIu16 "]",
			  RTC::RtpEncodingParameters::profile2String[profile].c_str(),
			  RTC::RtpEncodingParameters::profile2String[this->effectiveProfile].c_str(),
			  packet->GetSsrc(),
			  packet->GetSequenceNumber());

			this->seqNum += 1;

			auto now = static_cast<uint32_t>(DepLibUV::GetTime());

			if (now > this->rtpTimestamp)
				this->rtpTimestamp = now;

			this->syncRequired = false;
		}
		else
		{
			this->seqNum += packet->GetSequenceNumber() - this->lastRecvSeqNum;
			this->rtpTimestamp += packet->GetTimestamp() - this->lastRecvRtpTimestamp;
		}

		// Save the received sequence number.
		this->lastRecvSeqNum = packet->GetSequenceNumber();

		// Save the received timestamp.
		this->lastRecvRtpTimestamp = packet->GetTimestamp();

		// Save real SSRC.
		auto ssrc = packet->GetSsrc();

		// Rewrite packet SSRC.
		packet->SetSsrc(this->rtpParameters.encodings[0].ssrc);

		// Rewrite packet sequence number.
		packet->SetSequenceNumber(this->seqNum);

		// Rewrite packet timestamp.
		packet->SetTimestamp(this->rtpTimestamp);

		// Process the packet.
		if (this->rtpStream->ReceivePacket(packet))
		{
			// Send the packet.
			this->transport->SendRtpPacket(packet);

			// Update transmitted RTP data counter.
			this->transmittedCounter.Update(packet);
		}
		else
		{
			// TODO
			MS_ERROR(
			  "--- rtpStream->ReceivePacket() failed [profile:%s, effectiveProfile:%s, ssrc:%" PRIu32
			  ", packet->seq:%" PRIu16 "]",
			  RTC::RtpEncodingParameters::profile2String[profile].c_str(),
			  RTC::RtpEncodingParameters::profile2String[this->effectiveProfile].c_str(),
			  ssrc,
			  this->lastRecvSeqNum);
		}

		// Restore packet SSRC.
		packet->SetSsrc(ssrc);

		// Restore the original sequence number.
		packet->SetSequenceNumber(this->lastRecvSeqNum);

		// Restore the original timestamp.
		packet->SetTimestamp(this->lastRecvRtpTimestamp);
	}

	void Consumer::GetRtcp(RTC::RTCP::CompoundPacket* packet, uint64_t now)
	{
		MS_TRACE();

		if (static_cast<float>((now - this->lastRtcpSentTime) * 1.15) < this->maxRtcpInterval)
			return;

		auto* report = this->rtpStream->GetRtcpSenderReport(now);

		if (report == nullptr)
			return;

		// NOTE: This assumes a single stream.
		uint32_t ssrc     = this->rtpParameters.encodings[0].ssrc;
		std::string cname = this->rtpParameters.rtcp.cname;

		report->SetSsrc(ssrc);
		packet->AddSenderReport(report);

		// Build SDES chunk for this sender.
		auto sdesChunk = new RTC::RTCP::SdesChunk(ssrc);
		auto sdesItem =
		  new RTC::RTCP::SdesItem(RTC::RTCP::SdesItem::Type::CNAME, cname.size(), cname.c_str());

		sdesChunk->AddItem(sdesItem);
		packet->AddSdesChunk(sdesChunk);
		this->lastRtcpSentTime = now;
	}

	void Consumer::ReceiveNack(RTC::RTCP::FeedbackRtpNackPacket* nackPacket)
	{
		MS_TRACE();

		if (!IsEnabled())
			return;

		for (auto it = nackPacket->Begin(); it != nackPacket->End(); ++it)
		{
			RTC::RTCP::FeedbackRtpNackItem* item = *it;

			this->rtpStream->RequestRtpRetransmission(
			  item->GetPacketId(), item->GetLostPacketBitmask(), RtpRetransmissionContainer);

			auto it2 = RtpRetransmissionContainer.begin();
			for (; it2 != RtpRetransmissionContainer.end(); ++it2)
			{
				RTC::RtpPacket* packet = *it2;

				if (packet == nullptr)
					break;

				RetransmitRtpPacket(packet);
			}
		}
	}

	void Consumer::ReceiveRtcpReceiverReport(RTC::RTCP::ReceiverReport* report)
	{
		MS_TRACE();

		if (!IsEnabled())
			return;

		this->rtpStream->ReceiveRtcpReceiverReport(report);
	}

	void Consumer::RequestFullFrame()
	{
		MS_TRACE();

		if (!IsEnabled())
			return;

		if (this->kind == RTC::Media::Kind::AUDIO || IsPaused())
			return;

		for (auto& listener : this->listeners)
		{
			listener->OnConsumerFullFrameRequired(this);
		}
	}

	void Consumer::OnRtpStreamHealthReport(RtpStream* stream, bool healthy)
	{
		MS_TRACE();

		if (!IsEnabled())
			return;
	}

	void Consumer::FillSupportedCodecPayloadTypes()
	{
		MS_TRACE();

		for (auto& codec : this->rtpParameters.codecs)
		{
			this->supportedCodecPayloadTypes.insert(codec.payloadType);
		}
	}

	void Consumer::CreateRtpStream(RTC::RtpEncodingParameters& encoding)
	{
		MS_TRACE();

		uint32_t ssrc = encoding.ssrc;
		// Get the codec of the stream/encoding.
		auto& codec = this->rtpParameters.GetCodecForEncoding(encoding);
		bool useNack{ false };
		bool usePli{ false };

		for (auto& fb : codec.rtcpFeedback)
		{
			if (!useNack && fb.type == "nack")
			{
				MS_DEBUG_2TAGS(rtcp, rtx, "NACK supported");

				useNack = true;
			}
			if (!usePli && fb.type == "nack" && fb.parameter == "pli")
			{
				MS_DEBUG_TAG(rtcp, "PLI supported");

				usePli = true;
			}
		}

		// Create stream params.
		RTC::RtpStream::Params params;

		params.ssrc        = ssrc;
		params.payloadType = codec.payloadType;
		params.mime        = codec.mime;
		params.clockRate   = codec.clockRate;
		params.useNack     = useNack;
		params.usePli      = usePli;

		// Create a RtpStreamSend for sending a single media stream.
		if (useNack)
			this->rtpStream = new RTC::RtpStreamSend(params, 750);
		else
			this->rtpStream = new RTC::RtpStreamSend(params, 0);

		if (encoding.hasRtx && encoding.rtx.ssrc != 0u)
		{
			auto& codec = this->rtpParameters.GetRtxCodecForEncoding(encoding);

			this->rtpStream->SetRtx(codec.payloadType, encoding.rtx.ssrc);
		}
	}

	void Consumer::RetransmitRtpPacket(RTC::RtpPacket* packet)
	{
		MS_TRACE();

		RTC::RtpPacket* rtxPacket{ nullptr };

		if (this->rtpStream->HasRtx())
		{
			static uint8_t rtxBuffer[MtuSize];

			rtxPacket = packet->Clone(rtxBuffer);
			this->rtpStream->RtxEncode(rtxPacket);

			MS_DEBUG_TAG(
			  rtx,
			  "sending rtx packet [ssrc: %" PRIu32 ", seq: %" PRIu16
			  "] recovering original [ssrc: %" PRIu32 ", seq: %" PRIu16 "]",
			  rtxPacket->GetSsrc(),
			  rtxPacket->GetSequenceNumber(),
			  packet->GetSsrc(),
			  packet->GetSequenceNumber());
		}
		else
		{
			rtxPacket = packet;
			MS_DEBUG_TAG(
			  rtx,
			  "retransmitting packet [ssrc: %" PRIu32 ", seq: %" PRIu16 "]",
			  rtxPacket->GetSsrc(),
			  rtxPacket->GetSequenceNumber());
		}

		// Update retransmitted RTP data counter.
		this->retransmittedCounter.Update(rtxPacket);

		// Send the packet.
		this->transport->SendRtpPacket(rtxPacket);

		// Delete the RTX RtpPacket if it was created.
		if (rtxPacket != packet)
			delete rtxPacket;
	}

	void Consumer::RecalculateEffectiveProfile()
	{
		static const Json::StaticString JsonStringProfile{ "profile" };

		Json::Value eventData(Json::objectValue);

		RTC::RtpEncodingParameters::Profile newProfile;

		// If there is no preferred profile, take the best one available.
		if (this->preferredProfile == RTC::RtpEncodingParameters::Profile::NONE)
		{
			auto it = this->profiles.crbegin();
			newProfile = *it;
		}
		// Otherwise take the highest available profile equal or lower than the preferred.
		else
		{
			auto it = this->profiles.lower_bound(this->preferredProfile);
			newProfile = *it;
		}

		if (newProfile == this->effectiveProfile)
			return;

		this->effectiveProfile = newProfile;

		MS_DEBUG_TAG(rtp, "new effective profile: %s", RTC::RtpEncodingParameters::profile2String[this->effectiveProfile].c_str());;

		// Notify.
		eventData[JsonStringProfile] = RTC::RtpEncodingParameters::profile2String[this->effectiveProfile];
		this->notifier->Emit(this->consumerId, "effectiveprofilechange", eventData);

		if (IsEnabled() && !IsPaused())
		{
			this->rtpStream->ClearRetransmissionBuffer();

			RequestFullFrame();
		}

		this->syncRequired = true;
	}
} // namespace RTC
