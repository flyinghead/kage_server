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
#include "propa_rank.h"
#include "model.h"
#include "discord.h"
#include "asio.h"
#include <map>
#include <fstream>
#include <sstream>

static std::map<std::string, std::string> Config;

// propeller arena auth server key: "Propeller Arena Aviation Battle e4b045c70cef431403ab08b"
// console id: c7 45 b0 e4 14 43                                             ????    TCNT0!!
// (last char of key is truncated and replaced by '\0'

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

/*
static void testPropA()
{
	uint8_t data[] = {
		0x00, 0x00, 0x00, 0x02, 0x05, 0x27, 0x3a, 0x25, 0x30, 0x39, 0x39, 0x30, 0x27, 0x75, 0x14, 0x27,
		0x30, 0x3b, 0x34, 0x75, 0x14, 0x23, 0x3c, 0x34, 0x21, 0x3c, 0x3a, 0x3b, 0x75, 0x17, 0x34, 0x21,
		0x21, 0x39, 0x30, 0x75, 0x30, 0x61, 0x37, 0x65, 0x61, 0x60, 0x36, 0x62, 0x65, 0x36, 0x30, 0x33,
		0x61, 0x66, 0x64, 0x61, 0x65, 0x66, 0x65, 0x31, 0x6d, 0x62, 0x63, 0x55, 0x00, 0x00, 0x00, 0x48,
		0xfa, 0x36, 0xf8, 0xc8, 0x61, 0xf9, 0xb3, 0x5e, 0xed, 0x48, 0x2d, 0xf8, 0x98, 0x4b, 0xcb, 0x01,
		0xed, 0x48, 0x2d, 0xf8, 0x98, 0x4b, 0xcb, 0x01, 0xed, 0x48, 0x2d, 0xf8, 0x98, 0x4b, 0xcb, 0x01,
		0xb0, 0x43, 0x2e, 0x6c, 0x92, 0x69, 0x58, 0xd1, 0x3e, 0x66, 0xb0, 0x87, 0xa4, 0xfc, 0xf2, 0xea,
		0xf1, 0xcf, 0x35, 0xf9, 0xd4, 0x0a, 0x80, 0x1b, 0x50, 0x2a, 0x12, 0x73, 0x52, 0x51, 0x45, 0x39,
		0xed, 0x48, 0x2d, 0xf8, 0x98, 0x4b, 0xcb, 0x01
	};
	const char *key = "Propeller Arena Aviation Battle e4b045c70cef431403ab08b";
	BLOWFISH_CTX *ctx = new BLOWFISH_CTX();
	Blowfish_Init(ctx, (uint8_t *)key, strlen(key) + 1);
	uint8_t buf[14 * 4];
	for (unsigned i = 0; i < sizeof(buf); i++)
		buf[i] ^= 0x55;
	for (unsigned i = 0; i < sizeof(data); i += 8)
	{
		uint32_t *x = (uint32_t *)&data[i];
		Blowfish_Decrypt(ctx, x, x + 1);
	}
	dumpData(data, sizeof(data));
}
*/

static void loadConfig(const std::string& path)
{
	std::filebuf fb;
	if (!fb.open(path, std::ios::in)) {
		fprintf(stderr, "Warning: config file %s not found\n", path.c_str());
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
			fprintf(stderr, "Error: config file syntax error: %s\n", line.c_str());
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
			fprintf(stderr, "Caught signal %d. Exiting\n", signalNum);
			io_context.stop();
		}
	});
	loadConfig(argc >= 2 ? argv[1] : "kage.cfg");

	BootstrapServer server(9090, io_context);
	server.start();
	RankAcceptor rankServer(io_context);
	rankServer.start();
	printf("Kage server started\n");
	io_context.run();
	printf("Kage server stopped\n");

	return 0;
}
