#!/bin/bash

# Exit if not running as root
if ((EUID != 0)); then
    echo "Script must be run as root!" >&2
    exit 1
fi

VIRT_INTERFACE=tun0
ETHERNET_INTERFACE=eth0

# Virtual interface
# Lecture 3.2 - Virtual interfaces
ip tuntap add mode tun dev $VIRT_INTERFACE
ip addr add 11.11.11.1/24 dev $VIRT_INTERFACE
ip link set dev $VIRT_INTERFACE up

# IP forwarding & NAT
# https://wiki.archlinux.org/title/Internet_sharing#Enable_NAT
# Lecture 4.1 - IPTables
sysctl net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -o $ETHERNET_INTERFACE -j MASQUERADE # Slides say $VIRT_INTERFACE but Arch wiki says $ETHERNET_INTERFACE, which makes more sense
# iptables -A FORWARD -i $ETHERNET_INTERFACE -o $VIRT_INTERFACE -m state --state RELATED,ESTABLISHED -j ACCEPT # From lecture slides
iptables -A FORWARD -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT # From arch wiki, ChatGPT claims it is more modern
iptables -A FORWARD -i $VIRT_INTERFACE -o $ETHERNET_INTERFACE -j ACCEPT

# Rate limiting
# Lecture 4.1 - IPTables
# iptables -A FORWARD -i $ETHERNET_INTERFACE -o $VIRT_INTERFACE -m state --state RELATED,ESTABLISHED -m limit --limit 10/sec -j ACCEPT
