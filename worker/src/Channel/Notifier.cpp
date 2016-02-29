#define MS_CLASS "Channel::Notifier"

#include "Channel/Notifier.h"
#include "Logger.h"

namespace Channel
{
	/* Instance methods. */

	Notifier::Notifier(Channel::UnixStreamSocket* channel) :
		channel(channel)
	{
		MS_TRACE();
	}

	Notifier::~Notifier()
	{
		MS_TRACE();
	}

	void Notifier::Close()
	{
		MS_TRACE();

		delete this;
	}

	void Notifier::Emit(uint32_t targetId, std::string event)
	{
		MS_TRACE();

		static Json::Value empty_data(Json::objectValue);
		static const Json::StaticString k_targetId("targetId");
		static const Json::StaticString k_event("event");

		Json::Value json(Json::objectValue);

		json[k_targetId] = (Json::UInt)targetId;
		json[k_event] = event;

		this->channel->Send(json);
	}

	void Notifier::Emit(uint32_t targetId, std::string event, Json::Value &data)
	{
		MS_TRACE();

		static const Json::StaticString k_targetId("targetId");
		static const Json::StaticString k_event("event");
		static const Json::StaticString k_data("data");

		Json::Value json(Json::objectValue);

		json[k_targetId] = (Json::UInt)targetId;
		json[k_event] = event;
		json[k_data] = data;

		this->channel->Send(json);
	}

	void Notifier::Emit(const uint8_t* data, size_t len)
	{
		MS_TRACE();

		this->channel->SendBinary(data, len);
	}
}
