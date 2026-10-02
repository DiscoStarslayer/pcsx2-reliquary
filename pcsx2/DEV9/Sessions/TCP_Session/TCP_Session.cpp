// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "TCP_Session.h"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winsock2.h>
#else
#include <unistd.h>
#endif

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::TCP;

namespace Sessions
{
	void TCP_Session::PushRecvBuff(ReceivedPayload tcp)
	{
		_recvBuff.Enqueue(std::move(tcp));
	}
	std::optional<ReceivedPayload> TCP_Session::PopRecvBuff()
	{
		ReceivedPayload ret;
		if (_recvBuff.Dequeue(&ret))
			return ret;
		else
			return std::nullopt;
	}

	void TCP_Session::IncrementMyNumber(u32 amount, const u8* data)
	{
		std::lock_guard numberlock(myNumberSentry);
		if (data && amount != 0)
		{
			if (sentData.empty())
			{
				retransmitTimeout = std::chrono::seconds(1);
				retransmitDeadline = std::chrono::steady_clock::now() + retransmitTimeout;
			}
			sentData.push_back({_MySequenceNumber, std::vector<u8>(data, data + amount)});
		}
		_OldMyNumbers.push_back(_MySequenceNumber);
		_OldMyNumbers.erase(_OldMyNumbers.begin());

		_MySequenceNumber += amount;
	}
	void TCP_Session::AcknowledgeSentData(u32 ack)
	{
		while (!sentData.empty())
		{
			SentData& first = sentData.front();
			const int acknowledged = GetDelta(ack, first.sequence);
			if (acknowledged < 0)
				break;
			if (static_cast<size_t>(acknowledged) < first.bytes.size())
			{
				first.offset = static_cast<size_t>(acknowledged);
				break;
			}
			sentData.pop_front();
		}
	}
	void TCP_Session::UpdateReceivedAckNumber(const TCP_Packet* tcp)
	{
		std::lock_guard numberlock(myNumberSentry);
		const u32 ack = tcp->acknowledgementNumber;
		const int delta = GetDelta(ack, _ReceivedAckNumber);
		if (delta < 0)
			return;

		const int window = windowSize.load();
		const int previousWindow = lastAckWindow;
		lastAckWindow = window;
		if (delta > 0)
		{
			_ReceivedAckNumber = ack;
			duplicateACKs = 0;
			retransmitTimeout = std::chrono::seconds(1);
			retransmitDeadline = std::chrono::steady_clock::now() + retransmitTimeout;
			AcknowledgeSentData(ack);
			if (dataRecoveryActive)
				retransmitRequested = true;
		}
		else if (!sentData.empty() && window > 0 && window == previousWindow &&
				 tcp->GetACK() && !tcp->GetSYN() && !tcp->GetFIN() && tcp->GetPayload()->GetLength() == 0)
		{
			if (++duplicateACKs == 3)
				retransmitRequested = true;
		}
		else
			duplicateACKs = 0;

		if (sentData.empty())
		{
			dataRecoveryActive = false;
			retransmitRequested = false;
			duplicateACKs = 0;
			return;
		}
		if (previousWindow == 0 && window > 0)
			retransmitRequested = true;
		if (retransmitRequested)
			dataRecoveryActive = true;
	}

