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

////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  (c) 2001-2003 Electronic Arts Inc.                                        //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

// FILE: RandomAssign.cpp //////////////////////////////////////////////////////
// Randomly assign factions and start positions for LAN game setup.
// Mirrors the native populateRandomSideAndColor / populateRandomStartPosition
// algorithms from GameLogic.cpp, using rand() instead of GameLogicRandomValue.
////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "GameNetwork/RandomAssign.h"
#include "GameNetwork/GameInfo.h"
#include "Common/PlayerTemplate.h"
#include "Common/MultiplayerSettings.h"
#include "GameClient/MapUtil.h"
#include "GameClient/ChallengeGenerals.h"

#include <cstdlib>
#include <ctime>
#include <cmath>
#include <vector>

// Build the list of valid template indices for random assignment.
// Same filtering as populateRandomSideAndColor (GameLogic.cpp).
static void buildValidTemplates(const GameInfo *game, std::vector<Int> &out)
{
	Int count = ThePlayerTemplateStore->getPlayerTemplateCount();
	for (Int c = 0; c < count; ++c)
	{
		const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(c);
		if (!fac)
			continue;

		// Must have a starting building (filters out civilian etc)
		if (fac->getStartingBuilding().isEmpty())
			continue;

		// Respect old factions only mode
		if (game->oldFactionsOnly() && !fac->isOldFaction())
			continue;

		// Skip locked generals
		const GeneralPersona *general = TheChallengeGenerals->getGeneralByTemplateName(fac->getName());
		Bool startsLocked = general ? !general->isStartingEnabled() : FALSE;
		if (startsLocked)
			continue;

		out.push_back(c);
	}
}

// Phase 1: Assign random factions and colors.
// Mirrors populateRandomSideAndColor (GameLogic.cpp:691-781).
// Only touches slots that have PLAYERTEMPLATE_RANDOM / color == -1.
static void assignRandomFactions(GameInfo *game, const std::vector<Int> &validTemplates)
{
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied())
			continue;

		// Assign faction if random
		Int playerTemplateIdx = slot->getPlayerTemplate();
		if (playerTemplateIdx == PLAYERTEMPLATE_RANDOM && !validTemplates.empty())
		{
			playerTemplateIdx = validTemplates[rand() % validTemplates.size()];
			slot->setPlayerTemplate(playerTemplateIdx);
		}

		// Assign color if random (-1)
		Int colorIdx = slot->getColor();
		if (colorIdx < 0 || colorIdx >= TheMultiplayerSettings->getNumColors())
		{
			Int numColors = TheMultiplayerSettings->getNumColors();
			if (numColors > 0)
			{
				colorIdx = rand() % numColors;
				// Re-roll while taken (bounded by numColors iterations)
				Int attempts = 0;
				while (game->isColorTaken(colorIdx) && attempts < numColors)
				{
					colorIdx = rand() % numColors;
					++attempts;
				}
				slot->setColor(colorIdx);
			}
		}
	}
}

