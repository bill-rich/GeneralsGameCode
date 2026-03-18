/*
**	Command & Conquer Generals(tm)
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

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/StatsExporter.h"
#include "Common/StatsUploader.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/GlobalData.h"
#include "Common/Energy.h"
#include "Common/ThingTemplate.h"
#include "Common/RandomValue.h"
#include "GameLogic/Damage.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/BattlePlanUpdate.h"

#include <stdio.h>
#include <stdarg.h>
#include <zlib.h>

//-----------------------------------------------------------------------------
// In-memory JSON buffer that accumulates the entire document before compression.
//-----------------------------------------------------------------------------

struct JsonBuffer
{
	char *data;
	size_t len;
	size_t cap;
};

static void bufInit(JsonBuffer *b)
{
	b->cap = 8192;
	b->data = static_cast<char*>(malloc(b->cap));
	b->len = 0;
	if (b->data != nullptr) b->data[0] = '\0';
}

static void bufGrow(JsonBuffer *b, size_t needed)
{
	if (b->data == nullptr) return;
	while (b->cap - b->len < needed + 1)
	{
		b->cap *= 2;
		b->data = static_cast<char*>(realloc(b->data, b->cap));
		if (b->data == nullptr) return;
	}
}

static void bufPuts(JsonBuffer *b, const char *s)
{
	size_t n = strlen(s);
	bufGrow(b, n);
	if (b->data == nullptr) return;
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void bufPutc(JsonBuffer *b, char c)
{
	bufGrow(b, 1);
	if (b->data == nullptr) return;
	b->data[b->len++] = c;
	b->data[b->len] = '\0';
}

static void bufPrintf(JsonBuffer *b, const char *fmt, ...)
{
	char tmp[1024];
	va_list args;
	va_start(args, fmt);
	int n = _vsnprintf(tmp, sizeof(tmp) - 1, fmt, args);
	va_end(args);
	if (n > 0)
	{
		tmp[n] = '\0';
		bufPuts(b, tmp);
	}
}

static void bufFree(JsonBuffer *b)
{
	if (b->data != nullptr) free(b->data);
	b->data = nullptr;
	b->len = 0;
	b->cap = 0;
}

//-----------------------------------------------------------------------------

static void bufJsonString(JsonBuffer *b, const char *s)
{
	bufPutc(b, '"');
	if (s != nullptr)
	{
		for (; *s != '\0'; ++s)
		{
			switch (*s)
			{
				case '"':  bufPuts(b, "\\\""); break;
				case '\\': bufPuts(b, "\\\\"); break;
				case '\n': bufPuts(b, "\\n"); break;
				case '\r': bufPuts(b, "\\r"); break;
				case '\t': bufPuts(b, "\\t"); break;
				default:
					if (static_cast<unsigned char>(*s) < 0x20)
						bufPrintf(b, "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(*s)));
					else
						bufPutc(b, *s);
					break;
			}
		}
	}
	bufPutc(b, '"');
}

//-----------------------------------------------------------------------------

static void bufJsonWideString(JsonBuffer *b, const WideChar *s)
{
	bufPutc(b, '"');
	if (s != nullptr)
	{
		for (; *s != L'\0'; ++s)
		{
			unsigned int c = static_cast<unsigned int>(*s);
			if (c == '"')
				bufPuts(b, "\\\"");
			else if (c == '\\')
				bufPuts(b, "\\\\");
			else if (c < 0x20)
				bufPrintf(b, "\\u%04x", c);
			else if (c < 0x80)
				bufPutc(b, static_cast<char>(c));
			else
				bufPrintf(b, "\\u%04x", c);
		}
	}
	bufPutc(b, '"');
}

//-----------------------------------------------------------------------------

static const char* gameModeToString(GameMode mode)
{
	switch (mode)
	{
		case GAME_SINGLE_PLAYER: return "SinglePlayer";
		case GAME_LAN:           return "LAN";
		case GAME_SKIRMISH:      return "Skirmish";
		case GAME_REPLAY:        return "Replay";
		case GAME_SHELL:         return "Shell";
		case GAME_INTERNET:      return "Internet";
		case GAME_NONE:          return "None";
		default:                 return "Unknown";
	}
}

//-----------------------------------------------------------------------------

static Bool isGamePlayer(Player *player)
{
	if (player == nullptr) return FALSE;
	const PlayerTemplate *pt = player->getPlayerTemplate();
	if (pt == nullptr) return FALSE;
	const char *name = pt->getName().str();
	if (name == nullptr || name[0] == '\0') return FALSE;
	if (strcmp(name, "FactionObserver") == 0) return FALSE;
	if (strcmp(name, "FactionCivilian") == 0) return FALSE;
	return TRUE;
}

//-----------------------------------------------------------------------------

struct PlayerSnapshotData
{
	Int playerIndex;
	UnsignedInt money;
	Int moneyEarned;
	Int moneySpent;
};

struct PlayerStateData
{
	Int energyProduction;
	Int energyConsumption;
	Int rankLevel;
	Int skillPoints;
	Int sciencePurchasePoints;
	Bool hasRadar;
	Bool isDead;
	Int bombardment;
	Int holdTheLine;
	Int searchAndDestroy;
};

struct StateChangeEvent
{
	UnsignedInt frame;
	Int playerIndex;
};

struct EnergyEvent : StateChangeEvent { Int production; Int consumption; };
struct RankEvent : StateChangeEvent { Int rankLevel; };
struct SkillPointsEvent : StateChangeEvent { Int skillPoints; };
struct SciencePointsEvent : StateChangeEvent { Int sciencePurchasePoints; };
struct RadarEvent : StateChangeEvent { Bool hasRadar; };
struct DeathEvent : StateChangeEvent {};
struct BattlePlanEvent : StateChangeEvent { Int bombardment; Int holdTheLine; Int searchAndDestroy; };

struct FrameSnapshotData
{
	UnsignedInt frame;
	Int playerCount;
	PlayerSnapshotData players[MAX_PLAYER_COUNT];
};

struct KillEventData
{
	UnsignedInt frame;
	Int killerPlayerIndex;
	Int victimPlayerIndex;
	Real x;
	Real y;
	char killerTemplateName[64];
	char victimTemplateName[64];
	char damageType[32];
};

struct BuildEventData
{
	UnsignedInt frame;
	Int playerIndex;
	Real x;
	Real y;
	Int cost;
	Int buildTime;
	char templateName[64];
	char producerTemplateName[64];
};

struct CaptureEventData
{
	UnsignedInt frame;
	Int newOwnerPlayerIndex;
	Int oldOwnerPlayerIndex;
	Real x;
	Real y;
	char templateName[64];
};

struct StatsExporterState
{
	Bool exportingActive;
	Bool mappingInitialized;
	Int gamePlayerCount;
	Int originalToNewIndex[MAX_PLAYER_COUNT];
	UnsignedInt lastSnapshotFrame;
	PlayerStateData lastPlayerState[MAX_PLAYER_COUNT];

	std::vector<FrameSnapshotData> snapshots;
	std::vector<KillEventData> killEvents;
	std::vector<BuildEventData> buildEvents;
	std::vector<CaptureEventData> captureEvents;
	std::vector<EnergyEvent> energyEvents;
	std::vector<RankEvent> rankEvents;
	std::vector<SkillPointsEvent> skillPointsEvents;
	std::vector<SciencePointsEvent> sciencePointsEvents;
	std::vector<RadarEvent> radarEvents;
	std::vector<DeathEvent> deathEvents;
	std::vector<BattlePlanEvent> battlePlanEvents;

	void clear()
	{
		exportingActive = TRUE;
		mappingInitialized = FALSE;
		gamePlayerCount = 0;
		lastSnapshotFrame = 0;
		memset(originalToNewIndex, 0, sizeof(originalToNewIndex));
		memset(lastPlayerState, 0, sizeof(lastPlayerState));
		snapshots.clear();
		killEvents.clear();
		buildEvents.clear();
		captureEvents.clear();
		energyEvents.clear();
		rankEvents.clear();
		skillPointsEvents.clear();
		sciencePointsEvents.clear();
		radarEvents.clear();
		deathEvents.clear();
		battlePlanEvents.clear();
	}
};

static StatsExporterState s_state;

//-----------------------------------------------------------------------------

static void initPlayerMapping()
{
	if (s_state.mappingInitialized)
		return;

	s_state.gamePlayerCount = 0;
	memset(s_state.originalToNewIndex, 0, sizeof(s_state.originalToNewIndex));

	const Int totalPlayers = ThePlayerList->getPlayerCount();
	Int i;
	for (i = 0; i < totalPlayers && i < MAX_PLAYER_COUNT; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (isGamePlayer(player))
		{
			++s_state.gamePlayerCount;
			s_state.originalToNewIndex[i] = s_state.gamePlayerCount;
		}
	}

	// Only lock in the mapping once we find actual game players.
	// Early calls (before players are fully initialized) will retry.
	if (s_state.gamePlayerCount > 0)
		s_state.mappingInitialized = TRUE;
}

//-----------------------------------------------------------------------------

void StatsExporterCollectSnapshot()
{
	if (ThePlayerList == nullptr || TheGameLogic == nullptr)
		return;

	UnsignedInt currentFrame = TheGameLogic->getFrame();
	if (!s_state.snapshots.empty() && (currentFrame - s_state.lastSnapshotFrame) < 30)
		return;

	s_state.lastSnapshotFrame = currentFrame;

	initPlayerMapping();

	const Int totalPlayers = ThePlayerList->getPlayerCount();

	FrameSnapshotData snap;
	memset(&snap, 0, sizeof(snap));
	snap.frame = currentFrame;
	snap.playerCount = s_state.gamePlayerCount;

	Int gameIdx = 0;
	Int i;
	for (i = 0; i < totalPlayers && i < MAX_PLAYER_COUNT; ++i)
	{
		if (s_state.originalToNewIndex[i] == 0)
			continue;

		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr)
			continue;

		PlayerSnapshotData &pd = snap.players[gameIdx];
		ScoreKeeper *sk = player->getScoreKeeper();
		const Energy *energy = player->getEnergy();

		pd.playerIndex = s_state.originalToNewIndex[i];
		pd.money = player->getMoney()->countMoney();
		pd.moneyEarned = sk->getTotalMoneyEarned();
		pd.moneySpent = sk->getTotalMoneySpent();

		// Detect state changes and emit events
		{
			PlayerStateData &last = s_state.lastPlayerState[i];
			Int curVal, curVal2, curVal3;
			Bool curBool;

			curVal = energy->getProduction();
			curVal2 = energy->getConsumption();
			if (curVal != last.energyProduction || curVal2 != last.energyConsumption)
			{
				EnergyEvent eev;
				memset(&eev, 0, sizeof(eev));
				eev.frame = currentFrame;
				eev.playerIndex = i;
				eev.production = curVal;
				eev.consumption = curVal2;
				s_state.energyEvents.push_back(eev);
				last.energyProduction = curVal;
				last.energyConsumption = curVal2;
			}

			curVal = player->getRankLevel();
			if (curVal != last.rankLevel)
			{
				RankEvent rev;
				memset(&rev, 0, sizeof(rev));
				rev.frame = currentFrame;
				rev.playerIndex = i;
				rev.rankLevel = curVal;
				s_state.rankEvents.push_back(rev);
				last.rankLevel = curVal;
			}

			curVal = player->getSkillPoints();
			if (curVal != last.skillPoints)
			{
				SkillPointsEvent sev;
				memset(&sev, 0, sizeof(sev));
				sev.frame = currentFrame;
				sev.playerIndex = i;
				sev.skillPoints = curVal;
				s_state.skillPointsEvents.push_back(sev);
				last.skillPoints = curVal;
			}

			curVal = player->getSciencePurchasePoints();
			if (curVal != last.sciencePurchasePoints)
			{
				SciencePointsEvent spev;
				memset(&spev, 0, sizeof(spev));
				spev.frame = currentFrame;
				spev.playerIndex = i;
				spev.sciencePurchasePoints = curVal;
				s_state.sciencePointsEvents.push_back(spev);
				last.sciencePurchasePoints = curVal;
			}

			curBool = player->hasRadar();
			if (curBool != last.hasRadar)
			{
				RadarEvent raev;
				memset(&raev, 0, sizeof(raev));
				raev.frame = currentFrame;
				raev.playerIndex = i;
				raev.hasRadar = curBool;
				s_state.radarEvents.push_back(raev);
				last.hasRadar = curBool;
			}

			curBool = player->isPlayerDead();
			if (curBool && !last.isDead)
			{
				DeathEvent dev;
				memset(&dev, 0, sizeof(dev));
				dev.frame = currentFrame;
				dev.playerIndex = i;
				s_state.deathEvents.push_back(dev);
				last.isDead = curBool;
			}

			curVal = player->getBattlePlansActiveSpecific(PLANSTATUS_BOMBARDMENT);
			curVal2 = player->getBattlePlansActiveSpecific(PLANSTATUS_HOLDTHELINE);
			curVal3 = player->getBattlePlansActiveSpecific(PLANSTATUS_SEARCHANDDESTROY);
			if (curVal != last.bombardment || curVal2 != last.holdTheLine || curVal3 != last.searchAndDestroy)
			{
				BattlePlanEvent bev;
				memset(&bev, 0, sizeof(bev));
				bev.frame = currentFrame;
				bev.playerIndex = i;
				bev.bombardment = curVal;
				bev.holdTheLine = curVal2;
				bev.searchAndDestroy = curVal3;
				s_state.battlePlanEvents.push_back(bev);
				last.bombardment = curVal;
				last.holdTheLine = curVal2;
				last.searchAndDestroy = curVal3;
			}
		}

		++gameIdx;
	}

	s_state.snapshots.push_back(snap);
}

//-----------------------------------------------------------------------------

void StatsExporterClearSnapshots()
{
	s_state.clear();
}

//-----------------------------------------------------------------------------

void StatsExporterRecordKill(const Object *killer, const Object *victim, const DamageInfo *damageInfo)
{
	if (!s_state.exportingActive)
		return;
	if (killer == nullptr || victim == nullptr || TheGameLogic == nullptr)
		return;

	const Player *killerPlayer = killer->getControllingPlayer();
	const Player *victimPlayer = victim->getControllingPlayer();
	if (killerPlayer == nullptr || victimPlayer == nullptr)
		return;

	KillEventData ev;
	memset(&ev, 0, sizeof(ev));
	ev.frame = TheGameLogic->getFrame();

	// Store raw player indices; remapped to game-player indices at export time.
	ev.killerPlayerIndex = killerPlayer->getPlayerIndex();
	ev.victimPlayerIndex = victimPlayer->getPlayerIndex();

	const Coord3D *pos = victim->getPosition();
	if (pos != nullptr)
	{
		ev.x = pos->x;
		ev.y = pos->y;
	}

	strlcpy(ev.killerTemplateName, killer->getTemplate()->getName().str(), ARRAY_SIZE(ev.killerTemplateName));
	strlcpy(ev.victimTemplateName, victim->getTemplate()->getName().str(), ARRAY_SIZE(ev.victimTemplateName));

	if (damageInfo != nullptr && damageInfo->in.m_damageType >= 0 && damageInfo->in.m_damageType < DAMAGE_NUM_TYPES)
	{
		const char *name = DamageTypeFlags::s_bitNameList[damageInfo->in.m_damageType];
		if (name != nullptr)
			strlcpy(ev.damageType, name, ARRAY_SIZE(ev.damageType));
	}

	s_state.killEvents.push_back(ev);
}

//-----------------------------------------------------------------------------

void StatsExporterRecordBuild(const Object *producer, const Object *built)
{
	if (!s_state.exportingActive)
		return;
	if (built == nullptr || TheGameLogic == nullptr)
		return;

	const Player *player = built->getControllingPlayer();
	if (player == nullptr)
		return;

	BuildEventData ev;
	memset(&ev, 0, sizeof(ev));
	ev.frame = TheGameLogic->getFrame();

	// Store raw player index; remapped at export time.
	ev.playerIndex = player->getPlayerIndex();

	const Coord3D *pos = built->getPosition();
	if (pos != nullptr)
	{
		ev.x = pos->x;
		ev.y = pos->y;
	}
	ev.cost = built->getTemplate()->calcCostToBuild(player);
	ev.buildTime = built->getTemplate()->calcTimeToBuild(player);

	strlcpy(ev.templateName, built->getTemplate()->getName().str(), ARRAY_SIZE(ev.templateName));

	if (producer != nullptr)
		strlcpy(ev.producerTemplateName, producer->getTemplate()->getName().str(), ARRAY_SIZE(ev.producerTemplateName));

	s_state.buildEvents.push_back(ev);
}

//-----------------------------------------------------------------------------

void StatsExporterRecordCapture(const Object *captured, const Player *oldOwner, const Player *newOwner)
{
	if (!s_state.exportingActive)
		return;
	if (captured == nullptr || oldOwner == nullptr || newOwner == nullptr || TheGameLogic == nullptr)
		return;

	CaptureEventData ev;
	memset(&ev, 0, sizeof(ev));
	ev.frame = TheGameLogic->getFrame();

	// Store raw player indices; remapped at export time.
	ev.newOwnerPlayerIndex = newOwner->getPlayerIndex();
	ev.oldOwnerPlayerIndex = oldOwner->getPlayerIndex();

	const Coord3D *pos = captured->getPosition();
	if (pos != nullptr)
	{
		ev.x = pos->x;
		ev.y = pos->y;
	}

	strlcpy(ev.templateName, captured->getTemplate()->getName().str(), ARRAY_SIZE(ev.templateName));

	s_state.captureEvents.push_back(ev);
}

//-----------------------------------------------------------------------------

static void writeCaptureEvents(JsonBuffer *b)
{
	bufPuts(b, "  \"captureEvents\": [\n");
	for (size_t i = 0; i < s_state.captureEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const CaptureEventData &ev = s_state.captureEvents[i];

		Int newIdx = (ev.newOwnerPlayerIndex >= 0 && ev.newOwnerPlayerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.newOwnerPlayerIndex] : 0;
		Int oldIdx = (ev.oldOwnerPlayerIndex >= 0 && ev.oldOwnerPlayerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.oldOwnerPlayerIndex] : 0;

		bufPrintf(b, "    {\"frame\": %u, \"newOwner\": %d, \"oldOwner\": %d, \"x\": %.1f, \"y\": %.1f, \"object\": ",
			ev.frame, newIdx, oldIdx, ev.x, ev.y);
		bufJsonString(b, ev.templateName);
		bufPutc(b, '}');
	}
	if (!s_state.captureEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ]");
}

//-----------------------------------------------------------------------------

static void writeBuildEvents(JsonBuffer *b)
{
	bufPuts(b, "  \"buildEvents\": [\n");
	for (size_t i = 0; i < s_state.buildEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const BuildEventData &ev = s_state.buildEvents[i];

		Int playerIdx = (ev.playerIndex >= 0 && ev.playerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.playerIndex] : 0;

		bufPrintf(b, "    {\"frame\": %u, \"player\": %d, \"x\": %.1f, \"y\": %.1f, \"cost\": %d, \"buildTime\": %d, \"object\": ",
			ev.frame, playerIdx, ev.x, ev.y, ev.cost, ev.buildTime);
		bufJsonString(b, ev.templateName);
		bufPuts(b, ", \"producer\": ");
		bufJsonString(b, ev.producerTemplateName);
		bufPutc(b, '}');
	}
	if (!s_state.buildEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ]");
}

//-----------------------------------------------------------------------------

static void writeKillEvents(JsonBuffer *b)
{
	bufPuts(b, "  \"killEvents\": [\n");
	for (size_t i = 0; i < s_state.killEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const KillEventData &ev = s_state.killEvents[i];

		Int killerIdx = (ev.killerPlayerIndex >= 0 && ev.killerPlayerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.killerPlayerIndex] : 0;
		Int victimIdx = (ev.victimPlayerIndex >= 0 && ev.victimPlayerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.victimPlayerIndex] : 0;

		bufPrintf(b, "    {\"frame\": %u, \"killerPlayer\": %d, \"victimPlayer\": %d, \"x\": %.1f, \"y\": %.1f, \"killer\": ",
			ev.frame, killerIdx, victimIdx, ev.x, ev.y);
		bufJsonString(b, ev.killerTemplateName);
		bufPuts(b, ", \"victim\": ");
		bufJsonString(b, ev.victimTemplateName);
		bufPuts(b, ", \"damageType\": ");
		bufJsonString(b, ev.damageType);
		bufPutc(b, '}');
	}
	if (!s_state.killEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ]");
}

//-----------------------------------------------------------------------------

static void writeStateChangeEvents(JsonBuffer *b)
{
	size_t i;

	bufPuts(b, "  \"energyEvents\": [\n");
	for (i = 0; i < s_state.energyEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const EnergyEvent &ev = s_state.energyEvents[i];
		Int idx = (ev.playerIndex >= 0 && ev.playerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.playerIndex] : 0;
		bufPrintf(b, "    {\"frame\": %u, \"player\": %d, \"production\": %d, \"consumption\": %d}", ev.frame, idx, ev.production, ev.consumption);
	}
	if (!s_state.energyEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ],\n");

	bufPuts(b, "  \"rankEvents\": [\n");
	for (i = 0; i < s_state.rankEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const RankEvent &ev = s_state.rankEvents[i];
		Int idx = (ev.playerIndex >= 0 && ev.playerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.playerIndex] : 0;
		bufPrintf(b, "    {\"frame\": %u, \"player\": %d, \"rankLevel\": %d}", ev.frame, idx, ev.rankLevel);
	}
	if (!s_state.rankEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ],\n");

	bufPuts(b, "  \"skillPointsEvents\": [\n");
	for (i = 0; i < s_state.skillPointsEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const SkillPointsEvent &ev = s_state.skillPointsEvents[i];
		Int idx = (ev.playerIndex >= 0 && ev.playerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.playerIndex] : 0;
		bufPrintf(b, "    {\"frame\": %u, \"player\": %d, \"skillPoints\": %d}", ev.frame, idx, ev.skillPoints);
	}
	if (!s_state.skillPointsEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ],\n");

	bufPuts(b, "  \"sciencePointsEvents\": [\n");
	for (i = 0; i < s_state.sciencePointsEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const SciencePointsEvent &ev = s_state.sciencePointsEvents[i];
		Int idx = (ev.playerIndex >= 0 && ev.playerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.playerIndex] : 0;
		bufPrintf(b, "    {\"frame\": %u, \"player\": %d, \"sciencePurchasePoints\": %d}", ev.frame, idx, ev.sciencePurchasePoints);
	}
	if (!s_state.sciencePointsEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ],\n");

	bufPuts(b, "  \"radarEvents\": [\n");
	for (i = 0; i < s_state.radarEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const RadarEvent &ev = s_state.radarEvents[i];
		Int idx = (ev.playerIndex >= 0 && ev.playerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.playerIndex] : 0;
		bufPrintf(b, "    {\"frame\": %u, \"player\": %d, \"hasRadar\": %s}", ev.frame, idx, ev.hasRadar ? "true" : "false");
	}
	if (!s_state.radarEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ],\n");

	bufPuts(b, "  \"deathEvents\": [\n");
	for (i = 0; i < s_state.deathEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const DeathEvent &ev = s_state.deathEvents[i];
		Int idx = (ev.playerIndex >= 0 && ev.playerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.playerIndex] : 0;
		bufPrintf(b, "    {\"frame\": %u, \"player\": %d}", ev.frame, idx);
	}
	if (!s_state.deathEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ],\n");

	bufPuts(b, "  \"battlePlanEvents\": [\n");
	for (i = 0; i < s_state.battlePlanEvents.size(); ++i)
	{
		if (i > 0) bufPuts(b, ",\n");
		const BattlePlanEvent &ev = s_state.battlePlanEvents[i];
		Int idx = (ev.playerIndex >= 0 && ev.playerIndex < MAX_PLAYER_COUNT) ? s_state.originalToNewIndex[ev.playerIndex] : 0;
		bufPrintf(b, "    {\"frame\": %u, \"player\": %d, \"bombardment\": %d, \"holdTheLine\": %d, \"searchAndDestroy\": %d}",
			ev.frame, idx, ev.bombardment, ev.holdTheLine, ev.searchAndDestroy);
	}
	if (!s_state.battlePlanEvents.empty()) bufPutc(b, '\n');
	bufPuts(b, "  ],\n");
}

//-----------------------------------------------------------------------------

static void writeTimeSeries(JsonBuffer *b)
{
	size_t s;
	Int pi;

	bufPuts(b, "  \"timeSeries\": {\n");

	bufPuts(b, "    \"players\": [\n");

	for (pi = 0; pi < s_state.gamePlayerCount; ++pi)
	{
		if (pi > 0) bufPuts(b, ",\n");
		bufPuts(b, "      {\n");

		bufPrintf(b, "        \"index\": %d,\n", pi + 1);

		bufPuts(b, "        \"money\": [");
		for (s = 0; s < s_state.snapshots.size(); ++s)
		{
			if (s > 0) bufPutc(b, ',');
			bufPrintf(b, "%u", s_state.snapshots[s].players[pi].money);
		}
		bufPuts(b, "],\n");

		bufPuts(b, "        \"moneyEarned\": [");
		for (s = 0; s < s_state.snapshots.size(); ++s)
		{
			if (s > 0) bufPutc(b, ',');
			bufPrintf(b, "%d", s_state.snapshots[s].players[pi].moneyEarned);
		}
		bufPuts(b, "],\n");

		bufPuts(b, "        \"moneySpent\": [");
		for (s = 0; s < s_state.snapshots.size(); ++s)
		{
			if (s > 0) bufPutc(b, ',');
			bufPrintf(b, "%d", s_state.snapshots[s].players[pi].moneySpent);
		}
		bufPuts(b, "]\n");

		bufPuts(b, "      }");
	}

	bufPuts(b, "\n    ]\n");
	bufPuts(b, "  }\n");
}

//-----------------------------------------------------------------------------

void ExportGameStatsJSON(const AsciiString& replayDir, const AsciiString& replayFileName)
{
	if (ThePlayerList == nullptr || TheGameLogic == nullptr || TheGlobalData == nullptr)
		return;

	// Strip any directory components from the replay filename
	const char *replayBase = replayFileName.str();
	const char *lastSlash = strrchr(replayBase, '/');
	const char *lastBackslash = strrchr(replayBase, '\\');
	if (lastBackslash != nullptr && (lastSlash == nullptr || lastBackslash > lastSlash))
		lastSlash = lastBackslash;
	if (lastSlash != nullptr)
		replayBase = lastSlash + 1;

	// Build stats file path: replace .rep extension with .gamestats.json.gz
	char baseName[_MAX_PATH + 1];
	strlcpy(baseName, replayBase, ARRAY_SIZE(baseName));
	char *dot = strrchr(baseName, '.');
	if (dot != nullptr) *dot = '\0';

	AsciiString statsPath;
	statsPath.format("%s%s.gamestats.json.gz", replayDir.str(), baseName);

	initPlayerMapping();

	const Int playerCount = ThePlayerList->getPlayerCount();

	// Build JSON into memory buffer
	JsonBuffer buf;
	bufInit(&buf);
	if (buf.data == nullptr)
		return;

	bufPuts(&buf, "{\n");
	bufPuts(&buf, "  \"version\": 1,\n");

	// Game info
	bufPuts(&buf, "  \"game\": {\n");
	bufPuts(&buf, "    \"map\": "); bufJsonString(&buf, TheGlobalData->m_mapName.str()); bufPuts(&buf, ",\n");
	bufPrintf(&buf, "    \"mode\": \"%s\",\n", gameModeToString(TheGameLogic->getGameMode()));
	bufPrintf(&buf, "    \"frameCount\": %u,\n", TheGameLogic->getFrame());
	bufPrintf(&buf, "    \"seed\": %u,\n", GetGameLogicRandomSeed());
	bufPuts(&buf, "    \"replayFile\": "); bufJsonString(&buf, replayFileName.str()); bufPuts(&buf, ",\n");
	bufPrintf(&buf, "    \"playerCount\": %d,\n", s_state.gamePlayerCount);
	bufPuts(&buf, "    \"snapshotInterval\": 30\n");
	bufPuts(&buf, "  },\n");

	// Players array
	bufPuts(&buf, "  \"players\": [\n");
	Bool firstPlayer = TRUE;
	Int i;
	for (i = 0; i < playerCount; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr || !isGamePlayer(player))
			continue;

		if (!firstPlayer) bufPuts(&buf, ",\n");
		firstPlayer = FALSE;

		ScoreKeeper *sk = player->getScoreKeeper();
		const PlayerTemplate *pt = player->getPlayerTemplate();

		bufPuts(&buf, "    {\n");

		// Basic info
		bufPrintf(&buf, "      \"index\": %d,\n", s_state.originalToNewIndex[i]);
		bufPuts(&buf, "      \"displayName\": "); bufJsonWideString(&buf, player->getPlayerDisplayName().str()); bufPuts(&buf, ",\n");
		if (pt != nullptr)
		{
			bufPuts(&buf, "      \"faction\": "); bufJsonString(&buf, pt->getName().str()); bufPuts(&buf, ",\n");
		}
		bufPuts(&buf, "      \"side\": "); bufJsonString(&buf, player->getSide().str()); bufPuts(&buf, ",\n");
		bufPrintf(&buf, "      \"type\": \"%s\",\n", player->getPlayerType() == PLAYER_HUMAN ? "Human" : "Computer");
		bufPrintf(&buf, "      \"color\": \"#%06X\",\n", static_cast<unsigned int>(player->getPlayerColor()) & 0x00FFFFFFu);

		// Economy
		bufPrintf(&buf, "      \"money\": %u,\n", player->getMoney()->countMoney());
		bufPrintf(&buf, "      \"moneyEarned\": %d,\n", sk->getTotalMoneyEarned());
		bufPrintf(&buf, "      \"moneySpent\": %d,\n", sk->getTotalMoneySpent());

		// Score
		bufPrintf(&buf, "      \"score\": %d\n", sk->calculateScore());

		bufPuts(&buf, "    }");
	}
	bufPuts(&buf, "\n  ],\n");

	writeBuildEvents(&buf);
	bufPuts(&buf, ",\n");

	writeKillEvents(&buf);
	bufPuts(&buf, ",\n");

	writeCaptureEvents(&buf);
	bufPuts(&buf, ",\n");

	writeStateChangeEvents(&buf);

	writeTimeSeries(&buf);

	bufPuts(&buf, "}\n");

	// Write gzip-compressed output to file
	printf("[stats] Writing %u bytes JSON to %s\n", static_cast<unsigned int>(buf.len), statsPath.str());
	fflush(stdout);
	gzFile gz = gzopen(statsPath.str(), "wb9");
	if (gz != nullptr)
	{
		gzwrite(gz, buf.data, static_cast<unsigned int>(buf.len));
		gzclose(gz);
	}
	else
	{
		printf("[stats] ERROR: Failed to open %s for writing\n", statsPath.str());
		fflush(stdout);
	}

	// Upload gzip file to server if URL configured
	if (!TheGlobalData->m_statsUrl.isEmpty())
	{
		FILE *f = fopen(statsPath.str(), "rb");
		if (f != nullptr)
		{
			fseek(f, 0, SEEK_END);
			long fileSize = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (fileSize > 0)
			{
				void *fileData = malloc(static_cast<size_t>(fileSize));
				if (fileData != nullptr)
				{
					if (fread(fileData, 1, static_cast<size_t>(fileSize), f) == static_cast<size_t>(fileSize))
					{
						printf("[stats] Uploading %ld bytes to %s\n", fileSize, TheGlobalData->m_statsUrl.str());
						fflush(stdout);
						UploadStatsToServer(TheGlobalData->m_statsUrl, fileData, static_cast<unsigned int>(fileSize), GetGameLogicRandomSeed());
					}
					free(fileData);
				}
			}
			fclose(f);
		}
		else
		{
			printf("[stats] ERROR: Failed to read %s for upload\n", statsPath.str());
			fflush(stdout);
		}
	}
	bufFree(&buf);

	StatsExporterClearSnapshots();
	s_state.exportingActive = FALSE;
}
