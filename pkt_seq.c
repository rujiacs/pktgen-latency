#include "util.h"
#include "pkt_seq.h"

#include <rte_hash_crc.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>

#define IP_VERSION 0x40
#define IP_HDRLEN 0x05
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)
#define IP_TTL_DEF 64

static struct rte_ether_addr mac_src = {
	.addr_bytes = {12},
};
static struct rte_ether_addr mac_dst = {
	.addr_bytes = {21},
};

static uint64_t pkt_idx = 0;

static void __parse_mac_addr(const char *str,
				struct rte_ether_addr *addr)
{
	uint32_t mac[6] = {0};
	int ret = 0, i = 0;

	ret = sscanf(str, "%x:%x:%x:%x:%x:%x",
			&mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
	if (ret < 6) {
		LOG_ERROR("Failed to parse MAC from string %s", str);
	}

	for (i = 0; i < 6; i++) {
		addr->addr_bytes[i] = mac[i] & 0xff;
	}
}

void pkt_seq_set_default_mac(void)
{
	__parse_mac_addr(PKT_SEQ_MAC_SRC, &mac_src);
	__parse_mac_addr(PKT_SEQ_MAC_DST, &mac_dst);
}

void pkt_seq_init(struct pkt_seq_info *info)
{
	info->src_ip = PKT_SEQ_IP_SRC;
	info->dst_ip = PKT_SEQ_IP_DST;
	info->proto = PKT_SEQ_PROTO;

	info->src_port = PKT_SEQ_PORT_SRC;
	info->dst_port = PKT_SEQ_PORT_DST;

	info->pkt_len = PKT_SEQ_PKT_LEN;
//	info->seq_cnt = PKT_SEQ_CNT;
}

static void __setup_ip_hdr(struct rte_ipv4_hdr *ip)
{
	/* Setup IPv4 header */
	ip->version_ihl = IP_VHL_DEF;
	ip->type_of_service = 0;
	ip->fragment_offset = 0;
	ip->time_to_live = IP_TTL_DEF;

	/* Compute IPv4 header checksum */
	ip->hdr_checksum = rte_ipv4_cksum(ip);
}

void pkt_seq_setup_tcpip(struct pkt_seq_info *info,
				struct tcpip_hdr *tcpip, bool is_latency)
{
	memset(tcpip, 0, sizeof(struct tcpip_hdr));

	/* Setup TCP header */
	tcpip->tcp.src_port = rte_cpu_to_be_16(info->src_port);
	tcpip->tcp.dst_port = rte_cpu_to_be_16(info->dst_port);
	tcpip->tcp.sent_seq = rte_cpu_to_be_32(PKT_SEQ_TCP_SEQ);
	tcpip->tcp.recv_ack = rte_cpu_to_be_32(PKT_SEQ_TCP_ACK);
	tcpip->tcp.data_off = ((sizeof(struct rte_tcp_hdr) / sizeof(uint32_t)) << 4);
	tcpip->tcp.tcp_flags = PKT_SEQ_TCP_FLAGS;
	tcpip->tcp.rx_win = rte_cpu_to_be_16(PKT_SEQ_TCP_WINDOW);
	tcpip->tcp.tcp_urp = 0;

	/* Setup part of IP header */
	tcpip->ip.src_addr = rte_cpu_to_be_32(info->src_ip);
	tcpip->ip.dst_addr = rte_cpu_to_be_32(info->dst_ip);
	tcpip->ip.total_length = rte_cpu_to_be_16(info->pkt_len
								- sizeof(struct rte_ether_hdr));
	tcpip->ip.next_proto_id = IPPROTO_TCP;

	if (is_latency)
		tcpip->ip.packet_id = PKT_SEQ_LATENCY_PKTID;
	else
		tcpip->ip.packet_id = 0;

	/* Calculate tcp checksum */
//	tlen = info->pkt_len - sizeof(struct rte_ether_hdr);
	tcpip->tcp.cksum = rte_ipv4_udptcp_cksum(
					&(tcpip->ip), (const void *)&(tcpip->tcp));

	LOG_DEBUG("IP len %u", (info->pkt_len - sizeof(struct rte_ether_hdr)));

	/* Setup remaining part of ip header */
	__setup_ip_hdr(&tcpip->ip);
}

void pkt_seq_setup_udpip(struct pkt_seq_info *info,
				struct udpip_hdr *udpip, bool is_latency)
{
	struct rte_udp_hdr *udp = &udpip->udp;
	struct rte_ipv4_hdr *ip = &udpip->ip;

	memset(udpip, 0, sizeof(struct udpip_hdr));

	/* Setup UDP header */
	udp->src_port = rte_cpu_to_be_16(info->src_port);
	udp->dst_port = rte_cpu_to_be_16(info->dst_port);
	udp->dgram_len = rte_cpu_to_be_16(info->pkt_len
										- sizeof(struct rte_ether_hdr)
										- sizeof(struct rte_ipv4_hdr));

