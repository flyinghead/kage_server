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
#include "bomberman.h"
#include "log.h"
#include <numeric>

using namespace std::chrono_literals;

BMRoom::BMRoom(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context)
	: Room(lobby, id, name, attributes, owner, io_context), timer(io_context)
{
	// needed after the owner is added in the parent constructor
	updateSlots();
	resetMatch();
}

void BMRoom::addPlayer(Player *player) {
	Room::addPlayer(player);
	updateSlots();
}

bool BMRoom::removePlayer(Player *player)
{
	const bool wasOwner = player == owner;
	const int idx = getPlayerIndex(player);
	bool b = Room::removePlayer(player);
	if (!b)
	{
		updateSlots();
		for (unsigned i = idx; i < players.size() - 1; i++)
			states[i] = states[i + 1];
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
	BMCmd cmd { BMCmd::ROSTER_LIST, 0 };
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

void BMRoom::makeRoomJoin(Packet& packet, const Player *player)
{
	BMCmd cmd{};
	cmd.command = BMCmd::ROOM_JOIN;
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);			// flag?
	packet.writeData(player->getId());		// player kage id [914]
	packet.writeData((uint32_t)getPlayerIndex(player));		// FIXME [915] [0-F]? client id?
	uint32_t pos = (uint32_t)getPlayerPosition(player);
	packet.writeData(pos); 					// player pos [916]
	uint32_t slots = getSlotCount(player);
	packet.writeData(slots - 1);				// guest count [911]
	packet.writeData(owner->getId());		// room owner kage id [912]
	packet.writeData((uint32_t)getPlayerPosition(owner)); // room owner player pos [913]
	// for each guest: -1 or guest pos (1 - 7)
	for (unsigned i = 0; i < slots; i++)
		packet.writeData(++pos);
}

// owner: needs ROOM_JOIN only at creation
//        needs ROSTER_LIST when player joins
// joiner: needs ROOM_JOIN
//         and send ROSTER_LIST to all
void BMRoom::createJoinRoomReply(Packet& reply, Packet& relay, Player *player)
{
	Room::createJoinRoomReply(reply, relay, player);

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
		states[playerIndex].status = State::Joining;
		player->notifyRoomOnAck();
	}
	else {
		states[playerIndex].status = State::InRoom;
	}

	makeRoomJoin(reply, player);
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
		if (states[index].status != State::RulesAccepted)
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
	gameStarting = true;
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
	if (states[i].status == State::Joining)
	{
		DEBUG_LOG(Game::Bomberman, "[%s] BMRoom::rudpAcked (join)", player->getName().c_str());
		states[i].status = State::InRoom;
		broadcastReadySlotMask();
		Packet packet;
		sendRules(packet);
		if (!packet.empty())
			player->send(packet);
		return;
	}
	else if (states[i].status == State::GameEnd)
	{
		DEBUG_LOG(Game::Bomberman, "[%s] BMRoom::rudpAcked (end 2)", player->getName().c_str());
		states[i].status = State::CompletedDeadBits;
		bool allDone = true;
		for (unsigned j = 0; j < players.size(); j++)
			if (states[j].status != State::CompletedDeadBits) {
				allDone = false;
				break;
			}
		if (allDone)
		{
			DEBUG_LOG(Game::Bomberman, "All players have ended the game");
			gameNumber++;
			if (gameNumber < rules[2])
				resetGame();
			else
				resetMatch();
		}
	}
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

void BMRoom::setRules(const uint8_t *p)
{
	memcpy(rules.data(), p, rules.size());
	for (State& state : states)
		if (state.status == State::RulesAccepted)
			state.status = State::InRoom;
	broadcastReadySlotMask();
}

void BMRoom::agreeRules(Player *player)
{
	int index = getPlayerIndex(player);
	if (index >= 0) {
		states[index].status = State::RulesAccepted;
		broadcastReadySlotMask();
	}
}

void BMRoom::sendRules(Packet& packet)
{
	if (rules[8] == 0)
		// rules not set
		return;
	BMCmd cmd {};
	cmd.command = BMCmd::ACCEPT_RULES;	// means "new rules" when sent to game
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);
	packet.writeData(rules.data(), rules.size());
}

