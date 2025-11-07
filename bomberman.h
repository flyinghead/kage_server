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
#include "model.h"

union UdpCommand
{
	uint16_t full;
	struct {
		uint16_t size:9;
		uint16_t command:7;
	};
};

class BMRoom : public Room
{
public:
	BMRoom(Lobby& lobby, uint32_t id, const std::string& name, uint32_t attributes, Player *owner, asio::io_context& io_context);

	void addPlayer(Player *player) override;
	bool removePlayer(Player *player) override;
	uint32_t getPlayerCount() const override;
	void createJoinRoomReply(Packet& reply, Packet& relay, Player *player) override;

	uint32_t getHostCount() const {
		return (uint32_t)players.size();
	}
	int getSlotCount(const Player *player) const;
	int getPlayerPosition(const Player *player) const;

	const std::array<uint8_t, 9>& getRules() const {
		return rules;
	}
	void setRules(const uint8_t *p) {
		memcpy(rules.data(), p, rules.size());
	}

private:
	void updateSlots();
	void sendUdpPacketA(Packet& packet, Player *player);

	std::vector<int> slots;	// slots used by each player
	asio::steady_timer timer;
	std::array<uint8_t, 9> rules {};
};

class BombermanServer : public LobbyServer
{
public:
	BombermanServer(uint16_t port, asio::io_context& io_context);

	Room *addRoom(const std::string& name, uint32_t attributes, Player *owner) override;

protected:
	bool handlePacket(Player *player, const uint8_t *data, size_t len) override;

private:
	time_point joinRoomTime {};
};
