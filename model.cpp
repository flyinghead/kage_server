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
#include "discord.h"
#include "log.h"
#include <algorithm>

using namespace std::chrono_literals;

constexpr int TimeLimits[] { 120, 140, 160, 180, 200, 220, 240, 260, 280, 300, 360, 420, 480, 600, 900, 1200, -1 };

void Player::setAlive() {
	lastTime = Clock::now();
}

bool Player::timedOut() const
{
	if (room == nullptr)
		return Clock::now() - lastTime >= 2min;
	else
		return Clock::now() - lastTime >= 30s;
}

void Player::send(Packet& packet)
{
	packet.finalize();
	// Loop through all packets and set the player id (offset 4) and sequence number (offset 8)
	size_t i = 0;
	bool rudpSeen = false;
	while (i < packet.size)
	{
		uint16_t size = read16(packet.data, i);
		uint16_t flags = size & 0xfc00;
		size &= 0x3ff;
		Packet::Command com = (Packet::Command)packet.data[i + 3];
		if (flags & Packet::FLAG_RUDP)
		{
			// Only the first reliable packet has a seq#
			if (!rudpSeen)
				write32(packet.data, i + 8, relSeq++);
			rudpSeen = true;
		}
		else if (com != Packet::REQ_NOP) {
			// unreliable NOPs don't have a seq#
			write32(packet.data, i + 8, unrelSeq++);
		}
		write32(packet.data, i + 4, id);
		i += size;
	}
	if (rudpSeen)
		sendRel(packet, relSeq - 1);
	else
		server.send(packet, getEndpoint());
}

void Player::sendToAll(Packet& packet, const std::vector<Player *>& players, Player *except)
{
	for (Player *pl : players)
		if (pl != except)
			pl->send(packet);
}

void Player::sendRel(Packet& packet, uint32_t seq)
{
	if ((int)seq == ackedRelSeq + 1)
	{
		lastRelPacket = packet;
		sendCount = 0;
		resendTimer({});
	}
	else {
		relQueue.push_back(std::make_pair(seq, packet));
	}
}

void Player::resendTimer(const std::error_code& ec)
{
	if (ec)
		return;
	if (sendCount >= 5)
	{
		WARN_LOG(server.game, "Sending packet %x to %s failed after %d attempts", lastRelPacket.data[3], name.c_str(), sendCount);
		ackedRelSeq++;
		if (!relQueue.empty()) {
			sendRel(relQueue.front().second, relQueue.front().first);
			relQueue.pop_front();
		}
		return;
	}
	sendCount++;
	server.send(lastRelPacket, getEndpoint());
	timer.expires_after(500ms);
	// game (bba) apparently retries after 100 ms, 200 ms, 400 ms, 800 ms then timeout
	timer.async_wait(std::bind(&Player::resendTimer, this, asio::placeholders::error));
}

void Player::ackRUdp(uint32_t seq)
{
	ackedRelSeq = seq;
	std::error_code ec;
	timer.cancel(ec);
	if (!relQueue.empty()) {
		sendRel(relQueue.front().second, relQueue.front().first);
		relQueue.pop_front();
	}
	if (seq == (unsigned)waitingForSeq)
	{
		waitingForSeq = -1;
		if (room != nullptr)
			room->rudpAcked(this);
	}
}

void Server::read()
{
	socket.async_receive_from(asio::buffer(recvbuf), source,
		[this](const std::error_code& ec, size_t len)
		{
			if (ec) {
				ERROR_LOG(Game::None, "receive_from failed: %s", ec.message().c_str());
				read();
				return;
			}
			dump(recvbuf.data(), len);
			//printf("UdpSocket: received %d bytes to port %d from %s:%d\n", (int)len,
			//		socket.local_endpoint().port(), source.address().to_string().c_str(), source.port());
			if (len < 0x14)
			{
				ERROR_LOG(Game::None, "datagram too small: %zd bytes", len);
				read();
				return;
			}
			size_t idx = 0;
			len -= 4;	// ignore end of datagram tag
			do {
				uint16_t pktSize = read16(recvbuf.data(), idx) & 0x3ff;
				if (pktSize < 0x10) {
					ERROR_LOG(Game::None, "packet too small: %d bytes", pktSize);
					break;
				}
				// Ack packets have length 0x14 for some reason...
				if (pktSize > len - idx && recvbuf[idx + 3] != Packet::REQ_NOP) {
					ERROR_LOG(Game::None, "packet truncated: %d bytes > %zd bytes", pktSize, len - idx);
					break;
				}
				handlePacket(&recvbuf[idx], pktSize);
				idx += pktSize;
			} while (idx < len);
			handlePacketDone();
			read();
		});
}

void BootstrapServer::start()
{
	bombermanServer.start();
	outtriggerServer.start();
	propellerServer.start();
	Server::start();
}