void BMRoom::playerInGame(Player *player)
{
	int idx = getPlayerIndex(player);
	if (idx >= 0)
	{
		states[idx].status = State::InGame;
		inGame = true;
		gameStarting = false;
		for (unsigned i = 0; i < players.size(); i++)
			inGame = inGame && (states[i].status == State::InGame);
		if (inGame)
		{
			DEBUG_LOG(Game::Bomberman, "%s: all MapInfo sent. sending game time info", player->getName().c_str());
			Packet packet;
			BMCmd cmd { BMCmd::TIME_INFO, 12 };
			packet.init(Packet::REQ_CHAT);
			packet.flags |= Packet::FLAG_RUDP;
			packet.writeData(cmd.full);
			packet.writeData((uint16_t)0);
			packet.writeData(0u);
			packet.writeData(60u * 60u * (unsigned)getTimeLimit().count());
			packet.writeData(0u);
			Player::sendToAll(packet, players);
		}
	}
}

uint32_t BMRoom::getDeadPlayers() const
{
	uint32_t mask = (1 << getPlayerCount()) - 1;
	if (rules[0] == 0)
	{
		// Survival mode
		int i = 0;
		for (unsigned pl = 0; pl < players.size(); pl++)
		{
			const int slots = getSlotCount(players[pl]);
			for (int slot = 0; slot < slots; slot++, i++)
				if (!states[pl].dead[slot])
					mask &= ~(1 << i);
		}
	}
	else
	{
		// Hyper bomber mode
		int i = 0;
		for (unsigned pl = 0; pl < players.size(); pl++)
		{
			const int slots = getSlotCount(players[pl]);
			for (int slot = 0; slot < slots; slot++, i++)
				if (states[pl].positions[slot].unk == 0x80 && !states[pl].dead[slot]) {
					mask &= ~(1 << i);
					break;
				}
		}
	}
	return mask;
}

void BMRoom::savePlayerCoords(Player *player, const uint8_t *data)
{
	const int slots = getSlotCount(player);
	const int pos = getPlayerPosition(player);
	for (int i = 0; i < slots; i++)
	{
		State& state = states[getPlayerIndex(player)];
		state.positions[i].readFrom(data + sizeof(CompactUser) * (pos + i));
		if (state.positions[i].unk == 4 && !state.dead[i])
			// Player is dead
			state.dead[i] = true;
		else if (state.positions[i].unk == 0 && state.dead[i])
			// Player has respawned (hyper bomber mode)
			state.dead[i] = false;
	}
}

