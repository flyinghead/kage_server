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
#include "propeller.h"
#include "propa_rank.h"
#include "log.h"
#include <algorithm>
#include <cmath>

using namespace std::chrono_literals;

PARoom::PARoom(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context)
	: Room(lobby, id, name, attributes, owner, io_context), timer(io_context)
{
	rngSeed = (uint32_t)time(nullptr);
	srand(rngSeed);
	for (auto& state : playerState) {
		state.plane = rand() & 7;
	}
}

bool PARoom::removePlayer(Player *player)
{
	bool ownerLeft = player == owner;
	int index = getPlayerIndex(player);
	if (Room::removePlayer(player))
		// empty room
		return true;
	if (index < 0)
		// already logged error
		return false;
	for (int i = index; i < 5; i++)
		playerState[i] = playerState[i + 1];

	Packet packet;
	sendPlayerList(packet);
	Player::sendToAll(packet, players);

	if (ownerLeft)
	{
		// Notify of new room master
		Packet packet;
		sendRoomAttrs(packet);
		Player::sendToAll(packet, players);
	}

	return false;
}

void PARoom::updateSettings(int stage, int maxPoints, uint8_t flags, uint8_t field2, uint8_t field3)
{
	this->stage = stage;
	this->maxPoints = maxPoints;
	this->flags = flags;
	this->field2 = field2;
	this->field3 = field3;
}

void PARoom::setPlane(Player *player, uint8_t plane)
{
	int index = getPlayerIndex(player);
	if (index < 0)
		return;
	playerState[index].plane = plane;
}

void PARoom::setPlayerFlags(Player *player, uint8_t flags)
{
	int index = getPlayerIndex(player);
	if (index < 0)
		return;
	playerState[index].flags = flags;
}

void PARoom::setRank(Player *player, uint8_t rank)
{
	int index = getPlayerIndex(player);
	if (index < 0)
		return;
	playerState[index].rank = rank;
}

void PARoom::setStateData(int slot, const uint8_t *data)
{
	PlayerState& state = playerState[slot];
	bool shotdown = false;
	for (int frame = 0; frame < 4; frame++)
	{
		if (data[0x18 + frame] == 3)
		{
			shotdown = true;
			state.deaths++;
			std::string killed;
			if (slot < (int)players.size()) {
				killed = players[slot]->getName();
			}
			else {
				killed = std::string("C") + std::to_string(slot - players.size() + 1);
			}
			int killerSlot = data[0x10 + frame];
			if (killerSlot != 0xff)
			{
				std::string killer;
				if (killerSlot == slot) {
					killer = "himself";
				}
				else if (killerSlot < (int)players.size())
				{
					killer = players[killerSlot]->getName();
					playerState[killerSlot].score++;
					playerState[killerSlot].kills++;
				}
				else {
					killer = std::string("C") + std::to_string(killerSlot - players.size() + 1);
				}
				DEBUG_LOG(game, "%s was shot down by %s", killed.c_str(), killer.c_str());
			}
			break;
		}
	}
	if (!shotdown
			&& slot < (int)players.size() // not interested in bot crashes
			&& state.seqnum > 0)
	{
		for (int frame = 0; frame < 4; frame++)
		{
			// D2 00 or 01 with E=0 means conflict
			if (data[0xc + frame] != 0)
			{
				// D2 != ff && E != 1
				if ((data[0x10 + frame] != 0xff)
							&& data[0x18 + frame] != 1)
				{
					DEBUG_LOG(game, "%s crashed", players[slot]->getName().c_str());
					if (state.score > 0)
						state.score--;
					break;
				}
				// D3 = [12-17] and F == 80 -> zeppelin crash
				// D3 = [5-C]? and F == 80 -> crash into powerup
				else if (data[0x14 + frame] != 0xff && (data[0x1c + frame] & 0x80))
				{
					DEBUG_LOG(game, "%s crashed into a zeppelin/powerup", players[slot]->getName().c_str());
					if (state.score > 0)
						state.score--;
					break;
				}
			}
		}
	}
	for (int frame = 0; frame < 4; frame++)
	{
		uint8_t prev;
		if (frame == 0)
			prev = state.data[0xb];
		else
			prev = data[8 + frame - 1];
		if (prev != data[8 + frame])
			// min speed: 151 mph, normal speed: 226 mph
			state.flightDist += 0.033333f * 151.f * (1 + (data[8 + frame] >> 1) / 32.f) / 3600.f;
	}

	memcpy(state.data.data(), data, state.data.size());
	state.seqnum++;
	if (!timerStarted) {
		timerStarted = true;
		sendGameData({});
	}
}

