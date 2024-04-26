#include <errno.h>
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

#include <opts.h>

#define VIRTUAL_INTERFACE "tun0"
#define BUFLEN 65535
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
		while (!rf24_available(radio)) {
			sleep(1);
		}

		bytes = rf24_getPayloadSize(radio);
		if (i + bytes > BUFLEN) {
			printf("Buffer full");
			return i;
		}
		rf24_read(radio, &buffer[i], bytes);
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

	char role = argv[1][0];

	int tun_fd;

	// Init RF24
	if (role == 'r') {
		radio = new_rf24(RX_CE_PIN, RX_CSN_PIN);
		rf24_begin(radio);
		rf24_setChannel(radio, RX_CHANNEL);
		tun_fd = open("/dev/net/tun", O_WRONLY); // The receiver writes to the interface
	} else {
		radio = new_rf24(TX_CE_PIN, TX_CSN_PIN);
		rf24_begin(radio);
		rf24_setChannel(radio, TX_CHANNEL);
		tun_fd = open("/dev/net/tun", O_RDONLY); // The sender reads from the interface

	}
	rf24_setPALevel(radio, RF24_PA_LOW);
	rf24_openWritingPipe(radio, address[role == 'r']);
	rf24_openReadingPipe(radio, 1, address[!(role == 'r')]);

	// Init TUN interface
	if (tun_fd == -1) {
		#ifdef _GNU_SOURCE
		printf("Error opening /dev/net/tun: %s\n", strerrorname_np(errno));
		#else
		printf("Error opening /dev/net/tun: %s\n", strerror(errno));
		#endif
		return 1;
	}
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strncpy(ifr.ifr_name, VIRTUAL_INTERFACE, IFNAMSIZ);
	int res = ioctl(tun_fd, TUNSETIFF, &ifr);
	if (res == -1) {
		#ifdef _GNU_SOURCE
		printf("ioctl failed: %s\n", strerrorname_np(errno));
		#else
		printf("ioctl failed: %s\n", strerror(errno));
		#endif
		return 1;
	}

	printf("opened tun interface: %d\n", tun_fd);

	char buf[BUFLEN];
	if (role == 'r') {
		rf24_startListening(radio);
		while (1) {
			size_t size = listen_and_defragment(buf);
			if (size > 0) {
				printf("received %ld bytes\n", size);
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
			printf("sending:\n");
			for (int i = 0; i < count; i++) {
				printf("%02x ", buf[i]);
				if (i % 16 == 0) {
					printf("\n");
				} else if (i % 8 == 0){
					printf(" ");
				}
			}
			fragment_and_send(buf, count);
			printf("done");
		}
	}
}
