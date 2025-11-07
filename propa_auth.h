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
#include "log.h"
#include "shared_this.h"
#include "asio.h"
extern "C" {
#include "blowfish.h"
}
#include <array>
#include <string>
#include <vector>

constexpr Game game = Game::PropellerA;

static inline void xor55(uint8_t *data, size_t len)
{
	uint8_t *end = data + len;
	while (data != end)
		*data++ ^= 0x55;
}

class AuthConnection : public SharedThis<AuthConnection>
{
public:
	asio::ip::tcp::socket& getSocket() {
		return socket;
	}

	void receive() {
		// TODO packet matcher, use DynamicBuffer?
		asio::async_read(socket, asio::buffer(recvBuffer), asio::transfer_at_least(0x68),
				std::bind(&AuthConnection::onReceive, shared_from_this(), asio::placeholders::error, asio::placeholders::bytes_transferred));
	}

private:
	AuthConnection(asio::io_context& io_context)
		: socket(io_context) {}

	void send()
	{
		asio::async_write(socket, asio::buffer(sendBuffer),
			std::bind(&AuthConnection::onSent, shared_from_this(),
					asio::placeholders::error,
					asio::placeholders::bytes_transferred));
	}
	void onSent(const std::error_code& ec, size_t len)
	{
		if (ec) {
			ERROR_LOG(game, "Send error: %s", ec.message().c_str());
		}
		else {
			DEBUG_LOG(game, "sent %zd bytes", len);
			receive();
		}
	}

	void initBlowfish(const uint8_t *key) {
		Blowfish_Init(&blowfishCtx, (uint8_t *)key, KEY_SIZE);
	}

	void encrypt(uint8_t *data, size_t len)
	{
		for (size_t i = 0; i < len; i += 8)
		{
			uint32_t l = read32(data, i);
			uint32_t r = read32(data, i + 4);
			Blowfish_Encrypt(&blowfishCtx, &l, &r);
			write32(data, i, l);
			write32(data, i + 4, r);
		}
	}

	void decrypt(uint8_t *data, size_t len)
	{
		for (size_t i = 0; i < len; i += 8)
		{
			uint32_t l = read32(data, i);
			uint32_t r = read32(data, i + 4);
			Blowfish_Decrypt(&blowfishCtx, &l, &r);
			write32(data, i, l);
			write32(data, i + 4, r);
		}
	}

	void onReceive(const std::error_code& ec, size_t len)
	{
		if (ec || len == 0)
		{
			if (ec && ec != asio::error::eof)
				ERROR_LOG(game, "auth: %s", ec.message().c_str());
			return;
		}
		if (len < 4) {
			ERROR_LOG(game, "auth: small packet (%zd bytes)", len);
			return;
		}
		uint32_t msg = read32(recvBuffer.data(), 0);
		switch (msg)
		{
		case 1:
			{
				// Registration step 1
				memcpy(key.data(), recvBuffer.data() + 4, key.size());
				xor55(key.data(), key.size());
				initBlowfish(key.data());
				decrypt(recvBuffer.data() + 0x40, len - 0x40);
				INFO_LOG(game, "auth: registration for %s", &recvBuffer[0x54]);
				//dumpData(recvBuffer.data() + 0x40, len - 0x40);

				// expect 0x38 bytes
				// status 0 at offset 0
				// uses 16 bytes of key at offset 4-13
				// will respond with MSG3
				sendBuffer.fill(0);
				//strcpy((char *)&sendBuffer[4], "0123456789012345");
				encrypt(sendBuffer.data(), sendBuffer.size());
				send();
				break;
			}
		case 3:
			{
				// Registration step 2
				std::array<uint8_t, KEY_SIZE> key2;
				key2 = key;
				memset(key2.data(), 0, 16);
				//strcpy((char *)key2.data(), "0123456789012345");
				initBlowfish(key2.data());
				decrypt(recvBuffer.data() + 0x40, len - 0x40);
				DEBUG_LOG(game, "MSG3:");
				//dumpData(recvBuffer.data() + 0x40, len - 0x40);

				// same reply as msg1
				// user game id at offset 14
				sendBuffer.fill(0);
				// Use the player name as game id
				strcpy((char *)&sendBuffer[0x14], (const char *)&recvBuffer[0x54]);
				encrypt(sendBuffer.data(), sendBuffer.size());
				send();
				break;
			}
		case 2:
			{
				// Login step 1
				memcpy(key.data(), recvBuffer.data() + 4, key.size());
				xor55(key.data(), key.size());
				initBlowfish(key.data());
				decrypt(recvBuffer.data() + 0x40, len - 0x40);
				INFO_LOG(game, "auth: login for game id %s, user name: %s", &recvBuffer[0x40], &recvBuffer[0x74]);
				DEBUG_LOG(game, "Dricas game ID: %.16s", &recvBuffer[0x64]);

				// expect 0x38 bytes
				// status 0 at offset 0
				// uses 16 bytes of key at offset 4-13
				// will respond with MSG4
				sendBuffer.fill(0);
				//strcpy((char *)&sendBuffer[4], "0123456789012345");
				encrypt(sendBuffer.data(), sendBuffer.size());
				send();
				break;
			}
		case 4:
			{
				// Login step 2
				std::array<uint8_t, KEY_SIZE> key2;
				key2 = key;
				memset(key2.data(), 0, 16);
				//strcpy((char *)key2.data(), "0123456789012345");
				initBlowfish(key2.data());
				decrypt(recvBuffer.data() + 0x40, len - 0x40);
				DEBUG_LOG(game, "MSG4: user name: %s", &recvBuffer[0x74]);
				//dumpData(recvBuffer.data() + 0x40, len - 0x40);

				sendBuffer.fill(0);
				encrypt(sendBuffer.data(), sendBuffer.size());
				send();
				break;
			}
		default:
			ERROR_LOG(game, "auth: unhandled message %d", msg);
			receive();
		}
	}

	static constexpr size_t KEY_SIZE = 56;
	asio::ip::tcp::socket socket;
	std::array<uint8_t, 0x90> recvBuffer;
	std::array<uint8_t, 0x38> sendBuffer;
	std::array<uint8_t, KEY_SIZE> key;
	BLOWFISH_CTX blowfishCtx;

	friend super;
};

class AuthAcceptor
{
public:
	AuthAcceptor(asio::io_context& io_context)
		: io_context(io_context),
		  acceptor(asio::ip::tcp::acceptor(io_context,
				asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 20200)))
	{
		asio::socket_base::reuse_address option(true);
		acceptor.set_option(option);
	}

	void start()
	{
		AuthConnection::Ptr newConnection = AuthConnection::create(io_context);

		acceptor.async_accept(newConnection->getSocket(),
			[this, newConnection](const std::error_code& error) {
				if (!error) {
					INFO_LOG(game, "New connection from %s", newConnection->getSocket().remote_endpoint().address().to_string().c_str());
					newConnection->receive();
				}
				start();
			});
	}

private:
	asio::io_context& io_context;
	asio::ip::tcp::acceptor acceptor;
};
