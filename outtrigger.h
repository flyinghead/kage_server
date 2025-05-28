/*
	Kage game server.
	Copyright 2019 Shuouma <dreamcast-talk.com>
    Copyright 2025 Flyinghead <flyinghead.github@gmail.com>

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
#include "model.h"

// Outtrigger game data
struct TagCmd
{
	enum {
		SYNC = 0,
		SYS = 1,
		SYS2 = 2,
		SYS_OK = 3,
		START_OK = 4,
		READY = 5,
		GAME_START = 6,
		GAME_OVER = 7,
		JOIN_OK = 8,
		JOIN_NG = 9,
		PAUSE = 0xa,
		WAIT_OVER = 0xb,
		RESULT = 0xc,
		RESULT2 = 0xd,
		OWNER = 0xe,
		ECHO = 0xf,
		RESET = 0x10,
		TIME_OUT = 0x11,
	};

	TagCmd(uint16_t v = 0) {
		full = v;
	}

	union {
		struct {
			uint16_t :3;
			uint16_t id:3;		// seq# ?
			uint16_t player:4;	// == playerCount in SYS2
			uint16_t command:6;
		};
		uint16_t full;
	};
};

class OTRoom : public Room
{
public:
	OTRoom(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context)
		: Room(lobby, id, name, attributes, owner, io_context), timer(io_context), timeLimit(io_context)
	{}

	void setAttributes(uint32_t attributes) override;
	void rudpAcked(Player *player) override;

	using sysdata_t = std::array<uint8_t, 20>;
	std::vector<sysdata_t> getSysData() const;
	void setSysData(const Player *player, const sysdata_t& sysdata);

	void setGameData(const Player *player, const uint8_t *data);

	using result_t = std::array<uint8_t, 32>;
	std::vector<result_t> getResults() const;
	bool setResult(const Player *player, const uint8_t *result);

	bool setReady(const Player *player);

	uint16_t getNextFrame() {
		return frameNum++;
	}

	void reset();
	void startSync();
	void endGame();

private:
	struct PlayerState
	{
		enum State {
			Init,		// initial state
			SysData,	// SYS data received
			SysOk,		// SYS_OK is ack'ed
			Ready,		// READY received
			Started,	// START_GAME is ack'ed
			Result,		// RESULT received
			Gone,		// player left
		};
		State state;
		sysdata_t sysdata;
		std::array<uint8_t, 18> gamedata;
		result_t result;
	};

	void onRemovePlayer(Player *player, int index) override;
	void sendGameData(const std::error_code& ec);
	PlayerState& getPlayerState(unsigned index);
	void sendGameOver();

	uint16_t frameNum = 0;
	enum { Init, SyncStarted, InGame, GameOver, Result } roomState = Init;
	std::vector<PlayerState> playerState;
	asio::steady_timer timer;
	asio::steady_timer timeLimit;
	int pointLimit = 0;
};

class OuttriggerServer : public LobbyServer
{
public:
	OuttriggerServer(uint16_t port, asio::io_context& io_context)
		: LobbyServer(Game::Outtrigger, port, io_context) {}

	Room *addRoom(const std::string& name, uint32_t attributes, Player *owner) override;

protected:
	bool handlePacket(Player *player, const uint8_t *data, size_t len) override;
};