// Phase 2: Assign start positions using distance-based placement.
// Mirrors populateRandomStartPosition (GameLogic.cpp:787-1053).
// Different teams are placed far apart, teammates close together.
// Only touches slots that have startPos == -1.
static void assignRandomPositions(GameInfo *game)
{
	Int i;
	Int numPlayers = MAX_SLOTS;
	const MapMetaData *md = TheMapCache ? TheMapCache->findMap(game->getMap()) : nullptr;
	if (md)
		numPlayers = md->m_numPlayers;

	// Build distance matrix between all start positions using map waypoints
	Real startSpotDistance[MAX_SLOTS][MAX_SLOTS];
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		for (Int j = 0; j < MAX_SLOTS; ++j)
		{
			if (i != j && i < numPlayers && j < numPlayers)
			{
				static const WaypointMap s_emptyWaypoints;
				const WaypointMap &waypoints = md ? md->m_waypoints : s_emptyWaypoints;
				AsciiString w1, w2;
				w1.format("Player_%d_Start", i + 1);
				w2.format("Player_%d_Start", j + 1);
				WaypointMap::const_iterator c1 = waypoints.find(w1);
				WaypointMap::const_iterator c2 = waypoints.find(w2);
				if (c1 == waypoints.end() || c2 == waypoints.end())
				{
					startSpotDistance[i][j] = 1000000.0f;
				}
				else
				{
					Coord3D p1 = c1->second;
					Coord3D p2 = c2->second;
					startSpotDistance[i][j] = sqrt(sqr(p1.x - p2.x) + sqr(p1.y - p2.y));
				}
			}
			else
			{
				startSpotDistance[i][j] = 0.0f;
			}
		}
	}

	// Track which positions are already taken (deliberately assigned)
	Bool taken[MAX_SLOTS];
	for (i = 0; i < MAX_SLOTS; ++i)
		taken[i] = (i < numPlayers) ? FALSE : TRUE;

	Bool hasStartSpotBeenPicked = FALSE;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;

		Int posIdx = slot->getStartPos();
		if (posIdx >= 0 && posIdx < numPlayers)
		{
			hasStartSpotBeenPicked = TRUE;
			taken[posIdx] = TRUE;
		}
	}

	// Track first position per team for teammate clustering
	Int teamPosIdx[MAX_SLOTS];
	for (i = 0; i < MAX_SLOTS; ++i)
		teamPosIdx[i] = -1;

	// Seed teamPosIdx from already-assigned slots
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;
		Int posIdx = slot->getStartPos();
		if (posIdx >= 0 && posIdx < numPlayers)
		{
			Int team = slot->getTeamNumber();
			if (team >= 0 && teamPosIdx[team] == -1)
				teamPosIdx[team] = posIdx;
		}
	}

	// Assign positions for non-observer slots that don't have one yet
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() == PLAYERTEMPLATE_OBSERVER)
			continue;

		Int posIdx = slot->getStartPos();
		if (posIdx >= 0 && posIdx < numPlayers)
			continue; // already assigned

		Int team = slot->getTeamNumber();

		if (!hasStartSpotBeenPicked)
		{
			// First player: pick randomly
			posIdx = rand() % numPlayers;
			Int attempts = 0;
			while (taken[posIdx] && attempts < numPlayers)
			{
				posIdx = rand() % numPlayers;
				++attempts;
			}
			hasStartSpotBeenPicked = TRUE;
			slot->setStartPos(posIdx);
			taken[posIdx] = TRUE;
			if (team >= 0)
				teamPosIdx[team] = posIdx;
		}
		else if (team < 0 || teamPosIdx[team] == -1)
		{
			// New team or no team: pick position farthest from all taken positions
			Real farthestDistance = 0.0f;
			Int farthestIndex = -1;
			for (posIdx = 0; posIdx < numPlayers; ++posIdx)
			{
				if (taken[posIdx])
					continue;

				Real dist = 0.0f;
				for (Int n = 0; n < numPlayers; ++n)
				{
					if (taken[n] && n != posIdx)
						dist += startSpotDistance[posIdx][n];
				}
				if (farthestIndex < 0 || dist > farthestDistance)
				{
					farthestDistance = dist;
					farthestIndex = posIdx;
				}
			}

			if (farthestIndex >= 0)
			{
				slot->setStartPos(farthestIndex);
				taken[farthestIndex] = TRUE;
				if (team >= 0)
					teamPosIdx[team] = farthestIndex;
			}
		}
		else
		{
			// Teammate: pick position closest to team's existing position
			Real closestDist = FLT_MAX;
			Int closestIdx = 0;
			for (Int n = 0; n < numPlayers; ++n)
			{
				if (!taken[n] && startSpotDistance[teamPosIdx[team]][n] < closestDist)
				{
					closestDist = startSpotDistance[teamPosIdx[team]][n];
					closestIdx = n;
				}
			}
			slot->setStartPos(closestIdx);
			taken[closestIdx] = TRUE;
		}
	}

	// Assign observer slots to an existing player's position
	Int numPlayersInGame = 0;
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			++numPlayersInGame;
	}
	for (i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (!slot || !slot->isOccupied() || slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			continue;

		Int posIdx = -1;
		if (numPlayersInGame == 0)
		{
			posIdx = 0;
		}
		else
		{
			// Pick a random position that IS taken by a real player
			Int attempts = 0;
			while (posIdx == -1 && attempts < numPlayers * 10)
			{
				Int candidate = rand() % numPlayers;
				if (game->isStartPositionTaken(candidate))
					posIdx = candidate;
				++attempts;
			}
			if (posIdx == -1)
				posIdx = 0; // fallback
		}
		slot->setStartPos(posIdx);
	}
}

void performRandomAssign(GameInfo *game)
{
	if (!game)
		return;

	srand((unsigned int)time(nullptr));

	// Phase 1: Assign factions and colors for random slots
	std::vector<Int> validTemplates;
	buildValidTemplates(game, validTemplates);
	assignRandomFactions(game, validTemplates);

	// Phase 2: Assign start positions using distance-based placement
	assignRandomPositions(game);

	// Reset accepted state since we changed settings
	game->resetAccepted();
}
