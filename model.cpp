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
#include <dcserver/status.hpp>
#include <algorithm>
#include <cctype>

using namespace std::chrono_literals;

Player::~Player() {
	std::error_code ec;
	timer.cancel(ec);
}

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
		if (!(flags & Packet::FLAG_RELAY))
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
	if (sendCount >= 4)
	{
		WARN_LOG(server.game, "Sending packet %x to %s failed after %d attempts (ping %d)",
				lastRelPacket.data[3], name.c_str(), sendCount, (int)ping);
		ackedRelSeq++;
		if (!relQueue.empty()) {
			sendRel(relQueue.front().second, relQueue.front().first);
			relQueue.pop_front();
		}
		return;
	}
	sendCount++;
	server.send(lastRelPacket, getEndpoint());
	lastRUdpSend = Clock::now();
	timer.expires_after(std::chrono::milliseconds((int)ping) + sendCount * 200ms);
	// game (bba) apparently retries after 100 ms, 200 ms, 400 ms, 800 ms then timeout
	// propeller arena: 200 ms, 400 ms, 600 ms, 800 ms, timeout
	timer.async_wait(std::bind(&Player::resendTimer, this, asio::placeholders::error));
}

void Player::ackRUdp(uint32_t seq)
{
	if (ackedRelSeq >= (int)seq)
		// already ack'ed
		return;
	ackedRelSeq = seq;
	std::error_code ec;
	timer.cancel(ec);
	ping = ping * 0.5f + (Clock::now() - lastRUdpSend) / 1.0ms * 0.5f;
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

void Player::ackPacket(Packet& outPacket, const uint8_t *inPacket)
{
	if ((read16(inPacket, 0) & Packet::FLAG_RUDP) == 0)
		// Not an RUdp packet
		return;
	uint32_t seq = read32(inPacket, 8);
	if (seq != 0 || ackedClientSeq < (int)seq)
	{
		outPacket.ack(seq);
		if ((int)seq > ackedClientSeq)
			ackedClientSeq = seq;
	}
}

bool Player::packetAcked(const uint8_t *packet)
{
	if ((read16(packet, 0) & Packet::FLAG_RUDP) == 0)
		return false;
	uint32_t seq = read32(packet, 8);
	return (int)seq <= ackedClientSeq;
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

LobbyServer::LobbyServer(Game game, uint16_t port, asio::io_context& io_context)
	: Server(port, io_context), game(game), timer(io_context)

{
	lobbies.reserve(10);
	addLobby("DCNet");
	startTimer();
}

void LobbyServer::startTimer()
{
	timer.expires_after(30s);
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
	status::leave(getDCNetGameId(game), player->getEndpoint().address().to_string(),
			player->getEndpoint().port(), player->getName());
	delete player;
}

void LobbyServer::send(Packet& packet, const asio::ip::udp::endpoint& endpoint)
{
	size_t pktsize = packet.finalize();
	std::error_code ec;
	socket.send_to(asio::buffer(packet.data, pktsize), endpoint, 0, ec);
	if (ec)
		WARN_LOG(game, "send to %s:%d failed: %s", endpoint.address().to_string().c_str(), endpoint.port(), ec.message().c_str());
}

static inline void strtolower(std::string& str) {
	std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c){ return std::tolower(c); });
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
	// Check if we have handled this RUdp packet already
	if (read16(data, 0) & Packet::FLAG_RUDP)
	{
		if (rudpIgnore)
			return;
		if (!rudpSeen)
		{
			rudpSeen = true;
			if (player->packetAcked(data))
			{
				INFO_LOG(game, "[%s] RUdp packet %x already handled. Ignoring", player->getName().c_str(), data[3]);
				replyPacket.init(Packet::REQ_NOP);
				uint32_t seq = read32(data, 8);
				replyPacket.ack(seq);
				rudpIgnore = true;
				return;
			}
		}
	}

	// Game-specific packet handling
	if (handlePacket(player, data, len))
		return;

	switch (data[3])
	{
	case Packet::REQ_LOBBY_LOGIN:	// Only when using 2C response to bootstrap login
		{
			DEBUG_LOG(game, "REQ_LOBBY_LOGIN");
			//dumpData(data + 0x10, len - 0x10);
			player->setName((const char *)&data[0x20]);
			player->setExtraData(&data[0x138], read32(data, 0x14));
			status::join(getDCNetGameId(game), player->getEndpoint().address().to_string(),
					player->getEndpoint().port(), player->getName());

			replyPacket.init(Packet::RSP_LOGIN_SUCCESS2);
			replyPacket.writeData((uint32_t)socket.local_endpoint().port());
			replyPacket.writeData(0u);	// ? set to 1 by BM
			replyPacket.writeData(player->getId());
			break;
		}
	case Packet::REQ_LOBBY_LOGOUT:
		{
			replyPacket.respOK(Packet::REQ_LOBBY_LOGOUT);
			player->ackPacket(replyPacket, data);
			player->send(replyPacket);
			removePlayer(player);
			player = nullptr;
			break;
		}
	case Packet::REQ_QRY_LOBBIES:
		{
			replyPacket.init(Packet::REQ_QRY_LOBBIES);
			player->ackPacket(replyPacket, data);
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
			uint32_t status = read32(data, 0x10);
			DEBUG_LOG(game, "REQ_CHG_USER_STATUS %x", status);
			player->setStatus(status);
			replyPacket.respOK(Packet::REQ_CHG_USER_STATUS);
			player->ackPacket(replyPacket, data);
			replyPacket.writeData(0u);	// status?
			break;
		}
	case Packet::REQ_QRY_USERS:
		{
			replyPacket.init(Packet::REQ_QRY_USERS);
			player->ackPacket(replyPacket, data);
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
						const auto& extra = pl->getExtraData();
						replyPacket.writeData((uint32_t)extra.size());
						replyPacket.writeData(extra.data(), extra.size());
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
					else
					{
						const std::vector<Player *>& players = room->getPlayers();
						replyPacket.writeData((uint32_t)players.size());
						for (Player *pl : players)
						{
							replyPacket.writeData(pl->getName().c_str(), 0x10);
							replyPacket.writeData(pl->getId());
							const auto& extra = pl->getExtraData();
							replyPacket.writeData((uint32_t)extra.size());
							replyPacket.writeData(extra.data(), extra.size());
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
				if (lobby == nullptr)
				{
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
					const auto& extra = player->getExtraData();
					relayPacket.writeData((uint32_t)extra.size());
					relayPacket.writeData(extra.data(), extra.size());

					replyPacket.respOK(Packet::REQ_JOIN_LOBBY_ROOM);
					replyPacket.writeData(lobby->getId());
				}
				replyPacket.flags |= Packet::FLAG_LOBBY;
				player->ackPacket(replyPacket, data);
			}
			else
			{
				// room
				Lobby *lobby = player->getLobby();
				Room *room = lobby == nullptr ? nullptr : lobby->getRoom(id);
				if (room == nullptr)
				{
					replyPacket.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
					player->ackPacket(replyPacket, data);
					replyPacket.writeData(8u);
					WARN_LOG(game, "%s join room failed: unknown room id %x (lobby %p)", player->getName().c_str(), id, lobby);
					break;
				}
				if (room->getAttributes() & (Room::LOCKED | Room::PLAYING))
				{
					// Room locked or in game
					replyPacket.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
					player->ackPacket(replyPacket, data);
					// 9 is "room locked"
					replyPacket.writeData(9u);
					INFO_LOG(game, "%s join room failed: room locked", player->getName().c_str());
					break;
				}
				std::string password = (const char *)&data[0x18];
				if (password != room->getPassword())
				{
					replyPacket.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
					player->ackPacket(replyPacket, data);
					// 0xF is incorrect password
					replyPacket.writeData(0xfu);
					INFO_LOG(game, "%s join room failed: incorrect password", player->getName().c_str());
					break;
				}
				// TODO not enough for Bomberman if guests > 0
				if (room->getPlayerCount() >= room->getMaxPlayers())
				{
					replyPacket.respFailed(Packet::REQ_JOIN_LOBBY_ROOM);
					player->ackPacket(replyPacket, data);
					replyPacket.writeData(8u);
					WARN_LOG(game, "%s join room failed: room %s full", player->getName().c_str(), room->getName().c_str());
					break;
				}
				room->addPlayer(player);

				// Notify other players
				relayPacket.init(Packet::REQ_JOIN_LOBBY_ROOM);
				relayPacket.writeData(player->getName().c_str(), 0x10);
				relayPacket.writeData(player->getId());
				const auto& extra = player->getExtraData();
				relayPacket.writeData((uint32_t)extra.size());
				relayPacket.writeData(extra.data(), extra.size());

				replyPacket.respOK(Packet::REQ_JOIN_LOBBY_ROOM);
				replyPacket.writeData(room->getId());
				player->ackPacket(replyPacket, data);

				room->createJoinRoomReply(replyPacket, relayPacket, player);
			}
			break;
		}
	case Packet::REQ_LEAVE_LOBBY_ROOM:
		{
			if (data[0] & 0x10)
			{
				// lobby
				Lobby *lobby = player->getLobby();
				if (lobby != nullptr)
				{
					lobby->removePlayer(player);
					replyPacket.respOK(Packet::REQ_LEAVE_LOBBY_ROOM);
					replyPacket.writeData(lobby->getId());
				}
				else {
					replyPacket.respFailed(Packet::REQ_LEAVE_LOBBY_ROOM);
				}
				replyPacket.flags |= Packet::FLAG_LOBBY;
			}
			else
			{
				Room *room = player->getRoom();
				if (room != nullptr)
				{
					replyPacket.respOK(Packet::REQ_LEAVE_LOBBY_ROOM);
					replyPacket.writeData(room->getId());
					// Remove player from the room
					if (room->removePlayer(player))
						player->getLobby()->removeRoom(room);
				}
				else {
					replyPacket.respFailed(Packet::REQ_LEAVE_LOBBY_ROOM);
				}
			}
			player->ackPacket(replyPacket, data);

			relayPacket.init(Packet::REQ_LEAVE_LOBBY_ROOM);
			relayPacket.flags |= Packet::FLAG_RUDP;
			if (data[0] & 0x10)
				relayPacket.flags |= Packet::FLAG_LOBBY;
			relayPacket.writeData(player->getId());
			break;
		}
	case Packet::REQ_QRY_ROOMS:
		{
			replyPacket.init(Packet::REQ_QRY_ROOMS);
			player->ackPacket(replyPacket, data);
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
				player->ackPacket(replyPacket, data);
			}
			else
			{
				attributes |= Room::SERVER_READY;
				Room *room = addRoom(name, attributes, player);
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
				player->ackPacket(replyPacket, data);

				replyPacket.init(Packet::REQ_CHG_ROOM_ATTR);
				replyPacket.writeData(room->getId());
				replyPacket.writeData("STAT", 4);
				replyPacket.writeData(attributes);

				room->createJoinRoomReply(replyPacket, relayPacket, player);	// FIXME separate lobby from room players
			}
			break;
		}
	case Packet::REQ_CHG_ROOM_ATTR:
		{
			Room *room = player->getRoom();
			if (room == nullptr) {
				replyPacket.respFailed(Packet::REQ_CHG_ROOM_ATTR);
			}
			else
			{
				if (!memcmp(&data[0x10], "STAT", 4))
				{
					uint32_t attributes = read32(data, 0x14);
					room->setAttributes(attributes);
				}
				else if (!memcmp(&data[0x10], "NAME", 4))
				{
					std::string name = (const char *)&data[0x14];
					room->setName(name);
				}
				else if (!memcmp(&data[0x10], "MAXI", 4))
				{
					uint32_t maxPlayers = read32(data, 0x14);
					room->setMaxPlayers(maxPlayers);
				}
				else
				{
					ERROR_LOG(game, "CHG_ROOM_ATTR kind not implemented: %.4s", &data[0x10]);
					replyPacket.respFailed(Packet::REQ_CHG_ROOM_ATTR);
					player->ackPacket(replyPacket, data);
					break;
				}
				// Notify other users
				relayPacket.init(Packet::REQ_CHG_ROOM_ATTR);
				relayPacket.writeData(room->getId());
				relayPacket.writeData(&data[0x10], 4);
				if (!memcmp(&data[0x10], "NAME", 4))
					relayPacket.writeData(&data[0x14], 0x10);
				else
					relayPacket.writeData(&data[0x14], 4);

				replyPacket.respOK(Packet::REQ_CHG_ROOM_ATTR);
				replyPacket.writeData(room->getId());
				replyPacket.writeData(&data[0x10], 4);
				if (!memcmp(&data[0x10], "NAME", 4))
					replyPacket.writeData(&data[0x14], 0x10);
				else
					replyPacket.writeData(&data[0x14], 4);
			}
			player->ackPacket(replyPacket, data);
			break;
		}

	case Packet::REQ_CHAT:
		{
			uint16_t flags = read16(data, 0);
			if (flags & Packet::FLAG_RUDP)
			{
				if (flags & Packet::FLAG_RELAY)
				{
					DEBUG_LOG(game, "[%s] CHAT: %s", player->getName().c_str(), &data[0x10]);
					// Broadcast to other players in the lobby/room
					relayPacket.init(Packet::REQ_CHAT);
					relayPacket.flags |= Packet::FLAG_RUDP | (flags & Packet::FLAG_LOBBY);
					relayPacket.relay(player->getId());
					// message
					relayPacket.writeData(&data[0x10], (flags & 0x3ff) - 0x10);

					uint32_t seq = read32(data, 8);
					// TODO correct?
					if (seq == 0)
						// don't ack continued chat pkt
						break;
					replyPacket.respOK(Packet::REQ_CHAT);
					player->ackPacket(replyPacket, data);
					replyPacket.flags |= flags & Packet::FLAG_LOBBY;
				}
				else {
					INFO_LOG(game, "Non-relayed chat(F) ignored");
				}
			}
			else {
				INFO_LOG(game, "unreliable chat(F) ignored");
			}
			break;
		}

	case Packet::REQ_DM_CHAT:
		{
			// a4 1c 00 10 00 00 10 01 00 00 00 14 00 00 00 00 ................
			//             sender
			// 00 00 00 01 00 00 10 02 41 42 43 00             ........ABC.
			// dest count  dest[0]...  message
			uint16_t flags = read16(data, 0);
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);

			const std::vector<Player *> *players = nullptr;
			if (flags & Packet::FLAG_LOBBY)
			{
				const Lobby *lobby = player->getLobby();
				if (lobby != nullptr)
					players = &lobby->getPlayers();
			}
			else
			{
				const Room *room = player->getRoom();
				if (room != nullptr)
					players = &room->getPlayers();
			}
			const int destCount = read32(data, 0x10);
			DEBUG_LOG(game, "[%s] DM_CHAT: %s", player->getName().c_str(), &data[0x10 + (destCount + 1) * 4]);
			if (players != nullptr)
			{
				for (int i = 0; i < destCount; i++)
				{
					const uint32_t dest = read32(data, 0x14 + i * sizeof(uint32_t));
					for (Player *destPlayer : *players)
					{
						if (destPlayer->getId() == dest)
						{
							Packet packet;
							packet.init(Packet::REQ_DM_CHAT);
							packet.flags |= Packet::FLAG_RUDP | (flags & Packet::FLAG_LOBBY);
							packet.relay(player->getId());
							// message
							packet.writeData(data + 0x10, len - 0x10);
							destPlayer->send(packet);
							break;
						}
					}
				}
			}
			break;
		}

	case Packet::REQ_PING:
		//DEBUG_LOG(game, "REQ_PING");
		// outtrigger and propA send a single value (clock)
		// bomberman sends additional stuff but only cares about the first int32 in the response
		replyPacket.respOK(Packet::REQ_PING);
		replyPacket.writeData(&data[0x10], len - 0x10);
		break;

	case Packet::REQ_CHG_USER_PROP:
		DEBUG_LOG(game, "REQ_CHG_USER_PROP");
		//dumpData(data + 0x10, len - 0x10);
		player->setExtraData(data + 0x10, len - 0x10);
		replyPacket.respOK(Packet::REQ_CHG_USER_PROP);
		player->ackPacket(replyPacket, data);
		replyPacket.data[2] = data[2]; // propeller arena needs this (only checked for REQ_CHG_USER_PROP)
		{
			relayPacket.init(Packet::REQ_CHG_USER_PROP);
			relayPacket.flags |= Packet::FLAG_RUDP;
			relayPacket.data[2] = data[2];
			relayPacket.writeData(player->getId());
			const auto& extra = player->getExtraData();
			relayPacket.writeData((uint32_t)extra.size());
			relayPacket.writeData(extra.data(), extra.size());
		}
		break;

	case Packet::REQ_QRY_ROOM_ATTR:
		{
			uint32_t roomId = read32(data, 0x10);
			uint32_t attr = read32(data, 0x14);
			DEBUG_LOG(game, "REG_QRY_ROOM_ATTR %x %c%c%c%c", roomId, attr >> 24, (attr >> 16) & 0xff, (attr >> 8) & 0xff, attr & 0xff);
			Room *room = player->getRoom();
			if (room == nullptr) {
				replyPacket.respFailed(Packet::REQ_QRY_ROOM_ATTR);
			}
			else
			{
				replyPacket.init(Packet::REQ_QRY_ROOM_ATTR);
				replyPacket.writeData(roomId);
				replyPacket.writeData(attr);
				switch (attr)
				{
				case 0x4E414D45:	// NAME
					replyPacket.writeData(room->getName().c_str(), 0x10);
					break;
				case 0x4D415849:	// MAXI
					replyPacket.writeData(room->getMaxPlayers());
					break;
				case 0x53544154:	// STAT
					replyPacket.writeData(room->getAttributes());
					break;
				case 0x55534552:	// USER
					replyPacket.writeData(room->getPlayerCount());
					break;
				default:
					replyPacket.reset();
					replyPacket.respFailed(Packet::REQ_QRY_ROOM_ATTR);
					break;
				}
			}
			player->ackPacket(replyPacket, data);
			break;
		}

	case Packet::REQ_SEARCH_USERS:
		{
			// 0000   b0 28 00 25 00 00 10 02 00 00 00 07 00 00 00 00   .(.%............
			// 0010   48 4f 4d 45 00 00 00 00 00 00 00 00 00 00 00 00   HOME............
			//        searched name
			// 0020   00 00 00 04 00 00 00 06 ba 47 66 10               .........Gf.
			//        length      constant
			// expects:
			// u32		(ignored)
			// u32		error code (0: success)
			// u32		player count
			// {
			//  char[16]	player name
			//  u32			player id
			//  u32			lobby id
			//  u32			room id or 0
			//  u32			extra data size
			//  [...		extra data] (not used)
			// }
			std::string name(&data[0x10], &data[0x20]);
			name.resize(read32(data, 0x20));
			strtolower(name);
			DEBUG_LOG(game, "[%s] REQ_SEARCH_USERS: %s", player->getName().c_str(), name.c_str());
			replyPacket.init(Packet::REQ_SEARCH_USERS);
			if (data[0] & 0x10)
				replyPacket.flags |= Packet::FLAG_LOBBY;
			player->ackPacket(replyPacket, data);
			replyPacket.writeData(0u);
			replyPacket.writeData(0u);
			uint16_t countOffset = replyPacket.size;
			replyPacket.writeData(0u); // will get updated
			int count = 0;
			for (const auto& [endpoint, pl] : players)
			{
				if (pl == player)
					continue;
				std::string pname = pl->getName().substr(0, name.size());
				strtolower(pname);
				if (pname == name)
				{
					replyPacket.writeData(pl->getName().c_str(), 0x10);
					replyPacket.writeData(pl->getId());
					replyPacket.writeData(pl->getLobby() != nullptr ? pl->getLobby()->getId() : 0u);
					replyPacket.writeData(pl->getRoom() != nullptr ? pl->getRoom()->getId() : 0u);
					const auto& extra = player->getExtraData();
					replyPacket.writeData((uint32_t)extra.size());
					replyPacket.writeData(extra.data(), extra.size());
					count++;
				}
			}
			write32(replyPacket.data, countOffset, count);
			break;
		}

	case Packet::REQ_NOP:
		break;

	default:
		{
			ERROR_LOG(game, "Lobby: Unhandled msg type %x", data[3]);
			uint16_t flags = read16(data, 0);
			if (flags & Packet::FLAG_RUDP) {
				replyPacket.init(Packet::REQ_NOP);
				player->ackPacket(replyPacket, data);
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
	rudpSeen = false;
	rudpIgnore = false;
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

Room *LobbyServer::addRoom(const std::string& name, uint32_t attributes, Player *owner)
{
	uint32_t id = nextRoomId++;
	Room *room = new Room(*owner->getLobby(), id, name, attributes, owner, io_context);
	owner->getLobby()->addRoom(room);

	return room;
}

bool Room::DumpNetData = false;

Room::Room(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context)
	: lobby(lobby), id(id), name(name), attributes(attributes),
	  owner(owner), server(lobby.getServer()), game(server.game)
{
	assert(name.length() <= 16);
	addPlayer(owner);	// FIXME addPlayer is virtual. can't be called in constructor/destructor
	openNetdump();
}

Room::~Room() {
	closeNetdump();
	INFO_LOG(game, "Room %s was deleted", name.c_str());
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
	INFO_LOG(game, "%s joined room %s (ping %d)", player->getName().c_str(), name.c_str(), (int)player->getPing());
}

bool Room::removePlayer(Player *player)
{
	player->setRoom(nullptr);
	int i = getPlayerIndex(player);
	if (i < 0) {
		ERROR_LOG(server.game, "Player %s to remove not found in the room", player->getName().c_str());
		return false;
	}
	onRemovePlayer(player, i);

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
		// Select new owner
		owner = players[0];
		INFO_LOG(game, "%s is the new owner of %s", owner->getName().c_str(), name.c_str());
	}
	return false;
}

int Room::getPlayerIndex(const Player *player) const
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

void Room::createJoinRoomReply(Packet& reply, Packet& relay, Player *player)
{
	if (Room *room = player->getRoom())
	{
		// Push room status to new player
		reply.init(Packet::REQ_CHG_ROOM_ATTR);
		reply.writeData(room->getId());
		reply.writeData("STAT", 4);
		reply.writeData(room->getAttributes());
	}
}

void Room::openNetdump()
{
	if (!DumpNetData)
			return;
	time_t now = time(nullptr);
	struct tm tm = *localtime(&now);

	const char *gameId = "";
	switch (game)
	{
	case Game::Bomberman:
		gameId = "BM";
		break;
	case Game::Outtrigger:
		gameId = "OT";
		break;
	case Game::PropellerA:
		gameId = "PA";
		break;
	default:
		gameId = "";
		break;
	}
	char date[32];
	sprintf(date, "%02d_%02d-%02d-%02d_%s_", tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, gameId);

	std::string fname = std::string(date) + name + ".dmp";
	std::replace(fname.begin(), fname.end(), '/', '_');
	fname = DataDir + "/" + fname;
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
void Lobby::addRoom(Room *room)
{
	rooms[room->getId()] = room;
	// Discord presence
	std::vector<std::string> lobbyUsers;
	lobbyUsers.reserve(players.size());
	Player *owner = room->getOwner();
	for (Player *pl : players)
		if (pl != owner)
			lobbyUsers.push_back(pl->getName());
	discordGameCreated(server.game, owner->getName(), room->getName(), lobbyUsers);
	status::createGame(getDCNetGameId(server.game));

}

void Lobby::removeRoom(Room *room)
{
	status::deleteGame(getDCNetGameId(server.game));
	rooms.erase(room->getId());
	delete room;
}
