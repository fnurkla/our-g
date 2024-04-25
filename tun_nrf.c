#include <fcntl.h> // open()
#include <linux/if_tun.h> // IFF_TUN, IFF_NO_PI
#include <net/if.h> // ifreq
#include <rf24c.h> // rf24 stuff
#include <stdint.h> // uint8_t
#include <stdio.h> // printf()
#include <string.h> // memset()
#include <sys/ioctl.h> // ioctl()
#include <sys/types.h> // ssize_t
#include <unistd.h> // read()

#define CSN_PIN 0
#define CE_PIN 17
#define CHANNEL 111

#define VIRTUAL_INTERFACE "tun0"
#define BUFLEN 576
#define MAX_RETRY 5

RF24Handle radio;
uint8_t address[2][2] = {"1", "2"};

size_t listen_and_defragment(char* buffer) {
	uint8_t bytes;
	int total_length;
	if (rf24_available(radio)) {
		bytes = rf24_getPayloadSize(radio);
		rf24_read(radio, buffer, bytes);

		total_length = (buffer[2] << 8) + buffer[3];
	} else {
		return 0;
	}

	int i;
	for (i = bytes; i < total_length; i += 32) {
		if (rf24_available(radio)) {
			bytes = rf24_getPayloadSize(radio);
			if (i + bytes > BUFLEN) {
				printf("Buffer full");
				return i;
			}
			rf24_read(radio, &buffer[i], bytes);
		} else {
			printf("Fragment lost? or next fragment not arrived yet?");
		}

		// Maybe sleep to make sure next fragment has arrived
	}
	return total_length;
}

void fragment_and_send(char* payload, ssize_t size) {
	for (int i = 0; i < size; i += 32) {
		char* bytes = &payload[i];
		size_t cur_size = size - i < 32 ? size - i : 32;
		int success = 0;
		int tries = 0;
		while (!success && tries < MAX_RETRY) {
			tries++;
			success = rf24_write(radio, bytes, cur_size);
		}
		if (tries == MAX_RETRY) {
			printf("Max retries reached");
			return;
		}
	}
}

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("Please specify (r)eceiver or (s)ender");
		return 1;
	}

	int role = strncmp(argv[1], "r", 1) == 0; // 0: sender, 0: receiver

	// Init RF24
	radio = new_rf24(CE_PIN, CSN_PIN);
	rf24_begin(radio);
	rf24_setChannel(radio, CHANNEL);
	rf24_setPALevel(radio, RF24_PA_LOW);
	rf24_openWritingPipe(radio, address[role]);
	rf24_openReadingPipe(radio, 1, address[!role]);

	// Init TUN interface
	int tun_fd = open("/dev/net/tun", O_RDWR);
	if (tun_fd == -1) {
		printf("Error opening /dev/net/tun");
		return 1;
	}
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strncpy(ifr.ifr_name, VIRTUAL_INTERFACE, IFNAMSIZ);
	int res = ioctl(tun_fd, TUNSETIFF, &ifr);
	if (res == -1) {
		printf("ioctl failed");
		return 1;
	}

	char buf[BUFLEN];
	if (role == 0) {
		rf24_startListening(radio);
		while (1) {
			size_t size = listen_and_defragment(buf);
			if (size > 0) {
				write(tun_fd, buf, size);
			}
		}
	} else {
		rf24_stopListening(radio);
		while (1) {
			ssize_t count = read(tun_fd, buf, BUFLEN);
			if (count < 0) {
				printf("read error");
				return 1;
			}
			fragment_and_send(buf, count);
		}
	}
}