void BootstrapServer::handlePacket(const uint8_t *data, size_t len)
{
	DEBUG_LOG(Game::None, "Bootstrap: Packet: flags/size %02x %02x command %02x %02x", data[0], data[1], data[2], data[3]);
	Packet packet;
	switch (data[3])
	{
	case Packet::REQ_BOOTSTRAP_LOGIN:
		{
			/* Using 2D and blowfish encryption (works with outtrigger)
			packet.init(Packet::RSP_LOGIN_SUCCESS, &data[4]);
			packet.writeData(&data[0x10], 0x28);
			// FIXME auto addr = socket.local_endpoint().address().to_v4().to_bytes();
			std::array<uint8_t, 4>addr { 192, 168, 1, 31 };
			packet.writeData((uint8_t *)addr.data(), addr.size());
			packet.writeData((uint32_t)socket.local_endpoint().port());
			packet.writeData(0u);
			packet.writeData(0u);
			packet.writeData(0u); // size of following data, sent back when logging to lobby
			packet.size = ((packet.size + 7) / 8) * 8;
			BLOWFISH_CTX *ctx = new BLOWFISH_CTX();
			Blowfish_Init(ctx, (uint8_t *)OuttriggerKey, strlen(OuttriggerKey));
			for (int i = 0x10; i < packet.size; i += 8)
			{
				uint32_t *x = (uint32_t *)&packet.data[i];
				x[0] = ntohl(x[0]);
				x[1] = ntohl(x[1]);
				Blowfish_Encrypt(ctx, x, x + 1);
				x[0] = htonl(x[0]);
				x[1] = htonl(x[1]);
			}
			*/
			uint16_t port = OUTTRIGGER_PORT;
			LobbyServer *server = &outtriggerServer;
			std::string name;
			if (!strcmp((const char *)&data[0x10], "BombermanOnline"))
			{
				port = BOMBERMAN_PORT;
				server = &bombermanServer;
				name = (const char *)&data[0x38];
				auto sep = name.find('\1');
				if (sep != std::string::npos)
					// get rid of the password
					name = name.substr(0, sep);
			}
			else if (!strcmp((const char *)&data[0x10], "PropellerA"))
			{
				port = PROPELLERA_PORT;
				server = &propellerServer;
				name = (const char *)&data[0x38];	// FIXME this is the game key but no user name
			}
			else {
				// Outtrigger
				name = (const char *)&data[0x10];
			}

			uint32_t tmpUserId = read32(data, 4);

			// Using 29 (shu)
			packet.init(Packet::RSP_LOGIN_SUCCESS2);
			packet.writeData((uint32_t)port);
			packet.writeData(0u);	// ?
			packet.writeData(nextUserId);

			Player *player = new Player(*server, source, nextUserId, io_context);
			player->setName(name);
			server->addPlayer(player);
			nextUserId++;

			size_t pktsize = packet.finalize();
			write32(packet.data, 4, tmpUserId);
			write32(packet.data, 8, player->getUnrelSeqAndInc());
			std::error_code ec2;
			socket.send_to(asio::buffer(packet.data, pktsize), source, 0, ec2);
			break;
		}

	case Packet::REQ_PING:
		{
			packet.respOK(Packet::REQ_PING);
			packet.writeData(read32(data, 0x10));
			size_t pktsize = packet.finalize();
			write32(packet.data, 4, read32(data, 4));
			std::error_code ec2;
			socket.send_to(asio::buffer(packet.data, pktsize), source, 0, ec2);
			break;
		}

	case Packet::REQ_NOP:
		break;

	default:
		ERROR_LOG(Game::None, "Bootstrap: Unhandled msg type %x", data[3]);
		break;
	}
}

LobbyServer::LobbyServer(Game game, uint16_t port, asio::io_context& io_context)
	: Server(port, io_context), game(game), timer(io_context)

{
	lobbies.reserve(10);
	addLobby("ShuMania");
	startTimer();
}

void LobbyServer::startTimer()
{
	timer.expires_at(Clock::now() + 30s);
	timer.async_wait([this](const std::error_code& ec) {
		if (ec)
			return;
		std::vector<Player *> timeouts;
		for (auto& [ep, player] : players)
		{
			if (player->timedOut()) {
				INFO_LOG(game, "Player %s has timed out", player->getName().c_str());
				timeouts.push_back(player);
			}
			else if (player->getRoom() == nullptr && player->getLastTimeSeen() + 30s >= Clock::now())
			{
				// Send a reliable NOP and expect an ack
				Packet packet;
				packet.init(Packet::REQ_NOP);
				packet.flags |= Packet::FLAG_RUDP;
				player->send(packet);
			}
		}
		for (Player *player : timeouts)
			removePlayer(player);
		startTimer();
	});
}

void LobbyServer::addPlayer(Player *player)
{
	auto it = players.find(player->getEndpoint());
	if (it != players.end())
	{
		WARN_LOG(game, "Player %s [%x] from %s:%d already in lobby server",
				it->second->getName().c_str(), it->second->getId(),
				player->getEndpoint().address().to_string().c_str(), player->getEndpoint().port());
		removePlayer(it->second);
	}
	INFO_LOG(game, "Player %s [%x] joined lobby server from %s:%d",
			player->getName().c_str(), player->getId(),
			player->getEndpoint().address().to_string().c_str(), player->getEndpoint().port());
	players[player->getEndpoint()] = player;
}

void LobbyServer::removePlayer(Player *player)
{
	if (player->getLobby() != nullptr)
		player->getLobby()->removePlayer(player);
	players.erase(player->getEndpoint());
	INFO_LOG(game, "Player %s [%x] left lobby server", player->getName().c_str(), player->getId());
	delete player;
}

void LobbyServer::send(Packet& packet, const asio::ip::udp::endpoint& endpoint)
{
	size_t pktsize = packet.finalize();
	std::error_code ec2;
	socket.send_to(asio::buffer(packet.data, pktsize), endpoint, 0, ec2);
}

