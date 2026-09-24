/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// TheSuperHackers @feature bill-rich 24/09/2026 Live spectating of online matches in progress.
// The lobby owner's client streams the bytes of its replay recording to the service as
// the match runs; an observer lists the streams from the custom lobby (/watch), pulls
// the bytes down behind the service's broadcast delay into a local replay file, and the
// recorder plays that file as it grows (RECORDERMODETYPE_LIVE_OBSERVER).

#pragma once

#include "NGMP_include.h"
#include "Common/AsciiString.h"

struct LivestreamEntry
{
	int64_t lobby_id = -1;
	std::string name;
	std::string map_name;
	std::string map_path;
	int players = 0;
	int64_t total_bytes = 0;
	int seconds_live = 0;
};

class NGMP_OnlineServices_LivestreamInterface
{
public:
	NGMP_OnlineServices_LivestreamInterface();
	~NGMP_OnlineServices_LivestreamInterface();

	// Called every engine frame from the online services manager.
	void Tick();

	// Observer side.
	void ListStreams(std::function<void(bool bSuccess, std::vector<LivestreamEntry> streams)> cb);
	const std::vector<LivestreamEntry>& GetLastStreamList() const { return m_lastStreamList; }
	bool StartWatching(int64_t lobbyID);
	void StopWatching();
	bool IsWatching() const { return m_watchLobbyID != -1; }

	// Streamer side (lobby owner while recording an online match).
	bool IsStreaming() const { return m_streamLobbyID != -1; }

private:
	// streamer
	void TickStreamer(int64_t nowMS);
	void SendChunk(const std::vector<uint8_t>& bytes);
	void SendEnd();
	void StopStreaming();

	// observer
	void TickObserver(int64_t nowMS);
	void PollObserver();
	bool AppendObserverBytes(const std::vector<uint8_t>& bytes);
	AsciiString GetObserverFilePath() const;

	std::vector<LivestreamEntry> m_lastStreamList;

	// streamer state
	int64_t m_streamLobbyID = -1;
	int64_t m_streamOffset = 0;      // bytes of the replay file already accepted by the service
	AsciiString m_streamFilePath;
	bool m_streamRequestInFlight = false;
	bool m_streamEnding = false;
	int64_t m_streamLastSendMS = 0;
	int m_streamFailures = 0;

	// observer state
	int64_t m_watchLobbyID = -1;
	int64_t m_watchFrom = 0;         // bytes already appended to the local file
	bool m_watchRequestInFlight = false;
	bool m_watchPlaybackStarted = false;
	bool m_watchEnded = false;
	int64_t m_watchLastPollMS = 0;
	int m_watchFailures = 0;
};
