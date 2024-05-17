#include <assert.h>
#include <errno.h>
#include <fcntl.h> // open()
#include <gpiod.h>
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

#define PRINT		1	/* enable/disable prints. */

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

void interrupt_handler() {
	pr("interrupt!!!\n");
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
			pr("received %d bytes\n", args->total_length);
			write(args->tun_fd, buf, args->total_length);

			memset(buf, 0, BUFLEN); // Is this needed? Buf gets overwritten anyway and we keep track of which bytes are "valid"
			args->current_length = 0;
			args->total_length = -1;
		}
	}
}

/* Request a line as input with edge detection. */
static struct gpiod_line_request *request_input_line(const char *chip_path,
						     unsigned int offset,
						     const char *consumer) {
	struct gpiod_request_config *req_cfg = NULL;
	struct gpiod_line_request *request = NULL;
	struct gpiod_line_settings *settings;
	struct gpiod_line_config *line_cfg;
	struct gpiod_chip *chip;
	int ret;

	chip = gpiod_chip_open(chip_path);
	if (!chip)
		return NULL;

	settings = gpiod_line_settings_new();
	if (!settings)
		goto close_chip;

	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
	gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_FALLING);

	line_cfg = gpiod_line_config_new();
	if (!line_cfg)
		goto free_settings;

	ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1,
						  settings);
	if (ret)
		goto free_line_config;

	if (consumer) {
		req_cfg = gpiod_request_config_new();
		if (!req_cfg)
			goto free_line_config;

		gpiod_request_config_set_consumer(req_cfg, consumer);
	}

	request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

	gpiod_request_config_free(req_cfg);

free_line_config:
	gpiod_line_config_free(line_cfg);

free_settings:
	gpiod_line_settings_free(settings);

close_chip:
	gpiod_chip_close(chip);

	return request;
}

static const char *edge_event_type_str(struct gpiod_edge_event *event) {
	switch (gpiod_edge_event_get_event_type(event)) {
	case GPIOD_EDGE_EVENT_RISING_EDGE:
		return "Rising";
	case GPIOD_EDGE_EVENT_FALLING_EDGE:
		return "Falling";
	default:
		return "Unknown";
	}
}

int gpio_debug() {
	/* Example configuration - customize to suit your situation. */
	static const char *const chip_path = "/dev/gpiochip0";
	static const unsigned int line_offset = IRQ_PIN;

	struct gpiod_edge_event_buffer *event_buffer;
	struct gpiod_line_request *request;
	struct gpiod_edge_event *event;
	int i, ret, event_buf_size;

	request = request_input_line(chip_path, line_offset,
				     "watch-line-value");
	if (!request) {
		pr("failed to request line: %s\n", strerror(errno));
		return 1;
	}

	/*
	 * A larger buffer is an optimisation for reading bursts of events from
	 * the kernel, but that is not necessary in this case, so 1 is fine.
	 */
	event_buf_size = 1;
	event_buffer = gpiod_edge_event_buffer_new(event_buf_size);
	if (!event_buffer) {
		pr("failed to create event buffer: %s\n", strerror(errno));
		return 1;
	}

	for (;;) {
		/* Blocks until at least one event is available. */
		ret = gpiod_line_request_read_edge_events(request, event_buffer,
							  event_buf_size);
		if (ret == -1) {
			pr("error reading edge events: %s\n",
			   strerror(errno));
			return 1;
		}
		for (i = 0; i < ret; i++) {
			event = gpiod_edge_event_buffer_get_event(event_buffer,
								  i);
			printf("offset: %d  type: %-7s  event #%ld\n",
			       gpiod_edge_event_get_line_offset(event),
			       edge_event_type_str(event),
			       gpiod_edge_event_get_line_seqno(event));
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

	pthread_t sender, receiver;

	res = pthread_create(&sender, NULL, do_send, &tun_fd);
	sleep(1); // prevent race condition
	res |= pthread_create(&receiver, NULL, do_receive, &tun_fd);
	assert(!res);

	gpio_debug();

	res = pthread_join(sender, NULL);
	res |= pthread_join(receiver, NULL);
	assert(!res);

	return 0;
}
