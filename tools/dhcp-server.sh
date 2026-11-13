#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 opencamd contributors
# Bench DHCP server (dnsmasq) on the given interface, for a camera on the
# bootloader recovery subnet 192.168.1.0/24.

sudo dnsmasq -d -i "${1:?usage: $0 <interface>}" --bind-interfaces --except-interface=lo \
  --dhcp-range=192.168.1.50,192.168.1.100,255.255.255.0,1h
