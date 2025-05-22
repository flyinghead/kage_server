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
#include "discord.h"
#include "log.h"
#include "asio.h"
#include <map>
#include <fstream>
#include <sstream>

static std::map<std::string, std::string> Config;

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
