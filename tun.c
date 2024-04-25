#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <unistd.h>

#define BUFLEN 1500
#define VIRTUAL_INTERFACE "tun0"

int main() {
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
	while (1) {
		memset(buf, 0, BUFLEN);
		ssize_t count = read(tun_fd, buf, BUFLEN);
		if (count < 0) {
			printf("read error");
			return 1;
		}
		for (int i = 0; i < BUFLEN; i++) {
			printf("%x", buf[i]);
		}
		printf("\n\n");
	}
}
