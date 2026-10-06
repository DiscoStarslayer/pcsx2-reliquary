// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <algorithm>
#include <bit>
#include <thread>

#ifdef __POSIX__
#define SOCKET_ERROR -1
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#define SD_RECEIVE SHUT_RD
#define SD_SEND SHUT_WR
#endif

#include "TCP_Session.h"

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::TCP;

namespace Sessions
{
	bool TCP_Session::Send(PacketReader::IP::IP_Payload* payload)
	{
		IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(payload);
		TCP_Packet tcp(ipPayload->data, ipPayload->GetLength());

		if (destPort != 0)
		{
			if (!(tcp.destinationPort == destPort && tcp.sourcePort == srcPort))
			{
				Console.Error("DEV9: TCP: Packet invalid for current session (Duplicate key?)");
				return false;
			}
		}

		if (tcp.GetRST() == true)
		{
			// PS2 has reset connection;
			if (client != INVALID_SOCKET)
				CloseSocket();
			else
				Console.Error("DEV9: TCP: Reset closed connection");
			// PS2 sent RST, clearly not expecting more data
			state = TCP_State::CloseCompleted;
			RaiseEventConnectionClosed();
			return true;
		}

		switch (state)
		{
			case TCP_State::None:
				return SendConnect(&tcp);
			case TCP_State::SendingSYN_ACK:
				if (CheckRepeatSYNNumbers(&tcp) == NumCheckResult::Bad)
				{
					Console.Error("DEV9: TCP: Invalid repeated SYN (SendingSYN_ACK)");
					return false;
				}
				return true; // Ignore reconnect attempts while we are still attempting connection
			case TCP_State::SentSYN_ACK:
				return SendConnected(&tcp);
			case TCP_State::Connected:
				if (tcp.GetFIN() == true) // Connection close part 1, received FIN from PS2
					return CloseByPS2Stage1_2(&tcp);
				else
					return SendData(&tcp);

			case TCP_State::Closing_ClosedByPS2:
				return SendNoData(&tcp);
			case TCP_State::Closing_ClosedByPS2ThenRemote_WaitingForAck:
				return CloseByPS2Stage4(&tcp);

			case TCP_State::Closing_ClosedByRemote:
				if (tcp.GetFIN() == true) // Connection close part 3, received FIN from PS2
					return CloseByRemoteStage3_4(&tcp);

				return SendData(&tcp);
			case TCP_State::Closing_ClosedByRemoteThenPS2_WaitingForAck:
				return CloseByRemoteStage2_ButAfter4(&tcp);
			case TCP_State::CloseCompleted:
				Console.Error("DEV9: TCP: Attempt to send to a closed TCP connection");
				return false;
			default:
				CloseByRemoteRST();
				Console.Error("DEV9: TCP: Invalid TCP state");
				return true;
		}
	}

	// PS2 sent SYN
	bool TCP_Session::SendConnect(TCP_Packet* tcp)
	{
		// Expects SYN Packet
		destPort = tcp->destinationPort;
		srcPort = tcp->sourcePort;

		if (tcp->GetSYN() == false)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt to send data to a non connected connection");
			return true;
		}
		expectedSeqNumber = tcp->sequenceNumber + 1;
		receivedPS2SeqNumbers.assign(receivedPS2SeqNumberCount, tcp->sequenceNumber);

		ResetMyNumbers();

		for (const auto* option : tcp->options)
		{
			switch (option->GetCode())
			{
				case 0: // End
				case 1: // Nop
					continue;
				case 2: // MSS
					maxSegmentSize = static_cast<const TCPopMSS*>(option)->maxSegmentSize;
					break;
				case 3: // WindowScale
					windowScale = static_cast<const TCPopWS*>(option)->windowScale;
					if (windowScale > 0)
						Console.Error("DEV9: TCP: Non-zero window scale option");
					break;
				case 8: // TimeStamp
					lastReceivedTimeStamp = static_cast<const TCPopTS*>(option)->senderTimeStamp;
					sendTimeStamps = true;
					timeStampStart = std::chrono::steady_clock::now();
					break;
				default:
					Console.Error("DEV9: TCP: Got unknown option %d", option->GetCode());
					break;
			}
		}

