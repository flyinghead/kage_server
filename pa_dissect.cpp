#include "kage.h"
#include "propeller.h"
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <map>

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
	case Packet::REQ_CHG_ROOM_ATTR: return "CHG ROOM ATTR";
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
	std::map<std::pair<uint32_t, uint16_t>, uint32_t> relSeqs;
	std::map<std::pair<uint32_t, uint16_t>, int> scores;

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
			printf("%s%04x %04x ", data[0] & 0x80 ? "!" : " ", read32(data, 8), read32(data, 0xc));
			if (data[0] & 0x80)
			{
				uint32_t& relSeq = relSeqs[std::make_pair(uint32_t(h.addr), uint16_t(h.port))];
				uint32_t newSeq = read32(data, 8);
				//if (newSeq != 0 && newSeq <= relSeq)
				//	printf("***!!!*** ");
				relSeq = newSeq;
			}

			switch (data[3])
			{
			case Packet::REQ_CHAT:
				if (data[0] & 0x80)
					printf("CHAT %s\n", &data[0x10]);
				else
					printf("CHAT sysdata\n");
				break;
			case Packet::REQ_CHG_ROOM_ATTR:
				printf("CHG ROOM ATTR %.4s %x\n", &data[0x10], read32(data, 0x14));
				break;
			case Packet::REQ_CHG_USER_STATUS:
				printf("CHG USER STATUS %x\n", read32(data, 0x10));
				break;
			case Packet::REQ_QRY_ROOM_ATTR:
				printf("QRY_ROOM_ATTR room %x attr %.4s\n", read32(data, 0x10), &data[0x14]);
				break;
			case Packet::REQ_AUDIO_START:
				printf("AUDIO START\n");
				break;
			case Packet::REQ_AUDIO_STOP:
				printf("AUDIO STOP\n");
				break;
			case Packet::REQ_GAME_DATA:
				{
					uint8_t type = data[0x10];
					switch (type)
					{
					case IN_SET_PLAYER_ATTRS:
						printf("PA SET PLAYER ATTR: plane %d flags %x rank %d\n", data[0x11], data[0x12], data[0x13]);
						break;
					case IN_GET_ROOM_ATTRS:
						printf("PA GET ROOM ATTR\n");
						break;
					case IN_GAME_STARTING:
						printf("PA GAME STARTING: %x %x %x\n", data[0x11], data[0x12], data[0x13]);
						break;
					case IN_SET_ROOM_ATTRS:
						printf("PA SET ROOM ATTR: pts %d stage %d options %x\n", data[0x11] >> 4, data[0x11] & 0xf, data[0x14]);
						break;
					case IN_GAME_OVER:
						printf("PA GAME OVER\n");
						break;
					case IN_GAME_START:
						printf("PA GAME START\n");
						break;
					case IN_GAME_STOP:
						printf("PA GAME STOP\n");
						break;
					case IN_GAME_CDATA:
						printf("PA GAME CDATA: slot %d\n", data[0x11]);
						break;
					case IN_GAME_HDATA:
						printf("PA GAME HDATA: slot %d\n", data[0x11]);
						break;
					case IN_GAME_HDATA2:
						printf("PA GAME HDATA2: slot %d\n", data[0x11]);
						break;
					case IN_GAME_ENDED:
						printf("PA GAME ENDED: slot %d ? %x %x\n", data[0x11], read32(data, 0x14), read32(data, 0x18));
						break;
					default:
						printf("PA ? %02x\n", type);
						break;
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
