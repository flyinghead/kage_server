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
#pragma once
#include "kage.h"
#include "asio.h"
#include <stdint.h>
#include <string>
#include <array>
#include <map>
#include <chrono>

class Player;
class Room;
class Lobby;
class LobbyServer;
class Packet;

using Clock = std::chrono::steady_clock;
using time_point = std::chrono::time_point<Clock>;

class Player
{
public:
	Player(LobbyServer& server, const asio::ip::udp::endpoint& endpoint,
			uint32_t id)
		: server(server), id(id), endpoint(endpoint) {
	}

	uint32_t getId() const {
		return id;
	}

	const std::string& getName() const {
		return name;
	}
	void setName(const std::string& name) {
		this->name = name;
	}
	const asio::ip::udp::endpoint& getEndpoint() const {
		return endpoint;
	}

	void setStatus(uint32_t status) {
		this->status = status;
	}

	Lobby *getLobby() const {
		return lobby;
	}
	void setLobby(Lobby *lobby) {
		this->lobby = lobby;
	}

	Room *getRoom() const {
		return room;
	}
	void setRoom(Room *room) {
		this->room = room;
	}

	uint32_t getRelSeqAndInc() {
		return relSeq++;
	}

	void notifyRoomOnAck() {
		waitingForSeq = relSeq - 1;
	}
	void ackRUdp(uint32_t seq);

	void setAlive();
	bool timedOut() const;

	time_point getLastTimeSeen() const {
		return lastTime;
	}

private:
	LobbyServer& server;
	uint32_t id = 0;
	std::string name;
	asio::ip::udp::endpoint endpoint;

	uint32_t status = 0;
	Lobby *lobby = nullptr;
	Room *room = nullptr;
	uint32_t relSeq = 0; // must start at 0
	int waitingForSeq = -1;
	time_point lastTime;
};

class Room
{
public:
	Room(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context);
	~Room();

	uint32_t getId() const {
		return id;
	}
	const std::string& getName() const {
		return name;
	}
	Player *getOwner() const {
		return owner;
	}

	uint32_t getAttributes() const {
		return attributes;
	}
	void setAttributes(uint32_t attributes);

	uint32_t getMaxPlayers() const {
		return maxPlayers;
	}
	void setMaxPlayers(uint32_t maxPlayers) {
		this->maxPlayers = maxPlayers;
	}

	uint32_t getPlayerCount() const {
		return (uint32_t)players.size();
	}
	int getPlayerIndex(const Player *player);

	void addPlayer(Player *player);
	bool removePlayer(Player *player);

	const std::vector<Player *>& getPlayers() const {
		return players;
	}

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

	void startSync();
	void endGame();

	void rudpAcked(Player *player);

	void writeNetdump(const uint8_t *data, uint32_t len, const asio::ip::udp::endpoint& endpoint) const;

private:
	void reset();
	void openNetdump();
	void sendGameData(const std::error_code& ec);

	void closeNetdump() {
		if (netdump != nullptr)
			fclose(netdump);
	}

	struct PlayerState
	{
		enum State {
			Init,		// initial state
			SysData,	// SYS data received
			SysOk,		// SYS_OK is ack'ed
			Ready,		// READY received
			Started,	// START_GAME is ack'ed
			Result,		// RESULT received
		};
		State state;
		sysdata_t sysdata;
		std::array<uint8_t, 18> gamedata;
		result_t result;
	};

	Lobby& lobby;
	const uint32_t id;
	std::string name;
	uint32_t attributes;	// 80000000: playing, 40000000: locked, 04000000: can start?, 01000000: password, 1: server started/ready
							// c0000000 => start game
	Player *owner;
	uint32_t maxPlayers = 0;
	uint16_t frameNum = 0;
	enum { Init, SyncStarted, InGame, Result } roomState = Init;
	std::vector<Player *> players;
	std::vector<PlayerState> playerState;
	asio::steady_timer timer;
	LobbyServer& server;
	const Game game;
	FILE *netdump = nullptr;
};

class Lobby
{
public:
	Lobby(LobbyServer& server, uint32_t id, const std::string& name)
		: server(server), id(id), name(name)
	{
		assert(name.length() <= 16);
		/* Test player and room
		Player *p = new Player(server, asio::ip::udp::endpoint(), 0x4001);
		p->setLobby(this);
		p->setName("dummy");
		Room *room = addRoom("test", 1, p);
		room->setMaxPlayers(8);
		*/
	}

