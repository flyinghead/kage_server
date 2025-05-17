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

	  printf("[%02ld:%02ld:%02ld.%03ld] ", h.ts / 3600000, (h.ts % 3600000) / 60000, (h.ts % 60000) / 1000, h.ts % 1000);
	  inet_ntop(AF_INET, &h.addr, ip, INET_ADDRSTRLEN);
	  printf(" %15s:%d ", ip, h.port);

	  switch (buf[3])
	  {
	  case Packet::REQ_CHAT:
		  if (buf[0] & 0x80)
			  printf("CHAT %s\n", &buf[0x10]);
		  else
			  printf("CHAT sysdata\n");
		  break;
	  case Packet::REQ_CHG_ROOM_STATUS:
		  printf("CHG ROOM STATUS %x\n", read32(buf, 0x14));
		  break;
	  case Packet::REQ_CHG_USER_STATUS:
		  printf("CHG USER STATUS %x\n", read32(buf, 0x10));
		  break;
	  default:
		  printf("%02x\n", buf[3]);
		  break;
	  }
	}
	return 0;
}
