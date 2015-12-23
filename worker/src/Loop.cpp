#define MS_CLASS "Loop"

#include "Loop.h"
#include "DepLibUV.h"
#include "Settings.h"
#include "MediaSoupError.h"
#include "Logger.h"
#include <string>
#include <utility>  // std::pair()
#include <csignal>  // sigfillset, pthread_sigmask()
#include <cstdlib>  // std::genenv()
#include <cerrno>

/* Instance methods. */

Loop::Loop()
{
	MS_TRACE();

	int channelFd = std::stoi(std::getenv("MEDIASOUP_CHANNEL_FD"));

	// Set the signals handler.
	this->signalsHandler = new SignalsHandler(this);

	// Add signals to handle.
	this->signalsHandler->AddSignal(SIGINT, "INT");
	this->signalsHandler->AddSignal(SIGTERM, "TERM");

	// Set the Channel socket.
	this->channel = new Channel::UnixStreamSocket(this, channelFd);

	MS_DEBUG("starting libuv loop");
	DepLibUV::RunLoop();
	MS_DEBUG("libuv loop ended");
}

Loop::~Loop()
{
	MS_TRACE();
}

RTC::Room* Loop::GetRoomFromRequest(Channel::Request* request, unsigned int* roomId = nullptr)
{
	MS_TRACE();

	auto jsonRoomId = request->data["roomId"];

	if (!jsonRoomId.isUInt())
		MS_THROW_ERROR("Request has no numeric .roomId field");

	// If given, fill roomId.
	if (roomId)
		*roomId = jsonRoomId.asUInt();

	auto it = this->rooms.find(jsonRoomId.asUInt());

	if (it != this->rooms.end())
	{
		RTC::Room* room = it->second;

		return room;
	}
	else
	{
		return nullptr;
	}
}

void Loop::Close()
{
	MS_TRACE();

	int err;
	sigset_t signal_mask;

	if (this->closed)
	{
		MS_ERROR("already closed");

		return;
	}
	this->closed = true;

	// First block all the signals to not be interrupted while closing.
	sigfillset(&signal_mask);
	err = pthread_sigmask(SIG_BLOCK, &signal_mask, nullptr);
	if (err)
		MS_ERROR("pthread_sigmask() failed: %s", std::strerror(errno));

	// Close the SignalsHandler.
	this->signalsHandler->Close();

	// Close the Channel socket.
	this->channel->Close();

	// Close all the Rooms.
	for (auto& kv : this->rooms)
	{
		RTC::Room* room = kv.second;

		room->Close();
	}
}

void Loop::onSignal(SignalsHandler* signalsHandler, int signum)
{
	MS_TRACE();

	switch (signum)
	{
		case SIGINT:
			MS_DEBUG("signal INT received, exiting");
			Close();
			break;

		case SIGTERM:
			MS_DEBUG("signal TERM received, exiting");
			Close();
			break;

		default:
			MS_WARN("received a signal (with signum %d) for which there is no handling code", signum);
	}
}

void Loop::onSignalsHandlerClosed(SignalsHandler* signalsHandler)
{
	MS_TRACE();
}

void Loop::onChannelRequest(Channel::UnixStreamSocket* channel, Channel::Request* request)
{
	MS_TRACE();

	switch (request->methodId)
	{
		case Channel::Request::MethodId::updateSettings:
		{
			MS_DEBUG("'updateSettings' method");

			Settings::HandleUpdateRequest(request);
			break;
		}

		case Channel::Request::MethodId::createRoom:
		{
			MS_DEBUG("'createRoom' method");

			RTC::Room* room;
			unsigned int roomId;

			try
			{
				room = GetRoomFromRequest(request, &roomId);
			}
			catch (const MediaSoupError &error)
			{
				request->Reject(500, error.what());
				return;
			}

			if (room)
			{
				MS_ERROR("Room already exists");

				request->Reject(500, "Room already exists");
				return;
			}

			try
			{
				room = new RTC::Room(roomId, request->data);
			}
			catch (const MediaSoupError &error)
			{
				request->Reject(500, error.what());
				return;
			}

			this->rooms[roomId] = room;

			MS_DEBUG("Room created [roomId:%u]", roomId);
			request->Accept();

			break;
		}

		case Channel::Request::MethodId::closeRoom:
		{
			MS_DEBUG("'closeRoom' method");

			RTC::Room* room;
			unsigned int roomId;

			try
			{
				room = GetRoomFromRequest(request, &roomId);
			}
			catch (const MediaSoupError &error)
			{
				request->Reject(500, error.what());
				return;
			}

			if (!room)
			{
				MS_ERROR("Room does not exist");

				request->Reject(500, "Room does not exist");
				return;
			}

			room->Close();

			// TODO: Instead of this, the Room should fire an onRoomClosed() here.
			this->rooms.erase(roomId);

			MS_DEBUG("Room closed [roomId:%u]", roomId);
			request->Accept();

			break;
		}

		case Channel::Request::MethodId::createPeer:
		{
			MS_DEBUG("'createPeer' method");

			RTC::Room* room;

			try
			{
				room = GetRoomFromRequest(request);
			}
			catch (const MediaSoupError &error)
			{
				request->Reject(500, error.what());
				return;
			}

			if (!room)
			{
				MS_ERROR("Room does not exist");

				request->Reject(500, "Room does not exist");
				return;
			}

			room->HandleCreatePeerRequest(request);

			break;
		}

		case Channel::Request::MethodId::closePeer:
		{
			MS_DEBUG("'closePeer' method");

			RTC::Room* room;

			try
			{
				room = GetRoomFromRequest(request);
			}
			catch (const MediaSoupError &error)
			{
				request->Reject(500, error.what());
				return;
			}

			if (!room)
			{
				MS_ERROR("Room does not exist");

				request->Reject(500, "Room does not exist");
				return;
			}

			room->HandleClosePeerRequest(request);

			break;
		}

		default:
		{
			MS_ABORT("unexpected methodId");
		}
	}
}

void Loop::onChannelUnixStreamSocketRemotelyClosed(Channel::UnixStreamSocket* socket)
{
	MS_TRACE();

	// When mediasoup Node process ends it sends a SIGTERM to us so we close this
	// pipe and then exit.
	// If the pipe is remotely closed it means that mediasoup Node process
	// abruptly died (SIGKILL?) so we must die.

	MS_ERROR("Channel remotely closed, killing myself");

	Close();
}
