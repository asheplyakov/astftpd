#!/bin/sh
set -e

if [ "`id -u`" != "0" ]; then
	exec sudo $0 $@
fi

set -x

SERVER_IP=10.20.0.2/24
SERVER_IFACE=veth0
CLIENT_IP=10.20.0.4/24
CLIENT_IFACE=veth1
BROADCAST=10.20.0.255

make_netns () {
	local name="$1"
	if ! ip netns list | grep -q "$name"; then
		ip netns add "$name"
		ip netns exec "$name" ip link set up dev lo
	fi
}

get_ipv4_addr () {
	local ns="$1"
	local iface="$2"
	ip netns exec "$ns" ip -o -4 addr show dev "$iface" | awk '{ print $4 }'
}

set_ipv4_addr () {
	local ns="$1"
	local iface="$2"
	local addr="$3"
	local brd="$4"
	local actual_addr
	actual_addr=`get_ipv4_addr $ns $iface`
	if [ "$actual_addr" != "$addr" ]; then
		ip netns exec "$ns" ip addr add "$addr" brd "$brd" dev "$iface"
	else
		echo "interface $ns/$iface already has IP $addr" >&2
	fi
}

if_up () {
	local ns="$1"
	local iface="$2"
	ip netns exec "$ns" ip link set up dev "$iface"
}

make_netns tftpclient
make_netns tftpserver

if ! ip netns exec tftpserver ip -o link list | grep -q "$SERVER_IFACE"; then
	ip netns exec tftpserver ip link add name "$SERVER_IFACE" type veth peer name "$CLIENT_IFACE"
fi

if ip netns exec tftpserver ip -o link show "$CLIENT_IFACE" | grep -q "$CLIENT_IFACE"; then
	ip netns exec tftpserver ip link set netns tftpclient dev "$CLIENT_IFACE"
fi

set_ipv4_addr tftpclient $CLIENT_IFACE $CLIENT_IP $BROADCAST
set_ipv4_addr tftpserver $SERVER_IFACE $SERVER_IP $BROADCAST

if_up tftpserver $SERVER_IFACE
if_up tftpclient $CLIENT_IFACE
exec ip netns exec tftpserver ping -c 5 "${CLIENT_IP%/*}"