		windowSize.store(tcp->windowSize << windowScale);

		CloseSocket();

		client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (client == INVALID_SOCKET)
		{
			Console.Error("DEV9: TCP: Failed to open socket. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif
			RaiseEventConnectionClosed();
			return false;
		}

		int ret;
		if (adapterIP.integer != 0)
		{
			sockaddr_in endpoint{};
			endpoint.sin_family = AF_INET;
			endpoint.sin_addr = std::bit_cast<in_addr>(adapterIP);

			ret = bind(client, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));

			if (ret != 0)
				Console.Error("DEV9: UDP: Failed to bind socket. Error: %d",
#ifdef _WIN32
					WSAGetLastError());
#elif defined(__POSIX__)
					errno);
#endif
		}

#ifdef _WIN32
		u_long blocking = 1;
		ret = ioctlsocket(client, FIONBIO, &blocking);
#elif defined(__POSIX__)
		int blocking = 1;
		ret = ioctl(client, FIONBIO, &blocking);
#endif

		if (ret != 0)
			Console.Error("DEV9: TCP: Failed to set non-blocking. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif


		constexpr int noDelay = true; // BOOL on Windows
		ret = setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

		if (ret != 0)
			Console.Error("DEV9: TCP: Failed to set TCP_NODELAY. Error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		sockaddr_in endpoint{};
		endpoint.sin_family = AF_INET;
		endpoint.sin_addr = std::bit_cast<in_addr>(destIP);
		endpoint.sin_port = htons(destPort);

		ret = connect(client, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));

		if (ret != 0)
		{
#ifdef _WIN32
			const int err = WSAGetLastError();
			if (err != WSAEWOULDBLOCK)
#elif defined(__POSIX__)
			const int err = errno;
			if (err != EWOULDBLOCK && err != EINPROGRESS)
#endif
			{
				Console.Error("DEV9: TCP: Failed to connect socket. Error: %d", err);
				RaiseEventConnectionClosed();
				return false;
			}
			// Completion of the socket connection is checked in Recv.
		}

		state = TCP_State::SendingSYN_ACK;
		return true;
	}

	void TCP_Session::UpdateTimeStamp(const TCP_Packet* tcp)
	{
		for (const auto* option : tcp->options)
		{
			switch (option->GetCode())
			{
				case 0: // End
				case 1: // Nop
					break;
				case 8: // Timestamp
					lastReceivedTimeStamp = static_cast<const TCPopTS*>(option)->senderTimeStamp;
					break;
				default:
					Console.Error("DEV9: TCP: Got unknown option %d", option->GetCode());
					break;
			}
		}
	}

	// PS2 responding to our SYN-ACK (by sending ACK)
	bool TCP_Session::SendConnected(TCP_Packet* tcp)
	{
		if (tcp->GetSYN() == true)
		{
			if (CheckRepeatSYNNumbers(tcp) == NumCheckResult::Bad)
			{
				CloseByRemoteRST();
				Console.Error("DEV9: TCP: Invalid repeated SYN (SentSYN_ACK)");
				return true;
			}
			return true; // Ignore reconnect attempts while we are still attempting connection
		}
		if (CheckNumbers(tcp) == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP numbers received");
			return true;
		}

		UpdateTimeStamp(tcp);
		state = TCP_State::Connected;
		return true;
	}

	bool TCP_Session::SendData(TCP_Packet* tcp)
	{
		if (tcp->GetSYN())
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt to connect to an existing connection");
			return true;
		}

		if (tcp->GetURG())
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Urgent data not supported");
			return true;
		}

		UpdateTimeStamp(tcp);

		if (CheckNumbers(tcp) == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP numbers received");
			return true;
		}

		if (tcp->GetPayload()->GetLength() == 0)
			return true;

		const int delta = GetDelta(expectedSeqNumber, tcp->sequenceNumber);
		pxAssert(delta >= 0);
		if (tcp->GetPayload()->GetLength() - delta > 0)
		{
			DevCon.WriteLn("DEV9: TCP: [PS2] Sending: %d bytes", tcp->GetPayload()->GetLength());

			receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
			receivedPS2SeqNumbers.push_back(expectedSeqNumber);

			int sent = 0;
			PayloadPtr* payload = static_cast<PayloadPtr*>(tcp->GetPayload());
			while (sent != payload->GetLength())
			{
				const int ret = send(client, reinterpret_cast<const char*>(&payload->data[sent]), payload->GetLength() - sent, 0);

				if (ret == SOCKET_ERROR)
				{
#ifdef _WIN32
					const int err = WSAGetLastError();
					if (err == WSAEWOULDBLOCK)
#elif defined(__POSIX__)
					const int err = errno;
					if (err == EWOULDBLOCK)
#endif
						std::this_thread::yield();
					else
					{
						CloseByRemoteRST();
						Console.Error("DEV9: TCP: Send error: %d", err);
						return true;
					}
				}
				else
					sent += ret;
			}

			expectedSeqNumber += tcp->GetPayload()->GetLength() - delta;
		}

		std::unique_ptr<TCP_Packet> ret = CreateBasePacket();
		ret->SetACK(true);

		PushRecvBuff(ReceivedPayload{destIP, std::move(ret)});
		return true;
	}

	bool TCP_Session::SendNoData(TCP_Packet* tcp)
	{
		if (tcp->GetSYN() == true)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Attempt to connect to an existing connection");
			return true;
		}
		UpdateTimeStamp(tcp);

		ValidateEmptyPacket(tcp);

		return true;
	}

	TCP_Session::NumCheckResult TCP_Session::CheckRepeatSYNNumbers(TCP_Packet* tcp)
	{
		if (tcp->sequenceNumber != expectedSeqNumber - 1)
		{
			Console.Error("DEV9: TCP: [PS2] Sent unexpected sequence number from repeated SYN packet, got %u expected %u", tcp->sequenceNumber, (expectedSeqNumber - 1));
			return NumCheckResult::Bad;
		}
		return NumCheckResult::OK;
	}

	TCP_Session::NumCheckResult TCP_Session::CheckNumbers(TCP_Packet* tcp, bool rejectOldSeq)
	{
		const auto [oldestAck, seqNum] = GetAckRange();

		if (GetDelta(tcp->acknowledgementNumber, seqNum) > 0 || GetDelta(tcp->acknowledgementNumber, oldestAck) < 0)
		{
			Console.Error("DEV9: TCP: [PS2] Sent acknowledgement outside the valid range, got %u expected %u..%u", tcp->acknowledgementNumber, oldestAck, seqNum);
			return NumCheckResult::Bad;
		}

		windowSize.store(tcp->windowSize << windowScale);
		if (tcp->acknowledgementNumber == seqNum)
			myNumberACKed.store(true);

		UpdateReceivedAckNumber(tcp);

		if (tcp->sequenceNumber == expectedSeqNumber)
			return NumCheckResult::OK;

		if (rejectOldSeq)
		{
			Console.Error("DEV9: TCP: [PS2] Sent unexpected sequence number, got %u expected %u", tcp->sequenceNumber, expectedSeqNumber);
			return NumCheckResult::Bad;
		}

		if (tcp->GetPayload()->GetLength() == 0)
		{
			Console.Error("DEV9: TCP: [PS2] Sent unexpected sequence number in an empty packet, got %u expected %u", tcp->sequenceNumber, expectedSeqNumber);
			return NumCheckResult::OldSeq;
		}

		if (std::find(receivedPS2SeqNumbers.begin(), receivedPS2SeqNumbers.end(), tcp->sequenceNumber) == receivedPS2SeqNumbers.end())
		{
			Console.Error("DEV9: TCP: [PS2] Sent outdated sequence number in a data packet, got %u expected %u", tcp->sequenceNumber, expectedSeqNumber);
			return NumCheckResult::OldSeq;
		}

		Console.Error("DEV9: TCP: [PS2] Sent unexpected sequence number in a data packet, got %u expected %u", tcp->sequenceNumber, expectedSeqNumber);
		return NumCheckResult::Bad;
	}

	bool TCP_Session::ValidateEmptyPacket(TCP_Packet* tcp, bool ignoreOld)
	{
		if (CheckNumbers(tcp, !ignoreOld) == NumCheckResult::Bad)
		{
			CloseByRemoteRST();
			Console.Error("DEV9: TCP: Bad TCP numbers received");
			return true;
		}

		const int length = tcp->GetPayload()->GetLength();
		if (length <= 0)
			return false;

		const int delta = GetDelta(expectedSeqNumber, tcp->sequenceNumber);
		pxAssert(delta >= 0);
		if (delta >= length)
			return false;

		CloseByRemoteRST();
		Console.Error("DEV9: TCP: Invalid packet, packet has data");
		return true;
	}

	// Connection Closing Finished in CloseByPS2Stage4
	bool TCP_Session::CloseByPS2Stage1_2(TCP_Packet* tcp)
	{
		if (ValidateEmptyPacket(tcp, false)) // Check if valid packet for FIN
			return true;

		receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
		receivedPS2SeqNumbers.push_back(expectedSeqNumber);
		expectedSeqNumber += 1;

		state = TCP_State::Closing_ClosedByPS2;

		const int result = shutdown(client, SD_SEND);
		if (result == SOCKET_ERROR)
			Console.Error("DEV9: TCP: Shutdown SD_SEND error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		// Connection close part 2, send ACK to PS2
		std::unique_ptr<TCP_Packet> ret = CreateBasePacket();

		ret->SetACK(true);
		PushRecvBuff(ReceivedPayload{destIP, std::move(ret)});

		return true;
	}

	// PS2 responding to server response to PS2 closing connection
	bool TCP_Session::CloseByPS2Stage4(TCP_Packet* tcp)
	{
		// Close part 4, receive ACK from PS2

		if (ValidateEmptyPacket(tcp))
			return true;

		if (myNumberACKed.load())
		{
			CloseSocket();
			// recv buffer should be empty
			state = TCP_State::CloseCompleted;
			RaiseEventConnectionClosed();
		}

		return true;
	}

	bool TCP_Session::CloseByRemoteStage2_ButAfter4(TCP_Packet* tcp)
	{
		if (ValidateEmptyPacket(tcp))
			return true;

		if (myNumberACKed.load())
		{
			CloseSocket();
			// Receive buffer may not be empty
			state = TCP_State::CloseCompletedFlushBuffer;
		}
		return true;
	}

	bool TCP_Session::CloseByRemoteStage3_4(TCP_Packet* tcp)
	{
		if (ValidateEmptyPacket(tcp, false)) // Check if valid packet for FIN
			return true;

		receivedPS2SeqNumbers.erase(receivedPS2SeqNumbers.begin());
		receivedPS2SeqNumbers.push_back(expectedSeqNumber);
		expectedSeqNumber += 1;

		const int result = shutdown(client, SD_SEND);
		if (result == SOCKET_ERROR)
			Console.Error("DEV9: TCP: Shutdown SD_SEND error: %d",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif

		std::unique_ptr<TCP_Packet> ret = CreateBasePacket();

		ret->SetACK(true);

		PushRecvBuff(ReceivedPayload{destIP, std::move(ret)});

		if (myNumberACKed.load())
		{
			CloseSocket();
			state = TCP_State::CloseCompletedFlushBuffer;
			// Receive buffer may not be empty
		}
		else
			state = TCP_State::Closing_ClosedByRemoteThenPS2_WaitingForAck;

		return true;
	}

	// Error on sending data
	void TCP_Session::CloseByRemoteRST()
	{
		std::unique_ptr<TCP_Packet> reterr = CreateBasePacket();
		reterr->SetRST(true);
		PushRecvBuff(ReceivedPayload{destIP, std::move(reterr)});

		CloseSocket();
		state = TCP_State::CloseCompletedFlushBuffer;
	}
} // namespace Sessions
