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
#include "propa_auth.h"
#include "propa_rank.h"
#include "model.h"
#include "outtrigger.h"
#include "discord.h"
#include "log.h"
#include "asio.h"
#include <map>
#include <fstream>
#include <sstream>

static std::map<std::string, std::string> Config;

class BootstrapServer : public Server
{
public:
	BootstrapServer(uint16_t port, asio::io_context& io_context)
		: Server(port, io_context),
		  bombermanServer(Game::Bomberman, BOMBERMAN_PORT, io_context),
		  outtriggerServer(OUTTRIGGER_PORT, io_context),
		  propellerServer(Game::PropellerA, PROPELLERA_PORT, io_context)
	{
	}

	void start();

private:
	void handlePacket(const uint8_t *data, size_t len) override;

	uint32_t nextUserId = 0x1001;
	LobbyServer bombermanServer;
	OuttriggerServer outtriggerServer;
	LobbyServer propellerServer;
	static constexpr uint16_t BOMBERMAN_PORT = 9091;
	static constexpr uint16_t OUTTRIGGER_PORT = 9092;
	static constexpr uint16_t PROPELLERA_PORT = 9093;
	static constexpr const char *OuttriggerKey = "reggirttuO";
	static constexpr const char *PropellerKey = "ArelleporP";
};

void BootstrapServer::start()
{
	bombermanServer.start();
	outtriggerServer.start();
	propellerServer.start();
	Server::start();
}

void BootstrapServer::handlePacket(const uint8_t *data, size_t len)
{
	DEBUG_LOG(Game::None, "Bootstrap: Packet: flags/size %02x %02x command %02x %02x", data[0], data[1], data[2], data[3]);
	Packet packet;
	switch (data[3])
	{
	case Packet::REQ_BOOTSTRAP_LOGIN:
		{
			/* Using 2D and blowfish encryption (works with outtrigger)
			packet.init(Packet::RSP_LOGIN_SUCCESS, &data[4]);
			packet.writeData(&data[0x10], 0x28);
			// FIXME auto addr = socket.local_endpoint().address().to_v4().to_bytes();
			std::array<uint8_t, 4>addr { 192, 168, 1, 31 };
			packet.writeData((uint8_t *)addr.data(), addr.size());
			packet.writeData((uint32_t)socket.local_endpoint().port());
			packet.writeData(0u);
			packet.writeData(0u);
			packet.writeData(0u); // size of following data, sent back when logging to lobby
			packet.size = ((packet.size + 7) / 8) * 8;
			BLOWFISH_CTX *ctx = new BLOWFISH_CTX();
			Blowfish_Init(ctx, (uint8_t *)OuttriggerKey, strlen(OuttriggerKey));
			for (int i = 0x10; i < packet.size; i += 8)
			{
				uint32_t *x = (uint32_t *)&packet.data[i];
				x[0] = ntohl(x[0]);
				x[1] = ntohl(x[1]);
				Blowfish_Encrypt(ctx, x, x + 1);
				x[0] = htonl(x[0]);
				x[1] = htonl(x[1]);
			}
			*/
			uint16_t port = OUTTRIGGER_PORT;
			LobbyServer *server = &outtriggerServer;
			std::string name;
			if (!strcmp((const char *)&data[0x10], "BombermanOnline"))
			{
				port = BOMBERMAN_PORT;
				server = &bombermanServer;
				name = (const char *)&data[0x38];
				auto sep = name.find('\1');
				if (sep != std::string::npos)
					// get rid of the password
					name = name.substr(0, sep);
			}
			else if (!strcmp((const char *)&data[0x10], "PropellerA"))
			{
				port = PROPELLERA_PORT;
				server = &propellerServer;
				name = (const char *)&data[0x38];	// FIXME this is the game key but no user name
			}
			else {
				// Outtrigger
				name = (const char *)&data[0x10];
			}

			uint32_t tmpUserId = read32(data, 4);

			// Using 29 (shu)
			packet.init(Packet::RSP_LOGIN_SUCCESS2);
			packet.writeData((uint32_t)port);
			packet.writeData(0u);	// ?
			packet.writeData(nextUserId);

			Player *player = new Player(*server, source, nextUserId, io_context);
			player->setName(name);
			server->addPlayer(player);
			nextUserId++;

			size_t pktsize = packet.finalize();
			write32(packet.data, 4, tmpUserId);
			write32(packet.data, 8, player->getUnrelSeqAndInc());
			std::error_code ec2;
			socket.send_to(asio::buffer(packet.data, pktsize), source, 0, ec2);
			break;
		}

	case Packet::REQ_PING:
		{
			packet.respOK(Packet::REQ_PING);
			packet.writeData(read32(data, 0x10));
			size_t pktsize = packet.finalize();
			write32(packet.data, 4, read32(data, 4));
			std::error_code ec2;
			socket.send_to(asio::buffer(packet.data, pktsize), source, 0, ec2);
			break;
		}

// TODO this msg is received in some cases
//	case Packet::REQ_LOBBY_LOGOUT:
//		break;

	case Packet::REQ_NOP:
		break;

	default:
		ERROR_LOG(Game::None, "Bootstrap: Unhandled msg type %x", data[3]);
		break;
	}
}

void dumpData(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len;)
	{
		char ascii[17] {};
		for (int j = 0; j < 16 && i + j < len; j++) {
			uint8_t b = data[i + j];
			fprintf(stderr, "%02x ", b);
			if (b >= ' ' && b < 0x7f)
				ascii[j] = (char)b;
			else
				ascii[j] = '.';
		}
		fprintf(stderr, "%s\n", ascii);
		i += 16;
	}
}

static void loadConfig(const std::string& path)
{
	std::filebuf fb;
	if (!fb.open(path, std::ios::in)) {
		WARN_LOG(Game::None, "config file %s not found", path.c_str());
		return;
	}

	std::istream istream(&fb);
	std::string line;
	while (std::getline(istream, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		auto pos = line.find_first_of("=:");
		if (pos != std::string::npos)
			Config[line.substr(0, pos)] = line.substr(pos + 1);
		else
			ERROR_LOG(Game::None, "config file syntax error: %s", line.c_str());
	}
	if (Config.count("DISCORD_WEBHOOK") > 0)
		setDiscordWebhook(Config["DISCORD_WEBHOOK"]);
}

int main(int argc, char *argv[])
{
	setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

	asio::io_context io_context;
	asio::signal_set signals(io_context, SIGINT, SIGTERM);
	signals.async_wait([&io_context](const std::error_code& ec, int signalNum) {
		if (!ec) {
			ERROR_LOG(Game::None, "Caught signal %d. Exiting", signalNum);
			io_context.stop();
		}
	});
	loadConfig(argc >= 2 ? argv[1] : "kage.cfg");

	BootstrapServer server(9090, io_context);
	server.start();
	AuthAcceptor authServer(io_context);
	authServer.start();
	RankAcceptor rankServer(io_context);
	rankServer.start();
	NOTICE_LOG(Game::None, "Kage server started");
	try {
		io_context.run();
	} catch (const std::exception& e) {
		ERROR_LOG(Game::None, "Uncaught exception: %s", e.what());
	}
	NOTICE_LOG(Game::None, "Kage server stopped");

	return 0;
}
