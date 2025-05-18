#include "kage.h"
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct __attribute__((packed))
{
  time_t ts;
  uint32_t addr;
  uint16_t port;
  uint32_t size;
} Header;

const char *commandName(int cmd)
{
	switch (cmd)
	{
	case Packet::REQ_NOP: return "NOP";
	case Packet::REQ_CHAT: return "CHAT";
	case Packet::REQ_CHG_ROOM_STATUS: return "CHG ROOM STATUS";
	case Packet::REQ_CHG_USER_STATUS: return "CHG USER STATUS";
	case Packet::REQ_CHG_USER_PROP: return "CHG USER PROP";
	case Packet::REQ_CREATE_ROOM: return "CREATE ROOM";
	case Packet::REQ_GAME_DATA: return "GAME DATA";
	case Packet::REQ_JOIN_LOBBY_ROOM: return "JOIN";
	case Packet::REQ_LEAVE_LOBBY_ROOM: return "LEAVE";
	case Packet::REQ_PING: return "PING";
	case Packet::REQ_QRY_LOBBIES: return "QRY LOBBIES";
	case Packet::REQ_QRY_ROOMS: return "QRY ROOMS";
	case Packet::REQ_QRY_USERS: return "QRY USERS";
	default:
		break;
	}
	static char s[3];
	sprintf(s, "%02x", cmd & 0xff);
	return s;
}

int main(int argc, char *argv[])
{
	/*
	int opt;
	int det_time = 0;
	while ((opt = getopt(argc, argv, "t")) != -1)
	{
		switch (opt) {
		case 't':
			det_time = 1;
			break;
		}
	}
	*/

	Header h;
	uint8_t buf[1500];
	time_t start = 0;
	char ip[INET_ADDRSTRLEN];

	while (fread(&h, sizeof(h), 1, stdin) == 1)
	{
		if (fread(buf, 1, h.size, stdin) != h.size) {
			printf("Last packet truncated\n");
			break;
		}
		if (start == 0)
			start = h.ts;
		h.ts -= start;

		const uint8_t *data = buf;
		bool firstChunk = true;
		while (data < buf + h.size - 4)
		{
			uint32_t size = read16(data, 0) & 0x3ff;

			if (firstChunk)
			{
				printf("[%02ld:%02ld:%02ld.%03ld] ", h.ts / 3600000, (h.ts % 3600000) / 60000, (h.ts % 60000) / 1000, h.ts % 1000);
				inet_ntop(AF_INET, &h.addr, ip, INET_ADDRSTRLEN);
				printf(" %15s:%d\t", ip, h.port);
				firstChunk = false;
			}
			else {
				printf("\t\t\t\t\t");
			}

			switch (data[3])
			{
			case Packet::REQ_CHAT:
				if (data[0] & 0x80)
					printf("CHAT %s\n", &data[0x10]);
				else
					printf("CHAT sysdata\n");
				break;
			case Packet::REQ_CHG_ROOM_STATUS:
				printf("CHG ROOM STATUS %x\n", read32(data, 0x14));
				break;
			case Packet::REQ_CHG_USER_STATUS:
				printf("CHG USER STATUS %x\n", read32(data, 0x10));
				break;
			case Packet::REQ_GAME_DATA:
				{
					TagCmd tag(read16(data, 0x10));
					switch (tag.command)
					{
					case TagCmd::ECHO:
						printf("tag:ECHO\n");
						break;
					case TagCmd::GAME_OVER:
						printf("tag:GAME_OVER\n");
						break;
					case TagCmd::GAME_START:
						printf("tag:GAME_START\n");
						break;
					case TagCmd::READY:
						printf("tag:READY\n");
						break;
					case TagCmd::RESET:
						printf("tag:RESET\n");
						break;
					case TagCmd::RESULT:
						printf("tag:RESULT\n");
						break;
					case TagCmd::START_OK:
						printf("tag:START_OK\n");
						break;
					case TagCmd::SYNC:
						printf("tag:SYNC\n");
						break;
					case TagCmd::SYS:
						printf("tag:SYS\n");
						break;
					case TagCmd::TIME_OUT:
						printf("tag:TIME_OUT\n");
						break;
					default:
						printf("tag:UNEXPECTED %02x\n", tag.command);
					}
					break;
				}
			default:
				printf("%s\n", commandName(data[3]));
				break;
			}

			data += size;
		}
	}
	return 0;
}
