#!/bin/bash

# Exit if not running as root
if ((EUID != 0)); then
    echo "Script must be run as root!"
    exit 1
fi

VIRT_INTERFACE=tun0

# Virtual interface
# Lecture 3.2 - Virtual interfaces
ip tuntap add mode tun dev $VIRT_INTERFACE
ip addr add 11.11.11.2/24 dev $VIRT_INTERFACE
ip link set dev $VIRT_INTERFACE up

ip route add 8.8.8.8 dev $VIRT_INTERFACE