void LobbyServer::handlePacket(const uint8_t *data, size_t len)
{
	if (player == nullptr)
	{
		auto it = players.find(source);
		if (it == players.end()) {
			WARN_LOG(game, "Packet from unknown endpoint %s:%d ignored", source.address().to_string().c_str(), source.port());
			return;
		}
		player = it->second;
		player->setAlive();
	}
	//printf("Lobby: %s packet: flags/size %02x %02x command %02x %02x\n", player->getName().c_str(), data[0], data[1], data[2], data[3]);
	// Game-specific packet handling
	if (handlePacket(player, data, len))
		return;

	switch (data[3])
	{
	case Packet::REQ_LOBBY_LOGIN:	// Only when using 2C response to bootstrap login
		{
			replyPacket.respOK(Packet::REQ_LOBBY_LOGIN);
			replyPacket.ack(read32(data, 8));
			replyPacket.writeData(player->getId());
			break;
		}
	case Packet::REQ_LOBBY_LOGOUT:
		{
			replyPacket.respOK(Packet::REQ_LOBBY_LOGOUT);
			replyPacket.ack(read32(data, 8));
			player->send(replyPacket);
			removePlayer(player);
			player = nullptr;
			break;
		}
	case Packet::REQ_QRY_LOBBIES:
		{
			replyPacket.init(Packet::REQ_QRY_LOBBIES);
			replyPacket.ack(read32(data, 8));
			replyPacket.writeData(0u);
			replyPacket.writeData(0u);
			replyPacket.writeData((uint32_t)lobbies.size());
			for (const Lobby& lobby : lobbies)
			{
				replyPacket.writeData(lobby.getName().c_str(), 0x10);
				replyPacket.writeData(lobby.getPlayerCount());
				replyPacket.writeData(lobby.getRoomCount());
				replyPacket.writeData(lobby.getId());
			}
			break;
		}
	case Packet::REQ_CHG_USER_STATUS:
		{
			player->setStatus(read32(data, 0x10));
			replyPacket.respOK(Packet::REQ_CHG_USER_STATUS);
			replyPacket.ack(read32(data, 8));
			replyPacket.writeData(0u);	// status?
			break;
		}
	case Packet::REQ_QRY_USERS:
		{
			replyPacket.init(Packet::REQ_QRY_USERS);
			replyPacket.ack(read32(data, 8));
			if (data[0] & 0x10)
			{
				// lobby
				replyPacket.flags |= Packet::FLAG_LOBBY;
				replyPacket.writeData(0u);
				replyPacket.writeData(0u);
				uint32_t lobbyId = read32(data, 0x10);
				Lobby *lobby = getLobby(lobbyId);
				if (lobby == nullptr) {
					replyPacket.writeData(0u);	// user count
				}
				else
				{
					replyPacket.writeData(lobby->getPlayerCount());
					for (Player *pl : lobby->getPlayers())
					{
						replyPacket.writeData(pl->getName().c_str(), 0x10);
						replyPacket.writeData(pl->getId());
						replyPacket.writeData(0u);	// controllers
					}
				}
			}
			else
			{
				// room
				replyPacket.writeData(0u);
				replyPacket.writeData(0u);
				int roomId = read32(data, 0x10);
				if (player->getLobby() == nullptr) {
					replyPacket.writeData(0u);	// user count
				}
				else
				{
					Room *room = player->getLobby()->getRoom(roomId);
					if (room == nullptr) {
						replyPacket.writeData(0u);	// user count
					}
					else {
						const std::vector<Player *> players = room->getPlayers();
						replyPacket.writeData(room->getPlayerCount());
						for (Player *pl : players)
						{
							replyPacket.writeData(pl->getName().c_str(), 0x10);
							replyPacket.writeData(pl->getId());
							replyPacket.writeData(0u);	// controllers
						}
					}
				}
			}
			break;
		}
	case Packet::REQ_JOIN_LOBBY_ROOM:
		{
			uint32_t id = read32(data, 0x10);
			if (data[0] & 0x10)
			{
				// lobby
				Lobby *lobby = getLobby(id);
				if (lobby == nullptr) {
					replyPacket.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
					replyPacket.writeData(8u);
					WARN_LOG(game, "%s join lobby failed: unknown lobby id %x", player->getName().c_str(), id);
				}
				else
				{
					lobby->addPlayer(player);

					// Notify other players
					relayPacket.init(Packet::REQ_JOIN_LOBBY_ROOM);
					relayPacket.flags |= Packet::FLAG_LOBBY;
					relayPacket.writeData(player->getName().c_str(), 0x10);
					relayPacket.writeData(player->getId());
					relayPacket.writeData(0u);	// controllers

					replyPacket.respOK(Packet::REQ_JOIN_LOBBY_ROOM);
					replyPacket.writeData(lobby->getId());
				}
				replyPacket.flags |= Packet::FLAG_LOBBY;
				replyPacket.ack(read32(data, 8));
			}
			else
			{
				// room
				Lobby *lobby = player->getLobby();
				Room *room = lobby == nullptr ? nullptr : lobby->getRoom(id);
				if (room == nullptr)
				{
					replyPacket.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
					replyPacket.ack(read32(data, 8));
					replyPacket.writeData(8u);
					WARN_LOG(game, "%s join room failed: unknown room id %x (lobby %p)", player->getName().c_str(), id, lobby);
				}
				else
				{
					if (room->getAttributes() & 0xc0000000)
					{
						// Room locked or in game
						replyPacket.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
						replyPacket.ack(read32(data, 8));
						// 9 is "room locked"
						replyPacket.writeData(9u);
						INFO_LOG(game, "%s join room failed: room locked", player->getName().c_str());
						break;
					}
					std::string password = (const char *)&data[0x18];
					if (password != room->getPassword())
					{
						replyPacket.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
						replyPacket.ack(read32(data, 8));
						// 0xF is incorrect password
						replyPacket.writeData(0xfu);
						INFO_LOG(game, "%s join room failed: incorrect password", player->getName().c_str());
						break;
					}
					room->addPlayer(player);

					// Notify other players
					relayPacket.init(Packet::REQ_JOIN_LOBBY_ROOM);
					relayPacket.writeData(player->getName().c_str(), 0x10);
					relayPacket.writeData(player->getId());
					relayPacket.writeData(0u);	// controllers

					replyPacket.respOK(Packet::REQ_JOIN_LOBBY_ROOM);
					replyPacket.writeData(room->getId());
					replyPacket.ack(read32(data, 8));

					// Push room status to new player
					replyPacket.init(Packet::REQ_CHG_ROOM_STATUS);
					replyPacket.writeData(room->getId());
					replyPacket.writeData("STAT", 4);
					replyPacket.writeData(room->getAttributes());
				}
			}
			break;
		}
	case Packet::REQ_LEAVE_LOBBY_ROOM:
		{
			if (data[0] & 0x10)
			{
				// lobby
				replyPacket.respOK(Packet::REQ_LEAVE_LOBBY_ROOM);
				replyPacket.flags |= Packet::FLAG_LOBBY;
				Lobby *lobby = player->getLobby();
				if (lobby != nullptr)
					lobby->removePlayer(player);
			}
			else
			{
				replyPacket.respOK(Packet::REQ_LEAVE_LOBBY_ROOM);
				Room *room = player->getRoom();
				if (room != nullptr) {
					// Remove player from the room
					if (room->removePlayer(player))
						player->getLobby()->removeRoom(room);
				}
			}
			replyPacket.ack(read32(data, 8));
			break;
		}
	case Packet::REQ_QRY_ROOMS:
		{
			replyPacket.init(Packet::REQ_QRY_ROOMS);
			replyPacket.ack(read32(data, 8));
			replyPacket.flags |= Packet::FLAG_LOBBY;
			int lobbyId = read32(data, 0x10);
			Lobby *lobby = getLobby(lobbyId);
			replyPacket.writeData(0u);	// ?
			replyPacket.writeData(0u);	// ?
			if (lobby == nullptr) {
				replyPacket.writeData(0u);	// room count
			}
			else
			{
				auto rooms = lobby->getRooms();
				replyPacket.writeData((uint32_t)rooms.size());
				for (Room *room : rooms)
				{
					replyPacket.writeData(room->getName().c_str(), 0x10);
					// This is different in outtrigger vs. bomberman
					if (game == Game::Bomberman) {
						replyPacket.writeData(room->getOwner()->getId());
						replyPacket.writeData(room->getPlayerCount());
					}
					else {
						replyPacket.writeData(room->getPlayerCount());
						replyPacket.writeData(room->getOwner()->getId());
					}
					replyPacket.writeData(room->getAttributes());
					replyPacket.writeData(room->getMaxPlayers());
					replyPacket.writeData(room->getId());
				}
			}
			break;
		}
	case Packet::REQ_CREATE_ROOM:
		{
			std::string name = (const char *)&data[0x10];
			uint32_t maxPlayers = read32(data, 0x20);
			std::string password = (const char *)&data[0x24];
			uint32_t attributes = read32(data, 0x38);
			if (player->getLobby() == nullptr) {
				replyPacket.respFailed(Packet::REQ_CREATE_ROOM);
				replyPacket.ack(read32(data, 8));
			}
			else
			{
				attributes |= Room::SERVER_READY;
				Room *room = player->getLobby()->addRoom(name, attributes, player, io_context);
				room->setMaxPlayers(maxPlayers);
				room->setPassword(password);

				// Notify other players in lobby
				relayPacket.init(Packet::REQ_CREATE_ROOM);
				relayPacket.flags |= Packet::FLAG_LOBBY;
				relayPacket.writeData(name.c_str(), 16);
				relayPacket.writeData(1u); // player count
				relayPacket.writeData(player->getId());
				relayPacket.writeData(attributes);
				relayPacket.writeData(maxPlayers);
				relayPacket.writeData(room->getId());

				replyPacket.respOK(Packet::REQ_CREATE_ROOM);
				replyPacket.writeData(room->getId());
				replyPacket.ack(read32(data, 8));

				replyPacket.init(Packet::REQ_CHG_ROOM_STATUS);
				replyPacket.writeData(room->getId());
				replyPacket.writeData("STAT", 4);
				replyPacket.writeData(attributes);

			}
			break;
		}
	case Packet::REQ_CHG_ROOM_STATUS:
		{
			Room *room = player->getRoom();
			if (room == nullptr) {
				replyPacket.respFailed(Packet::REQ_CHG_ROOM_STATUS);
			}
			else
			{
				uint32_t attributes = read32(data, 0x14);
				room->setAttributes(attributes);

				// Notify other users
				relayPacket.init(Packet::REQ_CHG_ROOM_STATUS);
				relayPacket.writeData(room->getId());
				relayPacket.writeData("STAT", 4);
				relayPacket.writeData(attributes);

				replyPacket.respOK(Packet::REQ_CHG_ROOM_STATUS);
				replyPacket.writeData(room->getId());
				replyPacket.writeData("STAT", 4);
				replyPacket.writeData(attributes);
			}
			replyPacket.ack(read32(data, 8));
			break;
		}

	case Packet::REQ_CHAT:
		{
			uint16_t flags = read16(data, 0);
			if (flags & Packet::FLAG_RUDP)
			{
				if (flags & Packet::FLAG_RELAY)
				{
					// Broadcast to other players in the lobby/room
					relayPacket.init(Packet::REQ_CHAT);
					relayPacket.flags |= Packet::FLAG_RUDP | (flags & (Packet::FLAG_LOBBY | Packet::FLAG_RELAY));
					relayPacket.writeData(&data[0x10], (flags & 0x3ff) - 0x10);

					uint32_t seq = read32(data, 8);
					// TODO correct?
					if (seq == 0)
						// don't ack continued chat pkt
						break;
					replyPacket.respOK(Packet::REQ_CHAT);
					replyPacket.ack(seq);
					replyPacket.flags |= flags & Packet::FLAG_LOBBY;
				}
				else
				{
					// FIXME not sure what to do with these. send msg10 OWNER -> game replies with msgF OWNER.
					// If broadcast, other players can change game settings
					TagCmd tag;
					tag.full = read16(data, 0x10);
					INFO_LOG(game, "relUDP msgF: tag=%x (%04x)", tag.command, tag.full);
					replyPacket.init(Packet::REQ_NOP);
					replyPacket.ack(read32(data, 8));
				}
			}
			else
			{
				uint16_t cmd = read16(data, 0x10);
				if (cmd == 0x3804)	// ping
				{
					//bomberman
					INFO_LOG(game, "chat(F) ping %04x %08x %x", read16(data, 0x12), read32(data, 0x14), data[0x18]);
					replyPacket.init(Packet::REQ_CHAT);
					replyPacket.writeData((uint16_t)0x3804u);
					replyPacket.writeData((uint16_t)0x7800u);
					replyPacket.writeData(read32(data, 0x14));
					replyPacket.writeData((uint8_t)0);
				}
				else {
					INFO_LOG(game, "unreliable chat(F) ignored");
				}
			}
			break;
		}
	case Packet::REQ_PING:
		{
			if (game != Game::Bomberman)
			{
				replyPacket.respOK(Packet::REQ_PING);
				// outtrigger and propA send a single value (clock)
				replyPacket.writeData(read32(data, 0x10));
				//packet.writeData(&data[0x10], (read16(data, 0) & 0x3ff) - 0x10);
			}
			else {
				// sends 4 ints, not sure what should be returned
				// FIXME bomberman test -> causes chat(F) packets to be sent
				// works: no logged failure (can be forced with seq=-1)
				replyPacket.init(Packet::REQ_PING);
				replyPacket.writeData(&data[0x10], len - 0x10);
			}
			break;
		}

	case Packet::REQ_CHG_USER_PROP:
		replyPacket.respOK(Packet::REQ_CHG_USER_PROP);
		replyPacket.ack(read32(data, 8));
		// TODO parse
		break;

	case Packet::REQ_NOP:
		break;

	default:
		{
			ERROR_LOG(game, "Lobby: Unhandled msg type %x", data[3]);
			uint16_t flags = read16(data, 0);
			if (flags & Packet::FLAG_RUDP) {
				replyPacket.init(Packet::REQ_NOP);
				replyPacket.ack(read32(data, 8));
			}
			break;
		}
	}
}

