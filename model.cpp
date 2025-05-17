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

void Player::setAlive() {
	lastTime = Clock::now();
}

bool Player::timedOut() const
{
	if (room == nullptr)
		return false;
	return Clock::now() - lastTime >= 30s;
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

			Player *player = new Player(*server, source, nextUserId);
			player->setName(name);
			server->addPlayer(player);
			nextUserId++;

			size_t pktsize = packet.finalize(0, tmpUserId);
			std::error_code ec2;
			socket.send_to(asio::buffer(packet.data, pktsize), source, 0, ec2);
			break;
		}

	case Packet::REQ_PING:
		{
			packet.respOK(Packet::REQ_PING);
			packet.writeData(read32(data, 0x10));
			size_t pktsize = packet.finalize(0, read32(data, 4));
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

void LobbyServer::send(Packet& packet, Player& player)
{
	uint32_t sequence;
	if (packet.flags & Packet::FLAG_RUDP)
		sequence = player.getRelSeqAndInc();
	else
		sequence = unrelSeq++;
	size_t pktsize = packet.finalize(sequence, player.getId());
	std::error_code ec2;
	socket.send_to(asio::buffer(packet.data, pktsize), player.getEndpoint(), 0, ec2);
}

void LobbyServer::sendToAll(Packet& packet, const std::vector<Player *>& players, Player *except)
{
	for (Player *pl : players)
		if (pl != except)
			send(packet, *pl);
}

void LobbyServer::handlePacket(const uint8_t *data, size_t len)
{
	auto it = players.find(source);
	if (it == players.end()) {
		WARN_LOG(game, "Packet from unknown endpoint %s:%d ignored", source.address().to_string().c_str(), source.port());
		return;
	}
	Player *player = it->second;
	player->setAlive();
	//printf("Lobby: %s packet: flags/size %02x %02x command %02x %02x\n", player->getName().c_str(), data[0], data[1], data[2], data[3]);
	// Game-specific packet handling
	if (handlePacket(player, data, len))
		return;

	Packet packet;
	switch (data[3])
	{
	case Packet::REQ_LOBBY_LOGIN:	// Only when using 2C response to bootstrap login
		{
			packet.respOK(Packet::REQ_LOBBY_LOGIN);
			packet.ack(read32(data, 8));
			packet.writeData(player->getId());
			break;
		}
	case Packet::REQ_LOBBY_LOGOUT:
		{
			packet.respOK(Packet::REQ_LOBBY_LOGOUT);
			packet.ack(read32(data, 8));
			removePlayer(player);
			player = nullptr;
			break;
		}
	case Packet::REQ_QRY_LOBBIES:
		{
			packet.init(Packet::REQ_QRY_LOBBIES);
			packet.ack(read32(data, 8));
			packet.writeData(0u);
			packet.writeData(0u);
			packet.writeData((uint32_t)lobbies.size());
			for (const Lobby& lobby : lobbies)
			{
				packet.writeData(lobby.getName().c_str(), 0x10);
				packet.writeData(lobby.getPlayerCount());
				packet.writeData(lobby.getRoomCount());
				packet.writeData(lobby.getId());
			}
			break;
		}
	case Packet::REQ_CHG_USER_STATUS:
		{
			player->setStatus(read32(data, 0x10));
			packet.respOK(Packet::REQ_CHG_USER_STATUS);
			packet.ack(read32(data, 8));
			packet.writeData(0u);	// status?
			break;
		}
	case Packet::REQ_QRY_USERS:
		{
			packet.init(Packet::REQ_QRY_USERS);
			packet.ack(read32(data, 8));
			if (data[0] & 0x10)
			{
				// lobby
				packet.flags |= Packet::FLAG_LOBBY;
				packet.writeData(0u);
				packet.writeData(0u);
				uint32_t lobbyId = read32(data, 0x10);
				Lobby *lobby = getLobby(lobbyId);
				if (lobby == nullptr) {
					packet.writeData(0u);	// user count
				}
				else
				{
					packet.writeData(lobby->getPlayerCount());
					for (Player *pl : lobby->getPlayers())
					{
						packet.writeData(pl->getName().c_str(), 0x10);
						packet.writeData(pl->getId());
						packet.writeData(0u);	// controllers
					}
				}
			}
			else
			{
				// room
				packet.writeData(0u);
				packet.writeData(0u);
				int roomId = read32(data, 0x10);
				if (player->getLobby() == nullptr) {
					packet.writeData(0u);	// user count
				}
				else
				{
					Room *room = player->getLobby()->getRoom(roomId);
					if (room == nullptr) {
						packet.writeData(0u);	// user count
					}
					else {
						const std::vector<Player *> players = room->getPlayers();
						packet.writeData(room->getPlayerCount());
						for (Player *pl : players)
						{
							packet.writeData(pl->getName().c_str(), 0x10);
							packet.writeData(pl->getId());
							packet.writeData(0u);	// controllers
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
					packet.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
					WARN_LOG(game, "%s join lobby failed: unknown lobby id %x", player->getName().c_str(), id);
				}
				else
				{
					lobby->addPlayer(player);

					// Notify other players
					Packet relay;
					relay.init(Packet::REQ_JOIN_LOBBY_ROOM);
					relay.flags |= Packet::FLAG_LOBBY;
					relay.writeData(player->getName().c_str(), 0x10);
					relay.writeData(player->getId());
					relay.writeData(0u);	// controllers
					sendToAll(relay, lobby->getPlayers(), player);

					packet.respOK(Packet::REQ_JOIN_LOBBY_ROOM);
					packet.writeData(lobby->getId());
				}
				packet.flags |= Packet::FLAG_LOBBY;
				packet.ack(read32(data, 8));
			}
			else
			{
				// room
				Lobby *lobby = player->getLobby();
				Room *room = lobby == nullptr ? nullptr : lobby->getRoom(id);
				if (room == nullptr) {
					packet.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
					WARN_LOG(game, "%s join room failed: unknown room id %x (lobby %p)", player->getName().c_str(), id, lobby);
				}
				else
				{
					room->addPlayer(player);

					// Notify other players
					Packet relay;
					relay.init(Packet::REQ_JOIN_LOBBY_ROOM);
					relay.writeData(player->getName().c_str(), 0x10);
					relay.writeData(player->getId());
					relay.writeData(0u);	// controllers
					sendToAll(relay, room->getPlayers(), player);

					packet.respOK(Packet::REQ_JOIN_LOBBY_ROOM);
					packet.writeData(room->getId());
					packet.ack(read32(data, 8));
					packet.finalize(unrelSeq++, player->getId());

					// Push room status to new player
					packet.append(Packet::REQ_CHG_ROOM_STATUS);
					packet.writeData(room->getId());
					packet.writeData("STAT", 4);
					packet.writeData(room->getAttributes());
				}
			}
			break;
		}
	case Packet::REQ_LEAVE_LOBBY_ROOM:
		{
			if (data[0] & 0x10)
			{
				// lobby
				packet.respOK(Packet::REQ_LEAVE_LOBBY_ROOM);
				packet.flags |= Packet::FLAG_LOBBY;
				Lobby *lobby = player->getLobby();
				if (lobby != nullptr)
					lobby->removePlayer(player);
			}
			else
			{
				packet.respOK(Packet::REQ_LEAVE_LOBBY_ROOM);
				Room *room = player->getRoom();
				if (room != nullptr) {
					// Remove player from the room
					if (room->removePlayer(player))
						player->getLobby()->removeRoom(room);
				}
			}
			packet.ack(read32(data, 8));
			break;
		}
	case Packet::REQ_QRY_ROOMS:
		{
			packet.init(Packet::REQ_QRY_ROOMS);
			packet.ack(read32(data, 8));
			packet.flags |= Packet::FLAG_LOBBY;
			int lobbyId = read32(data, 0x10);
			Lobby *lobby = getLobby(lobbyId);
			packet.writeData(0u);	// ?
			packet.writeData(0u);	// ?
			if (lobby == nullptr) {
				packet.writeData(0u);	// room count
			}
			else
			{
				auto rooms = lobby->getRooms();
				packet.writeData((uint32_t)rooms.size());
				for (Room *room : rooms)
				{
					packet.writeData(room->getName().c_str(), 0x10);
					// This is different in outtrigger vs. bomberman
					if (game == Game::Bomberman) {
						packet.writeData(room->getOwner()->getId());
						packet.writeData(room->getPlayerCount());
					}
					else {
						packet.writeData(room->getPlayerCount());
						packet.writeData(room->getOwner()->getId());
					}
					packet.writeData(room->getAttributes());
					packet.writeData(room->getMaxPlayers());
					packet.writeData(room->getId());
				}
			}
			break;
		}
	case Packet::REQ_CREATE_ROOM:
		{
			std::string name = (const char *)&data[0x10];
			uint32_t maxPlayers = read32(data, 0x20);
			uint32_t attributes = read32(data, 0x38);
			if (player->getLobby() == nullptr) {
				packet.respFailed(Packet::REQ_CREATE_ROOM);
				packet.ack(read32(data, 8));
			}
			else
			{
				attributes |= 1;	// server ready?
				Room *room = player->getLobby()->addRoom(name, attributes, player);
				room->setMaxPlayers(maxPlayers);
				/* Should it be sent to other players in lobby?
				packet.init(Packet::REQ_CREATE_ROOM);
				packet.ack(read32(data, 8));
				packet.writeData(name.c_str(), 16);
				packet.writeData(1u); // player count
				packet.writeData(player->getId());
				packet.writeData(attributes);
				packet.writeData(maxPlayers);
				packet.writeData(room->getId());
				*/

				packet.respOK(Packet::REQ_CREATE_ROOM);
				packet.writeData(room->getId());
				packet.ack(read32(data, 8));
				packet.finalize(unrelSeq++, player->getId());

				packet.append(Packet::REQ_CHG_ROOM_STATUS);
				packet.writeData(room->getId());
				packet.writeData("STAT", 4);
				packet.writeData(attributes);

			}
			break;
		}
	case Packet::REQ_CHG_ROOM_STATUS:
		{
			Room *room = player->getRoom();
			if (room == nullptr) {
				packet.respFailed(Packet::REQ_CHG_ROOM_STATUS);
			}
			else
			{
				uint32_t attributes = read32(data, 0x14);
				room->setAttributes(attributes);

				// Notify other users
				Packet relay;
				relay.init(Packet::REQ_CHG_ROOM_STATUS);
				relay.writeData(room->getId());
				relay.writeData("STAT", 4);
				relay.writeData(attributes);
				if (player->getLobby() != nullptr)
					sendToAll(relay, room->getPlayers(), player);

				packet.respOK(Packet::REQ_CHG_ROOM_STATUS);
				packet.writeData(room->getId());
				packet.writeData("STAT", 4);
				packet.writeData(attributes);
			}
			packet.ack(read32(data, 8));
			break;
		}

	case Packet::REQ_CHAT:
		{
			uint16_t flags = read16(data, 0);
			if (flags & Packet::FLAG_RUDP)
			{
				// Broadcast to other players in the lobby/room
				Packet relay;
				relay.init(Packet::REQ_CHAT);
				relay.flags |= (flags & Packet::FLAG_LOBBY) | Packet::FLAG_RELAY;
				relay.writeData(&data[0x10], (flags & 0x3ff) - 0x10);
				if (flags & Packet::FLAG_LOBBY)
				{
					Lobby *lobby = player->getLobby();
					if (lobby != nullptr)
						sendToAll(relay, lobby->getPlayers(), player);
				}
				else
				{
					Room *room = player->getRoom();
					if (room != nullptr)
						sendToAll(relay, room->getPlayers(), player);
				}

				uint32_t seq = read32(data, 8);
				// TODO correct?
				if (seq == 0)
					// don't ack continued chat pkt
					return;
				packet.respOK(Packet::REQ_CHAT);
				packet.ack(seq);
				packet.flags |= flags & Packet::FLAG_LOBBY;
			}
			else
			{
				uint16_t cmd = read16(data, 0x10);
				if (cmd == 0x3804)	// ping
				{
					//bomberman
					INFO_LOG(game, "chat(F) ping %04x %08x %x", read16(data, 0x12), read32(data, 0x14), data[0x18]);
					packet.init(Packet::REQ_CHAT);
					packet.writeData((uint16_t)0x3804u);
					packet.writeData((uint16_t)0x7800u);
					packet.writeData(read32(data, 0x14));
					packet.writeData((uint8_t)0);
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
				packet.respOK(Packet::REQ_PING);
				// outtrigger and propA send a single value (clock)
				packet.writeData(read32(data, 0x10));
				//packet.writeData(&data[0x10], (read16(data, 0) & 0x3ff) - 0x10);
			}
			else {
				// sends 4 ints, not sure what should be returned
				// FIXME bomberman test -> causes chat(F) packets to be sent
				// works: no logged failure (can be forced with seq=-1)
				packet.init(Packet::REQ_PING);
				packet.writeData(&data[0x10], len - 0x10);
			}
			break;
		}

	case Packet::REQ_CHG_USER_PROP:
		packet.respOK(Packet::REQ_CHG_USER_PROP);
		packet.ack(read32(data, 8));
		// TODO parse
		break;

	case Packet::REQ_NOP:
		return;

	default:
		ERROR_LOG(game, "Lobby: Unhandled msg type %x", data[3]);
		return;
	}
	if (player != nullptr)
		send(packet, *player);
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
	{
		player->ackRUdp(read32(data, 0xc));
		if (player->getRoom() != nullptr)
			player->getRoom()->startGame();
	}

	if (data[3] != Packet::REQ_GAME_DATA)
		return false;

	Packet packet;
	TagCmd tag(read16(data, 0x10));
	switch (tag.command)
	{
	case TagCmd::ECHO:
		// called regularly (< 10s) by all players in the room
		//printf("tag: ECHO\n");
		packet.init(Packet::RSP_TAG_CMD);
		packet.writeData(0u);
		packet.writeData(&data[0x10], 4);
		send(packet, *player);
		break;

	case TagCmd::START_OK:
		{
			INFO_LOG(game, "tag: START OK");
			packet.init(Packet::REQ_NOP);
			packet.ack(read32(data, 8));
			send(packet, *player);

			Room *room = player->getRoom();
			if (room != nullptr && room->getPlayerCount() >= 2)
			{
				// start ok
				INFO_LOG(game, "Sending START_OK to owner");
				packet.init(Packet::RSP_TAG_CMD);
				packet.writeData(0u);	// list: count [int ...]
				packet.writeData(tag.full);
				packet.flags |= Packet::FLAG_RUDP;
				send(packet, *room->getOwner());
			}
			break;
		}

	case TagCmd::SYS:
		{
			INFO_LOG(game, "tag: SYS from %s", player->getName().c_str());
			packet.init(Packet::RSP_TAG_CMD);
			packet.ack(read32(data, 8));
			packet.flags |= Packet::FLAG_RUDP;
			packet.writeData(0u);	// list: count [int ...]
			TagCmd tag;
			tag.command = TagCmd::SYS_OK;
			packet.writeData(tag.full);
			send(packet, *player);

			Room *room = player->getRoom();
			if (room != nullptr)
			{
				Room::sysdata_t sysdata;
				memcpy(sysdata.data(), &data[0x12], sysdata.size());
				if (room->setSysData(player, sysdata))
				{
					// send SYS2
					INFO_LOG(game, "%s: Sending SYS2 to all players", room->getName().c_str());
					std::vector<Room::sysdata_t> sysdata  = room->getSysData();
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
					for (Player *pl : room->getPlayers())
					{
						// notify each player of his position in the game
						tag.id = userId++;
						write16(sys2.data, 0x14, tag.full);
						send(sys2, *pl);
					}
				}
			}
			break;
		}

	case TagCmd::READY:
		{
			INFO_LOG(game, "tag: READY from %s", player->getName().c_str());
			packet.init(Packet::REQ_NOP);
			packet.ack(read32(data, 8));
			send(packet, *player);

			Room *room = player->getRoom();
			if (room != nullptr && room->setReady(player))
			{
				// send GAME_START
				INFO_LOG(game, "%s: Sending GAME_START to all players", room->getName().c_str());
				Packet gameStart;
				gameStart.init(Packet::REQ_CHAT);
				gameStart.flags |= Packet::FLAG_RUDP;
				TagCmd tag;
				tag.command = TagCmd::GAME_START;
				gameStart.writeData(tag.full);
				sendToAll(gameStart, room->getPlayers());
				// wait for this packet to be ack'ed by all players before sending game data
				room->startSync();
			}
			break;
		}

	case TagCmd::SYNC:	// actual game data
		{
			//DEBUG_LOG(game, "tag: SYNC from %s", player->getName().c_str());
			if (data[0] & 0x80) {
				// propA sends rel SYNC after creating room
				packet.init(Packet::REQ_NOP);
				packet.ack(read32(data, 8));
				send(packet, *player);
			}
			Room *room = player->getRoom();
			if (room != nullptr)
			{
				Packet packet;
				packet.init(Packet::REQ_CHAT);
				packet.writeData(room->getNextFrame());

				room->setGameData(player, &data[0x12]);
				std::vector<Room::gamedata_t> vec = room->getGameData();
				for (auto& gd : vec)
					packet.writeData(gd.data(), gd.size());
				sendToAll(packet, room->getPlayers(), player);
			}
			break;
		}

	case TagCmd::RESULT:
		{
			INFO_LOG(game, "tag: RESULT from %s", player->getName().c_str());
			packet.init(Packet::REQ_NOP);
			packet.ack(read32(data, 8));
			send(packet, *player);

			Room *room = player->getRoom();
			if (room != nullptr && room->setResult(player, &data[0x12]))
			{
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
				sendToAll(result2, room->getPlayers());
			}
			break;
		}

	case TagCmd::RESET:
		WARN_LOG(game, "tag: RESET from %s", player->getName().c_str());
		// TODO send game_over to all players?
		break;

	case TagCmd::TIME_OUT:
		WARN_LOG(game, "tag: TIME OUT from %s", player->getName().c_str());
		break;

	default:
		ERROR_LOG(game, "Unhandled tag command: %x (tag %04x)", tag.command, tag.full);
		break;
	}
	return true;
}

Room::~Room() {
	closeNetdump();
	INFO_LOG(lobby.getServer().game, "Room %s was deleted", name.c_str());
}

void Room::setAttributes(uint32_t attributes)
{
	INFO_LOG(lobby.getServer().game, "Room %s status set to %08x", name.c_str(), attributes);
	if ((attributes & 0x80000000) != 0 && (this->attributes & 0x80000000) == 0)
		// reset when starting a game
		reset();
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
	INFO_LOG(lobby.getServer().game, "%s joined room %s", player->getName().c_str(), name.c_str());
}

bool Room::removePlayer(Player *player)
{
	player->setRoom(nullptr);
	for (auto it = players.begin(); it != players.end(); ++it)
		if (player == *it) {
			INFO_LOG(lobby.getServer().game, "%s left room %s", player->getName().c_str(), name.c_str());
			players.erase(it);
			break;
		}
	if (players.empty())
		return true;
	// Notify other players
	Packet relay;
	relay.init(Packet::REQ_LEAVE_LOBBY_ROOM);
	relay.writeData(player->getId());
	lobby.getServer().sendToAll(relay, getPlayers());

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
		lobby.getServer().send(packet, *owner);
		INFO_LOG(lobby.getServer().game, "%s is the new owner of %s", owner->getName().c_str(), name.c_str());
	}
	return false;
}

std::vector<Room::sysdata_t> Room::getSysData() const
{
	std::vector<sysdata_t> sysdata;
	sysdata.reserve(sysData.size());
	for (const auto& [status, data] : sysData)
		sysdata.push_back(data);
	return sysdata;
}

bool Room::setSysData(const Player *player, const sysdata_t& sysdata)
{
	sysData.resize(players.size());
	int i = getPlayerIndex(player);
	if (i < 0) {
		WARN_LOG(lobby.getServer().game, "setSysData: player not found in room");
		return false;
	}
	sysData[i] = std::make_pair(true, sysdata);
	for (const auto& [status, data] : sysData)
		if (!status)
			return false;
	return true;
}

bool Room::setReady(const Player *player)
{
	ready.resize(players.size());
	int i = getPlayerIndex(player);
	if (i < 0) {
		WARN_LOG(lobby.getServer().game, "setReady: player not found in room");
		return false;
	}
	ready[i] = true;
	for (bool r : ready)
		if (!r)
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

void Room::setGameData(const Player *player, const uint8_t *data)
{
	gameData.resize(players.size());
	int i = getPlayerIndex(player);
	if (i < 0)
		return;
	gamedata_t gamedata;
	memcpy(gamedata.data(), data, gamedata.size());
	gameData[i] = gamedata;
}

std::vector<Room::result_t> Room::getResults() const
{
	std::vector<result_t> results;
	results.reserve(this->results.size());
	for (const auto& [status, result] : this->results)
		results.push_back(result);
	return results;
}

bool Room::setResult(const Player *player, const uint8_t *data)
{
	results.resize(players.size());
	int i = getPlayerIndex(player);
	if (i < 0)
		return false;
	result_t result;
	memcpy(result.data(), data, result.size());
	results[i] = std::make_pair(true, result);
	for (const auto& [status, result] : results)
		if (!status)
			return false;
	return true;
}

void Room::reset()
{
	for (auto& [status, data] : sysData)
		status = false;
	ready.clear();
	for (auto& [status, data] : results)
		status = false;
	frameNum = 0;
	syncStarted = false;
}

void Room::startSync()
{
	syncStarted = true;
	for (Player *pl : players)
		pl->startSync();
}

void Room::startGame()
{
	if (!syncStarted)
		return;
	for (Player *pl : players) {
		if (!pl->readyToStart())
			return;
	}
	// send empty UDP data to owner to kick start the game
	Packet packet;
	packet.init(Packet::REQ_CHAT);
	packet.writeData(0u);	// frame#?
	lobby.getServer().send(packet, *owner);
	syncStarted = false;
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
		WARN_LOG(lobby.getServer().game, "Can't open netdump file %s: error %d", fname.c_str(), errno);
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
	server.sendToAll(relay, players);
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

Room *Lobby::addRoom(const std::string& name, uint32_t attributes, Player *owner)
{
	uint32_t id = nextRoomId++;
	Room *room = new Room(*this, id, name, attributes, owner);
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