void PARoom::setAudio(uint8_t slot, const uint8_t *data)
{
	if (slot != talkingSlot)
		return;
	if (data == nullptr) {
		talkingSlot = 0xff;
		return;
	}
	PlayerState& state = playerState[slot];
	memcpy(state.audio.data(), data, state.audio.size());
	/*
	Packet packet;
	packet.init(Packet::REQ_CHAT);
	packet.writeData(OUT_AUDIO);
	packet.writeData(slot);
	packet.writeData(audioSeq++);
	packet.writeData((uint8_t)0);		// ?
	packet.writeData((uint8_t)0x14);	// data sizes
	packet.writeData((uint8_t)0x14);
	packet.writeData((uint8_t)0);
	packet.writeData((uint8_t)0);		// padding
	packet.writeData(data, 0x28);
	Player::sendToAll(packet, players, players[slot]);
	*/
}

void PARoom::sendGameData(const std::error_code& ec)
{
	if (ec)
		return;

	if (talkingSlot != 0xff && Clock::now() - audioStart >= 5s) {
		talkingSlot = 0xff;
	}
	else
	{
		for (int frame = 0; frame < 4 && talkingSlot == 0xff; frame++)
		{
			for (int slot = 0; slot < 6; slot++)
			{
				if (playerState[slot].data[0x1c + frame] & 0x10)
				{
					talkingSlot = slot;
					audioSeq = 1;
					audioStart = Clock::now();
					DEBUG_LOG(game, "[%d] %s talking\n", slot, players[slot]->getName().c_str());
					break;
				}
			}
		}
	}
	bool missingData = false;
	for (const PlayerState& state : playerState)
	{
		if (state.seqnum == 0) {
			// no data received yet
			missingData = true;
			break;
		}
	}

	int slot = 0;
	while (slot < 6)
	{
		Packet packet;
		packet.init(Packet::REQ_CHAT);
		uint8_t *payload = &packet.data[packet.size];
		if (talkingSlot != 0xff || slot >= 3 || missingData)
		{
			// Use packet 1C: 1 to 3 planes with audio data
			packet.writeData(OUT_GAME_DATA_AUDIO);
			packet.writeData(talkingSlot);	// slot of the player currently talking, or FF
			packet.writeData((uint8_t)0);	// flag. adds 6 to previous slot if != 0 (?)
			packet.size += 0x2b;
			payload[0x2b] = 0xff;
			payload[0x2c] = 0xff;
			payload[0x2d] = 0xff;

			int idx = 0;
			for (; idx < 3 && slot < 6; slot++)
			{
				PlayerState& state = playerState[slot];
				if (state.seqnum == 0)
					// no data received yet so skip it
					continue;
				if (idx == 0)
					packet.size += 0xa;
				payload[0x2b + idx] = slot;
				// score
				payload[0x2e + idx] = state.score;
				// state seqnum
				*(uint16_t *)&payload[0x32 + idx * 2] = ntohs(state.seqnum);
				packet.size += 0x3c;
				memcpy(&payload[0x38 + idx * 0x3c], state.data.data(), state.data.size());
				if (talkingSlot == slot) {
					memcpy(&payload[3], state.audio.data(), state.audio.size());
					memset(state.audio.data(), 0, state.audio.size());
				}
				idx++;
			}
		}
		else
		{
			// Use packet 1D: 4 planes without audio
			packet.writeData(OUT_GAME_DATA);
			packet.writeData(talkingSlot);	// slot of the player currently talking, or FF
			packet.size += 0x102;
			for (int idx = 0; idx < 4 && slot < 6; slot++, idx++)
			{
				const PlayerState& state = playerState[slot];
				payload[2 + idx] = slot;
				// score
				payload[6 + idx] = state.score;
				// state seqnum
				*(uint16_t *)&payload[0xa + idx * 2] = ntohs(state.seqnum);
				memcpy(&payload[0x14 + idx * 0x3c], state.data.data(), state.data.size());
			}
		}
		for (unsigned i = 0; i < players.size(); i++)
			if (playerState[i].inGame)
				players[i]->send(packet);
	}

	// The game seems to send a state every 4 frames
	if (timer.expiry().time_since_epoch() != 0ms)
		timer.expires_at(timer.expiry() + 133ms);	// 7.5/s
	else
		timer.expires_after(133ms);
	timer.async_wait(std::bind(&PARoom::sendGameData, this, asio::placeholders::error));
}

