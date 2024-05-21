#include <assert.h>
#include <errno.h>
#include <fcntl.h> // open()
#include <linux/if_tun.h> // IFF_TUN, IFF_NO_PI
#include <net/if.h> // ifreq
#include <pthread.h>
#include <rf24c.h> // rf24 stuff
#include <stdint.h> // uint8_t
#include <stdio.h> // printf()
#include <string.h> // memset()
#include <sys/ioctl.h> // ioctl()
#include <sys/types.h> // ssize_t
#include <time.h>
#include <unistd.h> // read()

#include <opts.h>

#define PRINT		0	/* enable/disable prints. */

/* the funny do-while next clearly performs one iteration of the loop.
 * if you are really curious about why there is a loop, please check
 * the course book about the C preprocessor where it is explained. it
 * is to avoid bugs and/or syntax errors in case you use the pr in an
 * if-statement without { }.
 *
 */

#if PRINT
#define pr(...)		do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define pr(...)		/* no effect at all */
#endif

#define VIRTUAL_INTERFACE "tun0"
#define BUFLEN 65535
#define MTU 1500

struct timespec delay = {0, 50000}; // 50 µs
struct timespec send_delay = {0, 500000}; // 500 µs

uint8_t payload_size;


RF24Handle make_radio(int ce_pin, int csn_pin, int channel, int is_receiver) {
	uint8_t address[2][2] = {"1", "2"};
	RF24Handle radio = new_rf24(ce_pin, csn_pin);
	rf24_begin(radio);
	rf24_setChannel(radio, channel);
	rf24_setPALevel(radio, RF24_PA_LOW);
	rf24_setDataRate(radio, 1);
	rf24_openWritingPipe(radio, address[is_receiver]);
	rf24_openReadingPipe(radio, 1, address[!is_receiver]);

	payload_size = rf24_getPayloadSize(radio);

	return radio;
}

int is_good_ip_header(uint8_t *buf) {
	// Ip version
	if ((buf[0] & 0b11110000) != (4 << 4)) {
		pr("Not beginning of ipv4 packet.\n");
		return 0;
	}
	// Header length
	if ((buf[0] & 0b00001111) < 5) {
		pr("Header too short\n");
		return 0;
	}
	// Evil bit
	if (((buf[6] & 0b10000000) >> 7) != 0) {
		pr("Packet is evil\n");
		return 0;
	}
	// Good packet :^)
	return 1;
}

size_t listen_and_defragment(RF24Handle radio, uint8_t* buffer) {
	int total_length;
	if (rf24_available(radio)) {
		rf24_read(radio, buffer, payload_size);

		if (!is_good_ip_header(buffer)) {
			pr("Discarding.\n");
			return 0;
		}
		total_length = (buffer[2] << 8) | buffer[3];
		if (total_length > MTU) {
			pr("Incoming package (%d) longer than %d bytes, discarding.\n", total_length, MTU);
			return 0;
		}
		pr("Expecting packet of length %d\n", total_length);
	} else {
		return 0;
	}

	int i;
	for (i = payload_size; i < total_length; i += payload_size) {
		while (!rf24_available(radio)) {
			nanosleep(&delay, NULL);
		}

		if (i + payload_size > BUFLEN) {
			pr("Buffer full");
			return i;
		}
		rf24_read(radio, buffer + i, payload_size);
		pr("have received %d/%d bytes\n", i, total_length);
	}
	return total_length;
}

void fragment_and_send(RF24Handle radio, uint8_t* payload, ssize_t size) {
	for (int i = 0; i < size; i += 32) {
		uint8_t* bytes = payload + i;
		size_t cur_size = size - i < 32 ? size - i : 32;
		int success = rf24_write(radio, bytes, cur_size);
		if (!success) {
			pr("Transmission failed\n");
		}
		nanosleep(&send_delay, NULL);
	}
}

void *do_receive(void *argument) {
	int tun_fd = *((int *) argument);

	RF24Handle radio = make_radio(RX_CE_PIN, RX_CSN_PIN, RX_CHANNEL, 1);
	uint8_t buf[BUFLEN];

	rf24_startListening(radio);

	while (1) {
		size_t size = listen_and_defragment(radio, buf);
		if (size > 0) {
			pr("received %ld bytes\n", size);
			write(tun_fd, buf, size);
		}
	}
}

void *do_send(void *argument) {
	int tun_fd = *((int *) argument);

	RF24Handle radio = make_radio(TX_CE_PIN, TX_CSN_PIN, TX_CHANNEL, 0);
	uint8_t buf[BUFLEN];

	rf24_stopListening(radio);
	while (1) {
		ssize_t count = read(tun_fd, buf, BUFLEN);
		if (count < 0) {
			pr("read error\n");
			return NULL;
		}
		pr("sending packet of length %ld... ", count);
		fragment_and_send(radio, buf, count);
		pr("done\n");
	}
}

int main() {
	int tun_fd;

	tun_fd = open("/dev/net/tun", O_RDWR);

	// Init TUN interface
	if (tun_fd == -1) {
		#ifdef _GNU_SOURCE
		pr("Error opening /dev/net/tun: %s\n", strerrorname_np(errno));
		#else
		pr("Error opening /dev/net/tun: %s\n", strerror(errno));
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
		pr("ioctl failed: %s\n", strerrorname_np(errno));
		#else
		pr("ioctl failed: %s\n", strerror(errno));
		#endif
		return 1;
	}

	pr("opened tun interface: %d\n", tun_fd);

	pthread_t sender, receiver;

	res = pthread_create(&sender, NULL, do_send, &tun_fd);
	sleep(1); // prevent race condition
	res |= pthread_create(&receiver, NULL, do_receive, &tun_fd);
	assert(!res);

	res = pthread_join(sender, NULL);
	res |= pthread_join(receiver, NULL);
	assert(!res);

	return 0;
}
