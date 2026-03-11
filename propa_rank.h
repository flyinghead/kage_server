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
#include "log.h"
#include <dcserver/shared_this.hpp>
#include <dcserver/asio.hpp>
#include <dcserver/database.hpp>
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
		socket.async_read_some(asio::buffer(recvBuffer),
				std::bind(&RankConnection::onReceive, shared_from_this(), asio::placeholders::error, asio::placeholders::bytes_transferred));
	}

	void send(const std::vector<uint32_t>& data)
	{
		memcpy(&sendBuffer[sendIdx], data.data(), data.size() * sizeof(uint32_t));
		sendIdx += data.size() * sizeof(uint32_t);
		send();
	}

private:
	RankConnection(asio::io_context& io_context, Database& database)
		: socket(io_context), database(database) {}

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
			ERROR_LOG(Game::PropellerA, "Send error: %s", ec.message().c_str());
		else
			DEBUG_LOG(Game::PropellerA, "sent %zd bytes", len);
		sendIdx = 0;
		sending = false;
	}

	void onReceive(const std::error_code& ec, size_t len);

	asio::ip::tcp::socket socket;
	std::array<uint8_t, 1024> recvBuffer;
	std::array<uint8_t, 8500> sendBuffer;
	size_t sendIdx = 0;
	bool sending = false;
	Database& database;

	friend super;
};

class RankAcceptor
{
public:
	RankAcceptor(asio::io_context& io_context, const std::string& dbpath);

	~RankAcceptor() {
		Instance = nullptr;
	}

	void start()
	{
		RankConnection::Ptr newConnection = RankConnection::create(io_context, database);

		acceptor.async_accept(newConnection->getSocket(),
			[this, newConnection](const std::error_code& error) {
				if (!error) {
					INFO_LOG(Game::PropellerA, "New connection from %s", newConnection->getSocket().remote_endpoint().address().to_string().c_str());
					newConnection->receive();
				}
				start();
			});
	}

	void updateRank(const std::string& name, int kills, int wins, int games,
			int flightTime, int flightDistance, int shotDown, int points);

	static RankAcceptor *Instance;

private:
	asio::io_context& io_context;
	asio::ip::tcp::acceptor acceptor;
	Database database;
};