void PARoom::setInGame(Player *player, bool inGame) {
	PlayerState& state = playerState[getPlayerIndex(player)];
	state.inGame = inGame;
}

void PARoom::gameStop(Player *player)
{
	std::error_code ec;
	timer.cancel(ec);
	timerStarted = false;
	startState = 0;

	PlayerState& state = playerState[getPlayerIndex(player)];
	// Reset player ready flag
	state.flags &= ~1;
}

void PARoom::resetState()
{
	rngSeed = (uint32_t)time(nullptr);
	talkingSlot = 0xff;
	audioSeq = 1;
	for (PlayerState& state : playerState)
	{
		state.data = {};
		state.seqnum = 0;

		state.score = 0;
		state.flightDist = 0.f;
		state.kills = 0;
		state.deaths = 0;
		state.wins = 0;
		state.rankUpdated = false;
	}
}

void PARoom::sendRankUpdate(Player *player, uint32_t flightTime, Packet& packet)
{
	if ((flags & 2) != 0)
	{
		// Ranking match
		uint8_t slot = getPlayerIndex(player);
		PlayerState& state = playerState[slot];
		if (!state.rankUpdated)
		{
			auto it = std::max_element(playerState.begin(), playerState.begin() + players.size(), [](const PlayerState& l, const PlayerState& r) {
				return l.score < r.score;
			});
			uint8_t bestScore = it->score;
			if (state.score == bestScore)
				state.wins++;
			RankAcceptor::Instance->updateRank(player->getName(), state.kills, state.wins, 1,
					flightTime / 30, std::round(state.flightDist), state.deaths, state.score);
			state.rankUpdated = true;
		}
		packet.init(Packet::REQ_CHAT);
		packet.flags |= Packet::FLAG_RUDP;
		packet.writeData(OUT_UPDATE_SCORE);
		packet.writeData(slot);
		packet.writeData((uint16_t)0);
		packet.writeData((uint32_t)std::round(state.flightDist));
		packet.writeData(flightTime / 30);
		packet.writeData((uint32_t)state.score);
		packet.writeData(state.kills);
		packet.writeData(state.deaths);
		packet.writeData(state.wins);
	}
	else {
		packet.init(Packet::REQ_NOP);
	}
}

void PARoom::sendPlayerList(Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(OUT_PLAYER_LIST);
	packet.writeData(startState);

	uint8_t pdata[14] {};
	// Human planes
	for (unsigned i = 0; i < players.size(); i++)
	{
		const PARoom::PlayerState& state = playerState[i];
		unsigned shift = (i & 1) * 4;
		pdata[i / 2 + 0] |= state.plane << shift;
		pdata[i / 2 + 3] |= state.flags << shift;
		pdata[i / 2 + 6] |= controllingSlot(i) << shift; 	// slot
		pdata[i / 2 + 9] |= state.rank << shift;
	}
	// AI planes
	for (unsigned i = players.size(); i < 6; i++)
	{
		const PARoom::PlayerState& state = playerState[i];
		unsigned shift = (i & 1) * 4;
		// AI plane types are randomly chosen, or the type used by the last player in this slot if any
		pdata[i / 2 + 0] |= state.plane << shift;
		// controlling slot for AI planes: assign to each player in a round-robin fashion
		pdata[i / 2 + 6] |= controllingSlot(i) << shift;
	}
	packet.writeData(pdata, sizeof(pdata));
	for (unsigned i = 0; i < 6; i++)
		if (i < players.size())
			packet.writeData(players[i]->getId());
		else
			packet.writeData(~0u);
}

void PARoom::sendRoomAttrs(Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(OUT_SET_ROOM_ATTRS);
	packet.writeData((uint8_t)((maxPoints << 4) | stage));
	packet.writeData(field2);
	packet.writeData(field3);
	packet.writeData(flags);
	packet.writeData((uint8_t)0);
	const uint8_t owner = 1 << getPlayerIndex(this->owner);
	packet.writeData(owner);
	packet.writeData((uint8_t)0xac);
}

void PARoom::sendRngSeed(Packet& packet)
{
	packet.init(Packet::REQ_CHAT);
	packet.flags |= Packet::FLAG_RUDP;
	packet.writeData(OUT_SET_RNG_SEED);
	packet.writeData("", 3);
	packet.writeData(rngSeed);
}

