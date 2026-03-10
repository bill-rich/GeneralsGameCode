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

// TheSuperHackers @feature hrich 10/03/2026 Game stats JSON exporter.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/StatsExporter.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/GlobalData.h"
#include "Common/Energy.h"
#include "Common/ThingTemplate.h"
#include "Common/RandomValue.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Module/BattlePlanUpdate.h"

#include <cstdio>

//-----------------------------------------------------------------------------

static void fprintJsonString(FILE *f, const char *s)
{
	fputc('"', f);
	if (s != nullptr)
	{
		for (; *s != '\0'; ++s)
		{
			switch (*s)
			{
				case '"':  fputs("\\\"", f); break;
				case '\\': fputs("\\\\", f); break;
				case '\n': fputs("\\n", f); break;
				case '\r': fputs("\\r", f); break;
				case '\t': fputs("\\t", f); break;
				default:
					if (static_cast<unsigned char>(*s) < 0x20)
						fprintf(f, "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(*s)));
					else
						fputc(*s, f);
					break;
			}
		}
	}
	fputc('"', f);
}

//-----------------------------------------------------------------------------

static void fprintJsonWideString(FILE *f, const WideChar *s)
{
	fputc('"', f);
	if (s != nullptr)
	{
		for (; *s != L'\0'; ++s)
		{
			unsigned int c = static_cast<unsigned int>(*s);
			if (c == '"')
				fputs("\\\"", f);
			else if (c == '\\')
				fputs("\\\\", f);
			else if (c < 0x20)
				fprintf(f, "\\u%04x", c);
			else if (c < 0x80)
				fputc(static_cast<char>(c), f);
			else
				fprintf(f, "\\u%04x", c);
		}
	}
	fputc('"', f);
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

static void writeObjectCountMap(FILE *f, const ScoreKeeper::ObjectCountMap &map, const char *indent)
{
	fprintf(f, "{\n");
	Bool first = TRUE;
	for (ScoreKeeper::ObjectCountMap::const_iterator it = map.begin(); it != map.end(); ++it)
	{
		if (!first) fprintf(f, ",\n");
		first = FALSE;
		const ThingTemplate *tmpl = it->first;
		fprintf(f, "%s  ", indent);
		if (tmpl != nullptr)
			fprintJsonString(f, tmpl->getName().str());
		else
			fprintJsonString(f, "unknown");
		fprintf(f, ": %d", it->second);
	}
	if (!map.empty()) fprintf(f, "\n%s", indent);
	fprintf(f, "}");
}

//-----------------------------------------------------------------------------

void ExportGameStatsJSON(const AsciiString& replayDir, const AsciiString& replayFileName)
{
	if (ThePlayerList == nullptr || TheGameLogic == nullptr || TheGlobalData == nullptr)
		return;

	// Build stats file path: replace .rep extension with .gamestats.json
	char baseName[_MAX_PATH + 1];
	strlcpy(baseName, replayFileName.str(), ARRAY_SIZE(baseName));
	char *dot = strrchr(baseName, '.');
	if (dot != nullptr) *dot = '\0';

	AsciiString statsPath;
	statsPath.format("%s%s.gamestats.json", replayDir.str(), baseName);

	FILE *f = fopen(statsPath.str(), "w");
	if (f == nullptr)
		return;

	const Int playerCount = ThePlayerList->getPlayerCount();

	fprintf(f, "{\n");
	fprintf(f, "  \"version\": 1,\n");

	// Game info
	fprintf(f, "  \"game\": {\n");
	fprintf(f, "    \"map\": "); fprintJsonString(f, TheGlobalData->m_mapName.str()); fprintf(f, ",\n");
	fprintf(f, "    \"mode\": \"%s\",\n", gameModeToString(TheGameLogic->getGameMode()));
	fprintf(f, "    \"frameCount\": %u,\n", TheGameLogic->getFrame());
	fprintf(f, "    \"seed\": %u,\n", GetGameLogicRandomSeed());
	fprintf(f, "    \"replayFile\": "); fprintJsonString(f, replayFileName.str()); fprintf(f, ",\n");
	fprintf(f, "    \"playerCount\": %d\n", playerCount);
	fprintf(f, "  },\n");

	// Players array
	fprintf(f, "  \"players\": [\n");
	Bool firstPlayer = TRUE;
	for (Int i = 0; i < playerCount; ++i)
	{
		Player *player = ThePlayerList->getNthPlayer(i);
		if (player == nullptr)
			continue;

		if (!firstPlayer) fprintf(f, ",\n");
		firstPlayer = FALSE;

		ScoreKeeper *sk = player->getScoreKeeper();
		const Energy *energy = player->getEnergy();
		const PlayerTemplate *pt = player->getPlayerTemplate();
		const AcademyStats *academy = player->getAcademyStats();

		fprintf(f, "    {\n");

		// Basic info
		fprintf(f, "      \"index\": %d,\n", player->getPlayerIndex());
		fprintf(f, "      \"displayName\": "); fprintJsonWideString(f, player->getPlayerDisplayName().str()); fprintf(f, ",\n");
		if (pt != nullptr)
		{
			fprintf(f, "      \"faction\": "); fprintJsonString(f, pt->getName().str()); fprintf(f, ",\n");
		}
		fprintf(f, "      \"side\": "); fprintJsonString(f, player->getSide().str()); fprintf(f, ",\n");
		fprintf(f, "      \"baseSide\": "); fprintJsonString(f, player->getBaseSide().str()); fprintf(f, ",\n");
		fprintf(f, "      \"type\": \"%s\",\n", player->getPlayerType() == PLAYER_HUMAN ? "Human" : "Computer");
		fprintf(f, "      \"color\": %d,\n", static_cast<int>(player->getPlayerColor()));
		fprintf(f, "      \"isDead\": %s,\n", player->isPlayerDead() ? "true" : "false");

		// Economy
		fprintf(f, "      \"money\": %u,\n", player->getMoney()->countMoney());
		fprintf(f, "      \"moneyEarned\": %d,\n", sk->getTotalMoneyEarned());
		fprintf(f, "      \"moneySpent\": %d,\n", sk->getTotalMoneySpent());

		// Energy
		fprintf(f, "      \"energyProduction\": %d,\n", energy->getProduction());
		fprintf(f, "      \"energyConsumption\": %d,\n", energy->getConsumption());

		// Rank
		fprintf(f, "      \"rankLevel\": %d,\n", player->getRankLevel());
		fprintf(f, "      \"skillPoints\": %d,\n", player->getSkillPoints());
		fprintf(f, "      \"sciencePurchasePoints\": %d,\n", player->getSciencePurchasePoints());

		// Units/Buildings summary
		fprintf(f, "      \"unitsBuilt\": %d,\n", sk->getTotalUnitsBuilt());
		fprintf(f, "      \"unitsLost\": %d,\n", sk->getTotalUnitsLost());
		fprintf(f, "      \"buildingsBuilt\": %d,\n", sk->getTotalBuildingsBuilt());
		fprintf(f, "      \"buildingsLost\": %d,\n", sk->getTotalBuildingsLost());
		fprintf(f, "      \"techBuildingsCaptured\": %d,\n", sk->getTotalTechBuildingsCaptured());
		fprintf(f, "      \"factionBuildingsCaptured\": %d,\n", sk->getTotalFactionBuildingsCaptured());

		// Radar & Battle plans
		fprintf(f, "      \"hasRadar\": %s,\n", player->hasRadar() ? "true" : "false");
		fprintf(f, "      \"battlePlans\": {\n");
		fprintf(f, "        \"bombardment\": %d,\n", player->getBattlePlansActiveSpecific(PLANSTATUS_BOMBARDMENT));
		fprintf(f, "        \"holdTheLine\": %d,\n", player->getBattlePlansActiveSpecific(PLANSTATUS_HOLDTHELINE));
		fprintf(f, "        \"searchAndDestroy\": %d\n", player->getBattlePlansActiveSpecific(PLANSTATUS_SEARCHANDDESTROY));
		fprintf(f, "      },\n");

		// Score
		fprintf(f, "      \"score\": %d,\n", sk->calculateScore());

		// Per-player destroy counts
		fprintf(f, "      \"unitsDestroyedPerPlayer\": [");
		for (Int j = 0; j < MAX_PLAYER_COUNT; ++j)
		{
			if (j > 0) fprintf(f, ", ");
			fprintf(f, "%d", sk->getUnitsDestroyedByPlayer(j));
		}
		fprintf(f, "],\n");

		fprintf(f, "      \"buildingsDestroyedPerPlayer\": [");
		for (Int j = 0; j < MAX_PLAYER_COUNT; ++j)
		{
			if (j > 0) fprintf(f, ", ");
			fprintf(f, "%d", sk->getBuildingsDestroyedByPlayer(j));
		}
		fprintf(f, "],\n");

		// Per-object-type maps
		fprintf(f, "      \"objectsBuilt\": "); writeObjectCountMap(f, sk->getObjectsBuilt(), "      "); fprintf(f, ",\n");
		fprintf(f, "      \"objectsLost\": "); writeObjectCountMap(f, sk->getObjectsLost(), "      "); fprintf(f, ",\n");
		fprintf(f, "      \"objectsCaptured\": "); writeObjectCountMap(f, sk->getObjectsCaptured(), "      "); fprintf(f, ",\n");

		// Per-player per-object-type destroyed
		fprintf(f, "      \"objectsDestroyedPerPlayer\": [\n");
		const ScoreKeeper::ObjectCountMap *destroyedArr = sk->getObjectsDestroyedArray();
		for (Int j = 0; j < MAX_PLAYER_COUNT; ++j)
		{
			if (j > 0) fprintf(f, ",\n");
			fprintf(f, "        "); writeObjectCountMap(f, destroyedArr[j], "        ");
		}
		fprintf(f, "\n      ],\n");

		// AcademyStats (Zero Hour only)
		fprintf(f, "      \"academy\": {\n");
		fprintf(f, "        \"supplyCentersBuilt\": %u,\n", academy->getSupplyCentersBuilt());
		fprintf(f, "        \"peonsBuilt\": %u,\n", academy->getPeonsBuilt());
		fprintf(f, "        \"structuresCaptured\": %u,\n", academy->getStructuresCaptured());
		fprintf(f, "        \"generalsPointsSpent\": %u,\n", academy->getGeneralsPointsSpent());
		fprintf(f, "        \"specialPowersUsed\": %u,\n", academy->getSpecialPowersUsed());
		fprintf(f, "        \"structuresGarrisoned\": %u,\n", academy->getStructuresGarrisoned());
		fprintf(f, "        \"upgradesPurchased\": %u,\n", academy->getUpgradesPurchased());
		fprintf(f, "        \"gatherersBuilt\": %u,\n", academy->getGatherersBuilt());
		fprintf(f, "        \"heroesBuilt\": %u,\n", academy->getHeroesBuilt());
		fprintf(f, "        \"controlGroupsUsed\": %u,\n", academy->getControlGroupsUsed());
		fprintf(f, "        \"secondaryIncomeUnitsBuilt\": %u,\n", academy->getSecondaryIncomeUnitsBuilt());
		fprintf(f, "        \"clearedGarrisonedBuildings\": %u,\n", academy->getClearedGarrisonedBuildings());
		fprintf(f, "        \"salvageCollected\": %u,\n", academy->getSalvageCollected());
		fprintf(f, "        \"guardAbilityUsedCount\": %u,\n", academy->getGuardAbilityUsedCount());
		fprintf(f, "        \"doubleClickAttackMoveOrdersGiven\": %u,\n", academy->getDoubleClickAttackMoveOrdersGiven());
		fprintf(f, "        \"minesCleared\": %u,\n", academy->getMinesCleared());
		fprintf(f, "        \"vehiclesDisguised\": %u,\n", academy->getVehiclesDisguised());
		fprintf(f, "        \"firestormsCreated\": %u\n", academy->getFirestormsCreated());
		fprintf(f, "      }\n");

		fprintf(f, "    }");
	}
	fprintf(f, "\n  ]\n");
	fprintf(f, "}\n");

	fclose(f);
}
