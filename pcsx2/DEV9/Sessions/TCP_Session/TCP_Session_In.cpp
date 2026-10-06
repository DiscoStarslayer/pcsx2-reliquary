// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#ifdef __POSIX__
#define SOCKET_ERROR -1
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#define SD_RECEIVE SHUT_RD
#endif

#include "TCP_Session.h"

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::TCP;

namespace Sessions
{
	std::optional<ReceivedPayload> TCP_Session::Recv()
	{
		std::optional<ReceivedPayload> ret = PopRecvBuff();
		if (ret.has_value())
			return ret;

		switch (state)
		{
			case TCP_State::SendingSYN_ACK:
			{
				fd_set writeSet;
				fd_set exceptSet;

				FD_ZERO(&writeSet);
				FD_ZERO(&exceptSet);

				FD_SET(client, &writeSet);
				FD_SET(client, &exceptSet);

				timeval nowait{0};
				select(client + 1, nullptr, &writeSet, &exceptSet, &nowait);

				if (FD_ISSET(client, &writeSet))
					return ConnectTCPComplete(true);
				if (FD_ISSET(client, &exceptSet))
					return ConnectTCPComplete(false);

				return std::nullopt;
			}
			case TCP_State::SentSYN_ACK:
				// Don't read data untill PS2 ACKs connection
				return std::nullopt;
			case TCP_State::CloseCompletedFlushBuffer:
				/*
				 * When TCP connection is closed by the server
				 * the server is the last to send a packet
				 * so the event must be raised here
				 */
				state = TCP_State::CloseCompleted;
				RaiseEventConnectionClosed();
				return std::nullopt;
			case TCP_State::Connected:
			case TCP_State::Closing_ClosedByPS2:
			case TCP_State::Closing_ClosedByRemote:
			case TCP_State::Closing_ClosedByRemoteThenPS2_WaitingForAck:
			case TCP_State::Closing_ClosedByPS2ThenRemote_WaitingForAck:
			{
				ret = RecvDataRetransmission();
				if (ret.has_value())
					return ret;

				if (state != TCP_State::Connected && state != TCP_State::Closing_ClosedByPS2)
					return std::nullopt;

				break;
			}
			default:
				return std::nullopt;
		}

		const int maxSize = GetReceiveSize();
		if (maxSize <= 0)
			return std::nullopt;

		int err = 0;

		// FIONREAD writes an unsigned long on Windows and an int on POSIX.
		unsigned long available = 0;
#ifdef _WIN32
		err = ioctlsocket(client, FIONREAD, &available);
#elif defined(__POSIX__)
		err = ioctl(client, FIONREAD, &available);
#endif
		if (err == SOCKET_ERROR)
			return std::nullopt;

		if (available > static_cast<uint>(maxSize))
			Console.WriteLn("DEV9: TCP: Got a lot of data: %lu using: %d", available, maxSize);

		const auto buffer = std::make_unique<u8[]>(maxSize);
		const int received = recv(client, reinterpret_cast<char*>(buffer.get()), maxSize, 0);
		if (received == -1)
#ifdef _WIN32
			err = WSAGetLastError();
#elif defined(__POSIX__)
			err = errno;
#endif

		switch (err)
		{
#ifdef _WIN32
			case WSAEINVAL:
			case WSAESHUTDOWN:
			case WSAEWOULDBLOCK:
				return std::nullopt;
#elif defined(__POSIX__)
			case EINVAL:
			case ESHUTDOWN:
			case EWOULDBLOCK:
				return std::nullopt;
#endif
			case 0:
				break;
			default:
				CloseByRemoteRST();
				Console.Error("DEV9: TCP: Recv error: %d", err);
				return std::nullopt;
		}

		if (received == 0)
		{
			const int result = shutdown(client, SD_RECEIVE);
			if (result == SOCKET_ERROR)
				Console.Error("DEV9: TCP: Shutdown SD_RECEIVE error: %d",
#ifdef _WIN32
					WSAGetLastError());
#elif defined(__POSIX__)
					errno);
#endif

			switch (state)
			{
				case TCP_State::Connected:
					return CloseByRemoteStage1();
				case TCP_State::Closing_ClosedByPS2:
					return CloseByPS2Stage3();
				default:
					CloseByRemoteRST();
					Console.Error("DEV9: TCP: Remote close occured with invalid TCP state");
					break;
			}

			return std::nullopt;
		}

		DevCon.WriteLn("DEV9: TCP: [SRV] Sending %d bytes", received);

		PayloadData* receivedData = new PayloadData(received);
		memcpy(receivedData->data.get(), buffer.get(), received);

		std::unique_ptr<TCP_Packet> packet = CreateBasePacket(receivedData);
		IncrementMyNumber(static_cast<u32>(received), receivedData->data.get());

		packet->SetACK(true);
		packet->SetPSH(true);

		myNumberACKed.store(false);
		return ReceivedPayload{destIP, std::move(packet)};
	}

	std::optional<ReceivedPayload> TCP_Session::ConnectTCPComplete(bool success)
	{
		if (!success)
		{
			int error = 0;
#ifdef _WIN32
			int len = sizeof(error);
			if (getsockopt(client, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) < 0)
				Console.Error("DEV9: TCP: Unkown TCP connection error (getsockopt error: %d)", WSAGetLastError());
#elif defined(__POSIX__)
			socklen_t len = sizeof(error);
			if (getsockopt(client, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) < 0)
				Console.Error("DEV9: TCP: Unkown TCP connection error (getsockopt error: %d)", errno);
#endif
			else
				Console.Error("DEV9: TCP: Connect error: %d", error);

			state = TCP_State::CloseCompleted;
			RaiseEventConnectionClosed();
			return std::nullopt;
		}

		state = TCP_State::SentSYN_ACK;

		std::unique_ptr<TCP_Packet> ret = CreateBasePacket();
		IncrementMyNumber(1);

		ret->SetSYN(true);
		ret->SetACK(true);

		ret->options.insert(ret->options.begin(), {new TCPopMSS(maxSegmentSize), new TCPopNOP(), new TCPopWS(0)});

		return ReceivedPayload{destIP, std::move(ret)};
	}

	ReceivedPayload TCP_Session::CloseByPS2Stage3()
	{
		std::unique_ptr ret = CreateBasePacket();
		IncrementMyNumber(1);

		ret->SetACK(true);
		ret->SetFIN(true);

		myNumberACKed.store(false);

		state = TCP_State::Closing_ClosedByPS2ThenRemote_WaitingForAck;
		return ReceivedPayload{destIP, std::move(ret)};
	}

	ReceivedPayload TCP_Session::CloseByRemoteStage1()
	{
		std::unique_ptr<TCP_Packet> ret = CreateBasePacket();
		IncrementMyNumber(1);

		ret->SetACK(true);
		ret->SetFIN(true);

		myNumberACKed.store(false);

		state = TCP_State::Closing_ClosedByRemote;
		return ReceivedPayload{destIP, std::move(ret)};
	}
} // namespace Sessions