Room *PropellerServer::addRoom(const std::string& name, uint32_t attributes, Player *owner)
{
	uint32_t id = nextRoomId++;
	PARoom *room = new PARoom(*owner->getLobby(), id, name, attributes, owner, io_context);
	owner->getLobby()->addRoom(room);

	return room;
}

void PropellerServer::sendPlayerAttrs(Player *player)
{
	PARoom *room = (PARoom *)player->getRoom();
	if (room == nullptr)
		return;

	replyPacket.init(Packet::REQ_CHAT);
	replyPacket.flags |= Packet::FLAG_RUDP;
	replyPacket.writeData(OUT_SET_PLAYER_ATTRS);
	const PARoom::PlayerState& state = room->getPlayerState(room->getPlayerIndex(player));
	replyPacket.writeData(state.plane);
	replyPacket.writeData(state.flags);
	replyPacket.writeData(state.rank);
	replyPacket.writeData(player->getId());
}

static void dumpGameData(Player *player, int slot, const uint8_t *data)
{
	printf("[%6s] gamedata[%d]: C ", player == nullptr ? "" : player->getName().c_str(), slot);
//	for (int i = 0; i < 4; i++)
//		printf("%02x ", data[i]);
//	printf(" B ");
//	for (int i = 0; i < 4; i++)
//		printf("%02x ", data[4 + i]);
//	printf(" C ");
	//for (int i = 1; i < 4; i++)
	//	printf("%c", data[8 + i] == data[8 + i - 1] ? '*' : ' ');
	for (int i = 0; i < 4; i++)
		printf("%02x ", data[8 + i]);
	printf(" D ");
	for (int i = 0; i < 4; i++)
		if (data[0xc + i] != 0x1e)
			printf("%02x ", data[0xc + i]);
	printf(" E ");
	for (int i = 0; i < 4; i++)
		printf("%c", data[0x18 + i] == 3 ? 'X' : data[0x18 + i] == 1 ? '*' : ' ');
	printf(" F ");
	for (int i = 0; i < 4; i++)
		//printf("%c", data[0x1c + i] ? '.' : ' ');
		printf("%02x ", data[0x1c + i]);
	if (data[0x20] != 0 || data[0x21] != 0 || data[0x22] != 0 || data[0x23] != 0)
	{
		printf(" G ");
		for (int i = 0; i < 4; i++)
			printf("%02x ", data[0x20 + i]);
	}
	if (data[0x24] != 0 || data[0x25] != 0 || data[0x26] != 0 || data[0x27] != 0)
	{
		printf(" H ");
		for (int i = 0; i < 4; i++)
			printf("%02x ", data[0x24 + i]);
	}
	printf(" Z ");
	printf("%02x", data[0x28]);
	if (data[0x28] != 0xff)
		printf(" ang: %d, %d, %d pos: %d, %d, %d",
				(int16_t)read16(data, 0x2a), (int16_t)read16(data, 0x2c), (int16_t)read16(data, 0x2e),
				(int16_t)read16(data, 0x30), (int16_t)read16(data, 0x32), (int16_t)read16(data, 0x34));
	if (read32(data, 0x38) != 0)
		printf(" FLAG %d", read32(data, 0x38));
	for (int i = 0; i < 4; i++)
		if (data[0x10 + i] != 0xff)
		{
			printf(" D2 %02x %02x %02x %02x", data[0x10], data[0x11], data[0x12], data[0x13]);
			break;
		}
	for (int i = 0; i < 4; i++)
		if (data[0x14 + i] != 0xff)
		{
			printf(" D3 %02x %02x %02x %02x", data[0x14], data[0x15], data[0x16], data[0x17]);
			break;
		}
	printf("\n");

	// 0c 01 00 00 00 01 00 00 00 00 00 00 00 00 00 00 ................
	//   slot        seq       A           B
	// 32 32 32 32 1e 1e 1e 1e ff ff ff ff ff ff ff ff 2222............
	// C           D           n/a
	// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
	// E           F           n/a
	// ff 00 00 00 00 80 00 00 00 00 58 1b 21 d9 00 00 ..........X.!...
	// by    short short short short short short
	// 8ae   pl[c] pl[d] pl[e] p[51] p[52] p[53]        c,d,e: angles, 51,52,53: position?
	// 00 00 00 00 ....
	// long (flag?)
}

