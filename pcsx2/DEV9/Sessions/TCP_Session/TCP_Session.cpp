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
		_OldMyNumbers.pop_front();

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
			if (dataRecoveryState != DataRecoveryState::None)
				dataRecoveryState = DataRecoveryState::RetransmitPending;
		}

		if (sentData.empty())
		{
			dataRecoveryState = DataRecoveryState::None;
			duplicateACKs = 0;
			return;
		}

		if (delta == 0)
		{
			const bool duplicate = window > 0 && window == previousWindow &&
			                       tcp->GetACK() && !tcp->GetSYN() && !tcp->GetFIN() &&
			                       tcp->GetPayload()->GetLength() == 0;
			duplicateACKs = duplicate ? duplicateACKs + 1 : 0;

			if (duplicateACKs == 3)
				dataRecoveryState = DataRecoveryState::RetransmitPending;
		}

		if (previousWindow == 0 && window > 0)
			dataRecoveryState = DataRecoveryState::RetransmitPending;
	}

	std::optional<ReceivedPayload> TCP_Session::RecvDataRetransmission()
	{
		std::unique_ptr<PayloadData> data;
		u32 sequence;

		{
			std::lock_guard numberlock(myNumberSentry);

			if (sentData.empty())
				return std::nullopt;

			const bool requested = dataRecoveryState == DataRecoveryState::RetransmitPending;
			const auto now = std::chrono::steady_clock::now();
			if (!requested && now < retransmitDeadline)
				return std::nullopt;

			if (!requested)
				dataRecoveryState = DataRecoveryState::WaitingForAck;

			const int window = windowSize.load();
			const SentData& first = sentData.front();
			const int length = std::min({static_cast<int>(first.bytes.size() - first.offset),
				window, maxSegmentSize - (sendTimeStamps ? 12 : 0)});
			if (length <= 0)
				return std::nullopt;

			if (!requested)
				retransmitTimeout = std::min(retransmitTimeout * 2, std::chrono::seconds(60));

			data = std::make_unique<PayloadData>(length);
			memcpy(data->data.get(), first.bytes.data() + first.offset, length);
			sequence = first.sequence + static_cast<u32>(first.offset);

			dataRecoveryState = DataRecoveryState::WaitingForAck;
			retransmitDeadline = now + retransmitTimeout;
		}

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

	int TCP_Session::GetReceiveSize()
	{
		std::lock_guard numberlock(myNumberSentry);

		if (dataRecoveryState != DataRecoveryState::None ||
			GetDelta(_ReceivedAckNumber, _OldMyNumbers.front()) <= 0)
			return 0;

		const int outstanding = GetDelta(_MySequenceNumber, _ReceivedAckNumber);
		return std::min(maxSegmentSize - (sendTimeStamps ? 12 : 0), windowSize.load() - outstanding);
	}

	std::tuple<u32, u32> TCP_Session::GetAckRange()
	{
		std::lock_guard numberlock(myNumberSentry);

		const u32 oldestAck = GetDelta(_ReceivedAckNumber, _OldMyNumbers.front()) < 0 ?
		                          _ReceivedAckNumber :
		                          _OldMyNumbers.front();
		return {oldestAck, _MySequenceNumber};
	}

	void TCP_Session::ResetMyNumbers()
	{
		std::lock_guard numberlock(myNumberSentry);

		_MySequenceNumber = 1;
		_ReceivedAckNumber = 1;

		sentData.clear();
		dataRecoveryState = DataRecoveryState::None;
		duplicateACKs = 0;
		lastAckWindow = 0;

		_OldMyNumbers.assign(oldMyNumCount, 1);
	}

	TCP_Session::TCP_Session(ConnectionKey parKey, IP_Address parAdapterIP)
		: BaseSession(parKey, parAdapterIP)
	{
	}

	s32 TCP_Session::GetDelta(u32 a, u32 b)
	{
		return static_cast<s32>(a - b);
	}

	void TCP_Session::AddTimeStampOption(TCP_Packet* tcp)
	{
		if (!sendTimeStamps)
			return;

		tcp->options.push_back(new TCPopNOP());
		tcp->options.push_back(new TCPopNOP());

		const auto timestampChrono = std::chrono::steady_clock::now() - timeStampStart;
		const u32 timestampSeconds = std::chrono::duration_cast<std::chrono::seconds>(timestampChrono).count() % UINT_MAX;

		tcp->options.push_back(new TCPopTS(timestampSeconds, lastReceivedTimeStamp));
	}

	std::unique_ptr<TCP_Packet> TCP_Session::CreateBasePacket(PayloadData* data)
	{
		if (data == nullptr)
			data = new PayloadData(0);

		std::unique_ptr<TCP_Packet> ret = std::make_unique<TCP_Packet>(data);

		ret->sourcePort = destPort;
		ret->destinationPort = srcPort;

		ret->sequenceNumber = GetMyNumber();
		ret->acknowledgementNumber = expectedSeqNumber;

		ret->windowSize = 2 * maxSegmentSize;

		AddTimeStampOption(ret.get());

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
