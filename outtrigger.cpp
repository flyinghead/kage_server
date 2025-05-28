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
#include "outtrigger.h"
#include "log.h"

using namespace std::chrono_literals;

constexpr int TimeLimits[] { 120, 140, 160, 180, 200, 220, 240, 260, 280, 300, 360, 420, 480, 600, 900, 1200, -1 };

Room *OuttriggerServer::addRoom(const std::string& name, uint32_t attributes, Player *owner)
{
	uint32_t id = nextRoomId++;
	OTRoom *room = new OTRoom(*owner->getLobby(), id, name, attributes, owner, io_context);
	owner->getLobby()->addRoom(room);

	return room;
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

			OTRoom *room = (OTRoom *)player->getRoom();
			if (room != nullptr)
			{
				OTRoom::sysdata_t sysdata;
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

			OTRoom *room = (OTRoom *)player->getRoom();
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
			OTRoom *room = (OTRoom *)player->getRoom();
			if (room != nullptr)
				room->setGameData(player, &data[0x12]);
			break;
		}

	case TagCmd::RESULT:
		{
			INFO_LOG(game, "tag: RESULT from %s", player->getName().c_str());
			replyPacket.init(Packet::REQ_NOP);
			replyPacket.ack(read32(data, 8));

			OTRoom *room = (OTRoom *)player->getRoom();
			if (room != nullptr && room->setResult(player, &data[0x12]))
			{
				// Make sure we ack before anything else
				player->send(replyPacket);
				replyPacket.reset();
				// Send RESULT2
				INFO_LOG(game, "%s: Sending RESULT2 to all players", room->getName().c_str());
				std::vector<OTRoom::result_t> results  = room->getResults();
				Packet result2;
				result2.init(Packet::REQ_CHAT);
				result2.flags |= Packet::FLAG_RUDP;
				TagCmd tag;
				tag.command = TagCmd::RESULT2;
				result2.writeData(tag.full);
				for (const OTRoom::result_t& result : results)
					result2.writeData(result.data(), result.size());
				Player::sendToAll(result2, room->getPlayers());
			}
			break;
		}

	case TagCmd::RESET:
		{
			WARN_LOG(game, "tag: RESET from %s", player->getName().c_str());
			OTRoom *room = (OTRoom *)player->getRoom();
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

void OTRoom::setAttributes(uint32_t attributes)
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
	Room::setAttributes(attributes);
}

void OTRoom::onRemovePlayer(Player *player, int index)
{
	if (roomState == SyncStarted)
	{
		PlayerState& state = getPlayerState(index);
		if (state.state == PlayerState::Ready)
			// Allow the game to start
			rudpAcked(player);
		state.state = PlayerState::Gone;
	}
	else if (roomState == InGame)
	{
		PlayerState& state = getPlayerState(index);
		state.state = PlayerState::Gone;
	}
}

std::vector<OTRoom::sysdata_t> OTRoom::getSysData() const
{
	std::vector<sysdata_t> sysdata;
	sysdata.reserve(playerState.size());
	for (const auto& state : playerState)
		sysdata.push_back(state.sysdata);
	return sysdata;
}

void OTRoom::setSysData(const Player *player, const sysdata_t& sysdata)
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

bool OTRoom::setReady(const Player *player)
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

OTRoom::PlayerState& OTRoom::getPlayerState(unsigned index)
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

void OTRoom::setGameData(const Player *player, const uint8_t *data)
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

void OTRoom::sendGameData(const std::error_code& ec)
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
	timer.async_wait(std::bind(&OTRoom::sendGameData, this, asio::placeholders::error));
}

void OTRoom::sendGameOver()
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

std::vector<OTRoom::result_t> OTRoom::getResults() const
{
	std::vector<result_t> results;
	results.reserve(playerState.size());
	for (const PlayerState& state : playerState)
		results.push_back(state.result);
	return results;
}

bool OTRoom::setResult(const Player *player, const uint8_t *data)
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

void OTRoom::reset()
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

void OTRoom::startSync()
{
	roomState = SyncStarted;
	for (Player *pl : players)
		pl->notifyRoomOnAck();
}

void OTRoom::endGame()
{
	std::error_code ec;
	timer.cancel(ec);
	timeLimit.cancel(ec);
	roomState = Result;
}

void OTRoom::rudpAcked(Player *player)
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
		std::vector<OTRoom::sysdata_t> sysdata  = getSysData();
		Packet sys2;
		sys2.init(Packet::RSP_TAG_CMD);
		sys2.flags |= Packet::FLAG_RUDP;
		sys2.writeData(0u);	// list: count [int ...]
		TagCmd tag;
		tag.command = TagCmd::SYS2;
		tag.player = (uint16_t)sysdata.size();
		sys2.writeData(tag.full);
		for (const OTRoom::sysdata_t& data : sysdata)
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
