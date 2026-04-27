/*
	Kage game server.
	Copyright 2019 Shuouma <dreamcast-talk.com>
    Copyright 2026 Flyinghead <flyinghead.github@gmail.com>
    Copyright 2026 Larkus / LikeAGFeld

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
#include "bomberman.h"
#include "log.h"
#include <numeric>

using namespace std::chrono_literals;

BMRoom::BMRoom(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context)
	: Room(lobby, id, name, attributes, owner, io_context), timer(io_context)
{
	// needed after the owner is added in the parent constructor
	updateSlots();
	// TODO add resetRoom()
	brickMap.fill(0xff);
}

void BMRoom::addPlayer(Player *player) {
	Room::addPlayer(player);
	updateSlots();
}

bool BMRoom::removePlayer(Player *player)
{
	bool wasOwner = player == owner;
	bool b = Room::removePlayer(player);
	if (!b)
	{
		// TODO need to update states
		updateSlots();
		for (Player *pl : players)
		{
			Packet packet;
			sendRosterList(packet);
			pl->send(packet);
		}
		if (wasOwner)
			broadcastKeyholder();
	}
	return b;
}

uint32_t BMRoom::getPlayerCount() const {
	return std::accumulate(slots.begin(), slots.end(), 0);
}

void BMRoom::sendRosterList(Packet& packet)
{
	BMCmd cmd {};
	cmd.command = BMCmd::ROSTER_LIST;
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);			// flag?

	packet.writeData(getHostCount());
	for (const Player *pl : players)
	{
		packet.writeData(pl->getId());		// player kage id
		uint32_t slots = (uint32_t)getSlotCount(pl);
		packet.writeData(slots);				// guest+1 count
		uint32_t pos = (uint32_t)getPlayerPosition(pl);
		for (unsigned i = 0; i < slots; i++)
			// playerId [0-7]
			packet.writeData(pos + i);		// FIXME different from udp_8 but looks better.
	}
}

// owner: needs ROOM_JOIN only at creation
//        needs ROSTER_LIST when player joins
// joiner: needs ROOM_JOIN
//         and send ROSTER_LIST to all
void BMRoom::createJoinRoomReply(Packet& reply, Packet& relay, Player *player)
{
	player->send(reply);
	reply.reset();

	Room::createJoinRoomReply(reply, relay, player);
	reply.init(Packet::REQ_CHG_ROOM_ATTR);
	reply.writeData(id);
	reply.writeData("USER", 4);
	reply.writeData(getPlayerCount());

	relay.init(Packet::REQ_CHG_ROOM_ATTR);
	relay.writeData(id);
	relay.writeData("USER", 4);
	relay.writeData(getPlayerCount());
	Player::sendToAll(relay, players, player);
	relay.reset();

	player->send(reply);
	reply.reset();

	const int playerIndex = getPlayerIndex(player);
	if (players.size() > 1)
	{
		states[playerIndex].joining = true;
		player->notifyRoomOnAck();
	}
	else {
		states[playerIndex].joining = false;
	}
	states[playerIndex].rulesAccepted = false;
	states[playerIndex].mapInfoSent = false;

	BMCmd cmd{};
	cmd.command = BMCmd::ROOM_JOIN;
	reply.init(Packet::REQ_CHAT);
	reply.flags |= Packet::FLAG_RUDP;
	reply.writeData(cmd.full);
	reply.writeData((uint16_t)0);			// flag?
	reply.writeData(player->getId());		// player kage id [914]
	reply.writeData((uint32_t)getPlayerIndex(player));		// FIXME [915] [0-F]? client id?
	uint32_t pos = (uint32_t)getPlayerPosition(player);
	reply.writeData(pos); 					// player pos [916]
	uint32_t slots = getSlotCount(player);
	reply.writeData(slots - 1);				// guest count [911]
	reply.writeData(owner->getId());		// room owner kage id [912]
	reply.writeData((uint32_t)getPlayerPosition(owner)); // room owner player pos [913]
	// for each guest: -1 or guest pos (1 - 7)
	for (unsigned i = 1; i < slots; i++)
		reply.writeData(++pos);

	player->send(reply);
	reply.reset();
	// Fixes the '' is the new room owner when joining
	reply.init(Packet::REQ_QRY_USERS);
	reply.writeData(0u);
	reply.writeData(0u);
	Room *room = player->getRoom();
	if (room == nullptr) {
		reply.writeData(0u);	// user count
	}
	else
	{
		const std::vector<Player *>& players = room->getPlayers();
		reply.writeData((uint32_t)players.size());
		for (Player *pl : players)
		{
			reply.writeData(pl->getName().c_str(), 0x10);
			reply.writeData(pl->getId());
			const auto& extra = pl->getExtraData();
			reply.writeData((uint32_t)extra.size());
			reply.writeData(extra.data(), extra.size());
		}
	}
	player->send(reply);
	reply.reset();

	// not needed for owner at creation
	if (player != owner)
		sendRosterList(reply);

	sendRosterList(relay);
}

void BMRoom::broadcastKeyholder() const
{

	Packet packet;
	BMCmd cmd{};
	cmd.command = BMCmd::NEW_MASTER;
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);
	packet.writeData(owner->getId());		// room owner kage id
	packet.writeData((uint32_t)getPlayerPosition(owner)); // room owner player pos
	Player::sendToAll(packet, players);
	DEBUG_LOG(Game::Bomberman, "broadcastKeyholder: owner %s", owner->getName().c_str());
}

// return a slot mask of all players (including guests) that have accepted the rules
uint8_t BMRoom::getReadySlotMask() const
{
	uint8_t mask = 0;
	for (const Player *player : players)
	{
		int index = getPlayerIndex(player);
		if (!states[index].rulesAccepted)
			continue;
		mask |= getSlotMask(player);
	}
	return mask;
}

uint8_t BMRoom::getSlotMask(const Player *player) const
{
	const int pos = getPlayerPosition(player);
	const int slots = getSlotCount(player);
	uint8_t mask = 0;
	for (int i = 0; i < slots; i++)
		mask |= 1 << (pos + i);
	return mask;
}

void BMRoom::broadcastReadySlotMask() const
{
	Packet packet;
	BMCmd cmd{};
	cmd.command = BMCmd::READY_MASK;
	cmd.size = 4;
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);
	const uint32_t mask = getReadySlotMask();
	packet.writeData(mask);
	Player::sendToAll(packet, players);
	DEBUG_LOG(Game::Bomberman, "broadcastReadySlotMask: mask %x", mask);
}

void BMRoom::sendGameStarting(Packet& packet, uint16_t clientId)
{
	BMCmd cmd {};
	cmd.command = BMCmd::GAME_STARTING;
	cmd.size = 4;
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(cmd.full);
	packet.writeData(clientId);
	packet.writeData(id);
}

void BMRoom::rudpAcked(Player *player)
{
	int i = getPlayerIndex(player);
	if (i < 0)
		return;
	if (!states[i].joining)
		return;
	printf("[%s] BMRoom::rudpAcked\n", player->getName().c_str());
	states[i].joining = false;
	broadcastKeyholder();
	broadcastReadySlotMask();
	Packet packet;
	sendRules(packet);
	if (!packet.empty())
		player->send(packet);
}

int BMRoom::getSlotCount(const Player *player) const
{
	int idx = getPlayerIndex(player);
	if (idx < 0)
		return 0;
	return slots[idx];
}

int BMRoom::getPlayerPosition(const Player *player) const
{
	int idx = getPlayerIndex(player);
	if (idx < 0)
		return idx;
	return std::accumulate(slots.begin(), slots.begin() + idx, 0);
}

void BMRoom::updateSlots()
{
	slots.clear();
	for (const Player *player : players)
	{
		const auto& extra = player->getExtraData();
		int slotCount = read32(extra.data(), 0) + 1;
		slots.push_back(slotCount);
	}
}

void BMRoom::setRules(const uint8_t *p, uint16_t ruleSetter)
{
	this->ruleSetter = ruleSetter;
	memcpy(rules.data(), p, rules.size());
	for (State& state : states)
		state.rulesAccepted = false;
	broadcastReadySlotMask();
}

void BMRoom::agreeRules(Player *player)
{
	int index = getPlayerIndex(player);
	if (index >= 0) {
		states[index].rulesAccepted = true;
		broadcastReadySlotMask();
	}
}

void BMRoom::sendRules(Packet& packet)
{
	if (ruleSetter == 0)
		// rules not set
		return;
	BMCmd cmd {};
	cmd.command = BMCmd::ACCEPT_RULES;	// means "new rules" when sent to game
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(cmd.full);
	packet.writeData(ruleSetter);
	packet.writeData(rules.data(), rules.size());
}

void BMRoom::mapInfoSent(Player *player)
{
	int idx = getPlayerIndex(player);
	if (idx >= 0)
	{
		states[idx].mapInfoSent = true;
		inGame = true;
		for (unsigned i = 0; i < players.size(); i++)
			inGame = inGame && states[i].mapInfoSent;
		if (inGame)
		{
			// TODO should be sent only once?
			DEBUG_LOG(Game::Bomberman, "%s: all mapInfoSent. sending game time info", player->getName().c_str());
			Packet packet;
			BMCmd cmd {};
			cmd.command = BMCmd::TIME_INFO;
			cmd.size = 12;
			packet.init(Packet::REQ_CHAT);
			packet.flags |= Packet::FLAG_RUDP;
			packet.writeData(cmd.full);
			packet.writeData((uint16_t)0);
			packet.writeData(0u);
			packet.writeData(60u * 180u);
			packet.writeData(0u);
			Player::sendToAll(packet, players);
		}
	}
}

void BMRoom::savePlayerCoords(Player *player, const uint8_t *data)
{
	const int slots = getSlotCount(player);
	const int pos = getPlayerPosition(player);
	for (int i = 0; i < slots; i++)
		states[pos].positions[i].readFrom(data + sizeof(CompactUser) * (pos + i));
}

// cmd2
void BMRoom::savePowerUps(Player *player, const uint8_t *data)
{
	// FIXME how to know which record is authoritative
	for (unsigned i = 0; i < powerUps.size(); i++)
	{
		PowerUp pup(data + i * sizeof(pup));
		if (pup.param != 0x1000 || powerUps[i].param == 0)
			powerUps[i] = pup;
	}
//	startTickTimer();
}

// cmd1 and cmd2
void BMRoom::saveBrickMap(Player *player, const uint8_t *data) {
	for (unsigned i = 0; i < brickMap.size(); i++)
		brickMap[i] &= data[i];
}

// cmd1
void BMRoom::saveTimestamp(Player *player, const uint8_t *data)
{
	const int pos = getPlayerPosition(player);
	if (pos >= 0)
		states[pos].cmd1Timestamp = *(const uint32_t *)data;
}

// cmd1
void BMRoom::saveBombState(Player *player, const uint8_t *data)
{
	// Some maps allow more bombs than other (Full fire allows 3)
	const int slots = getSlotCount(player);
	const int pos = getPlayerPosition(player);
	for (int slot = 0; slot < slots; slot++)
		for (int i = 0; i < bombsPerPlayer; i++)
		{
			Bomb& bomb = bombs[(pos + slot) * bombsPerPlayer + i];
			bomb.readFrom(data + ((pos + slot) * bombsPerPlayer + i) * sizeof(Bomb));
			if (bomb.type == 4)
				// change to type 5 to materialize it
				bomb.type = 5;
		}
}

void BMRoom::writePlayersPos(Packet& packet)
{
	int slots = 0;
	int pos = 0;
	for (const Player *player : players)
	{
		slots = getSlotCount(player);
		pos = getPlayerPosition(player);
		const State& state = states[getPlayerIndex(player)];
		for (int slot = 0; slot < slots; slot++)
			state.positions[slot].writeTo(packet.advance(sizeof(CompactUser)));

	}
	for (int i = pos + slots; i < 8; i++)
		memset(packet.advance(sizeof(CompactUser)), 0, sizeof(CompactUser));
}

void BMRoom::makeCmd1Packet(Player *player, Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	BMCmd cmd { 1, 0xC8 };
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);	// client id? mask?
	writePlayersPos(packet);
	packet.writeData((const uint8_t *)&states[getPlayerIndex(player)].cmd1Timestamp, sizeof(uint32_t));
	for (const Bomb& bomb : bombs)
		bomb.writeTo(packet.advance(sizeof(Bomb)));
	packet.writeData(brickMap.data(), brickMap.size());
}

void BMRoom::makeCmd2Packet(Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	BMCmd cmd { 2, 0xA4 };
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);	// client id? mask?
	writePlayersPos(packet);
	for (const PowerUp& pup : powerUps)
		pup.writeTo(packet.advance(sizeof(PowerUp)));
	packet.writeData(brickMap.data(), brickMap.size());
}

void BMRoom::makeCmd3Packet(Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	BMCmd cmd { 3, 0x3C };
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);	// client id? mask?
	writePlayersPos(packet);
}

/*
void BMRoom::startTickTimer()
{
	if (tickTimerStarted)
		return;
	tickTimerStarted = true;
	onTickTimer({});
}

void BMRoom::stopTickTimer()
{
	std::error_code ignored;
	timer.cancel(ignored);
	tickTimerStarted = false;
}

void BMRoom::onTickTimer(const std::error_code& ec)
{
	if (ec)
		return;

	// send state packets
	for (Player *player : players)
	{
		Packet packet;
		makeCmd1Packet(player, packet);
		player->send(packet);
	}
	Packet cmd23;
	makeCmd2Packet(cmd23);
	makeCmd3Packet(cmd23);
	Player::sendToAll(cmd23, players);

	// send game data every 200 ms (6 frames) like the game does
	if (timer.expiry().time_since_epoch() == 0ms)
		timer.expires_after(200ms);
	else
		timer.expires_at(timer.expiry() + 200ms);
	timer.async_wait(std::bind(&BMRoom::onTickTimer, this, asio::placeholders::error));
}
*/

