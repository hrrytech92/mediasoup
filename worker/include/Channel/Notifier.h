#ifndef MS_CHANNEL_NOTIFIER_H
#define MS_CHANNEL_NOTIFIER_H

#include "common.h"
#include "Channel/UnixStreamSocket.h"
#include <string>
#include <json/json.h>

namespace Channel
{
	class Notifier
	{
	public:
		Notifier(Channel::UnixStreamSocket* channel);
		virtual ~Notifier();

		void Close();

		void Emit(uint32_t targetId, std::string eventName);
		void Emit(uint32_t targetId, std::string eventName, Json::Value& eventData);

	public:
		// Passed by argument.
		Channel::UnixStreamSocket* channel = nullptr;
	};
}

#endif