// cmd2
void BMRoom::savePowerUps(Player *player, const uint8_t *data)
{
	// States marked like (this) are set by the server
	// Appearing sequence:
	//   hidden -> appearing -> visible
	// Successful pick up
	//   claimed -> (granted) -> acquired
	// TODO: Denied pick up
	// Hyper bomber death sequence:
	//   acquired -> released (with coords, unk1=1) -> (random) -> appearing -> visible
	for (unsigned i = 0; i < powerUps.size(); i++)
	{
		PowerUp pup(data + i * sizeof(pup));
		uint16_t curState = powerUps[i].state;
		switch (curState)
		{
		case PowerUp::Hidden:
			powerUps[i] = pup;
			break;
		case PowerUp::Appearing:
			if (pup.state == PowerUp::Visible) {
				powerUps[i] = pup;
			}
			else if (pup.state == PowerUp::Claimed)
			{
				// TODO wait and arbitrate
				pup.state = PowerUp::Granted;
				powerUps[i] = pup;
			}
			break;
		case PowerUp::Visible:
			if (pup.state == PowerUp::Claimed)
			{
				// TODO wait and arbitrate
				pup.state = PowerUp::Granted;
				powerUps[i] = pup;
			}
			break;
		case PowerUp::Expired:
			// TODO verify that status 4 is really expired
			// This shouldn't be allowed if 4 is expired
			if (pup.state == PowerUp::Claimed)
			{
				WARN_LOG(Game::Bomberman, "Claiming an expired power-up?");
				pup.state = PowerUp::Granted;
				powerUps[i] = pup;
			}
			else if (pup.state == PowerUp::Gone) {
				powerUps[i] = pup;
			}
			break;
		case PowerUp::Acquired:	// Picked up by a player
			// Only transitions to 0 (or 4?) should be allowed? And only by the item owner
			if (pup.state == PowerUp::Gone && pup.slot == powerUps[i].slot)
				powerUps[i] = pup;
			if (pup.state == PowerUp::Released && pup.slot == powerUps[i].slot)
			{
				// item has been released to a random location. Make it visible
				pup.state = PowerUp::Random;
				powerUps[i] = pup;
				DEBUG_LOG(Game::Bomberman, "PowerUp Acquired -> Released (Random). Slot %d coords %d %d", pup.slot, pup.pos.x, pup.pos.y);
			}
			else if (pup.state != PowerUp::Acquired && pup.slot == powerUps[i].slot) {
				WARN_LOG(Game::Bomberman, "PowerUp transitioning from acquired to %x? by slot %d, owner %d",
						pup.state, pup.slot, powerUps[i].slot);
			}
			break;

		case PowerUp::Granted: // Granted to a player by the server
			if (pup.state == PowerUp::Acquired && pup.slot == powerUps[i].slot)
				powerUps[i] = pup;
			else if (pup.state != powerUps[i].state
					&& pup.state != PowerUp::Visible
					&& pup.state != PowerUp::Claimed
					&& pup.slot == powerUps[i].slot)
				WARN_LOG(Game::Bomberman, "PowerUp current state not handled: Granted -> %x", pup.state);
			break;

		case PowerUp::Released: // Released by a dead player in hyper bomber mode
			if (pup.state == PowerUp::Random && pup.slot == powerUps[i].slot)
			{
				// TODO Can this happen now? Can't find any occurrence
				powerUps[i] = pup;
				DEBUG_LOG(Game::Bomberman, "PowerUp Released -> Random. coords %d %d", pup.pos.x, pup.pos.y);
			}
			else if (pup.state == PowerUp::Acquired) {
				powerUps[i] = pup;
				DEBUG_LOG(Game::Bomberman, "PowerUp Released -> Acquired. Slot %d coords %d %d", pup.slot, pup.pos.x, pup.pos.y);
			}
			else if (pup.state != powerUps[i].state && pup.slot == powerUps[i].slot)
				WARN_LOG(Game::Bomberman, "PowerUp current state not handled: Released -> %x", pup.state);
			break;

		case PowerUp::Random:
			if ((pup.state == PowerUp::Appearing || pup.state == PowerUp::Visible)
					&& pup.slot == powerUps[i].slot)
				powerUps[i] = pup;
			else if (pup.state != powerUps[i].state && pup.slot == powerUps[i].slot)
				WARN_LOG(Game::Bomberman, "PowerUp current state not handled: Random -> %x", pup.state);
			break;

		default:
			if (powerUps[i].param == 0)
				powerUps[i] = pup;
			else
				WARN_LOG(Game::Bomberman, "PowerUp current state not handled: %x -> %x", curState, pup.state);
			break;
		}
	}
}

// cmd1 and cmd2
void BMRoom::saveBrickMap(Player *player, const uint8_t *data) {
	for (unsigned i = 0; i < brickMap.size(); i++)
		brickMap[i] &= data[i];
}

// cmd1
void BMRoom::saveTimestamp(Player *player, const uint8_t *data)
{
	const int idx = getPlayerIndex(player);
	if (idx >= 0)
		states[idx].cmd1Timestamp = read32(data, 0);
}

// cmd1
void BMRoom::saveBombState(Player *player, const uint8_t *data)
{
	const int slots = getSlotCount(player);
	const int pos = getPlayerPosition(player);
	for (unsigned i = 0; i < bombs.size(); i++)
	{
		Bomb bomb { &data[i * sizeof(Bomb)] };
		if ((int)bomb.slot < pos || (int)bomb.slot >= pos + slots)
			continue;
		if (bomb.type == 0 && bombs[i].type != 1)
			// only allow transition from exploded to unused
			continue;
		else if (bomb.type == 4)
			// change to type 5 to materialize it
			bomb.type = 5;
		// change to 6 to refuse placement?
		bombs[i] = bomb;
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

void BMRoom::writeTimestamp(Packet& packet)
{
	uint32_t latest = 0;
	for (const State& state : states)
		latest = std::max(latest, state.cmd1Timestamp);
	packet.writeData(latest);
}

void BMRoom::makeCmd1Packet(Player *player, Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	BMCmd cmd { BMCmd::BOMB_DATA, 0xC8 };
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)(inGame ? 0 : EOG_MARK));
	writePlayersPos(packet);
	writeTimestamp(packet);
	for (const Bomb& bomb : bombs)
		bomb.writeTo(packet.advance(sizeof(Bomb)));
	packet.writeData(brickMap.data(), brickMap.size());
}