void LobbyServer::handlePacketDone()
{
	if (player != nullptr)
	{
		if (!replyPacket.empty())
			player->send(replyPacket);
		if (!relayPacket.empty())
		{
			if (relayPacket.flags & Packet::FLAG_LOBBY) {
				if (player->getLobby() != nullptr)
					Player::sendToAll(relayPacket, player->getLobby()->getPlayers(), player);
			}
			else if (player->getRoom() != nullptr) {
				Player::sendToAll(relayPacket, player->getRoom()->getPlayers(), player);
			}
		}
		player = nullptr;
	}
	replyPacket.reset();
	relayPacket.reset();
}

void LobbyServer::dump(const uint8_t* data, size_t len)
{
	auto it = players.find(source);
	if (it == players.end())
		return;
	Player *player = it->second;
	if (player->getRoom() != nullptr)
		player->getRoom()->writeNetdump(data, len, source);
}

bool OuttriggerServer::handlePacket(Player *player, const uint8_t *data, size_t len)
{
	uint16_t flags = read16(data, 0);
	if (flags & Packet::FLAG_ACK)
		player->ackRUdp(read32(data, 0xc));

	if (data[3] != Packet::REQ_GAME_DATA)
		return false;

	TagCmd tag(read16(data, 0x10));
	switch (tag.command)
	{
	case TagCmd::ECHO:
		// called regularly (< 10s) by all players in the room
		//printf("tag: ECHO\n");
		replyPacket.init(Packet::RSP_TAG_CMD);
		replyPacket.writeData(0u);
		replyPacket.writeData(&data[0x10], 4);
		break;

	case TagCmd::START_OK:
		{
			INFO_LOG(game, "tag: START OK");
			replyPacket.init(Packet::REQ_NOP);
			replyPacket.ack(read32(data, 8));

			Room *room = player->getRoom();
			if (room != nullptr && room->getPlayerCount() >= 2)
			{
				// Make sure we ack before anything else
				player->send(replyPacket);
				replyPacket.reset();
				// send START_OK
				INFO_LOG(game, "Sending START_OK to owner");
				Packet startOk;
				startOk.init(Packet::RSP_TAG_CMD);
				startOk.writeData(0u);	// list: count [int ...]
				startOk.writeData(tag.full);
				startOk.flags |= Packet::FLAG_RUDP;
				room->getOwner()->send(startOk);
			}
			break;
		}

	case TagCmd::SYS:
		{
			INFO_LOG(game, "tag: SYS from %s", player->getName().c_str());
			replyPacket.init(Packet::RSP_TAG_CMD);
			replyPacket.ack(read32(data, 8));
			replyPacket.flags |= Packet::FLAG_RUDP;
			replyPacket.writeData(0u);	// list: count [int ...]
			TagCmd tag;
			tag.command = TagCmd::SYS_OK;
			replyPacket.writeData(tag.full);
			// FIXME what if SYS_OK has already been sent and ack'ed once?
			player->notifyRoomOnAck();

			Room *room = player->getRoom();
			if (room != nullptr)
			{
				Room::sysdata_t sysdata;
				memcpy(sysdata.data(), &data[0x12], sysdata.size());
				room->setSysData(player, sysdata);
			}
			break;
		}

	case TagCmd::READY:
		{
			INFO_LOG(game, "tag: READY from %s", player->getName().c_str());
			replyPacket.init(Packet::REQ_NOP);
			replyPacket.ack(read32(data, 8));

			Room *room = player->getRoom();
			if (room != nullptr && room->setReady(player))
			{
				// Make sure we ack before anything else
				player->send(replyPacket);
				replyPacket.reset();
				// send GAME_START
				INFO_LOG(game, "%s: Sending GAME_START to all players", room->getName().c_str());
				Packet gameStart;
				gameStart.init(Packet::REQ_CHAT);
				gameStart.flags |= Packet::FLAG_RUDP;
				TagCmd tag;
				tag.command = TagCmd::GAME_START;
				gameStart.writeData(tag.full);
				// wait for this packet to be ack'ed by all players before sending game data
				// must be called before sending to get the current rel seq#
				room->startSync();
				Player::sendToAll(gameStart, room->getPlayers());
			}
			break;
		}

	case TagCmd::SYNC:	// actual game data
		{
			//DEBUG_LOG(game, "tag: SYNC from %s", player->getName().c_str());
			if (data[0] & 0x80) {
				// propA sends rel SYNC after creating room
				replyPacket.init(Packet::REQ_NOP);
				replyPacket.ack(read32(data, 8));
			}
			Room *room = player->getRoom();
			if (room != nullptr)
				room->setGameData(player, &data[0x12]);
			break;
		}

	case TagCmd::RESULT:
		{
			INFO_LOG(game, "tag: RESULT from %s", player->getName().c_str());
			replyPacket.init(Packet::REQ_NOP);
			replyPacket.ack(read32(data, 8));

			Room *room = player->getRoom();
			if (room != nullptr && room->setResult(player, &data[0x12]))
			{
				// Make sure we ack before anything else
				player->send(replyPacket);
				replyPacket.reset();
				// Send RESULT2
				INFO_LOG(game, "%s: Sending RESULT2 to all players", room->getName().c_str());
				std::vector<Room::result_t> results  = room->getResults();
				Packet result2;
				result2.init(Packet::REQ_CHAT);
				result2.flags |= Packet::FLAG_RUDP;
				TagCmd tag;
				tag.command = TagCmd::RESULT2;
				result2.writeData(tag.full);
				for (const Room::result_t& result : results)
					result2.writeData(result.data(), result.size());
				Player::sendToAll(result2, room->getPlayers());
			}
			break;
		}

	case TagCmd::RESET:
		{
			WARN_LOG(game, "tag: RESET from %s", player->getName().c_str());
			Room *room = player->getRoom();
			if (room != nullptr)
			{
				// Send game_over to all players?
				Packet packet;
				packet.init(Packet::REQ_CHAT);
				packet.flags |= Packet::FLAG_RUDP;
				TagCmd tag;
				tag.command = TagCmd::GAME_OVER;
				packet.writeData(tag.full);
				Player::sendToAll(packet, room->getPlayers());
				room->reset();
			}
			break;
		}

	case TagCmd::TIME_OUT:
		WARN_LOG(game, "tag: TIME OUT from %s", player->getName().c_str());
		break;

	default:
		ERROR_LOG(game, "Unhandled tag command: %x (tag %04x)", tag.command, tag.full);
		break;
	}
	return true;
}

