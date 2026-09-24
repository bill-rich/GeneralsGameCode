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

// TheSuperHackers @feature bill-rich 24/09/2026 Live spectating transport: streams the
// host's replay recording to the service and pulls it back for observers. See the header.

#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_LivestreamInterface.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "Common/Recorder.h"
#include "Common/FileSystem.h"
#include "GameClient/Shell.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"

std::string Base64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> Base64Decode(const std::string& encodedData);

namespace
{
	const int STREAM_SEND_INTERVAL_MS = 1000;
	const int STREAM_POST_TIMEOUT_MS = 20000;    // a 340 KB base64 chunk on a slow uplink outlives the default 5 s
	const int STREAM_MAX_CHUNK_BYTES = 256 * 1024;
	const int STREAM_MAX_FAILURES = 30;          // consecutive failed sends before the streamer gives up (~30 s)
	const int WATCH_POLL_INTERVAL_MS = 1000;
	const int WATCH_MAX_FAILURES = 60;           // consecutive failed polls before the observer gives up
	const int64_t WATCH_MIN_BYTES_TO_START = 4096; // enough for the replay header plus the first frames
	const char* WATCH_FILE_NAME = "LiveObserver";

	int64_t NowMS()
	{
		return (int64_t)GetTickCount64(); // 64-bit: timeGetTime() wraps after 49.7 days and the interval math would go negative
	}

	// Reads [offset, offset+maxBytes) of a file that another handle is still writing.
	// A fresh handle every time so the recorder's own buffered writes, which it flushes
	// after every frame, are always visible.
	std::vector<uint8_t> ReadFileTail(const char* path, int64_t offset, int maxBytes)
	{
		std::vector<uint8_t> bytes;
		FILE* fp = fopen(path, "rb");
		if (fp == nullptr)
		{
			return bytes;
		}
		if (_fseeki64(fp, offset, SEEK_SET) == 0)
		{
			bytes.resize(maxBytes);
			size_t n = fread(bytes.data(), 1, maxBytes, fp);
			bytes.resize(n);
		}
		fclose(fp);
		return bytes;
	}
}

NGMP_OnlineServices_LivestreamInterface::NGMP_OnlineServices_LivestreamInterface()
{
}

NGMP_OnlineServices_LivestreamInterface::~NGMP_OnlineServices_LivestreamInterface()
{
}

void NGMP_OnlineServices_LivestreamInterface::Tick()
{
	const int64_t nowMS = NowMS();
	TickStreamer(nowMS);
	TickObserver(nowMS);
}

// ---------------------------------------------------------------------------
// Streamer: the lobby owner while an online match is being recorded.
// ---------------------------------------------------------------------------

void NGMP_OnlineServices_LivestreamInterface::TickStreamer(int64_t nowMS)
{
	NGMP_OnlineServices_LobbyInterface* pLobby = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	NGMP_OnlineServices_AuthInterface* pAuth = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

	const bool bRecordingOnlineMatch = TheRecorder != nullptr && TheRecorder->getMode() == RECORDERMODETYPE_RECORD
		&& TheNGMPGame != nullptr && pLobby != nullptr && pLobby->IsInLobby()
		&& pAuth != nullptr && pLobby->GetCurrentLobbyOwnerID() == pAuth->GetUserID();

	if (!IsStreaming())
	{
		if (!bRecordingOnlineMatch)
		{
			return;
		}
		m_streamLobbyID = pLobby->GetCurrentLobby().lobbyID;
		m_streamOffset = 0;
		m_streamFilePath = RecorderClass::getReplayDir();
		m_streamFilePath.concat(RecorderClass::getLastReplayFileName());
		m_streamFilePath.concat(RecorderClass::getReplayExtention());
		m_streamRequestInFlight = false;
		m_streamEnding = false;
		m_streamLastSendMS = 0;
		m_streamFailures = 0;
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: streaming lobby %lld from %s", (long long)m_streamLobbyID, m_streamFilePath.str());
	}

	if (m_streamRequestInFlight || nowMS - m_streamLastSendMS < STREAM_SEND_INTERVAL_MS)
	{
		return;
	}

	// The recording stopped: push whatever is left, then tell the service the stream is over.
	if (!bRecordingOnlineMatch)
	{
		m_streamEnding = true;
	}

	std::vector<uint8_t> bytes = ReadFileTail(m_streamFilePath.str(), m_streamOffset, STREAM_MAX_CHUNK_BYTES);
	if (!bytes.empty())
	{
		SendChunk(bytes);
	}
	else if (m_streamEnding)
	{
		SendEnd();
	}
	else
	{
		m_streamLastSendMS = nowMS;
	}
}