bool PropellerServer::handlePacket(Player *player, const uint8_t *data, size_t len)
{
	uint16_t flags = read16(data, 0);
	if (flags & Packet::FLAG_ACK)
		player->ackRUdp(read32(data, 0xc));

	// TODO? if (data[3] == Packet::REQ_CHAT)

	if (data[3] != Packet::REQ_GAME_DATA)
		return false;

	PARoom *room = (PARoom *)player->getRoom();
	if (room == nullptr)
		return true;

	uint8_t type = data[0x10];
	switch (type)
	{
	case IN_SET_PLAYER_ATTRS: // Select plane, change player flags
		// FIXME some players are setting all to 0 at game start (payload 0x44 bytes)
		DEBUG_LOG(game, "[%s] SET PLAYER ATTRS %02x %02x %02x %02x %02x %02x %02x", player->getName().c_str(),
				data[0x11], data[0x12], data[0x13], data[0x14], data[0x15], data[0x16], data[0x17]);
		// [0] 00 00 08 01 10 00 00
		// d[1] plane type (0)
		// d[2] flags (1: ready | 2:has mike | 4)
		// d[3] rank (8)
		// d[4-7] playerId
		if (*(uint32_t *)&data[0x14] == player->getId())
		{
			room->setPlane(player, data[0x11]);
			room->setPlayerFlags(player, data[0x12]);
			room->setRank(player, data[0x13]);

			replyPacket.init(Packet::REQ_CHAT);
			replyPacket.flags |= Packet::FLAG_RUDP;
			replyPacket.ack(read32(data, 8));
			replyPacket.writeData(OUT_ACK_PLAYER_ATTRS);
			replyPacket.writeData("", 3);
			replyPacket.writeData(room->getOwner()->getId());

			room->sendPlayerList(replyPacket);
			room->sendPlayerList(relayPacket);
		}
		break;

	case IN_GET_ROOM_ATTRS:
		DEBUG_LOG(game, "[%s] GET ROOM ATTRS", player->getName().c_str());
		room->sendRoomAttrs(replyPacket);
		replyPacket.ack(read32(data, 8));
		// FIXME owner gets network error after sending this *4 when game ends
		// with 3+ players all guests fail after game with network error
		break;

	case IN_GAME_STARTING: // ready to start? from owner
		{
			// 02 01 01 00
			// 02 00 02 00
			DEBUG_LOG(game, "[%s] GAME STARTING %02x %02x", player->getName().c_str(), data[0x11], data[0x12]);

			if (data[0x12] == 1)
				room->resetState();
			room->setStartState(data[0x12]);

			room->sendPlayerList(replyPacket);
			replyPacket.ack(read32(data, 8));
			room->sendPlayerList(relayPacket);
			break;
		}

	case IN_SET_ROOM_ATTRS: // Set game settings. Sent every 5 s by owner
		{
			//00000608  a0 18 00 11 00 00 10 01  00 00 00 06 00 00 00 00   ........ ........
			//00000618  03 a0 00 00 70 00 00 ac  ba 47 66 10               ....p... .Gf.
			//          payload     ntohl
			//00000700  a0 18 00 11 00 00 10 01  00 00 00 09 00 00 00 00   ........ ........
			//00000710  03 a0 00 00 70 00 00 ac  ba 47 66 10               ....p... .Gf.
			// d[1] >> 4: points
			// d[1] & f: stage - 1
			// d[4] & 0x10: yellow option
			// d[4] & 0x20: green option
			// d[4] & 0x40: red option
			// d[4] & 0x02: ranking
			// d[7] == AC
			DEBUG_LOG(game, "[%s] SET ROOM ATTRS %02x %02x %02x %02x %02x %02x %02x", player->getName().c_str(),
					data[0x11], data[0x12], data[0x13], data[0x14], data[0x15], data[0x16], data[0x17]);
			room->updateSettings(data[0x11] & 0xf, data[0x11] >> 4, data[0x14], data[0x12], data[0x13]);
			room->sendRoomAttrs(replyPacket);
			replyPacket.ack(read32(data, 8));
			room->sendRoomAttrs(relayPacket);
			break;
		}

	case IN_GAME_OVER: // End of game (sent by owner for ranking games)
		// TODO looks more like a game abort thing. Sent combined with LOBBY LOGOUT in case of error
		DEBUG_LOG(game, "[%s] GAME OVER", player->getName().c_str());
		// flags a000
		replyPacket.init(Packet::REQ_NOP);
		replyPacket.ack(read32(data, 8));
		room->gameStop(player);
		break;

	case IN_GAME_START:	// Start game
		DEBUG_LOG(game, "[%s] GAME START", player->getName().c_str());
		room->setInGame(player, true);
		// FIXME also sent by the new room master (?) after a player disconnects in game
		// send rng seed
		room->sendRngSeed(replyPacket);
		replyPacket.ack(read32(data, 8));
		break;

	case IN_GAME_STOP: // End game
		DEBUG_LOG(game, "[%s] GAME STOP", player->getName().c_str());
		// flags a000
		// 07 bf ef 0c
		room->setInGame(player, false);
		replyPacket.init(Packet::REQ_NOP);
		replyPacket.ack(read32(data, 8));
		room->gameStop(player);
		break;

	case IN_GAME_CDATA: // in game data for AI/computer planes
		//DEBUG_LOG(game, "[%s] gamedata[A] flags %4x slot %d", player->getName().c_str(), (data[0] >> 2) << 10, data[0x11]);
		//dumpGameData(nullptr, data[0x11], data + 0x18);
		// 0a 03 00 00 00 02 00 00 00 00 00 00 00 00 00 00 ................
		//   slot
		// 32 32 32 32 1e 1e 1e 1e ff ff ff ff ff ff ff ff 2222............
		// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
		// 00 00 00 00 00 a0 00 00 b3 e4 58 1b b3 e4 00 00 ..........X.....
		// 00 00 00 00 ....
		room->setStateData(data[0x11], data + 0x18);
		break;

	case IN_GAME_HDATA2: // in game data for human planes (with audio? doesn't look like it), len 6c
		//DEBUG_LOG(game, "[%s] gamedata[B] flags %4x slot %d", player->getName().c_str(), (data[0] >> 2) << 10, data[0x11]);
		//dumpData(data + 0x10, len - 0x10);
		//dumpGameData(player, data[0x11], data + 0x40);
		// 0b 00 00 00 00 69 00 00 00 00 00 00 00 00 00 00 .....i..........
		//               seq [audio #1
		// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
		//                     ...audio] [audio #2
		// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
		//                                 ...audio]
		// 00 00 00 00 00 00 00 00 34 32 34 32 1e 1e 1e 1e ........4242....
		// same as C[8]
		// ff ff ff ff ff ff ff ff 00 00 00 00 00 00 01 01 ................
		// 00 00 00 00 00 00 00 00 ff 00 5e f2 62 03 ce ff ..........^.b...
		// 78 fe 9d 1a 46 01 00 00 00 00 00 00             x...F.......
		// With audio
		//0010   0b 00 00 00 00 53 a1 0e f4 cc 0a 7b 54 e5 af 06   .....S.....{T...
		//                     seq [audio #1
		//0020   72 41 0d 0c 11 a5 d6 60 a4 fe a9 d2 f5 c0 47 a1   rA.....`......G.
		//                                   ] [audio #2
		//0030   42 d5 3a 11 aa 8e 0e 89 fb 18 1f 23 80 df 00 00   B.:........#....
		//                                               ]
		//0040   00 00 00 00 00 00 00 00 34 32 34 32 1e 1e 1e 1e   ........4242....
		//0050   ff ff ff ff ff ff ff ff 00 00 00 00 40 40 40 40   ............@@@@
		//0060   00 00 00 00 00 00 00 00 ff 00 00 00 01 22 00 00   ............."..
		//0070   98 f1 58 1b 9a 0e 00 00 00 00 00 00 20 54 00 11   ..X......... T..
		//0080   00 00 10 02 00 00 01 1b 00 00 00 00 0a 02 00 00   ................
		//0090   00 53 00 00 0c 0d 0f 10 b8 b8 ba bc 30 31 32 33   .S..........0123
		//00a0   1e 1e 1e 1e ff ff ff ff ff ff ff ff 00 00 00 00   ................
		//00b0   00 00 00 00 00 00 00 00 00 00 00 00 05 00 21 fd   ..............!.
		//00c0   b1 f5 74 f5 31 02 ca 19 86 fd 00 00 00 00 00 00   ..t.1...........
		//00d0   20 54 00 11 00 00 10 02 00 00 01 1c 00 00 00 00    T..............
		//00e0   0a 04 00 00 00 53 00 00 f3 f2 f1 f0 a6 a6 a7 a8   .....S..........
		//00f0   44 45 44 45 1e 1e 1e 1e ff ff ff ff ff ff ff ff   DEDE............
		//0100   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ................
		//0110   01 00 2c 16 6b 9f 74 dc 4d 04 f0 1a 53 fa 00 00   ..,.k.t.M...S...
		//0120   00 00 00 00 ba 47 66 10                           .....Gf.

		room->setStateData(data[0x11], data + 0x40);
		room->setAudio(data[0x11], data + 0x16);
		break;

	case IN_GAME_HDATA: // in game data for human planes
		//DEBUG_LOG(game, "[%s] gamedata[C] flags %4x slot %d", player->getName().c_str(), (data[0] >> 2) << 10, data[0x11]);
		//dumpData(data + 0x10, len - 0x10);
		//dumpGameData(player, data[0x11], data + 0x18);
		// 0c 01 00 00 00 01 00 00 00 00 00 00 00 00 00 00 ................
		//   slot        seq       plane data...
		// 32 32 32 32 1e 1e 1e 1e ff ff ff ff ff ff ff ff 2222............
		//             constants   constants
		// 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ................
		// constants   talk: & 10
		// ff 00 00 00 00 80 00 00 00 00 58 1b 21 d9 00 00 ..........X.!...
		// by    short short short short short short
		// 8ae   pl[c] pl[d] pl[e] p[51] p[52] p[53]        c,d,e: angles, 51,52,53: position?
		// 00 00 00 00 ....
		// long (flag?)
		// matches handle 1C/1D
		room->setStateData(data[0x11], data + 0x18);
		room->setAudio(data[0x11], nullptr);
		break;

	case IN_GAME_ENDED: // Game ended, sent by all players
		DEBUG_LOG(game, "[%s] GAME ENDED", player->getName().c_str());
		// flags a000
		// 0e 00 ef 0c 00 00 00 0c 00 00 23 28 02 00 00 00 ..........#(....
		// 0a 20 0c 0c 38 bf ef 0c 60 29 0c 0c             . ..8...`)..
		//
		// 0e 01 ef 0c 00 00 00 00 00 00 00 12 02 00 00 00 ................
		//   slot
		// 0a 20 0c 0c 38 bf ef 0c 60 29 0c 0c             . ..8...`)..
		//
		// 0e 01 ef 0c 00 00 00 00 00 00 00 0a 02 00 00 00 ................
		//    u8       int.BE..... int.BE.....
		//                         flight time (frames)
		// 0a 20 0c 0c 38 bf ef 0c 60 29 0c 0c             . ..8...`)..
		room->gameStop(player);
		room->sendRankUpdate(player, read32(data, 0x18), replyPacket);
		replyPacket.ack(read32(data, 8));
		break;


	default:
		DEBUG_LOG(game, "[%s] gamedata[%02x] ? flags %4x", player->getName().c_str(), data[0x10], (data[0] >> 2) << 10);
		dumpData(data + 0x10, len - 0x10);
		break;
	}
	return true;
}