Room::Room(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context)
	: lobby(lobby), id(id), name(name), attributes(attributes),
	  owner(owner), timer(io_context), server(lobby.getServer()), game(server.game), timeLimit(io_context)
{
	assert(name.length() <= 16);
	addPlayer(owner);
	openNetdump();
}

Room::~Room() {
	closeNetdump();
	INFO_LOG(game, "Room %s was deleted", name.c_str());
}

void Room::setAttributes(uint32_t attributes)
{
	INFO_LOG(game, "Room %s status set to %08x", name.c_str(), attributes);
	if ((attributes & PLAYING) != 0 && (this->attributes & PLAYING) == 0) {
		// reset when starting a game
		reset();
	}
	else if (roomState == InGame
			&& (attributes & (PLAYING | LOCKED)) == PLAYING
			&& (this->attributes & (PLAYING | LOCKED)) == (PLAYING | LOCKED))
	{
		// Start the time limit timer when the owner unlocks the room
		// time limit at offset 0xd in owner's sysdata
		int limit = TimeLimits[playerState[0].sysdata[0xd] & 0xf];
		std::error_code ec;
		timeLimit.cancel(ec);
		if (limit > 0)
		{
			timeLimit.expires_after(std::chrono::seconds(limit));
			timeLimit.async_wait([this](const std::error_code& ec)
			{
				if (ec)
					return;
				// Send game_over to all players
				INFO_LOG(server.game, "%s: time limit reached", name.c_str());
				sendGameOver();
			});
		}
		// match points at offset 3, point limit flag at offset 2 bit 4
		if (playerState[0].sysdata[2] & 0x10)
			pointLimit = (playerState[0].sysdata[3] >> 2) & 0x3f;
		else
			pointLimit = 0;
		INFO_LOG(server.game, "%s: Game started: time limit %d'%02d point limit %d",
				name.c_str(), limit / 60, limit % 60, pointLimit);
	}
	this->attributes = attributes;
}

