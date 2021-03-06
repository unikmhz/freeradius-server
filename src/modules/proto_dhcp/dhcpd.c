/*
 * dhcp.c	DHCP processing.
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2008 The FreeRADIUS server project
 * Copyright 2008,2011 Alan DeKok <aland@deployingradius.com>
 */

/*
 * Standard sequence:
 *	INADDR_ANY : 68 -> INADDR_BROADCAST : 67	DISCOVER
 *	CLIENT_IP : 68 <- DHCP_SERVER_IP : 67		OFFER
 *	INADDR_ANY : 68 -> INADDR_BROADCAST : 67	REQUEST
 *	CLIENT_IP : 68 <- DHCP_SERVER_IP : 67		ACK
 *
 * Relay sequence:
 *	INADDR_ANY : 68 -> INADDR_BROADCAST : 67	DISCOVER
 *	RELAY_IP : 67 -> NEXT_SERVER_IP : 67		DISCOVER
 *				(NEXT_SERVER_IP can be a relay itself)
 *	FIRST_RELAY_IP : 67 <- DHCP_SERVER_IP : 67	OFFER
 *	CLIENT_IP : 68 <- FIRST_RELAY_IP : 67		OFFER
 *	INADDR_ANY : 68 -> INADDR_BROADCAST : 67	REQUEST
 *	RELAY_IP : 67 -> NEXT_SERVER_IP : 67		REQUEST
 *				(NEXT_SERVER_IP can be a relay itself)
 *	FIRST_RELAY_IP : 67 <- DHCP_SERVER_IP : 67	ACK
 *	CLIENT_IP : 68 <- FIRST_RELAY_IP : 67		ACK
 *
 * Note: NACK are broadcasted, rest is unicast, unless client asked
 * for a broadcast
 */


#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/protocol.h>
#include <freeradius-devel/process.h>
#include <freeradius-devel/dhcp.h>
#include <freeradius-devel/rad_assert.h>

#ifndef __MINGW32__
#include <sys/ioctl.h>
#endif

/*
 *	Same contents as listen_socket_t.
 */
typedef struct dhcp_socket_t {
	listen_socket_t	lsock;

	/*
	 *	DHCP-specific additions.
	 */
	bool		suppress_responses;
	RADCLIENT	dhcp_client;
	char const	*src_interface;
	fr_ipaddr_t	src_ipaddr;
} dhcp_socket_t;