void NGMP_OnlineServices_LivestreamInterface::SendChunk(const std::vector<uint8_t>& bytes)
{
	std::string strURI = std::format("{}/{}/chunk", NGMP_OnlineServicesManager::GetAPIEndpoint("Livestream"), m_streamLobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["offset"] = m_streamOffset;
	j["data"] = Base64Encode(bytes);
	std::string strPostData = j.dump();

	const size_t sentBytes = bytes.size();
	m_streamRequestInFlight = true;
	m_streamLastSendMS = NowMS();
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [this, sentBytes](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			m_streamRequestInFlight = false;
			if (bSuccess && statusCode == 200)
			{
				m_streamOffset += (int64_t)sentBytes;
				m_streamFailures = 0;
				return;
			}

			// 409 carries the service's real length so we can resume from there after a
			// lost reply or a duplicate send.
			if (statusCode == 409)
			{
				try
				{
					nlohmann::json jResp = nlohmann::json::parse(strBody);
					if (jResp.contains("total"))
					{
						m_streamOffset = jResp["total"].get<int64_t>();
					}
				}
				catch (...)
				{
				}
			}

			++m_streamFailures;
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: chunk send failed (status %d, failure %d)", statusCode, m_streamFailures);
			if (m_streamFailures >= STREAM_MAX_FAILURES || statusCode == 401)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: giving up streaming lobby %lld", (long long)m_streamLobbyID);
				StopStreaming();
			}
		}, nullptr, STREAM_POST_TIMEOUT_MS);
}

void NGMP_OnlineServices_LivestreamInterface::SendEnd()
{
	std::string strURI = std::format("{}/{}/end", NGMP_OnlineServicesManager::GetAPIEndpoint("Livestream"), m_streamLobbyID);
	std::map<std::string, std::string> mapHeaders;
	const int64_t lobbyID = m_streamLobbyID;

	m_streamRequestInFlight = true;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "{}", [this, lobbyID](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			m_streamRequestInFlight = false;
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: ended stream for lobby %lld (status %d)", (long long)lobbyID, statusCode);
			StopStreaming();
		});
}

void NGMP_OnlineServices_LivestreamInterface::StopStreaming()
{
	m_streamLobbyID = -1;
	m_streamOffset = 0;
	m_streamFilePath.clear();
	m_streamEnding = false;
	m_streamFailures = 0;
}

// ---------------------------------------------------------------------------
// Observer: pulls a stream into a local replay file and plays it as it grows.
// ---------------------------------------------------------------------------

void NGMP_OnlineServices_LivestreamInterface::ListStreams(std::function<void(bool bSuccess, std::vector<LivestreamEntry> streams)> cb)
{
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Livestream");
	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [this, cb](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			std::vector<LivestreamEntry> streams;
			bool bParsed = false;
			if (bSuccess && statusCode == 200)
			{
				try
				{
					nlohmann::json jResp = nlohmann::json::parse(strBody);
					if (jResp.contains("streams") && jResp["streams"].is_array())
					{
						for (const auto& jStream : jResp["streams"])
						{
							LivestreamEntry entry;
							entry.lobby_id = jStream.value("lobby_id", (int64_t)-1);
							entry.name = jStream.value("name", std::string());
							entry.map_name = jStream.value("map_name", std::string());
							entry.map_path = jStream.value("map_path", std::string());
							entry.players = jStream.value("players", 0);
							entry.total_bytes = jStream.value("total_bytes", (int64_t)0);
							entry.seconds_live = jStream.value("seconds_live", 0);
							streams.push_back(entry);
						}
						bParsed = true;
					}
				}
				catch (...)
				{
				}
			}
			if (bParsed)
			{
				m_lastStreamList = streams;
			}
			if (cb)
			{
				cb(bParsed, streams);
			}
		});
}

