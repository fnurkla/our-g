CFLAGS = -Wall -Wextra -g -lrf24c -lgpiod

all: base_station mobile_unit

base_station: base_station.d/opts.h common.h tun_nrf.c
	gcc -Ibase_station.d tun_nrf.c -o base_station $(CFLAGS)

mobile_unit: mobile_unit.d/opts.h common.h tun_nrf.c
	gcc -Imobile_unit.d tun_nrf.c -o mobile_unit $(CFLAGS)
