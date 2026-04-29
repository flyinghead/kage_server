/*
	Kage game server.
	Copyright 2019 Shuouma <dreamcast-talk.com>
    Copyright 2026 Flyinghead <flyinghead.github@gmail.com>
    Copyright 2026 Farkus / LikeAGFeld

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

union BMCmd
{
	BMCmd(uint16_t v = 0) {
		full = v;
	}
	BMCmd(uint16_t command, uint16_t size) {
		this->command = command;
		this->size = size;
	}

	uint16_t full;
	struct {
		uint16_t size:9;
		uint16_t command:7;
	};

	enum {
		// GAME_DATA (0x11)
		BOMB_DATA = 1,
		MAP_DATA = 2,
		POS_DATA = 3,
		START_TIMER = 4,
		NEXT_TIMER = 5,
		SET_RULES = 7,
		START_BATTLE = 0xa,
		ACCEPT_RULES = 0xb,
		ACK_RULES = 0xc,
		ACK_START = 0xd,
		START_GAME = 0xe,
		RESTART_GAME = 0xf,
		END_GAME = 0x10,
		END_BATTLE = 0x13,
		MAP_INFO = 0x1a,
		MAP_INFO_LAST = 0x1b,

		// CHAT (0xF)
		KICK_PLAYER = 7,
		PING = 0x1c,

		// to game
		ROOM_JOIN = 8,
		ROSTER_LIST = 0xa,
		NEW_MASTER = 0xe,
		READY_MASK = 0x11,
		ABORT_GAME = 0x12,	// "A game was not able to be started."
		GAME_STARTING = 0x13,
		TIME_INFO = 0x14,
		//DISCONNECT? = 0x15,
		SET_DEAD_BITS = 0x16,
		CMPL_DEAD_BITS = 0x19,
	};
};

union Position
{
	uint16_t full;
	struct {
		uint16_t frac:4;
		uint16_t vaxis:1;
		uint16_t y:5;
		uint16_t x:6;
	};

	Position(uint16_t full = 0) {
		this->full = full;
	}
	Position(const uint8_t *data) {
		readFrom(data);
	}

	float xpos() const
	{
		if (vaxis)
			return (float)x;
		else
			return x + (frac - 8.f) / 16.f;
	}

	float ypos() const
	{
		if (!vaxis)
			return (float)y;
		else
			return y + (frac - 8.f) / 16.f;
	}

	void readFrom(const uint8_t *data) {
		this->full = ntohs(*(const uint16_t *)data);
	}
	void writeTo(uint8_t *data) const {
		*(uint16_t *)data = ntohs(this->full);
	}
};

struct CompactUser
{
	Position pos;
	uint8_t unk = 0;	// 4=dead?
	uint8_t dir = 0;	// 1=left, 2=right, 4=up, 8=down

	CompactUser() {
		unk = 0;
		dir = 0;
	}
	CompactUser(const uint8_t *data) {
		readFrom(data);
	}

	void readFrom(const uint8_t *data)
	{
		pos.readFrom(data);
		unk = data[2];
		dir = data[3];
	}
	void writeTo(uint8_t *data) const
	{
		pos.writeTo(data);
		data[2] = unk;
		data[3] = dir;
	}
};

struct PowerUp
{
	Position pos;
	union {
		uint16_t param;
		struct {
			uint16_t unk1:4;
			uint16_t unk2:4;
			uint16_t slot:4;
			uint16_t state:4;
		};
	};

	enum State {
		Gone = 0,
		Hidden = 1,
		Appearing = 2,
		Visible = 3,
		Expired = 4,
		Acquired = 5,
		Unknown6 = 6,
		Unknown9 = 9,
		Claimed = 0xb,
		Granted = 0xe,
		Random = 0xf,
	};

	PowerUp() {
		param = 0;
	}
	PowerUp(const uint8_t *data) {
		readFrom(data);
	}

	void readFrom(const uint8_t *data) {
		pos.readFrom(data);
		param = read16(data, 2);
	}
	void writeTo(uint8_t *data) const {
		pos.writeTo(data);
		write16(data, 2, param);
	}
};

struct Bomb
{
	Position pos;
	union {
		uint16_t param1;
		struct {
			uint16_t unk1:4;
			uint16_t slot:4;
			uint16_t unk2:4;
			uint16_t type:4;
		};
	};
	union {
		uint16_t param2;
		struct {
			uint16_t unk3:9;
			uint16_t unk4:2;
			uint16_t unk5:2;
			uint16_t unk6:3;
		};
	};

	Bomb() {
		param1 = 0;
		param2 = 0;
	}
	Bomb(const uint8_t *data) {
		readFrom(data);
	}

	void readFrom(const uint8_t *data)
	{
		pos.readFrom(data);
		param1 = read16(data, 2);
		param2 = read16(data, 4);
	}
	void writeTo(uint8_t *data) const
	{
		pos.writeTo(data);
		write16(data, 2, param1);
		write16(data, 4, param2);
	}
};

class BMRoom : public Room
{
public:
	BMRoom(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context);

	void addPlayer(Player *player) override;
	bool removePlayer(Player *player) override;
	uint32_t getPlayerCount() const override;
	void createJoinRoomReply(Packet& reply, Packet& relay, Player *player) override;
	void rudpAcked(Player *player) override;
	void agreeRules(Player *player);
	void setRules(const uint8_t *p, uint16_t rulesWord);
	void sendRules(Packet& packet);
	void sendGameStarting(Packet& packet, uint16_t clientId);
	void playerInGame(Player *player);

	uint32_t getHostCount() const {
		return (uint32_t)players.size();
	}
	int getSlotCount(const Player *player) const;
	int getPlayerPosition(const Player *player) const;
	uint8_t getReadySlotMask() const;
	uint8_t getSlotMask(const Player *player) const;

	const std::array<uint8_t, 9>& getRules() const {
		// 9 bytes
		// 0: game mode 0=survival, 1=hyper bomber
		// 1: stage [0-6]
		// 2: games per match
		// 3: time limit - 1 (min)
		// 5: 1=random placement
		return rules;
	}

	void saveMapInfo(Player *player, const uint8_t *data, bool last);
	void savePlayerCoords(Player *player, const uint8_t *data);
	void savePowerUps(Player *player, const uint8_t *data);
	void saveBrickMap(Player *player, const uint8_t *data);
	void saveTimestamp(Player *player, const uint8_t *data);
	void saveBombState(Player *player, const uint8_t *data);
	void makeCmd1Packet(Player *player, Packet& packet);
	void makeCmd2Packet(Packet& packet);
	void makeCmd3Packet(Packet& packet);
	bool checkEndOfGame(Player *player, uint8_t command);
	void sendEndOfGame(Player *player, Packet& packet);

private:
	void updateSlots();
	void sendRosterList(Packet& packet);
	void broadcastKeyholder() const;
	void broadcastReadySlotMask() const;
	void writePlayersPos(Packet& packet);
	void startGameTimer();
	void writeTimestamp(Packet& packet);
	std::chrono::minutes getTimeLimit() const;
	uint32_t getDeadPlayers() const;
	void resetGame();
	void resetMatch();

	struct State
	{
		enum Status {
			None = 0,
			Joining = 1,
			InRoom = 2,
			RulesAccepted = 3,
			MapInfoStarted = 4,
			MapInfoSent = 5,
			GameEnd = 6,
			CompletedDeadBits = 8,
		};
		Status status = None;
		std::array<CompactUser, 4> positions;
		uint32_t cmd1Timestamp = 0;
		std::array<bool, 4> dead {};
		uint8_t endOfGameMask = 0;
	};
	std::array<State, 8> states;
	std::vector<int> slots;	// slots used by each player
	asio::steady_timer timer;
	asio::steady_timer gameTimer;
	std::array<uint8_t, 9> rules {};
	uint16_t ruleSetter = 0; // client ID of the room owner with msb set (8000)
	bool inGame = false;
	std::array<PowerUp, 28> powerUps;
	std::array<uint8_t, 16> brickMap;
	std::array<Bomb, 24> bombs;
	int bombsPerPlayer = 1;
	int gameNumber = 0;
};

class BombermanServer : public LobbyServer
{
public:
	BombermanServer(uint16_t port, asio::io_context& io_context);

	Room *addRoom(const std::string& name, uint32_t attributes, Player *owner) override;

protected:
	bool handlePacket(Player *player, const uint8_t *data, size_t len) override;
};