AsciiString NGMP_OnlineServices_LivestreamInterface::GetObserverFilePath() const
{
	AsciiString path = RecorderClass::getReplayDir();
	path.concat(WATCH_FILE_NAME);
	path.concat(RecorderClass::getReplayExtention());
	return path;
}

bool NGMP_OnlineServices_LivestreamInterface::HasMapFor(const LivestreamEntry& stream) const
{
	if (TheMapCache == nullptr || stream.map_path.empty())
	{
		return false;
	}
	AsciiString mapPath = stream.map_path.c_str();
	mapPath.toLower();
	return TheMapCache->findMap(mapPath) != nullptr;
}

bool NGMP_OnlineServices_LivestreamInterface::StartWatching(const LivestreamEntry& stream)
{
	const int64_t lobbyID = stream.lobby_id;
	// isInGame() is true on the shell map too, so gate on the shell being up instead.
	if (IsWatching() || TheRecorder == nullptr || TheShell == nullptr || !TheShell->isShellActive())
	{
		return false;
	}
	// The replay names the map; without it startNewGame has nothing to load.
	if (!HasMapFor(stream))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: map %s is not installed, not watching lobby %lld", stream.map_path.c_str(), (long long)lobbyID);
		return false;
	}

	// Start the local file empty; the service replays the stream from byte 0.
	TheFileSystem->createDirectory(RecorderClass::getReplayDir());
	FILE* fp = fopen(GetObserverFilePath().str(), "wb");
	if (fp == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: cannot create %s", GetObserverFilePath().str());
		return false;
	}
	fclose(fp);

	m_watchLobbyID = lobbyID;
	m_watchFrom = 0;
	m_watchRequestInFlight = false;
	m_watchPlaybackStarted = false;
	m_watchEnded = false;
	m_watchLastPollMS = 0;
	m_watchFailures = 0;
	++m_watchGeneration;
	NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: watching lobby %lld into %s", (long long)lobbyID, GetObserverFilePath().str());
	return true;
}

void NGMP_OnlineServices_LivestreamInterface::CancelWatchIfNotStarted()
{
	if (IsWatching() && !m_watchPlaybackStarted)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: watch of lobby %lld cancelled before playback began", (long long)m_watchLobbyID);
		StopWatching();
	}
}

void NGMP_OnlineServices_LivestreamInterface::StopWatching()
{
	if (!IsWatching())
	{
		return;
	}
	NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: stopped watching lobby %lld at %lld bytes", (long long)m_watchLobbyID, (long long)m_watchFrom);
	m_watchLobbyID = -1;
	m_watchFrom = 0;
	m_watchPlaybackStarted = false;
	m_watchEnded = false;
	m_watchRequestInFlight = false;
	++m_watchGeneration;
	if (TheRecorder != nullptr)
	{
		TheRecorder->setLiveObserverStreamOpen(FALSE);
	}
}

void NGMP_OnlineServices_LivestreamInterface::TickObserver(int64_t nowMS)
{
	if (!IsWatching())
	{
		return;
	}

	// Playback ended on the recorder's side (viewer quit, stream starved): forget the stream.
	if (m_watchPlaybackStarted && (TheRecorder == nullptr || !TheRecorder->isLiveObserverMode()))
	{
		StopWatching();
		return;
	}

	if (m_watchEnded)
	{
		// Everything the service had is in the file; the recorder plays it out and ends by
		// itself. A stream that ended before playback could even start is simply dropped.
		if (!m_watchPlaybackStarted)
		{
			StopWatching();
		}
		return;
	}

	if (m_watchRequestInFlight || nowMS - m_watchLastPollMS < WATCH_POLL_INTERVAL_MS)
	{
		return;
	}
	PollObserver();
}