	/* Setup part of IPv4 header */
	ip->next_proto_id = IPPROTO_UDP;
	ip->total_length = rte_cpu_to_be_16(info->pkt_len
										- sizeof(struct rte_ether_hdr));
	ip->src_addr = rte_cpu_to_be_32(info->src_ip);
	ip->dst_addr = rte_cpu_to_be_32(info->dst_ip);

	if (is_latency)
		tcpip->ip.packet_id = PKT_SEQ_LATENCY_PKTID;
	else
		tcpip->ip.packet_id = 0;

	/* Calculate UDP checksum */
	udp->dgram_cksum = 0;

	/* Setup remaining part of ip header */
	__setup_ip_hdr(ip);
}

void __setup_latency(struct rte_mbuf *mbuf)
{
	struct pkt_latency *lat = NULL;

	if (mbuf->pkt_len < PKT_SEQ_LATENCY_MINSIZE) {
		LOG_ERROR("Packet (len = %u) dones't have enough space for"
					" latency fields", mbuf->len);
		return;
	}
	lat = rte_pktmbuf_mtod_offset(mbuf, struct pkt_latency*,
						mbuf->pkt_len - sizeof(struct pkt_latency));
	lat->id = pkt_idx;
	lat->timestamp = rte_get_tsc_cycles();
	pkt_idx ++;
}

void pkt_seq_fill_mbuf(struct rte_mbuf *mbuf, struct pkt_seq_info *info,
						bool is_latency)
{
	struct rte_ether_hdr *eth_hdr;
	uint8_t *payload = NULL;
	unsigned len = 0;

	if (info == NULL) {
		LOG_ERROR("Wrong data to fill into mbuf");
		return;
	}

	mbuf->pkt_len = info->pkt_len;
	mbuf->data_len = info->pkt_len;

	// /* Setup payload */
	// if (info->proto == IPPROTO_TCP) {
	// 	len = info->pkt_len - sizeof(struct rte_ether_hdr)
	// 						- sizeof(struct tcpip_hdr);
	// 	payload = rte_pktmbuf_mtod_offset(mbuf, uint8_t *,
	// 					sizeof(struct rte_ether_hdr) + sizeof(struct tcpip_hdr));
	// } else {
	// 	len = info->pkt_len - sizeof(struct rte_ether_hdr)
	// 						- sizeof(struct udpip_hdr);
	// 	payload = rte_pktmbuf_mtod_offset(mbuf, uint8_t *,
	// 					sizeof(struct rte_ether_hdr) + sizeof(struct udpip_hdr));
	// }
	// memset(payload, 0, len);
	// LOG_DEBUG("payload len %u", len);
	if (is_latency)
		__setup_latency(mbuf);

	/* Setup TCP/UDP+IP */
	if (info->proto == IPPROTO_TCP) {
		struct tcpip_hdr *tcpip = NULL;

		tcpip = rte_pktmbuf_mtod_offset(mbuf, struct tcpip_hdr*,
						sizeof(struct rte_ether_hdr));
		pkt_seq_setup_tcpip(info, tcpip, is_latency);

	} else {
		struct udpip_hdr *udpip;

		udpip = rte_pktmbuf_mtod_offset(mbuf, struct udpip_hdr*,
						sizeof(struct rte_ether_hdr));
		pkt_seq_setup_udpip(info, udpip, is_latency);
	}

	/* Setup Ethernet header */
	eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
	rte_ether_addr_copy(&mac_src, &eth_hdr->s_addr);
	rte_ether_addr_copy(&mac_dst, &eth_hdr->d_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	// /* Setup Eth FCS */
	// crc = rte_pktmbuf_mtod_offset(mbuf, uint32_t*, info->pkt_len);
	// *crc = rte_hash_crc(rte_pktmbuf_mtod(mbuf, void *),
	// 				info->pkt_len, 0);
}

struct pkt_latency *pkt_seq_get_latency(struct rte_mbuf *mbuf)
{
	struct rte_ether_hdr *eth_hdr = NULL;
	struct rte_ipv4_hdr *ip_hdr = NULL;

	eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

	if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
		LOG_INFO("Not IPv4");
		return NULL;
	}

	ip_hdr = rte_pktmbuf_mtod_offset(mbuf, struct rte_ipv4_hdr *,
					sizeof(struct rte_ether_hdr));
	if (ip_hdr->packet_id != PKT_SEQ_LATENCY_PKTID ||
					mbuf->pkt_len < PKT_SEQ_LATENCY_MINSIZE) {
		LOG_INFO("Not Latency packet");
		return NULL;
	}

	return rte_pktmbuf_mtod_offset(mbuf, struct pkt_latency *,
					mbuf->pkt_len - sizeof(struct pkt_latency));
}
