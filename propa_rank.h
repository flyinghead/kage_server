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
#include "shared_this.h"
#include "asio.h"
#include <array>
#include <string>
#include <vector>

class RankConnection : public SharedThis<RankConnection>
{
public:
	asio::ip::tcp::socket& getSocket() {
		return socket;
	}

	void receive()
	{
		recvBuffer.clear();
		socket.async_read_some(asio::buffer(recvBuffer),
				std::bind(&RankConnection::onReceive, shared_from_this(), asio::placeholders::error, asio::placeholders::bytes_transferred));
	}

	void send(const std::vector<uint8_t>& data)
	{
		memcpy(&sendBuffer[sendIdx], data.data(), data.size());
		sendIdx += data.size();
		send();
	}

private:
	RankConnection(asio::io_context& io_context)
		: socket(io_context) {}

	void send()
	{
		if (sending)
			return;
		sending = true;
		asio::async_write(socket, asio::buffer(sendBuffer, sendIdx),
			std::bind(&RankConnection::onSent, shared_from_this(),
					asio::placeholders::error,
					asio::placeholders::bytes_transferred));
	}
	void onSent(const std::error_code& ec, size_t len)
	{
		if (ec)
			fprintf(stderr, "Send error: %s\n", ec.message().c_str());
		else
			printf("sent %zd bytes\n", len);
		sendIdx = 0;
		sending = false;
	}

	void onReceive(const std::error_code& ec, size_t len)
	{
		std::vector<uint8_t> data(4 * 8);
		for (int i = 0; i < 8; i++) {
			uint32_t v = ntohl(i + 1);
			memcpy(&data[i * 4], &v, 4);
		}
		send(data);
	}

	asio::ip::tcp::socket socket;
	std::vector<uint8_t> recvBuffer;
	std::array<uint8_t, 8500> sendBuffer;
	size_t sendIdx = 0;
	bool sending = false;

	friend super;
};

class RankAcceptor
{
public:
	RankAcceptor(asio::io_context& io_context)
		: io_context(io_context),
		  acceptor(asio::ip::tcp::acceptor(io_context,
				asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 10100)))
	{
		asio::socket_base::reuse_address option(true);
		acceptor.set_option(option);
	}

	void start()
	{
		RankConnection::Ptr newConnection = RankConnection::create(io_context);

		acceptor.async_accept(newConnection->getSocket(),
			[this, newConnection](const std::error_code& error) {
				if (!error) {
					printf("New connection from %s\n", newConnection->getSocket().remote_endpoint().address().to_string().c_str());
					newConnection->receive();
				}
				start();
			});
	}

private:
	asio::io_context& io_context;
	asio::ip::tcp::acceptor acceptor;
};