// SENDING
// more than 1 msg per packet
// d[0] code
//		F	size 8		byte (plane) byte (flags 1|2|4) byte (rank? [0-f[) int (player id)
//		10	size 0x28	byte byte
//                      	{ packed 4-bit values * 6: plane flags ? ?-same-as-f } pad pad int (player id) * 6
//		12	size 8		same format as recv_03: point+stage ? ? flags ... AC (hardcoded)
//						new game settings?
//						d[6] & 0x3f: more flags? user slot used?
//		13	sz >= 4		? ? ? (ignored) int	(srand seed)
//		14	sz 0x68?	byte (slot) byte (seq?) byte  byte*3 (sizes, max 0x18) (pad) AUDIO
//                      can also be sent w/o rudp
//		15	size 4		bool (sets a flag, doesn't seem to be used) should be sent when 9 is received (not used)
//		16	size 4		byte (user idx) byte (sets DAT_0c5a5ee8, not read? reset to 0 on recv 1a) sent when player joins?
//		18	size 8		? ? ? (ignored) int (player id?, same effect as flag in msg 12)
//		1a	?			no payload, sets flags, doesn't seem used
//		1e	size 1c		byte ? ? int (flight dist) int (flight time) int (total pts) int (total kills) int (shot down count) int (wins count)
//						update rank data, values are deltas, number of games not included (incremented by one)
//
// !reliable:
//		1c	up to CC?	p[1,2]		kept, p[1] tested against FF
//									p[2] flag (some index += 6 if 1, audio related?)
//						p[3-16]		audio data 1
//						p[17-2a]	audio data 2
//                                  loop 0-2
//						p[2b-2d]	array of slots, if ff => loop stops
//						p[2e-30]	array of scores
//						p[32,+3*2]	short array (BE), checks if increasing
//                      p[38,+3*3c] +28: ?(short) +2a,2c,2e: angles, +30,32,34: plane position (same as C offset 30, up to 3c here)
//						(p[54,+3*3c])
//		1d	up to E4	p[1] 		kept, tested against ff (same as 1c)
//						p[2-5]		array of slots
//						p[6-9]		array of scores
//						p[a-11]		array of 4 shorts (BE)
//						p[14,+4*3c?]	plane pos data (same as C offset 30)
//