void Room::addPlayer(Player *player)
{
	Room *other = player->getRoom();
	if (other != nullptr && other != this) {
		if (other->removePlayer(player))
			other->lobby.removeRoom(other);
	}
	if (getPlayerIndex(player) >= 0)
		return;
	players.push_back(player);
	player->setRoom(this);
	INFO_LOG(game, "%s joined room %s", player->getName().c_str(), name.c_str());
}

bool Room::removePlayer(Player *player)
{
	player->setRoom(nullptr);
	int i = getPlayerIndex(player);
	if (i < 0) {
		ERROR_LOG(server.game, "Player %s to remove not found in the room", player->getName().c_str());
		return false;
	}
	if (roomState == SyncStarted)
	{
		PlayerState& state = getPlayerState(i);
		if (state.state == PlayerState::Ready)
			// Allow the game to start
			rudpAcked(player);
		state.state = PlayerState::Gone;
	}
	else if (roomState == InGame)
	{
		PlayerState& state = getPlayerState(i);
		state.state = PlayerState::Gone;
	}
	INFO_LOG(game, "%s left room %s", player->getName().c_str(), name.c_str());
	players.erase(players.begin() + i);
	if (players.empty())
		return true;

	// Notify other players
	Packet relay;
	relay.init(Packet::REQ_LEAVE_LOBBY_ROOM);
	relay.writeData(player->getId());
	Player::sendToAll(relay, getPlayers());

	if (owner == player)
	{
		// Select new owner and notify him
		owner = players[0];
		Packet packet;
		packet.init(Packet::RSP_TAG_CMD);
		packet.flags |= Packet::FLAG_RUDP;
		packet.writeData(0u);
		TagCmd tag;
		tag.command = TagCmd::OWNER;
		packet.writeData(tag.full);
		INFO_LOG(game, "%s is the new owner of %s", owner->getName().c_str(), name.c_str());

		if (players.size() >= 2)
		{
			// Send START_OK
			packet.init(Packet::RSP_TAG_CMD);
			packet.flags |= Packet::FLAG_RUDP;
			packet.writeData(0u);
			tag.command = TagCmd::START_OK;
			packet.writeData(tag.full);
		}
		owner->send(packet);
	}
	return false;
}