void BMRoom::makeCmd2Packet(Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	BMCmd cmd { BMCmd::MAP_DATA, 0xA4 };
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)(inGame ? 0 : EOG_MARK));
	writePlayersPos(packet);
	for (const PowerUp& pup : powerUps)
		pup.writeTo(packet.advance(sizeof(PowerUp)));
	packet.writeData(brickMap.data(), brickMap.size());
}

void BMRoom::makeCmd3Packet(Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	BMCmd cmd { BMCmd::POS_DATA, 0x3C };
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)(inGame ? 0 : EOG_MARK));
	writePlayersPos(packet);
}

std::chrono::minutes BMRoom::getTimeLimit() const {
	return std::chrono::minutes(rules[3] + 1);
}

void BMRoom::saveMapInfo(Player *player, const uint8_t *data, bool last)
{
	int idx = getPlayerIndex(player);
	if (idx < 0)
		return;
	if (states[idx].status == State::MapInfoStarted)
		return;
	states[idx].status = State::MapInfoStarted;
	if (player != owner)
		return;
	bombsPerPlayer = read32(data, 0xc);
	DEBUG_LOG(Game::Bomberman, "%d bomb(s) per player", bombsPerPlayer);
}

bool BMRoom::checkEndOfGame(Player *player, uint8_t command, uint8_t mark)
{
	if (mark != EOG_MARK)
		return false;
	State& playerState = states[getPlayerIndex(player)];
	if (playerState.status != State::InGame)
		return false;
	INFO_LOG(Game::Bomberman, "[%s] Game end", player->getName().c_str());
	playerState.endOfGameMask |= 1 << (command - 1);
	return playerState.endOfGameMask == 7;
}

void BMRoom::sendEndOfGame(Player *player, Packet& packet)
{
	State& state = states[getPlayerIndex(player)];
	if (state.status == State::GameEnd)
		return;
	if (state.endOfGameMask != 7)
		return;
	state.status = State::GameEnd;
	inGame = false;
	player->notifyRoomOnAck();
	BMCmd cmd { BMCmd::CMPL_DEAD_BITS, 4 };
	if (gameNumber < rules[2] - 1)
		cmd.command = BMCmd::SET_DEAD_BITS;
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(cmd.full);
	packet.writeData((uint16_t)0);
	packet.writeData(getDeadPlayers());
}

void BMRoom::resetGame()
{
	inGame = false;
	gameStarting = false;
	for (unsigned i = 0; i < players.size(); i++)
	{
		State& state = states[i];
		state.cmd1Timestamp = 0;
		state.dead.fill(false);
		state.endOfGameMask = 0;
		state.positions.fill({});
		// we don't reset the status since it should be correct
	}
	powerUps.fill({});
	brickMap.fill(0xff);
	bombs.fill({});
}

void BMRoom::resetMatch() {
	resetGame();
	gameNumber = 0;
}