void RankConnection::onReceive(const std::error_code& ec, size_t len)
{
	if (ec || len == 0)
	{
		if (ec && ec != asio::error::eof)
			ERROR_LOG(Game::PropellerA, "rank: %s", ec.message().c_str());
		socket.shutdown(asio::ip::tcp::socket::shutdown_both);
		return;
	}
	if (len >= 0x34)
	{
		std::string username = (const char *)&recvBuffer[0x14];
		Statement stmt(database, "SELECT kills, wins, games, flightTime, flightDistance, shotDown, points, rank from ranking where user_id = ?");
		stmt.bind(1, username);
		// total kills, number of wins, number of games, total flight time, total flight distance,
		// number of times shot down, total points, rank
		// ranks: 0: none, 1:general, 2:lieutenant general, 3:major general, 4:colonel
		// 		5:lieutenant colonel, 6:major, 7:captain, 8:first lieutenant, 9:second lieutenant,
		// 		10:sergeant, 11:senior airman, 12:airman first class, 13:airman, 14:airman basic, 15:none...
		// after game: 1st: 10 pts, 2nd: 5 pts, 3rd: 2 pts
		std::vector<uint32_t> data(8);
		if (!stmt.step()) {
			data[7] = ntohl(14);
		}
		else {
			for (int i = 0; i < 8; i++)
				data[i] = ntohl(stmt.getIntColumn(i));
		}
		send(data);
	}
	receive();
}