void NGMP_OnlineServices_LivestreamInterface::PollObserver()
{
	std::string strURI = std::format("{}/{}?from={}", NGMP_OnlineServicesManager::GetAPIEndpoint("Livestream"), m_watchLobbyID, m_watchFrom);
	std::map<std::string, std::string> mapHeaders;
	const int64_t lobbyID = m_watchLobbyID;
	const uint32_t generation = m_watchGeneration;

	m_watchRequestInFlight = true;
	m_watchLastPollMS = NowMS();
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [this, lobbyID, generation](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			if (generation != m_watchGeneration)
			{
				return; // a reply for a watch that was stopped or restarted since
			}
			m_watchRequestInFlight = false;
			if (!IsWatching() || m_watchLobbyID != lobbyID)
			{
				return;
			}

			bool bOK = false;
			bool bEnded = false;
			int64_t available = 0;
			std::vector<uint8_t> bytes;
			if (bSuccess && statusCode == 200)
			{
				try
				{
					nlohmann::json jResp = nlohmann::json::parse(strBody);
					bOK = jResp.value("success", false);
					bEnded = jResp.value("ended", false);
					available = jResp.value("available", (int64_t)0);
					std::string data = jResp.value("data", std::string());
					if (!data.empty())
					{
						bytes = Base64Decode(data);
					}
				}
				catch (...)
				{
					bOK = false;
				}
			}

			if (!bOK)
			{
				++m_watchFailures;
				NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: poll failed (status %d, failure %d)", statusCode, m_watchFailures);
				if (m_watchFailures >= WATCH_MAX_FAILURES || statusCode == 404)
				{
					StopWatching();
				}
				return;
			}
			m_watchFailures = 0;

			if (!bytes.empty())
			{
				if (!AppendObserverBytes(bytes))
				{
					StopWatching();
					return;
				}
				if (m_watchPlaybackStarted && TheRecorder != nullptr)
				{
					TheRecorder->noteLiveObserverBytes((Int)m_watchFrom);
				}
			}

			// Keep pulling as long as the service has more ready; the interval only paces idle polls.
			if (m_watchFrom < available)
			{
				m_watchLastPollMS = 0;
			}

			if (bEnded && m_watchFrom >= available)
			{
				m_watchEnded = true;
				NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: stream for lobby %lld ended at %lld bytes", (long long)lobbyID, (long long)m_watchFrom);
				if (TheRecorder != nullptr)
				{
					TheRecorder->setLiveObserverStreamOpen(FALSE);
				}
			}

			if (!m_watchPlaybackStarted && (m_watchFrom >= WATCH_MIN_BYTES_TO_START || m_watchEnded) && m_watchFrom > 0)
			{
				if (TheShell == nullptr || !TheShell->isShellActive() || TheShell->top() == nullptr || TheGameLogic == nullptr || TheGameLogic->isLoadingMap())
				{
					return; // try again on the next poll
				}
				AsciiString fileName = WATCH_FILE_NAME;
				fileName.concat(RecorderClass::getReplayExtention());
				TheRecorder->setLiveObserverStreamOpen(!m_watchEnded);
				if (!TheRecorder->playbackFileLiveObserver(fileName))
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: playback of %s could not start", fileName.str());
					StopWatching();
					return;
				}
				m_watchPlaybackStarted = true;
				TheRecorder->noteLiveObserverBytes((Int)m_watchFrom);
			}
		});
}

bool NGMP_OnlineServices_LivestreamInterface::AppendObserverBytes(const std::vector<uint8_t>& bytes)
{
	FILE* fp = fopen(GetObserverFilePath().str(), "ab");
	if (fp == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Livestream: cannot append to %s", GetObserverFilePath().str());
		return false;
	}
	size_t written = fwrite(bytes.data(), 1, bytes.size(), fp);
	fclose(fp);
	if (written != bytes.size())
	{
		return false;
	}
	m_watchFrom += (int64_t)written;
	return true;
}
