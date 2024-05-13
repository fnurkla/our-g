#include <assert.h>
#include <errno.h>
#include <fcntl.h> // open()
#include <linux/if_tun.h> // IFF_TUN, IFF_NO_PI
#include <net/if.h> // ifreq
#include <pigpio.h>
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
#define MAX_RETRY 5

pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned int interrupt = 0;

struct timespec delay = {0, 50000}; // 50 µs

enum control_msg {
	JOIN_REQUEST = 0,
	JOIN_RESPONSE,
	GOODBYE,
};

struct cb_args {
	RF24Handle *radio;
	char *buf;
	int total_length;
	int current_length;
	int tun_fd;
};


RF24Handle make_radio(int ce_pin, int csn_pin, int channel, int is_receiver) {
	uint8_t address[2][2] = {"1", "2"};
	RF24Handle radio = new_rf24(ce_pin, csn_pin);
	rf24_begin(radio);
	rf24_setChannel(radio, channel);
	rf24_setPALevel(radio, RF24_PA_LOW);
	rf24_openWritingPipe(radio, address[is_receiver]);
	rf24_openReadingPipe(radio, 1, address[!is_receiver]);

	return radio;
}

int is_dataplane(char* buffer) {
	return (0b10000000 & buffer[0]) != 0;
}

enum control_msg ctrl_msg(char* buffer) {
	return (0b01100000 & buffer[0]) >> 5;
}

void create_join_request(char* buffer) {
	memset(buffer, 0, 32);
	buffer[0] |= JOIN_REQUEST << 5;
}

int do_handshake(RF24Handle radio, char* base_station_name) {
	char buffer[32];
	create_join_request(buffer);
	int success;

	rf24_stopListening(radio);

	do {
		success = rf24_write(radio, buffer, 32);
	} while (!success);

	rf24_startListening(radio);

	int i = 0;
	for (; i < 20000; nanosleep(&delay, NULL), ++i) {
		if (!rf24_available(radio))
			continue;

		uint8_t bytes = rf24_getPayloadSize(radio);
		rf24_read(radio, buffer, bytes);

		if (!is_dataplane(buffer) && ctrl_msg(buffer) == JOIN_RESPONSE)
			break;
	}
	if (i == 20000) {
		return 1;
	}

	strncpy(base_station_name, buffer + 1, 31);

	return 0;
}

void fragment_and_send(RF24Handle radio, char* payload, ssize_t size) {
	for (int i = 0; i < size; i += 32) {
		char* bytes = &payload[i];
		size_t cur_size = size - i < 32 ? size - i : 32;
		int success = 0;
		int tries = 0;
		while (!success && tries < MAX_RETRY) {
			tries++;
			success = rf24_writeFast(radio, bytes, cur_size);
		}
		if (tries == MAX_RETRY) {
			pr("Max retries reached\n");
			return;
		}
	}
	rf24_txStandBy(radio);
}

void interrupt_handler(int gpio, int level, uint32_t tick) {
	pthread_mutex_lock(&mutex);
	interrupt++;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);
}

void interrupt_callback(struct cb_args *args) {
	RF24Handle *radio = args->radio;
	char *buf = args->buf;

	cbool tx_ok, tx_fail, rx_ready;
	rf24_whatHappened(*radio, &tx_ok, &tx_fail, &rx_ready);
	if (rx_ready) {
		uint8_t bytes = rf24_getPayloadSize(radio);
		rf24_read(radio, buf + args->current_length, bytes);
		args->current_length += bytes;

		if (args->total_length == -1) {
			// First frame of a new IP packet, get total_length from IP header
			args->total_length = (buf[2] << 8) + buf[3];
		}

		if (args->current_length >= args->total_length) {
			// Full IP packet received, write to tun interface and reset buffer
			pr("received %ld bytes\n", args->total_length);
			write(args->tun_fd, buf, args->total_length);

			memset(buf, 0, BUFLEN); // Is this needed? Buf gets overwritten anyway and we keep track of which bytes are "valid"
			args->current_length = 0;
			args->total_length = -1;
		}
	}
}

void *do_receive(void *argument) {
	int tun_fd = *((int *) argument);

	RF24Handle radio = make_radio(RX_CE_PIN, RX_CSN_PIN, RX_CHANNEL, 1);
	char buf[BUFLEN];

	// Only listen for `rx_ready` interrupts
	rf24_maskIRQ(radio, 1, 1, 0);

	struct cb_args args;
	args.radio = &radio;
	args.buf = buf;
	args.total_length = -1;
	args.current_length = 0;
	args.tun_fd = tun_fd;

	gpioSetISRFunc(IRQ_PIN, FALLING_EDGE, 0, interrupt_handler);

	rf24_startListening(radio);

	while (1) {
		/*
		   Wait for interrupts...
		   Put thread in sleeping state somehow?
		   wait/notify?
		 */
		pthread_mutex_lock(&mutex);
		while (interrupt == 0) {
			pthread_cond_wait(&cond, &mutex);
		}
		// Maybe handle case when more than one interrupt has occured since last time?
		interrupt = 0;
		pthread_mutex_unlock(&mutex);

		// Do the things
		interrupt_callback(&args);
	}
}

void *do_send(void *argument) {
	int tun_fd = *((int *) argument);

	RF24Handle radio = make_radio(TX_CE_PIN, TX_CSN_PIN, TX_CHANNEL, 0);
	char buf[BUFLEN];

	rf24_stopListening(radio);
	while (1) {
		ssize_t count = read(tun_fd, buf, BUFLEN);
		if (count < 0) {
			pr("read error\n");
			return NULL;
		}
		pr("sending:\n");
		for (int i = 0; i < count; i++) {
			pr("%02x ", buf[i]);
			if (i % 16 == 0) {
				pr("\n");
			} else if (i % 8 == 0){
				pr(" ");
			}
		}
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

	if (gpioInitialise() < 0) {
		pr("GPIO Init failed\n");
		return 1;
	}

	pthread_t sender, receiver;

	res = pthread_create(&sender, NULL, do_send, &tun_fd);
	sleep(1); // prevent race condition
	res |= pthread_create(&receiver, NULL, do_receive, &tun_fd);
	assert(!res);

	res = pthread_join(sender, NULL);
	res |= pthread_join(receiver, NULL);
	assert(!res);

	gpioTerminate();

	return 0;
}