#ifdef WITH_UDPFROMTO
static int dhcprelay_process_client_request(REQUEST *request)
{
	uint8_t maxhops = 16;
	VALUE_PAIR *vp, *giaddr;
	dhcp_socket_t *sock;

	rad_assert(request->packet->data[0] == 1);

	/*
	 *	Do the forward by ourselves, do not rely on dhcp_socket_send()
	 */
	request->reply->code = 0;

	/*
	 * It's invalid to have giaddr=0 AND a relay option
	 */
	giaddr = fr_pair_find_by_num(request->packet->vps, 266, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Gateway-IP-Address */
	if (giaddr && (giaddr->vp_ipaddr == htonl(INADDR_ANY)) &&
	    fr_pair_find_by_num(request->packet->vps, 82, DHCP_MAGIC_VENDOR, TAG_ANY)) { /* DHCP-Relay-Agent-Information */
		DEBUG("DHCP: Received packet with giaddr = 0 and containing relay option: Discarding packet\n");
		return 1;
	}

	/*
	 * RFC 1542 (BOOTP), page 15
	 *
	 * Drop requests if hop-count > 16 or admin specified another value
	 */
	if ((vp = fr_pair_find_by_num(request->config, 271, DHCP_MAGIC_VENDOR, TAG_ANY))) { /* DHCP-Relay-Max-Hop-Count */
	    maxhops = vp->vp_integer;
	}
	vp = fr_pair_find_by_num(request->packet->vps, 259, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Hop-Count */
	rad_assert(vp != NULL);
	if (vp->vp_integer > maxhops) {
		DEBUG("DHCP: Number of hops is greater than %d: not relaying\n", maxhops);
		return 1;
	} else {
	    /* Increment hop count */
	    vp->vp_integer++;
	}

	sock = request->listener->data;

	/*
	 *	Forward the request to the next server using the
	 *	incoming request as a template.
	 */
	/* set SRC ipaddr/port to the listener ipaddr/port */
	request->packet->src_ipaddr.af = AF_INET;
	request->packet->src_ipaddr.ipaddr.ip4addr.s_addr = sock->lsock.my_ipaddr.ipaddr.ip4addr.s_addr;
	request->packet->src_port = sock->lsock.my_port;

	vp = fr_pair_find_by_num(request->config, 270, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Relay-To-IP-Address */
	rad_assert(vp != NULL);

	/* set DEST ipaddr/port to the next server ipaddr/port */
	request->packet->dst_ipaddr.af = AF_INET;
	request->packet->dst_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;
	request->packet->dst_port = sock->lsock.my_port;

	if (fr_dhcp_encode(request->packet) < 0) {
		DEBUG("dhcprelay_process_client_request: ERROR in fr_dhcp_encode\n");
		return -1;
	}

	return fr_dhcp_send_socket(request->packet);
}


/*
 *	We've seen a reply from a server.
 *	i.e. we're a relay.
 */
static int dhcprelay_process_server_reply(REQUEST *request)
{
	VALUE_PAIR *vp, *giaddr;
	dhcp_socket_t *sock;

	rad_assert(request->packet->data[0] == 2);

	/*
	 * Do the forward by ourselves, do not rely on dhcp_socket_send()
	 */
	request->reply->code = 0;

	sock = request->listener->data;

	/*
	 * Check that packet is for us.
	 */
	giaddr = fr_pair_find_by_num(request->packet->vps, 266, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Gateway-IP-Address */

	/* --with-udpfromto is needed just for the following test */
	if (!giaddr || giaddr->vp_ipaddr != request->packet->dst_ipaddr.ipaddr.ip4addr.s_addr) {
		DEBUG("DHCP: Packet received from server was not for us (was for 0x%x). Discarding packet",
		    ntohl(request->packet->dst_ipaddr.ipaddr.ip4addr.s_addr));
		return 1;
	}

	/* set SRC ipaddr/port to the listener ipaddr/port */
	request->packet->src_ipaddr.af = AF_INET;
	request->packet->src_port = sock->lsock.my_port;

	/* set DEST ipaddr/port to clientip/68 or broadcast in specific cases */
	request->packet->dst_ipaddr.af = AF_INET;

	/*
	 *	We're a relay, and send the reply to giaddr.
	 */
	request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = giaddr->vp_ipaddr;
	request->reply->dst_port = request->packet->dst_port;		/* server port */

	if ((request->packet->code == PW_DHCP_NAK) ||
	    !sock->src_interface ||
	    ((vp = fr_pair_find_by_num(request->packet->vps, 262, DHCP_MAGIC_VENDOR, TAG_ANY)) /* DHCP-Flags */ &&
	     (vp->vp_integer & 0x8000) &&
	     ((vp = fr_pair_find_by_num(request->packet->vps, 263, DHCP_MAGIC_VENDOR, TAG_ANY)) /* DHCP-Client-IP-Address */ &&
	      (vp->vp_ipaddr == htonl(INADDR_ANY))))) {
		/*
		 * RFC 2131, page 23
		 *
		 * Broadcast on
		 * - DHCPNAK
		 * or
		 * - Broadcast flag is set up and ciaddr == NULL
		 */
		RDEBUG("DHCP: response will be  broadcast");
		request->packet->dst_ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_BROADCAST);
	} else {
		/*
		 * RFC 2131, page 23
		 *
		 * Unicast to
		 * - ciaddr if present
		 * otherwise to yiaddr
		 */
		if ((vp = fr_pair_find_by_num(request->packet->vps, 263, DHCP_MAGIC_VENDOR, TAG_ANY)) /* DHCP-Client-IP-Address */ &&
		    (vp->vp_ipaddr != htonl(INADDR_ANY))) {
			request->packet->dst_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;
		} else {
			vp = fr_pair_find_by_num(request->packet->vps, 264, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Your-IP-Address */
			if (!vp) {
				DEBUG("DHCP: Failed to find IP Address for request");
				return -1;
			}

			RDEBUG("DHCP: response will be unicast to your-ip-address");
			request->packet->dst_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;

			/*
			 * When sending a DHCP_OFFER, make sure our ARP table
			 * contains an entry for the client IP address, or else
			 * packet may not be forwarded if it was the first time
			 * the client was requesting an IP address.
			 */
			if (request->packet->code == PW_DHCP_OFFER) {
				VALUE_PAIR *hwvp = fr_pair_find_by_num(request->packet->vps, 267, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Client-Hardware-Address */
				if (hwvp == NULL) {
					DEBUG("DHCP: DHCP_OFFER packet received with "
					    "no Client Hardware Address. Discarding packet");
					return 1;
				}
				if (fr_dhcp_add_arp_entry(request->packet->sockfd, sock->src_interface, hwvp, vp) < 0) {
					DEBUG("%s", fr_strerror());
					return -1;
				}
			}
		}
	}

	if (fr_dhcp_encode(request->packet) < 0) {
		DEBUG("dhcprelay_process_server_reply: ERROR in fr_dhcp_encode\n");
		return -1;
	}

	return fr_dhcp_send_socket(request->packet);
}
#else  /* WITH_UDPFROMTO */
static int dhcprelay_process_server_reply(UNUSED REQUEST *request)
{
	WARN("DHCP Relaying requires the server to be configured with UDPFROMTO");
	return -1;
}

static int dhcprelay_process_client_request(UNUSED REQUEST *request)
{
	WARN("DHCP Relaying requires the server to be configured with UDPFROMTO");
	return -1;
}

#endif	/* WITH_UDPFROMTO */

static const uint32_t attrnums[] = {
	57,	/* DHCP-DHCP-Maximum-Msg-Size */
	256,	/* DHCP-Opcode */
	257,	/* DHCP-Hardware-Type */
	258,	/* DHCP-Hardware-Address-Length */
	259,	/* DHCP-Hop-Count */
	260,	/* DHCP-Transaction-Id */
	262,	/* DHCP-Flags */
	263,	/* DHCP-Client-IP-Address */
	266,	/* DHCP-Gateway-IP-Address */
	267	/* DHCP-Client-Hardware-Address */
};

static int dhcp_process(REQUEST *request)
{
	int rcode;
	unsigned int i;
	VALUE_PAIR *vp;
	dhcp_socket_t *sock;

	/*
	 *	If there's a giaddr, save it as the Relay-IP-Address
	 *	in the response.  That way the later code knows where
	 *	to send the reply.
	 */
	vp = fr_pair_find_by_num(request->packet->vps, 266, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Gateway-IP-Address */
	if (vp && (vp->vp_ipaddr != htonl(INADDR_ANY))) {
		VALUE_PAIR *relay;

		/* DHCP-Relay-IP-Address */
		relay = radius_pair_create(request->reply, &request->reply->vps,
					  272, DHCP_MAGIC_VENDOR);
		if (relay) relay->vp_ipaddr = vp->vp_ipaddr;
	}

	vp = fr_pair_find_by_num(request->packet->vps, 53, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Message-Type */
	if (vp) {
		DICT_VALUE *dv = dict_valbyattr(53, DHCP_MAGIC_VENDOR, vp->vp_integer);
		DEBUG("Trying sub-section dhcp %s {...}",
		      dv ? dv->name : "<unknown>");
		rcode = process_post_auth(vp->vp_integer, request);
	} else {
		DEBUG("DHCP: Failed to find DHCP-Message-Type in packet!");
		rcode = RLM_MODULE_FAIL;
	}

	vp = fr_pair_find_by_num(request->reply->vps, 53, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Message-Type */
	if (vp) {
		request->reply->code = vp->vp_integer;
		if ((request->reply->code != 0) &&
		    (request->reply->code < PW_DHCP_OFFSET)) {
			request->reply->code += PW_DHCP_OFFSET;
		}
	}
	else switch (rcode) {
	case RLM_MODULE_OK:
	case RLM_MODULE_UPDATED:
		if (request->packet->code == PW_DHCP_DISCOVER) {
			request->reply->code = PW_DHCP_OFFER;
			break;

		} else if (request->packet->code == PW_DHCP_REQUEST) {
			request->reply->code = PW_DHCP_ACK;
			break;
		}
		request->reply->code = PW_DHCP_NAK;
		break;

	default:
	case RLM_MODULE_REJECT:
	case RLM_MODULE_FAIL:
	case RLM_MODULE_INVALID:
	case RLM_MODULE_NOOP:
	case RLM_MODULE_NOTFOUND:
		if (request->packet->code == PW_DHCP_DISCOVER) {
			request->reply->code = 0; /* ignore the packet */
		} else {
			request->reply->code = PW_DHCP_NAK;
		}
		break;

	case RLM_MODULE_HANDLED:
		request->reply->code = 0; /* ignore the packet */
		break;
	}

	/*
	 *	TODO: Handle 'output' of RLM_MODULE when acting as a
	 *	DHCP relay We may want to not forward packets in
	 *	certain circumstances.
	 */

	/*
	 * 	Handle requests when acting as a DHCP relay
	 */
	vp = fr_pair_find_by_num(request->packet->vps, 256, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Opcode */
	if (!vp) {
		RDEBUG("FAILURE: Someone deleted the DHCP-Opcode!");
		return 1;
	}

	/* BOOTREPLY received on port 67 (i.e. from a server) */
	if (vp->vp_integer == 2) {
		return dhcprelay_process_server_reply(request);
	}

	/* Packet from client, and we have DHCP-Relay-To-IP-Address */
	if (fr_pair_find_by_num(request->config, 270, DHCP_MAGIC_VENDOR, TAG_ANY)) {
		return dhcprelay_process_client_request(request);
	}

	/* else it's a packet from a client, without relaying */
	rad_assert(vp->vp_integer == 1); /* BOOTREQUEST */

	sock = request->listener->data;

	/*
	 *	Handle requests when acting as a DHCP server
	 */

	/*
	 *	Releases don't get replies.
	 */
	if (request->packet->code == PW_DHCP_RELEASE) {
		request->reply->code = 0;
	}

	if (request->reply->code == 0) {
		return 1;
	}

	request->reply->sockfd = request->packet->sockfd;

	/*
	 *	Copy specific fields from packet to reply, if they
	 *	don't already exist
	 */
	for (i = 0; i < sizeof(attrnums) / sizeof(attrnums[0]); i++) {
		uint32_t attr = attrnums[i];

		if (fr_pair_find_by_num(request->reply->vps, attr, DHCP_MAGIC_VENDOR, TAG_ANY)) continue;

		vp = fr_pair_find_by_num(request->packet->vps, attr, DHCP_MAGIC_VENDOR, TAG_ANY);
		if (vp) {
			fr_pair_add(&request->reply->vps, fr_pair_copy(request->reply, vp));
		}
	}

	vp = fr_pair_find_by_num(request->reply->vps, 256, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Opcode */
	rad_assert(vp != NULL);
	vp->vp_integer = 2; /* BOOTREPLY */

	/*
	 *	Allow NAKs to be delayed for a short period of time.
	 */
	if (request->reply->code == PW_DHCP_NAK) {
		vp = fr_pair_find_by_num(request->reply->vps, PW_FREERADIUS_RESPONSE_DELAY, 0, TAG_ANY);
		if (vp) {
			if (vp->vp_integer <= 10) {
				request->response_delay.tv_sec = vp->vp_integer;
				request->response_delay.tv_usec = 0;
			} else {
				request->response_delay.tv_sec = 10;
				request->response_delay.tv_usec = 0;
			}
		} else {
#define USEC 1000000
			vp = fr_pair_find_by_num(request->reply->vps, PW_FREERADIUS_RESPONSE_DELAY_USEC, 0, TAG_ANY);
			if (vp) {
				if (vp->vp_integer <= 10 * USEC) {
					request->response_delay.tv_sec = vp->vp_integer / USEC;
					request->response_delay.tv_usec = vp->vp_integer % USEC;
				} else {
					request->response_delay.tv_sec = 10;
					request->response_delay.tv_usec = 0;
				}
			}
		}
	}

	/*
	 *	Prepare the reply packet for sending through dhcp_socket_send()
	 */
	request->reply->dst_ipaddr.af = AF_INET;
	request->reply->src_ipaddr.af = AF_INET;
	request->reply->src_ipaddr.ipaddr.ip4addr.s_addr = sock->src_ipaddr.ipaddr.ip4addr.s_addr;
	request->reply->src_ipaddr.prefix = 32;

	/*
	 *	They didn't set a proper src_ipaddr, but we want to
	 *	send the packet with a source IP.  If there's a server
	 *	identifier, use it.
	 */
	if (request->reply->src_ipaddr.ipaddr.ip4addr.s_addr == INADDR_ANY) {
		vp = fr_pair_find_by_num(request->reply->vps, PW_PACKET_SRC_IP_ADDRESS, 0, TAG_ANY);
		if (!vp) vp = fr_pair_find_by_num(request->reply->vps, 54, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-DHCP-Server-Identifier */
		if (vp) {
			request->reply->src_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;
		}
	}

	request->reply->dst_port = request->packet->src_port;
	request->reply->src_port = request->packet->dst_port;

	/*
	 *	Answer to client's nearest DHCP relay.
	 *
	 *	Which may be different than the giaddr given in the
	 *	packet to the client.  i.e. the relay may have a
	 *	public IP, but the gateway a private one.
	 */
	vp = fr_pair_find_by_num(request->reply->vps, 272, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Relay-IP-Address */
	if (vp && (vp->vp_ipaddr != ntohl(INADDR_ANY))) {
		RDEBUG("DHCP: Reply will be unicast to giaddr from original packet");
		request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;
		request->reply->dst_port = request->packet->dst_port;

		vp = fr_pair_find_by_num(request->reply->vps, PW_PACKET_DST_PORT, 0, TAG_ANY);
		if (vp) request->reply->dst_port = vp->vp_integer;

		return 1;
	}

	/*
	 *	Answer to client's nearest DHCP gateway.  In this
	 *	case, the client can reach the gateway, as can the
	 *	server.
	 *
	 *	We also use *our* source port as the destination port.
	 *	Gateways are servers, and listen on the server port,
	 *	not the client port.
	 */
	vp = fr_pair_find_by_num(request->reply->vps, 266, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Gateway-IP-Address */
	if (vp && (vp->vp_ipaddr != htonl(INADDR_ANY))) {
		RDEBUG("DHCP: Reply will be unicast to giaddr");
		request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;
		request->reply->dst_port = request->packet->dst_port;
		return 1;
	}

	/*
	 *	If it's a NAK, or the broadcast flag was set, ond
	 *	there's no client-ip-address, send a broadcast.
	 */
	if ((request->reply->code == PW_DHCP_NAK) ||
	    ((vp = fr_pair_find_by_num(request->reply->vps, 262, DHCP_MAGIC_VENDOR, TAG_ANY)) && /* DHCP-Flags */
	     (vp->vp_integer & 0x8000) &&
	     ((vp = fr_pair_find_by_num(request->reply->vps, 263, DHCP_MAGIC_VENDOR, TAG_ANY)) && /* DHCP-Client-IP-Address */
	      (vp->vp_ipaddr == htonl(INADDR_ANY))))) {
		/*
		 * RFC 2131, page 23
		 *
		 * Broadcast on
		 * - DHCPNAK
		 * or
		 * - Broadcast flag is set up and ciaddr == NULL
		 */
		RDEBUG("DHCP: Reply will be broadcast");
		request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_BROADCAST);
		return 1;
	}

	/*
	 *	RFC 2131, page 23
	 *
	 *	Unicast to ciaddr if present, otherwise to yiaddr.
	 */
	if ((vp = fr_pair_find_by_num(request->reply->vps, 263, DHCP_MAGIC_VENDOR, TAG_ANY)) && /* DHCP-Client-IP-Address */
	    (vp->vp_ipaddr != htonl(INADDR_ANY))) {
		RDEBUG("DHCP: Reply will be sent unicast to client-ip-address");
		request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;
		return 1;
	}

	vp = fr_pair_find_by_num(request->reply->vps, 264, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Your-IP-Address */
	if (!vp) {
		RDEBUG("DHCP: Failed to find DHCP-Client-IP-Address or DHCP-Your-IP-Address for request; "
		       "not responding");
		/*
		 *	There is nowhere to send the response to, so don't bother.
		 */
		request->reply->code = 0;
		return -1;
	}

#ifdef SIOCSARP
	/*
	 *	The system is configured to listen for broadcast
	 *	packets, which means we'll need to send unicast
	 *	replies, to IPs which haven't yet been assigned.
	 *	Therefore, we need to update the ARP table.
	 *
	 *	However, they haven't specified a interface.  So we
	 *	can't update the ARP table.  And we must send a
	 *	broadcast response.
	 */
	if (sock->lsock.broadcast && !sock->src_interface) {
		WARN("You MUST set \"interface\" if you have \"broadcast = yes\"");
		RDEBUG("DHCP: Reply will be broadcast as no interface was defined");
		request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_BROADCAST);
		return 1;
	}

	RDEBUG("DHCP: Reply will be unicast to your-ip-address");
	request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = vp->vp_ipaddr;

	/*
	 *	When sending a DHCP_OFFER, make sure our ARP table
	 *	contains an entry for the client IP address.
	 *	Otherwise the packet may not be sent to the client, as
	 *	the OS has no ARP entry for it.
	 *
	 *	This is a cute hack to avoid us having to create a raw
	 *	socket to send DHCP packets.
	 */
	if (request->reply->code == PW_DHCP_OFFER) {
		VALUE_PAIR *hwvp = fr_pair_find_by_num(request->reply->vps, 267, DHCP_MAGIC_VENDOR, TAG_ANY); /* DHCP-Client-Hardware-Address */

		if (!hwvp) return -1;

		if (fr_dhcp_add_arp_entry(request->reply->sockfd, sock->src_interface, hwvp, vp) < 0) {
			RDEBUG("Failed adding arp entry: %s", fr_strerror());
			return -1;
		}
	}
#else
	if (request->packet->src_ipaddr.ipaddr.ip4addr.s_addr != ntohl(INADDR_NONE)) {
		RDEBUG("DHCP: Reply will be unicast to the unicast source IP address");
		request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = request->packet->src_ipaddr.ipaddr.ip4addr.s_addr;
	} else {
		RDEBUG("DHCP: Reply will be broadcast as this system does not support ARP updates");
		request->reply->dst_ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_BROADCAST);
	}
#endif

	return 1;
}

/*
 *	We allow using PCAP, but only if there's no SO_BINDTODEVICE
 */
#ifndef SO_BINDTODEVICE
#ifdef HAVE_PCAP_H
#define PCAP_RAW_SOCKETS (1)
#endif
#endif

#ifdef PCAP_RAW_SOCKETS
/** Build PCAP filter string to pass to libpcap based on listen section
 * Will be called by init_pcap.
 *
 * @param this listen section
 * @return PCAP filter string
 */
static const char *dhcp_pcap_filter_build(rad_listen_t *this)
{
	dhcp_socket_t	*sock = this->data;
	char		*filter;

	/*
	 *	Set the port filter
	 */
	filter = talloc_strdup(this, "(udp and dst port ");
	if (sock->lsock.my_port) {
		filter = talloc_asprintf_append_buffer(filter, "%u)",  sock->lsock.my_port);
	} else {
		filter = talloc_strdup_append_buffer(filter, "bootps)");
	}

	if (!fr_is_inaddr_any(&sock->lsock.my_ipaddr)) {
		char buffer[INET_ADDRSTRLEN];
		ip_ntoh(&sock->lsock.my_ipaddr, buffer, sizeof(buffer));

		if (sock->lsock.broadcast) {
			filter = talloc_asprintf_append_buffer(filter, " and (dst host %s or dst host 255.255.255.255)",
							       buffer);
		} else {
			filter = talloc_asprintf_append_buffer(filter, " and dst host %s", buffer);
		}
	}

	return filter;
}
#endif

static int dhcp_socket_parse(CONF_SECTION *cs, rad_listen_t *this)
{
	int rcode;
	dhcp_socket_t *sock = this->data;
	RADCLIENT *client;
	CONF_PAIR *cp;

#ifdef PCAP_RAW_SOCKETS
	sock->lsock.pcap_filter_builder = dhcp_pcap_filter_build;
	sock->lsock.pcap_type = PCAP_INTERFACE_IN_OUT;
#endif

	/*
	 *	Set if before parsing, so the user can forcibly turn
	 *	it off later.
	 */
	this->nodup = true;

	rcode = common_socket_parse(cs, this);
	if (rcode != 0) return rcode;

	if (check_config) return 0;

	if (!sock->lsock.interface) WARN("No \"interface\" setting is defined.  Only unicast DHCP will work");

	/*
	 *	Undocumented extension for testing without
	 *	destroying your network!
	 */
	sock->suppress_responses = false;
	cp = cf_pair_find(cs, "suppress_responses");
	if (cp) {
		rcode = cf_item_parse(cs, "suppress_responses", FR_ITEM_POINTER(PW_TYPE_BOOLEAN, &sock->suppress_responses), NULL, T_INVALID);
		if (rcode < 0) return -1;
	}

	cp = cf_pair_find(cs, "src_interface");
	if (cp) {
		rcode = cf_item_parse(cs, "src_interface", FR_ITEM_POINTER(PW_TYPE_STRING, &sock->src_interface), NULL, T_INVALID);
		if (rcode < 0) return -1;
	} else {
		sock->src_interface = sock->lsock.interface;
	}

	if (!sock->src_interface && sock->lsock.interface) {
		sock->src_interface = talloc_typed_strdup(sock, sock->lsock.interface);
	}

	/*
	 *	Set the source IP address explicitly.
	 */
	cp = cf_pair_find(cs, "src_ipaddr");
	if (cp) {
		memset(&sock->src_ipaddr, 0, sizeof(sock->src_ipaddr));
		sock->src_ipaddr.ipaddr.ip4addr.s_addr = htonl(INADDR_NONE);
		rcode = cf_item_parse(cs, "src_ipaddr", FR_ITEM_POINTER(PW_TYPE_IPV4_ADDR, &sock->src_ipaddr), NULL, T_INVALID);
		if (rcode < 0) return -1;

		sock->src_ipaddr.af = AF_INET;
	/*
	 *	Or by looking up the IP address associated with the
	 *	src_interface or interface (if we're binding to INADDR_ANY).
	 */
	} else {
		char		buffer[INET_ADDRSTRLEN];
		fr_ipaddr_t	ipaddr;

		if (fr_is_inaddr_any(&sock->lsock.my_ipaddr) && sock->src_interface) {
			if (fr_ipaddr_from_interface(&ipaddr, AF_INET, sock->src_interface) < 0) {
				WARN("Failed resolving interface %s to IP address: %s", sock->src_interface,
				     fr_strerror());
				WARN("Will continue, but source address must be set within the DHCP virtual server");
				goto src_addr_is_bound_addr;
			}
			rad_assert(ipaddr.af == AF_INET);
		} else {
		src_addr_is_bound_addr:
			memcpy(&ipaddr, &sock->lsock.my_ipaddr, sizeof(ipaddr));
		}

		/*
		 *	Magic defaults FTW.
		 *
		 *	This lets %{config:} work as expected, if we want to set
		 *	DHCP-DHCP-Server-Identifier.
		 */
		inet_ntop(ipaddr.af, &ipaddr.ipaddr, buffer, sizeof(buffer));
		cp = cf_pair_alloc(cs, "src_ipaddr", buffer, T_OP_SET, T_BARE_WORD, T_BARE_WORD);
		cf_pair_add(cs, cp);
		rcode = cf_item_parse(cs, "src_ipaddr", FR_ITEM_POINTER(PW_TYPE_IPV4_ADDR, &sock->src_ipaddr),
				      NULL, T_INVALID);
		if (rcode < 0) return -1;
	}

	/*
	 *	Initialize the fake client.
	 */
	client = &sock->dhcp_client;
	memset(client, 0, sizeof(*client));
	client->ipaddr.af = AF_INET;
	client->ipaddr.ipaddr.ip4addr.s_addr = ntohl(INADDR_NONE);
	client->ipaddr.prefix = 0;
	client->longname = client->shortname = "dhcp";
	client->secret = client->shortname;
	client->nas_type = talloc_typed_strdup(sock, "none");

	return 0;
}


/*
 *	Check if an incoming request is "ok"
 *
 *	It takes packets, not requests.  It sees if the packet looks
 *	OK.  If so, it does a number of sanity checks on it.
 */
static int dhcp_socket_recv(rad_listen_t *listener)
{
	RADIUS_PACKET	*packet;
	dhcp_socket_t	*sock = listener->data;

#ifdef PCAP_RAW_SOCKETS
	if (sock->lsock.pcap) {
		packet = fr_dhcp_recv_pcap(sock->lsock.pcap);
	} else
#endif
	{
		packet = fr_dhcp_recv_socket(listener->fd);
	}

	if (!packet) {
		ERROR("%s", fr_strerror());
		return 0;
	}

	if (!request_receive(NULL, listener, packet, &sock->dhcp_client, dhcp_process)) {
		rad_free(&packet);
		return 0;
	}

	return 1;
}


/*
 *	Send an authentication response packet
 */
static int dhcp_socket_send(rad_listen_t *listener, REQUEST *request)
{
	dhcp_socket_t	*sock = listener->data;

	rad_assert(request->listener == listener);
	rad_assert(listener->send == dhcp_socket_send);

	if (request->reply->code == 0) return 0; /* don't reply */

	if (fr_dhcp_encode(request->reply) < 0) {
		DEBUG("dhcp_socket_send: ERROR\n");
		return -1;
	}

	if (sock->suppress_responses) return 0;

#ifdef PCAP_RAW_SOCKETS
	if (sock->lsock.pcap) {
		/* set ethernet destination address to DHCP-Client-Hardware-Address in request. */
		uint8_t dhmac[ETHER_HDR_LEN] = { 0 };
		bool found = false;
		VALUE_PAIR *vp;
		if ((vp = fr_pair_find_by_num(request->packet->vps, 267, DHCP_MAGIC_VENDOR, TAG_ANY))) {
			if (vp->data.length == sizeof(vp->vp_ether)) {
				memcpy(dhmac, vp->vp_ether, vp->data.length);
				found = true;
			}
		}

		if (!found) {
			DEBUG("DHCP-Client-Hardware-Address not found in request: ERROR\n");
			return -1;
		}

		return fr_dhcp_send_pcap(sock->lsock.pcap, dhmac, request->reply);
	} else
#endif
	{
		return fr_dhcp_send_socket(request->reply);
	}
}

/*
 *	Debug the packet if requested.
 */
static void dhcp_packet_debug(REQUEST *request, RADIUS_PACKET *packet, bool received)
{
	char src_ipaddr[128];
	char dst_ipaddr[128];

	if (!packet) return;
	if (!RDEBUG_ENABLED) return;

	/*
	 *	Client-specific debugging re-prints the input
	 *	packet into the client log.
	 *
	 *	This really belongs in a utility library
	 */
	if ((packet->code > PW_DHCP_OFFSET) && (packet->code < PW_DHCP_MAX)) {
		RDEBUG("%s %s Id %08x from %s%s%s:%i to %s%s%s:%i length %zu",
		       received ? "Received" : "Sent",
		       dhcp_message_types[packet->code - PW_DHCP_OFFSET],
		       packet->id,
		       packet->src_ipaddr.af == AF_INET6 ? "[" : "",
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 src_ipaddr, sizeof(src_ipaddr)),
		       packet->src_ipaddr.af == AF_INET6 ? "]" : "",
		       packet->src_port,
		       packet->dst_ipaddr.af == AF_INET6 ? "[" : "",
		       inet_ntop(packet->dst_ipaddr.af,
				 &packet->dst_ipaddr.ipaddr,
				 dst_ipaddr, sizeof(dst_ipaddr)),
		       packet->dst_ipaddr.af == AF_INET6 ? "]" : "",
		       packet->dst_port,
		       packet->data_len);
	} else {
		RDEBUG("%s code %u Id %08x from %s%s%s:%i to %s%s%s:%i length %zu\n",
		       received ? "Received" : "Sent",
		       packet->code,
		       packet->id,
		       packet->src_ipaddr.af == AF_INET6 ? "[" : "",
		       inet_ntop(packet->src_ipaddr.af,
				 &packet->src_ipaddr.ipaddr,
				 src_ipaddr, sizeof(src_ipaddr)),
		       packet->src_ipaddr.af == AF_INET6 ? "]" : "",
		       packet->src_port,
		       packet->dst_ipaddr.af == AF_INET6 ? "[" : "",
		       inet_ntop(packet->dst_ipaddr.af,
				 &packet->dst_ipaddr.ipaddr,
				 dst_ipaddr, sizeof(dst_ipaddr)),
		       packet->dst_ipaddr.af == AF_INET6 ? "]" : "",
		       packet->dst_port,
		       packet->data_len);
	}

	if (received) {
		rdebug_pair_list(L_DBG_LVL_1, request, packet->vps, NULL);
	} else {
		rdebug_proto_pair_list(L_DBG_LVL_1, request, packet->vps);
	}
}

static int dhcp_socket_encode(UNUSED rad_listen_t *listener, UNUSED REQUEST *request)
{
	DEBUG2("NO ENCODE!");
	return 0;
}


static int dhcp_socket_decode(UNUSED rad_listen_t *listener, REQUEST *request)
{
	return fr_dhcp_decode(request->packet);
}

extern fr_protocol_t proto_dhcp;
fr_protocol_t proto_dhcp = {
	.magic		= RLM_MODULE_INIT,
	.name		= "dhcp",
	.inst_size	= sizeof(dhcp_socket_t),
	.parse		= dhcp_socket_parse,
	.recv		= dhcp_socket_recv,
	.send		= dhcp_socket_send,
	.print		= common_socket_print,
	.debug		= dhcp_packet_debug,
	.encode		= dhcp_socket_encode,
	.decode		= dhcp_socket_decode
};