	LobbyServer& getServer() const {
		return server;
	}
	const uint32_t getId() const {
		return id;
	}
	const std::string& getName() const {
		return name;
	}

	uint32_t getPlayerCount() const {
		return (uint32_t)players.size();
	}
	void addPlayer(Player *player);
	void removePlayer(Player *player);

	const std::vector<Player *>& getPlayers() const {
		return players;
	}

	uint32_t getRoomCount() const {
		return (uint32_t)rooms.size();
	}
	Room *getRoom(uint32_t id) const;
	std::vector<Room *> getRooms() const;
	Room *addRoom(const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context);
	void removeRoom(Room *room);

private:
	LobbyServer& server;
	const uint32_t id;
	std::string name;
	std::vector<Player *> players;
	std::map<uint32_t, Room *> rooms;
	uint32_t nextRoomId = 0x2001;
};

class Server
{
public:
	virtual ~Server() {}

	Server(uint16_t port, asio::io_context& io_context)
		: socket(io_context, asio::ip::udp::endpoint(asio::ip::udp::v4(), port))
	{
		asio::socket_base::reuse_address option(true);
		socket.set_option(option);
	}

	void start() {
		read();
	}

protected:
	void read();
	virtual void handlePacket(const uint8_t *data, size_t len) = 0;
	virtual void dump(const uint8_t* data, size_t len) {
	}

	asio::ip::udp::socket socket;
	std::array<uint8_t, 1510> recvbuf;
	asio::ip::udp::endpoint source;	// source endpoint when receiving packets
	uint32_t unrelSeq = 1;
};

class LobbyServer : public Server
{
public:
	LobbyServer(Game game, uint16_t port, asio::io_context& io_context);

	void addLobby(const std::string& name)
	{
		assert(lobbies.size() < 10);
		uint32_t id = (uint32_t)(lobbies.size() + LOBBY_ID_BASE);
		lobbies.emplace_back(*this, id, name);
	}
	Lobby *getLobby(uint32_t id)
	{
		id -= LOBBY_ID_BASE;
		if (id >= lobbies.size())
			return nullptr;
		return &lobbies[id];
	}

	void addPlayer(Player *player);
	void removePlayer(Player *player);
	void send(Packet& packet, Player& player);
	void sendToAll(Packet& packet, const std::vector<Player *>& players, Player *except = nullptr);

	const Game game;

protected:
	void dump(const uint8_t* data, size_t len) override;
	void handlePacket(const uint8_t *data, size_t len) override;
	void startTimer();
	virtual bool handlePacket(Player *player, const uint8_t *data, size_t len) {
		return false;
	}

	std::vector<Lobby> lobbies;
	using PlayerMap = std::map<asio::ip::udp::endpoint, Player *>;
	PlayerMap players;
	asio::io_context& io_context;
	asio::steady_timer timer;
	static constexpr uint32_t LOBBY_ID_BASE = 0x3001;
};

class OuttriggerServer : public LobbyServer
{
public:
	OuttriggerServer(uint16_t port, asio::io_context& io_context)
		: LobbyServer(Game::Outtrigger, port, io_context) {}

protected:
	bool handlePacket(Player *player, const uint8_t *data, size_t len) override;
};

class BootstrapServer : public Server
{
public:
	BootstrapServer(uint16_t port, asio::io_context& io_context)
		: Server(port, io_context),
		  bombermanServer(Game::Bomberman, BOMBERMAN_PORT, io_context),
		  outtriggerServer(OUTTRIGGER_PORT, io_context),
		  propellerServer(Game::PropellerA, PROPELLERA_PORT, io_context)
	{
	}

	void start();

private:
	void handlePacket(const uint8_t *data, size_t len) override;

	uint32_t nextUserId = 0x1001;
	LobbyServer bombermanServer;
	OuttriggerServer outtriggerServer;
	LobbyServer propellerServer;
	static constexpr uint16_t BOMBERMAN_PORT = 9091;
	static constexpr uint16_t OUTTRIGGER_PORT = 9092;
	static constexpr uint16_t PROPELLERA_PORT = 9093;
	static constexpr const char *OuttriggerKey = "reggirttuO";
	static constexpr const char *PropellerKey = "ArelleporP";
};