RankAcceptor *RankAcceptor::Instance;

RankAcceptor::RankAcceptor(asio::io_context& io_context, const std::string& dbpath)
	: io_context(io_context),
	  acceptor(asio::ip::tcp::acceptor(io_context,
			asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 10100)))
{
	asio::socket_base::reuse_address option(true);
	acceptor.set_option(option);
	database.open(dbpath);
	Statement stmt(database, "SELECT name FROM sqlite_master WHERE type='table' AND name='ranking'");
	if (!stmt.step()) {
		WARN_LOG(Game::PropellerA, "Table 'ranking' doesn't exist. Creating it");
		database.exec("CREATE TABLE ranking (id INTEGER PRIMARY KEY AUTOINCREMENT, "
				"user_id VARCHAR(32) UNIQUE, "
				"kills INTEGER DEFAULT 0, "
				"wins INTEGER DEFAULT 0, "
				"games INTEGER DEFAULT 0, "
				"flightTime INTEGER DEFAULT 0, "
				"flightDistance INTEGER DEFAULT 0, "
				"shotDown INTEGER DEFAULT 0, "
				"points INTEGER DEFAULT 0, "
				"rank INTEGER DEFAULT 14)");
	}
	Instance = this;
}

void RankAcceptor::updateRank(const std::string& name, int kills, int wins, int games,
		int flightTime, int flightDistance, int shotDown, int points)
{
	Statement stmt(database, "UPDATE ranking SET kills = kills + ?, wins = wins + ?, games = games + ?,"
			"flightTime = flightTime + ?, flightDistance = flightDistance + ?, shotDown = shotDown + ?, points = points + ?"
			"WHERE user_id = ?");
	stmt.bind(1, kills);
	stmt.bind(2, wins);
	stmt.bind(3, games);
	stmt.bind(4, flightTime);
	stmt.bind(5, flightDistance);
	stmt.bind(6, shotDown);
	stmt.bind(7, points);
	stmt.bind(8, name);
	stmt.step();
	if (stmt.changedRows() == 0)
	{
		Statement stmt(database, "INSERT INTO ranking (kills, wins, games, flightTime, flightDistance, shotDown, points, user_id)"
				"VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
		stmt.bind(1, kills);
		stmt.bind(2, wins);
		stmt.bind(3, games);
		stmt.bind(4, flightTime);
		stmt.bind(5, flightDistance);
		stmt.bind(6, shotDown);
		stmt.bind(7, points);
		stmt.bind(8, name);
		stmt.step();
	}
}
