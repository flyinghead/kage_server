/*
	Propeller Arena game server.
    Copyright 2026 Flyinghead <flyinghead.github@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "model.h"

enum PropMessages : uint8_t
{
	IN_SET_PLAYER_ATTRS = 0,
	IN_GET_ROOM_ATTRS = 1,
	IN_GAME_STARTING = 2,
	IN_SET_ROOM_ATTRS = 3,
	IN_GAME_OVER = 4,
	IN_GAME_START = 6,
	IN_GAME_STOP = 7,
	IN_GAME_CDATA = 0xa,
	IN_GAME_HDATA2 = 0xb,
	IN_GAME_HDATA = 0xc,
	IN_GAME_ENDED = 0xe,

	OUT_SET_PLAYER_ATTRS = 0xf,
	OUT_PLAYER_LIST = 0x10,
	OUT_SET_ROOM_ATTRS = 0x12,
	OUT_SET_RNG_SEED = 0x13,
	OUT_ACK_PLAYER_ATTRS = 0x18,
	OUT_GAME_DATA = 0x1c,
	OUT_GAME_DATA2 = 0x1d,
	OUT_UPDATE_SCORE = 0x1e,
};

class PARoom : public Room
{
public:
	struct PlayerState
	{
		uint8_t plane = 0;
		uint8_t flags = 0;
		uint8_t rank = 0;
		uint8_t score = 0;
		std::array<uint8_t, 0x3c> data {};
		bool rankUpdated = false;
		bool inGame = false;
		uint16_t seqnum = 0;

		float flightDist = 0.f;
		uint32_t kills = 0;
		uint32_t deaths = 0;
		uint32_t wins = 0;
	};

	PARoom(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context)
		: Room(lobby, id, name, attributes, owner, io_context), timer(io_context)
	{
		rngSeed = (uint32_t)time(nullptr);
	}

	bool removePlayer(Player *player) override;

	void updateSettings(int stage, int maxPoints, uint8_t flags, uint8_t field2, uint8_t field3);
	void setPlane(Player *player, uint8_t plane);
	void setPlayerFlags(Player *player, uint8_t flags);
	void setRank(Player *player, uint8_t rank);
	void setStartState(uint8_t state) { startState = state; }
	void setStateData(int slot, const uint8_t *data);

	const PlayerState& getPlayerState(int i) const { return playerState[i]; }
	void setInGame(Player *player, bool inGame);
	void gameStop(Player *player);
	void sendRankUpdate(Player *player, uint32_t flightTime, Packet& packet);
	void sendPlayerList(Packet& packet);
	void sendRoomAttrs(Packet& packet);
	void sendRngSeed(Packet& packet);
	void resetState();

private:
	void sendGameData(const std::error_code& ec);

	int stage = 0;
	int maxPoints = 0;
	uint8_t flags = 0x72;
	int slots = 0;
	uint8_t field2 = 0;
	uint8_t field3 = 0;
	uint8_t startState = 0;
	uint32_t rngSeed;
	std::array<PlayerState, 6> playerState;
	asio::steady_timer timer;
	bool timerStarted = false;
};

class PropellerServer : public LobbyServer
{
public:
	PropellerServer(uint16_t port, asio::io_context& io_context)
		: LobbyServer(Game::PropellerA, port, io_context) {}

	Room *addRoom(const std::string& name, uint32_t attributes, Player *owner) override;

protected:
	bool handlePacket(Player *player, const uint8_t *data, size_t len) override;

private:
	void sendPlayerAttrs(Player *player);
};