void BMRoom::saveMapInfo(Player *player, const uint8_t *data, bool last)
{
	int pos = getPlayerIndex(player);
	if (pos < 0)
		return;
	if (states[pos].mapInfoStarted)
		return;
	states[pos].mapInfoStarted = true;
	bombsPerPlayer = read32(data, 0xc);
	DEBUG_LOG(Game::Bomberman, "%d bombs per player", bombsPerPlayer);
}

BombermanServer::BombermanServer(uint16_t port, asio::io_context& io_context)
	: LobbyServer(Game::Bomberman, port, io_context)
{
}

bool BombermanServer::handlePacket(Player *player, const uint8_t *data, size_t len)
{
	BMRoom *room = (BMRoom *)player->getRoom();
	if (room == nullptr)
		return false;

	uint16_t subtype = read16(data, 0x10);
	BMCmd cmd;
	cmd.full = subtype;
	if (data[3] == Packet::REQ_GAME_DATA)
	{
		switch (cmd.command)
		{
		case 1:
			{
				// 20 d8 00 11 00 00 10 01 00 00 00 10 00 00 00 00  ...............
				// 02 c8 08 00 00 00 00 00 35 78 00 00 00 00 00 00 ........5x......
				// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
				// (...)
				// 00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff ................
				// ff 0f ff ff ff ff ff ff                         ........
				if (room->getPlayerIndex(player) == 0) {
					Position p1(read16(data, 0x14));
					DEBUG_LOG(Game::Bomberman, "GAME_DATA_1: P1 %g:%g %x %x", p1.xpos(), p1.ypos(), data[0x16], data[0x17]);
				}
				else {
					Position p2(read16(data, 0x18));
					DEBUG_LOG(Game::Bomberman, "GAME_DATA_1: P2 %g:%g %x %x", p2.xpos(), p2.ypos(), data[0x16], data[0x17]);
				}
				room->savePlayerCoords(player, data + 0x14);
				room->saveTimestamp(player, data + 0x34);
				room->saveBombState(player, data + 0x38);
				room->saveBrickMap(player, data + 0xC8);
				room->makeCmd1Packet(player, replyPacket);
			}
			break;

		case 2:
			// 20 b4 00 11 00 00 10 02 00 00 00 13 00 00 00 00  ...............
			// 04 a4 80 00 04 38 00 00 00 00 00 00 00 00 00 00 .....8..........
			// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
			// 00 00 00 00 00 00 10 00 00 00 10 00 00 00 10 00 ................
			// 00 00 10 00 00 00 10 00 00 00 10 00 00 00 10 00 ................
			// 00 00 10 00 00 00 10 00 00 00 10 00 00 00 10 00 ................
			// 00 00 10 00 00 00 10 00 00 00 10 00 00 00 10 00 ................
			// 00 00 10 00 00 00 10 00 00 00 10 00 00 00 10 00 ................
			// 00 00 10 00 00 00 10 00 00 00 10 00 00 00 00 00 ................
			// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
			// 00 00 00 00 ff ff ff ff ff ff ff ff ff 0f ff ff ................
			// ff ff ff ff ....
			if (room->getPlayerIndex(player) == 0) {
				Position p1(read16(data, 0x14));
				DEBUG_LOG(Game::Bomberman, "GAME_DATA_2: P1 %g:%g %x %x", p1.xpos(), p1.ypos(), data[0x16], data[0x17]);
			}
			room->savePlayerCoords(player, data + 0x14);
			room->savePowerUps(player, data + 0x34);
			room->saveBrickMap(player, data + 0xA4);
			room->makeCmd2Packet(replyPacket);
			break;

		case 3:
			// 20 34 00 11 00 00 10 01 00 00 00 12 00 00 00 00  4..............
			// 06 3c 08 00 00 00 00 00 35 78 00 00 00 00 00 00 .<......5x......
			// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
			// 00 00 00 00 ....
			if (room->getPlayerIndex(player) == 0) {
				Position p1(read16(data, 0x14));
				DEBUG_LOG(Game::Bomberman, "GAME_DATA_3: P1 %g:%g %x %x", p1.xpos(), p1.ypos(), data[0x16], data[0x17]);
			}
			room->savePlayerCoords(player, data + 0x14);
			room->makeCmd3Packet(replyPacket);
			break;

		case BMCmd::START_TIMER:		// CalcTimer_First
			DEBUG_LOG(Game::Bomberman, "%s: start SyncTimer. client ID %x", player->getName().c_str(), *(const uint16_t *)&data[0x12]);
			// owner:
			// 60 14 00 11 00 00 10 01 00 00 00 08 00 00 00 09 `...............
			// 08 00 00 00                                     ....
			// non owner:
			// 60 14 00 11 00 00 10 02 00 00 00 04 00 00 00 07 `...............
			// 08 00 08 00                                     ....
			relayPacket.init(Packet::REQ_CHAT);
			relayPacket.relay(player->getId());
			relayPacket.writeData(&data[0x10], (int)(len - 0x10));
			break;

		case BMCmd::NEXT_TIMER:		// CalcTimer_Next
			DEBUG_LOG(Game::Bomberman, "%s: timer tick: %x", player->getName().c_str(), read32(data, 0x14));
			// 20 18 00 11 00 00 10 02 00 00 00 09 00 00 00 00  ...............
			// 0a 04 08 00 00 00 00 07                          ........
			//             timer
			// then
			// 20 18 00 11 00 00 10 02 00 00 00 0a 00 00 00 00  ...............
			// 0a 04 08 00 00 00 00 18                          ........
			//             timer
			//                      29
			relayPacket.init(Packet::REQ_CHAT);
			relayPacket.relay(player->getId());
			relayPacket.writeData(&data[0x10], (int)(len - 0x10));
			break;

		case BMCmd::SET_RULES:		// Set game rules
			{
				const uint16_t clientId = read16(data, 0x12);
				DEBUG_LOG(Game::Bomberman, "%s: set game rules. client ID %x", player->getName().c_str(), clientId);
				replyPacket.init(Packet::REQ_NOP);
				player->ackPacket(replyPacket, data);
				player->send(replyPacket);
				replyPacket.reset();

				room->setRules(&data[0x14], clientId);
				room->sendRules(relayPacket);
				room->sendRules(replyPacket); // important
				break;
			}

		//case 9:	// ?
		// 2 ints per slot: be room pos [0-7], be p[8bf + pos * 5]	-> set by msg C
		// only sent by owner?
		// NOT USED...

		case BMCmd::START_GAME:
			{
				// 0000   a0 18 00 11 00 00 10 02 00 00 00 09 00 00 00 00   ................
				// 0010   14 04 80 00 00 00 de 92 ba 47 66 10               .........Gf.
				DEBUG_LOG(Game::Bomberman, "%s: START BATTLE!", player->getName().c_str());
				replyPacket.respOK(Packet::REQ_CHAT);
				player->ackPacket(replyPacket, data);
				player->send(replyPacket);
				replyPacket.reset();

				const uint16_t clientId = *(uint16_t *)&data[12];
				room->sendGameStarting(replyPacket, clientId);
				room->sendGameStarting(relayPacket, clientId);
				break;
			}

		case BMCmd::ACCEPT_RULES:	// Agree new rules
			// non owner:
			// 0000   a0 14 00 11 00 00 10 04 00 00 00 08 00 00 00 00   ................
			// 0010   16 00 08 00 ba 47 66 10                           .....Gf.
			//             clientID (<<11)
			DEBUG_LOG(Game::Bomberman, "%s: agree new rules", player->getName().c_str());
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			room->agreeRules(player);
			break;

		case BMCmd::ACK_RULES:	// Received new rules
			// non owner:
			// 0000   a0 14 00 11 00 00 10 04 00 00 00 06 00 00 00 00   ................
			// 0010   18 00 08 00 ba 47 66 10                           .....Gf.
			//             clientID (<<11)
			DEBUG_LOG(Game::Bomberman, "%s: received new rules", player->getName().c_str());
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			break;

		case BMCmd::ACK_START:
			DEBUG_LOG(Game::Bomberman, "%s: ack game starting. client ID %x", player->getName().c_str(), *(const uint16_t *)&data[0x12]);
			// owner:
			// a0 14 00 11 00 00 10 01 00 00 00 0a 00 00 00 00 ................
			// 1a 00 80 00 ....
			// non owner:
			// a0 14 00 11 00 00 10 02 00 00 00 08 00 00 00 00 ................
			// 1a 00 08 00                                     ....
			//       clientID
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);

			relayPacket.init(Packet::REQ_CHAT);
			relayPacket.flags |= Packet::FLAG_RUDP;
			relayPacket.relay(player->getId());
			relayPacket.writeData(data + 0x10, len - 0x10);
			break;

		case BMCmd::POST_MAP:	// Game-phase marker after map block exchange
			DEBUG_LOG(Game::Bomberman, "%s: map block done. client ID %x", player->getName().c_str(), *(const uint16_t *)&data[0x12]);
			// a0 14 00 11 00 00 10 01 00 00 00 0f 00 00 00 00 ................
			// 1c 00 08 00                                     ....
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			room->mapInfoSent(player);
			break;

		case 0xf:	// ??? response to udpF 17
			DEBUG_LOG(Game::Bomberman, "%s: received UDP 11 sub F", player->getName().c_str());
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);

			relayPacket.init(Packet::REQ_CHAT);
			relayPacket.flags |= Packet::FLAG_RUDP;
			relayPacket.writeData(cmd.full);
			relayPacket.writeData(read16(data, 0x12));
			break;

		case BMCmd::MAP_INFO: // SendGameMapBlock: Send MapInfo
			DEBUG_LOG(Game::Bomberman, "%s: SendGameMapBlock 1A", player->getName().c_str());
			room->saveMapInfo(player, data + 0x14, false);
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			break;

		case BMCmd::MAP_INFO_LAST: // SendGameMapBlock: Send MapInfo
			DEBUG_LOG(Game::Bomberman, "%s: SendGameMapBlock 1B", player->getName().c_str());
			room->saveMapInfo(player, data + 0x14, true);
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			break;

		default:
			ERROR_LOG(game, "Unhandled udp 11 command: %x (%04x)", cmd.command, cmd.full);
			dumpData(data, len);
			return false;
		}
		return true;
	}
	if (data[3] != Packet::REQ_CHAT || (read16(data, 0) & Packet::FLAG_RELAY))
		return false;

	switch (cmd.command)
	{
	case BMCmd::KICK_PLAYER:
		{
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			const uint32_t playerPos = data[0x14];
			for (Player *player : room->getPlayers())
			{
				if (room->getPlayerPosition(player) == (int)playerPos)
				{
					Packet pkt;
					pkt.init(Packet::REQ_CHAT);
					pkt.flags |= Packet::FLAG_RUDP;
					pkt.writeData(&data[0x10], 4);
					pkt.writeData((const uint8_t *)&playerPos, sizeof(playerPos));
					player->send(pkt);
					break;
				}
			}
			break;
		}

	case BMCmd::PING:
		DEBUG_LOG(Game::Bomberman, "%s: ping", player->getName().c_str());
		replyPacket.init(Packet::REQ_CHAT);
		replyPacket.writeData(cmd.full);
		replyPacket.writeData((uint16_t)data[0x12]); // Client ID
		replyPacket.writeData(0x10000000u);	// LE int. ping value? only lsbyte used. 1,4,10,80,c8 is red
		replyPacket.writeData(room->getSlotMask(player));
		break;

	default:
		ERROR_LOG(game, "Unhandled udp F command: %x (%04x)", cmd.command, cmd.full);
		dumpData(data, len);
		return false;
	}

	return true;
}

Room *BombermanServer::addRoom(const std::string& name, uint32_t attributes, Player *owner)
{
	uint32_t id = nextRoomId++;
	BMRoom *room = new BMRoom(*owner->getLobby(), id, name, attributes, owner, io_context);
	owner->getLobby()->addRoom(room);

	return room;
}
