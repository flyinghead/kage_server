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
#include <stdint.h>
#include <string.h>
#include <stdexcept>
#include <arpa/inet.h>

enum class Game
{
	None = -1,
	Bomberman = 0,
	Outtrigger = 1,
	PropellerA = 2,
};

static inline uint16_t read16(const uint8_t *p, unsigned offset) {
	return ntohs(*(const uint16_t *)&p[offset]);
}
static inline uint32_t read32(const uint8_t *p, unsigned offset) {
	return ntohl(*(const uint32_t *)&p[offset]);
}
static inline void write16(uint8_t *p, unsigned offset, uint16_t v) {
	*(uint16_t *)&p[offset] = htons(v);
}
static inline void write32(uint8_t *p, unsigned offset, uint32_t v) {
	*(uint32_t *)&p[offset] = htonl(v);
}

class Packet
{
public:
	enum Command : uint8_t {
		REQ_BOOTSTRAP_LOGIN = 0x2c,

		REQ_NOP = 0,
		REQ_LOBBY_LOGIN = 1,
		REQ_LOBBY_LOGOUT = 2,
		REQ_CREATE_ROOM = 4,
		REQ_JOIN_LOBBY_ROOM = 6,
		REQ_LEAVE_LOBBY_ROOM = 7,
		REQ_CHG_ROOM_STATUS = 8,
		REQ_QRY_USERS = 0xa,
		REQ_QRY_ROOMS = 0xb,
		REQ_CHG_USER_PROP = 0xc,
		REQ_CHG_USER_STATUS = 0xd,
		REQ_QRY_LOBBIES = 0xe,
		REQ_CHAT = 0xf,
		REQ_GAME_DATA = 0x11,
		REQ_PING = 0x14,

		RSP_TAG_CMD = 0x10,
		RSP_OK = 0x28,
		RSP_FAILED = 0x27,
		RSP_LOGIN_SUCCESS2 = 0x29,
		RSP_LOGIN_SUCCESS = 0x2d,
	};
	enum {
		FLAG_RELAY = 0x400,
		FLAG_CONTINUE = 0x800,
		FLAG_LOBBY = 0x1000,
		FLAG_UNKNOWN = 0x2000,
		FLAG_ACK = 0x4000,
		FLAG_RUDP = 0x8000,
	};

	uint8_t data[0x800] {};
	uint16_t size = 0x10;
	uint16_t startOffset = 0;
	uint16_t flags = FLAG_UNKNOWN;
	Command type = REQ_NOP;

	void reset()
	{
		startOffset = 0;
		size = 0x10;
		this->type = REQ_NOP;
		memset(data, 0, sizeof(data));
		flags = FLAG_UNKNOWN;
	}

	void init(Command type)
	{
		if (!empty()) {
			finalize();
			append(type);
		}
		else {
			reset();
			this->type = type;
		}
	}
	void respOK(Command type) {
		init(RSP_OK);
		writeData((uint32_t)type);
	}
	void respFailed(Command type) {
		init(RSP_FAILED);
		writeData((uint32_t)type);
	}

	void writeData(uint32_t v) {
		write32(data, size, v);
		size += sizeof(v);
	}
	void writeData(uint16_t v) {
		write16(data, size, v);
		size += sizeof(v);
	}
	void writeData(uint8_t v) {
		data[size] = v;
		size += sizeof(v);
	}
	void writeData(const uint8_t *data, int size) {
		memcpy(&this->data[this->size], data, size);
		this->size += size;
	}
	void writeData(const char *str, int size)
	{
		strncpy((char *)&this->data[this->size], str, size);
		int l = std::min<int>(size, strlen(str));
		size -= l;
		this->size += l;
		memset(&this->data[this->size], 0, size);
		this->size += size;
	}

	void ack(uint32_t seq) {
		flags |= Packet::FLAG_ACK;
		write32(data, startOffset + 0xc, seq);
	}

	size_t finalize()
	{
		const uint16_t chunkSize = size - startOffset;
		if (chunkSize > 0x3ff)
			throw std::runtime_error("Packet too big");
		write16(data, startOffset, flags | chunkSize);
		data[startOffset + 3] = type;
		memcpy(&data[size], &ServerTag, sizeof(ServerTag));
		return size + sizeof(ServerTag);
	}

	void append(Command type)
	{
		if (startOffset == 0)
			write16(data, 0, read16(data, 0) | FLAG_CONTINUE);
		startOffset = size;
		// reset server tag to 0
		memset(&data[size], 0, sizeof(ServerTag));
		size += 0x10;
		this->type = type;
		flags = FLAG_UNKNOWN;
	}

	bool empty() const {
		return size == 0x10 && flags == FLAG_UNKNOWN && type == REQ_NOP && startOffset == 0;
	}

	static constexpr uint32_t ServerTag = 0x006647BA;
};

// Outtrigger game data

struct TagCmd
{
	enum {
		SYNC = 0,
		SYS = 1,
		SYS2 = 2,
		SYS_OK = 3,
		START_OK = 4,
		READY = 5,
		GAME_START = 6,
		GAME_OVER = 7,
		JOIN_OK = 8,
		JOIN_NG = 9,
		PAUSE = 0xa,
		WAIT_OVER = 0xb,
		RESULT = 0xc,
		RESULT2 = 0xd,
		OWNER = 0xe,
		ECHO = 0xf,
		RESET = 0x10,
		TIME_OUT = 0x11,
	};

	TagCmd(uint16_t v = 0) {
		full = v;
	}

	union {
		struct {
			uint16_t :3;
			uint16_t id:3;		// seq# ?
			uint16_t player:4;	// == playerCount in SYS2
			uint16_t command:6;
		};
		uint16_t full;
	};
};