	std::optional<ReceivedPayload> TCP_Session::RecvDataRetransmission(bool& waiting)
	{
		std::unique_ptr<PayloadData> data;
		u32 sequence;
		waiting = false;
		{
			std::lock_guard numberlock(myNumberSentry);
			if (sentData.empty())
				return std::nullopt;
			waiting = dataRecoveryActive;
			const auto now = std::chrono::steady_clock::now();
			if (!retransmitRequested && now < retransmitDeadline)
				return std::nullopt;

			dataRecoveryActive = true;
			waiting = true;
			const int window = windowSize.load();
			const SentData& first = sentData.front();
			const int length = std::min({static_cast<int>(first.bytes.size() - first.offset),
				window, maxSegmentSize - (sendTimeStamps ? 12 : 0)});
			if (length <= 0)
				return std::nullopt;

			if (!retransmitRequested)
				retransmitTimeout = std::min(retransmitTimeout * 2, std::chrono::seconds(60));
			data = std::make_unique<PayloadData>(length);
			memcpy(data->data.get(), first.bytes.data() + first.offset, length);
			sequence = first.sequence + static_cast<u32>(first.offset);
			retransmitRequested = false;
			retransmitDeadline = now + retransmitTimeout;
		}

		// Retransmissions reuse sequence numbers and never read the host socket.
		std::unique_ptr<TCP_Packet> packet = CreateBasePacket(data.release());
		packet->sequenceNumber = sequence;
		packet->SetACK(true);
		packet->SetPSH(true);
		return ReceivedPayload{destIP, std::move(packet)};
	}
	u32 TCP_Session::GetMyNumber()
	{
		std::lock_guard numberlock(myNumberSentry);
		return _MySequenceNumber;
	}
	u32 TCP_Session::GetOutstandingSequenceLength()
	{
		std::lock_guard numberlock(myNumberSentry);
		return GetDelta(_MySequenceNumber, _ReceivedAckNumber);
	}
	bool TCP_Session::ShouldWaitForAck()
	{
		std::lock_guard numberlock(myNumberSentry);
		return GetDelta(_ReceivedAckNumber, _OldMyNumbers[0]) <= 0;
	}
	std::tuple<u32, u32> TCP_Session::GetAckRange()
	{
		std::lock_guard numberlock(myNumberSentry);
		const u32 oldestAck = GetDelta(_ReceivedAckNumber, _OldMyNumbers.front()) < 0 ? _ReceivedAckNumber : _OldMyNumbers.front();
		return {oldestAck, _MySequenceNumber};
	}
	void TCP_Session::ResetMyNumbers()
	{
		std::lock_guard numberlock(myNumberSentry);
		_MySequenceNumber = 1;
		_ReceivedAckNumber = 1;
		sentData.clear();
		dataRecoveryActive = false;
		retransmitRequested = false;
		duplicateACKs = 0;
		lastAckWindow = 0;
		_OldMyNumbers.clear();
		for (int i = 0; i < oldMyNumCount; i++)
			_OldMyNumbers.push_back(1);
	}

	TCP_Session::TCP_Session(ConnectionKey parKey, IP_Address parAdapterIP)
		: BaseSession(parKey, parAdapterIP)
	{
	}

	s32 TCP_Session::GetDelta(u32 a, u32 b)
	{
		s64 delta = static_cast<s64>(a) - static_cast<s64>(b);
		if (delta > 0.5 * UINT_MAX)
		{
			delta = -static_cast<s64>(UINT_MAX) + a - b - 1;
			Console.Error("DEV9: TCP: [PS2] Sequence number overflow detected");
			Console.Error("DEV9: TCP: [PS2] New data offset: %d bytes", delta);
		}
		if (delta < -0.5 * UINT_MAX)
		{
			delta = UINT_MAX - b + a + 1;
			Console.Error("DEV9: TCP: [PS2] Sequence number overflow detected");
			Console.Error("DEV9: TCP: [PS2] New data offset: %d bytes", delta);
		}
		return delta;
	}

	std::unique_ptr<TCP_Packet> TCP_Session::CreateBasePacket(PayloadData* data)
	{
		//DevCon.WriteLn("Creating base packet");
		if (data == nullptr)
			data = new PayloadData(0);

		std::unique_ptr<TCP_Packet> ret = std::make_unique<TCP_Packet>(data);

		// Setup common packet infomation
		ret->sourcePort = destPort;
		ret->destinationPort = srcPort;

		ret->sequenceNumber = GetMyNumber();
		//DevCon.WriteLn("With MySeq: %u", ret->sequenceNumber);
		ret->acknowledgementNumber = expectedSeqNumber;
		//DevCon.WriteLn("With MyAck: %u", ret->acknowledgementNumber);

		ret->windowSize = 2 * maxSegmentSize;

		if (sendTimeStamps)
		{
			ret->options.push_back(new TCPopNOP());
			ret->options.push_back(new TCPopNOP());

			const auto timestampChrono = std::chrono::steady_clock::now() - timeStampStart;
			const u32 timestampSeconds = std::chrono::duration_cast<std::chrono::seconds>(timestampChrono).count() % UINT_MAX;

			ret->options.push_back(new TCPopTS(timestampSeconds, lastRecivedTimeStamp));
		}
		return ret;
	}

	void TCP_Session::CloseSocket()
	{
		if (client != INVALID_SOCKET)
		{
#ifdef _WIN32
			closesocket(client);
#elif defined(__POSIX__)
			::close(client);
#endif
			client = INVALID_SOCKET;
		}
	}

	void TCP_Session::Reset()
	{
		RaiseEventConnectionClosed();
	}

	TCP_Session::~TCP_Session()
	{
		CloseSocket();

		// Clear out _recvBuff
		while (!_recvBuff.IsQueueEmpty())
		{
			ReceivedPayload retPay;
			if (!_recvBuff.Dequeue(&retPay))
			{
				using namespace std::chrono_literals;
				std::this_thread::sleep_for(1ms);
				continue;
			}
		}
	}
} // namespace Sessions
