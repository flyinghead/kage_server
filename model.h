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
#include <deque>
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
	Player(LobbyServer& server, const asio::ip::udp::endpoint& endpoint, uint32_t id, asio::io_context& io_context)
		: server(server), id(id), endpoint(endpoint), timer(io_context)
	{
		setAlive();
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

	void send(Packet& packet);
	static void sendToAll(Packet& packet, const std::vector<Player *>& players, Player *except = nullptr);

	uint32_t getUnrelSeqAndInc() {
		return unrelSeq++;
	}

	void notifyRoomOnAck() {
		waitingForSeq = relSeq;
	}
	void ackRUdp(uint32_t seq);

	void setAlive();
	bool timedOut() const;

	time_point getLastTimeSeen() const {
		return lastTime;
	}

private:
	void sendRel(Packet& packet, uint32_t seq);
	void resendTimer(const std::error_code& ec);

	LobbyServer& server;
	uint32_t id = 0;
	std::string name;
	asio::ip::udp::endpoint endpoint;

	uint32_t status = 0;
	Lobby *lobby = nullptr;
	Room *room = nullptr;
	uint32_t relSeq = 0; // must start at 0
	int ackedRelSeq = -1;
	uint32_t unrelSeq = 0;
	int waitingForSeq = -1;
	time_point lastTime;
	Packet lastRelPacket;
	std::deque<std::pair<uint32_t, Packet>> relQueue;
	asio::steady_timer timer;
	int sendCount = 0;
};

class Room
{
public:
	static constexpr uint32_t SERVER_READY = 0x00000001;
	static constexpr uint32_t PASSSWORD = 0x01000000;
	static constexpr uint32_t TEAM = 0x02000000;
	// 04000000: can start?
	static constexpr uint32_t LOCKED = 0x40000000;
	static constexpr uint32_t PLAYING = 0x80000000;

	Room(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context);
	virtual ~Room();

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
	virtual void setAttributes(uint32_t attributes) {
		this->attributes = attributes;
	}

	uint32_t getMaxPlayers() const {
		return maxPlayers;
	}
	void setMaxPlayers(uint32_t maxPlayers) {
		this->maxPlayers = maxPlayers;
	}

	const std::string& getPassword() const {
		return password;
	}
	void setPassword(const std::string& password) {
		this->password = password;
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

	virtual void rudpAcked(Player *player) {
	}

	void writeNetdump(const uint8_t *data, uint32_t len, const asio::ip::udp::endpoint& endpoint) const;

protected:
	virtual void onRemovePlayer(Player *player, int index) {
	}

	void openNetdump();
	void closeNetdump() {
		if (netdump != nullptr)
			fclose(netdump);
	}

	Lobby& lobby;
	const uint32_t id;
	std::string name;
	uint32_t attributes;
	Player *owner;
	uint32_t maxPlayers = 0;
	std::string password;
	std::vector<Player *> players;
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
	void addRoom(Room *room);
	void removeRoom(Room *room);

private:
	LobbyServer& server;
	const uint32_t id;
	std::string name;
	std::vector<Player *> players;
	std::map<uint32_t, Room *> rooms;
};

class Server
{
public:
	virtual ~Server() {}

	Server(uint16_t port, asio::io_context& io_context)
		: io_context(io_context),
		  socket(io_context, asio::ip::udp::endpoint(asio::ip::udp::v4(), port))
	{
		asio::socket_base::reuse_address option(true);
		socket.set_option(option);
	}

	void start() {
		read();
	}

protected:
	void read();
	// Called for each packet in the datagram
	virtual void handlePacket(const uint8_t *data, size_t len) = 0;
	// Called after all packets have been handled
	virtual void handlePacketDone() {
	}
	// Hook to dump all UDP data received
	virtual void dump(const uint8_t* data, size_t len) {
	}

	asio::io_context& io_context;
	asio::ip::udp::socket socket;
	std::array<uint8_t, 1510> recvbuf;
	asio::ip::udp::endpoint source;	// source endpoint when receiving packets
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
	void send(Packet& packet, const asio::ip::udp::endpoint& endpoint);
	virtual Room *addRoom(const std::string& name, uint32_t attributes, Player *owner);

	const Game game;

protected:
	void dump(const uint8_t* data, size_t len) override;
	void handlePacket(const uint8_t *data, size_t len) override;
	void handlePacketDone() override;
	// Game-specific packet handling called before normal handling to be overridden by subclasses.
	// Returns true if the packet was handled.
	virtual bool handlePacket(Player *player, const uint8_t *data, size_t len) {
		return false;
	}
	void startTimer();

	std::vector<Lobby> lobbies;
	uint32_t nextRoomId = 0x2001;
	using PlayerMap = std::map<asio::ip::udp::endpoint, Player *>;
	PlayerMap players;
	asio::steady_timer timer;
	// Current player and packets during packet handling
	Player *player = nullptr;
	Packet replyPacket;
	Packet relayPacket;
	static constexpr uint32_t LOBBY_ID_BASE = 0x3001;
};