void BMRoom::endBattle(Player *player)
{
	int idx = getPlayerIndex(player);
	if (idx >= 0)
		states[idx].status = State::RulesAccepted;
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
		case BMCmd::BOMB_DATA:
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
				if (room->checkEndOfGame(player, BMCmd::BOMB_DATA, data[0x13]))
				{
					player->send(replyPacket);
					replyPacket.reset();
					room->sendEndOfGame(player, replyPacket);
				}
			}
			break;

		case BMCmd::MAP_DATA:
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
			if (room->checkEndOfGame(player, BMCmd::MAP_DATA, data[0x13]))
			{
				player->send(replyPacket);
				replyPacket.reset();
				room->sendEndOfGame(player, replyPacket);
			}
			break;

		case BMCmd::POS_DATA:
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
			if (room->checkEndOfGame(player, BMCmd::POS_DATA, data[0x13]))
			{
				player->send(replyPacket);
				replyPacket.reset();
				room->sendEndOfGame(player, replyPacket);
			}
			break;

		case BMCmd::START_TIMER:		// CalcTimer_First
			DEBUG_LOG(Game::Bomberman, "%s: start SyncTimer. client ID %x", player->getName().c_str(), *(const uint16_t *)&data[0x12]);
			// owner:
			// 60 14 00 11 00 00 10 01 00 00 00 08 00 00 00 09 `...............
			// 08 00 00 00                                     ....
			// non owner:
			// 60 14 00 11 00 00 10 02 00 00 00 04 00 00 00 07 `...............
			// 08 00 08 00                                     ....
			/*
			relayPacket.init(Packet::REQ_CHAT);
			relayPacket.relay(player->getId());
			relayPacket.writeData(&data[0x10], (int)(len - 0x10));
			*/
			// Likely expects an initial timer value as payload
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
			// FIXME ***Relaying these packet makes the game freeze at EoB***
			/*
			relayPacket.init(Packet::REQ_CHAT);
			relayPacket.relay(player->getId());
			relayPacket.writeData(&data[0x10], (int)(len - 0x10));
			*/
			// send every 500 ms (if not answered) timer value increases by 0x11
			// ~ 29.4 ms/tick
			{
				BMCmd cmd { BMCmd::NEXT_TIMER, 4 };
				replyPacket.init(Packet::REQ_CHAT);
				replyPacket.writeData(cmd.full);
				replyPacket.writeData((uint16_t)0);
				replyPacket.writeData(0u);	// should be between -1 and 1
			}
			break;

		case BMCmd::SET_RULES:		// Set game rules
			{
				const uint16_t clientId = read16(data, 0x12);
				DEBUG_LOG(Game::Bomberman, "%s: set game rules. client ID %x", player->getName().c_str(), clientId);
				replyPacket.init(Packet::REQ_NOP);
				player->ackPacket(replyPacket, data);
				player->send(replyPacket);
				replyPacket.reset();

				room->setRules(&data[0x14]);
				room->sendRules(relayPacket);
				room->sendRules(replyPacket); // important
				break;
			}

		//case 9:	// ?
		// 2 ints per slot: be room pos [0-7], be p[8bf + pos * 5]	-> set by msg C
		// only sent by owner?
		// NOT USED...

		case BMCmd::START_BATTLE:
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
			// TODO also received by winner after a game. Reset to game room if match is over?
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
			/* FIXME This is very likely wrong since the game handler for D needs a payload
			relayPacket.init(Packet::REQ_CHAT);
			relayPacket.flags |= Packet::FLAG_RUDP;
			relayPacket.relay(player->getId());
			relayPacket.writeData(data + 0x10, len - 0x10);
			*/
			break;

		case BMCmd::START_GAME:	// map info sent, ready to start sending game data
			DEBUG_LOG(Game::Bomberman, "%s: START GAME. client ID %x", player->getName().c_str(), *(const uint16_t *)&data[0x12]);
			// a0 14 00 11 00 00 10 01 00 00 00 0f 00 00 00 00 ................
			// 1c 00 08 00                                     ....
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			room->playerInGame(player);
			break;

		case BMCmd::RESTART_GAME: // map info sent, ready to start sending game data (for 2nd+ games in same match)
			// 0000   a0 14 00 11 00 00 10 01 00 00 00 13 00 00 00 00   ................
			// 0010   1e 00 80 00 ba 47 66 10                           .....Gf.
			DEBUG_LOG(Game::Bomberman, "%s: RESTART GAME", player->getName().c_str());
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			room->playerInGame(player);
			break;

		case BMCmd::END_GAME: // game has ended, responding to settle/completed dead bits (16,19)
			// Game resets game time info received and start sending synctimer 2 s later if match isn't over (msg 16)
			DEBUG_LOG(Game::Bomberman, "%s: END GAME cmd=10", player->getName().c_str());
			replyPacket.init(Packet::REQ_NOP);
			player->ackPacket(replyPacket, data);
			break;

		case BMCmd::END_BATTLE:	// battle has ended, returning to game room
			{
				// Only received when match is over. Next message is room unlock if owner.
				// a8 14 00 11 00 00 10 02 00 00 00 10 00 00 00 00 ................
				// 26 00 80 00                                     &...
				DEBUG_LOG(Game::Bomberman, "%s: END BATTLE cmd=13", player->getName().c_str());
				if (player == room->getOwner())
				{
					// Hack to avoid disconnection error by the room owner
					replyPacket.init(Packet::REQ_QRY_USERS);
					replyPacket.writeData(0u);
					replyPacket.writeData(0u);
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
					player->send(replyPacket);
					replyPacket.reset();
				}
				room->endBattle(player);

				replyPacket.init(Packet::REQ_NOP);
				player->ackPacket(replyPacket, data);
				break;
			}

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
		replyPacket.writeData(data + 0x14, 5);
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