std::vector<Room::sysdata_t> Room::getSysData() const
{
	std::vector<sysdata_t> sysdata;
	sysdata.reserve(playerState.size());
	for (const auto& state : playerState)
		sysdata.push_back(state.sysdata);
	return sysdata;
}

void Room::setSysData(const Player *player, const sysdata_t& sysdata)
{
	int i = getPlayerIndex(player);
	if (i < 0) {
		WARN_LOG(game, "setSysData: player not found in room");
		return;
	}
	PlayerState& state = getPlayerState(i);
	state.sysdata = sysdata;
	state.state = PlayerState::SysData;
}

bool Room::setReady(const Player *player)
{
	int i = getPlayerIndex(player);
	if (i < 0) {
		WARN_LOG(game, "setReady: player not found in room");
		return false;
	}
	PlayerState& thisState = getPlayerState(i);
	thisState.state = PlayerState::Ready;
	for (const auto& state : playerState)
		if (state.state != PlayerState::Ready && state.state != PlayerState::Gone)
			return false;
	return true;
}

int Room::getPlayerIndex(const Player *player)
{
	unsigned i = 0;
	for (const Player *p : players)
	{
		if (p == player)
			break;
		i++;
	}
	if (i == players.size())
		return -1;
	else
		return i;
}

Room::PlayerState& Room::getPlayerState(unsigned index)
{
	for (unsigned i = 0; i < playerState.size(); i++)
	{
		if (playerState[i].state == PlayerState::Gone)
			// ignore players who have left the game
			continue;
		if (index == 0)
			return playerState[i];
		index--;
	}
	abort();
}

void Room::setGameData(const Player *player, const uint8_t *data)
{
	int i = getPlayerIndex(player);
	if (i < 0)
		return;
	PlayerState& state = getPlayerState(i);
	memcpy(state.gamedata.data(), data, state.gamedata.size());
	if (roomState == SyncStarted)
		sendGameData({});
	// 114 seems to be the max score you can have in game.
	// However the correct score is displayed on the result screen.
	if (pointLimit > 0 && data[8] <= 0xf6)
	{
		int score = (int)data[8] / 2 - 9;
		if (score >= pointLimit && roomState == InGame)
		{
			// Send game_over to all players
			INFO_LOG(server.game, "%s: point limit %d reached by %s", name.c_str(), pointLimit, player->getName().c_str());
			sendGameOver();
		}
	}
}

void Room::sendGameData(const std::error_code& ec)
{
	if (ec)
		return;

	Packet packet;
	packet.init(Packet::REQ_CHAT);
	packet.writeData(getNextFrame());
	for (const PlayerState& state : playerState)
		packet.writeData(state.gamedata.data(), state.gamedata.size());
	Player::sendToAll(packet, players);

	// send game data every 66.667 ms (4 frames) like the game does
	if (roomState == SyncStarted) {
		timer.expires_after(66667us);
		roomState = InGame;
	}
	else {
		timer.expires_at(timer.expiry() + 66667us);
	}
	timer.async_wait(std::bind(&Room::sendGameData, this, asio::placeholders::error));
}

void Room::sendGameOver()
{
	Packet packet;
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	TagCmd tag;
	tag.command = TagCmd::GAME_OVER;
	packet.writeData(tag.full);
	Player::sendToAll(packet, players);
	roomState = GameOver;
}

std::vector<Room::result_t> Room::getResults() const
{
	std::vector<result_t> results;
	results.reserve(playerState.size());
	for (const PlayerState& state : playerState)
		results.push_back(state.result);
	return results;
}

bool Room::setResult(const Player *player, const uint8_t *data)
{
	int i = getPlayerIndex(player);
	if (i < 0)
		return false;
	PlayerState& thisState = getPlayerState(i);
	memcpy(thisState.result.data(), data, thisState.result.size());
	thisState.state = PlayerState::Result;
	for (const PlayerState& state : playerState)
		if (state.state != PlayerState::Result && state.state != PlayerState::Gone)
			return false;
	endGame();
	return true;
}

void Room::reset()
{
	playerState.resize(players.size());
	for (auto& state : playerState)
		state.state = PlayerState::Init;
	frameNum = 0;
	roomState = Init;
	std::error_code ec;
	timer.cancel(ec);
	timeLimit.cancel(ec);
}

void Room::startSync()
{
	roomState = SyncStarted;
	for (Player *pl : players)
		pl->notifyRoomOnAck();
}

void Room::endGame()
{
	std::error_code ec;
	timer.cancel(ec);
	timeLimit.cancel(ec);
	roomState = Result;
}

void Room::rudpAcked(Player *player)
{
	int i = getPlayerIndex(player);
	if (i < 0)
		return;
	PlayerState::State& state = getPlayerState(i).state;
	if (state == PlayerState::SysData)
	{
		state = PlayerState::SysOk;
		for (const PlayerState& pstate : playerState)
			if (pstate.state != PlayerState::SysOk)
				return;
		// send SYS2
		INFO_LOG(game, "%s: Sending SYS2 to all players", name.c_str());
		std::vector<Room::sysdata_t> sysdata  = getSysData();
		Packet sys2;
		sys2.init(Packet::RSP_TAG_CMD);
		sys2.flags |= Packet::FLAG_RUDP;
		sys2.writeData(0u);	// list: count [int ...]
		TagCmd tag;
		tag.command = TagCmd::SYS2;
		tag.player = (uint16_t)sysdata.size();
		sys2.writeData(tag.full);
		for (const Room::sysdata_t& data : sysdata)
			sys2.writeData(data.data(), data.size());
		uint16_t userId = 0;
		for (Player *pl : players)
		{
			// notify each player of his position in the game
			tag.id = userId++;
			write16(sys2.data, 0x14, tag.full);
			pl->send(sys2);
		}

		return;
	}

	if (roomState != SyncStarted || state != PlayerState::Ready)
		return;
	state = PlayerState::Started;
	INFO_LOG(game, "%s: GAME_START ack'ed by %s", name.c_str(), player->getName().c_str());
	for (const PlayerState& pstate : playerState)
		if (pstate.state != PlayerState::Started && pstate.state != PlayerState::Gone)
			return;
	// send empty UDP data to owner to kick start the game
	Packet packet;
	packet.init(Packet::REQ_CHAT);
	packet.writeData(0u);	// frame#?
	owner->send(packet);
}

void Room::openNetdump()
{
	time_t now = time(nullptr);
	struct tm tm = *localtime(&now);

	char date[16];
	sprintf(date, "%02d_%02d-%02d-%02d", tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

	std::string fname = std::string(date) + "_" + name + ".dmp";
	std::replace(fname.begin(), fname.end(), '/', '_');
	netdump = fopen(fname.c_str(), "w");
	if (netdump == nullptr)
		WARN_LOG(game, "Can't open netdump file %s: error %d", fname.c_str(), errno);
}

void Room::writeNetdump(const uint8_t *data, uint32_t len, const asio::ip::udp::endpoint& endpoint) const
{
	if (netdump == nullptr)
		return;
	time_t now = std::chrono::time_point_cast<std::chrono::milliseconds>(Clock::now()).time_since_epoch().count();
	fwrite(&now, sizeof(now), 1, netdump);
	std::array<uint8_t, 4> addr = endpoint.address().to_v4().to_bytes();
	fwrite(addr.data(), addr.size(), 1, netdump);
	uint16_t port = endpoint.port();
	fwrite(&port, 2, 1, netdump);
	fwrite(&len, 4, 1, netdump);
	fwrite(data, 1, len, netdump);
}

void Lobby::addPlayer(Player *player)
{
	Lobby *other = player->getLobby();
	if (other != nullptr && other != this)
		other->removePlayer(player);
	player->setLobby(this);
	for (Player *pl : players)
		if (pl == player)
			return;
	players.push_back(player);
	INFO_LOG(server.game, "%s joined lobby %s", player->getName().c_str(), name.c_str());
	// Discord presence
	std::vector<std::string> names;
	names.reserve(players.size());
	for (Player *pl : players)
		if (pl != player)
			names.push_back(pl->getName());
	discordLobbyJoined(server.game, player->getName(), names);
}

void Lobby::removePlayer(Player *player)
{
	Room *room = player->getRoom();
	if (room != nullptr) {
		if (room->removePlayer(player))
			removeRoom(room);
	}
	for (auto it = players.begin(); it != players.end(); ++it)
		if (player == *it)
		{
			INFO_LOG(server.game, "%s left lobby %s", player->getName().c_str(), name.c_str());
			player->setLobby(nullptr);
			players.erase(it);
			break;
		}
	// Notify other players
	Packet relay;
	relay.init(Packet::REQ_LEAVE_LOBBY_ROOM);
	relay.flags |= Packet::FLAG_LOBBY;
	relay.writeData(player->getId());
	Player::sendToAll(relay, players);
}

std::vector<Room *> Lobby::getRooms() const
{
	std::vector<Room *> v;
	v.reserve(rooms.size());
	for (auto [id, room] : rooms)
		v.push_back(room);
	return v;
}

Room *Lobby::getRoom(uint32_t id) const
{
	auto it = rooms.find(id);
	if (it == rooms.end())
		return nullptr;
	else
		return it->second;
}

Room *Lobby::addRoom(const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context)
{
	uint32_t id = nextRoomId++;
	Room *room = new Room(*this, id, name, attributes, owner, io_context);
	rooms[id] = room;
	// Discord presence
	std::vector<std::string> lobbyUsers;
	lobbyUsers.reserve(players.size());
	for (Player *pl : players)
		if (pl != owner)
			lobbyUsers.push_back(pl->getName());
	discordGameCreated(server.game, owner->getName(), name, lobbyUsers);

	return room;
}

void Lobby::removeRoom(Room *room) {
	rooms.erase(room->getId());
	delete room;
}
