#include "util.h"
#include "pkt_seq.h"

#include <rte_hash_crc.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>

#define IP_VERSION 0x40
#define IP_HDRLEN 0x05
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)
#define IP_TTL_DEF 64

static struct ether_addr mac_src = {
	.addr_bytes = {12},
};
static struct ether_addr mac_dst = {
	.addr_bytes = {21},
};

static void __parse_mac_addr(const char *str,
				struct ether_addr *addr)
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

void pkt_seq_set_mac_src(const char *str)
{
	__parse_mac_addr(str, &mac_src);
}

void pkt_seq_set_mac_dst(const char *str)
{
	__parse_mac_addr(str, &mac_dst);
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

static uint16_t __checksum_16(const void *data, uint32_t len)
{
	uint32_t crc32 = 0;

	crc32 = rte_hash_crc(data, len, 0);

	crc32 = (crc32 & 0xffff) + (crc32 >> 16);
	crc32 = (crc32 & 0xffff) + (crc32 >> 16);

	return ~((uint16_t)crc32);
}

static void __setup_ip_hdr(struct ipv4_hdr *ip)
{
	/* Setup IPv4 header */
	ip->version_ihl = IP_VHL_DEF;
	ip->type_of_service = 0;
	ip->fragment_offset = 0;
	ip->time_to_live = IP_TTL_DEF;
	ip->packet_id = 0;

	/* Compute IPv4 header checksum */
	ip->hdr_checksum = __checksum_16(ip, sizeof(struct ipv4_hdr));
}

void pkt_seq_setup_tcpip(struct pkt_seq_info *info,
				struct tcpip_hdr *tcpip)
{
	uint16_t tlen = 0;

	memset(tcpip, 0, sizeof(struct tcpip_hdr));

	/* Setup TCP header */
	tcpip->tcp.src_port = rte_cpu_to_be_16(info->src_port);
	tcpip->tcp.dst_port = rte_cpu_to_be_16(info->dst_port);
	tcpip->tcp.sent_seq = rte_cpu_to_be_32(PKT_SEQ_TCP_SEQ);
	tcpip->tcp.recv_ack = rte_cpu_to_be_32(PKT_SEQ_TCP_ACK);
	tcpip->tcp.data_off = ((sizeof(struct tcp_hdr) / sizeof(uint32_t)) << 4);
	tcpip->tcp.tcp_flags = PKT_SEQ_TCP_FLAGS;
	tcpip->tcp.rx_win = rte_cpu_to_be_16(PKT_SEQ_TCP_WINDOW);
	tcpip->tcp.tcp_urp = 0;

	/* Setup part of IP header */
	tcpip->ip.src_addr = rte_cpu_to_be_32(info->src_ip);
	tcpip->ip.dst_addr = rte_cpu_to_be_32(info->dst_ip);
	tcpip->ip.total_length = rte_cpu_to_be_16(info->pkt_len
								- sizeof(struct ether_hdr)
								- sizeof(struct ipv4_hdr));
	tcpip->ip.next_proto_id = IPPROTO_TCP;

	/* Calculate tcp checksum */
	tlen = info->pkt_len - sizeof(struct ether_hdr);
	tcpip->tcp.cksum = __checksum_16(tcpip, tlen);

	/* Setup remaining part of ip header */
	__setup_ip_hdr(&tcpip->ip);
}

void pkt_seq_setup_udpip(struct pkt_seq_info *info,
				struct udpip_hdr *udpip)
{
	struct udp_hdr *udp = &udpip->udp;
	struct ipv4_hdr *ip = &udpip->ip;

	memset(udpip, 0, sizeof(struct udpip_hdr));

	/* Setup UDP header */
	udp->src_port = rte_cpu_to_be_16(info->src_port);
	udp->dst_port = rte_cpu_to_be_16(info->dst_port);
	udp->dgram_len = rte_cpu_to_be_16(info->pkt_len
										- sizeof(struct ether_hdr)
										- sizeof(struct ipv4_hdr));

	/* Setup part of IPv4 header */
	ip->packet_id = 0;
	ip->next_proto_id = IPPROTO_UDP;
	ip->total_length = rte_cpu_to_be_16(info->pkt_len
										- sizeof(struct ether_hdr));
	ip->src_addr = rte_cpu_to_be_32(info->src_ip);
	ip->dst_addr = rte_cpu_to_be_32(info->dst_ip);

	/* Calculate UDP checksum */
	udp->dgram_cksum = 0;

	/* Setup remaining part of ip header */
	__setup_ip_hdr(ip);
}

struct pkt_probe *pkt_seq_create_probe(void)
{
	struct pkt_probe *pkt = NULL;
	struct pkt_seq_info info = {
		.src_ip = PKT_SEQ_IP_SRC,
		.dst_ip = PKT_SEQ_IP_DST,
		.proto = IPPROTO_UDP,
		.src_port = PKT_SEQ_PROBE_PORT_SRC,
		.dst_port = PKT_SEQ_PROBE_PORT_DST,
		.pkt_len = PKT_SEQ_PROBE_PKT_LEN,
	};

	pkt = rte_zmalloc("pktgen: struct pkt_probe",
						sizeof(struct pkt_probe), 0);
	if (pkt == NULL) {
		LOG_ERROR("Failed to allocate memory for pkt_probe");
		return NULL;
	}

	LOG_DEBUG("eth_hdr %lu, ip_hdr %lu, udp_hdr %lu, idx %lu, magic %lu, pkt_len %lu",
					sizeof(pkt->eth_hdr), sizeof(pkt->ip_hdr),
					sizeof(pkt->udp_hdr), sizeof(pkt->probe_idx),
					sizeof(pkt->probe_idx), sizeof(struct pkt_probe));

	/* Setup UDP and IPv4 headers */
	pkt_seq_setup_udpip(&info, &pkt->udpip_hdr);

	/* Setup Ethernet header */
	ether_addr_copy(&mac_src, &pkt->eth_hdr.s_addr);
	ether_addr_copy(&mac_dst, &pkt->eth_hdr.d_addr);
	pkt->eth_hdr.ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

	/* Setup probe info */
	pkt->probe_idx = 0;
	pkt->probe_magic = PKT_PROBE_MAGIC;
	pkt->send_cycle = 0;
	return pkt;
}

void pkt_seq_fill_mbuf(struct rte_mbuf *mbuf, struct pkt_seq_info *info)
{
	struct ether_hdr *eth_hdr;
	uint8_t *payload = NULL;
	unsigned len = 0;
	uint32_t *crc = NULL;

	if (info == NULL) {
		LOG_ERROR("Wrong data to fill into mbuf");
		return;
	}

	mbuf->pkt_len = info->pkt_len + ETH_CRC_LEN;
	mbuf->data_len = info->pkt_len + ETH_CRC_LEN;

	/* Setup payload */
	if (info->proto == IPPROTO_TCP) {
		len = info->pkt_len - sizeof(struct ether_hdr)
							- sizeof(struct tcpip_hdr);
	} else {
		len = info->pkt_len - sizeof(struct ether_hdr)
							- sizeof(struct udpip_hdr);
	}
	payload = rte_pktmbuf_mtod_offset(mbuf, uint8_t *,
					sizeof(struct ether_hdr) + sizeof(struct tcpip_hdr));
	memset(payload, 0, len);

	/* Setup TCP/UDP+IP */
	if (info->proto == IPPROTO_TCP) {
		struct tcpip_hdr *tcpip = NULL;

		tcpip = rte_pktmbuf_mtod_offset(mbuf, struct tcpip_hdr*,
						sizeof(struct ether_hdr));
		pkt_seq_setup_tcpip(info, tcpip);

	} else {
		struct udpip_hdr *udpip;

		udpip = rte_pktmbuf_mtod_offset(mbuf, struct udpip_hdr*,
						sizeof(struct ether_hdr));
		pkt_seq_setup_udpip(info, udpip);
	}

	/* Setup Ethernet header */
	eth_hdr = rte_pktmbuf_mtod(mbuf, struct ether_hdr*);
	ether_addr_copy(&mac_src, &eth_hdr->s_addr);
	ether_addr_copy(&mac_dst, &eth_hdr->d_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

	/* Setup Eth FCS */
	crc = rte_pktmbuf_mtod_offset(mbuf, uint32_t*, info->pkt_len);
	*crc = rte_hash_crc(rte_pktmbuf_mtod(mbuf, void *),
					info->pkt_len, 0);
}

int pkt_seq_get_idx(struct rte_mbuf *pkt, uint32_t *idx)
{
	struct ether_hdr *eth_hdr = NULL;
	struct ipv4_hdr *ip_hdr = NULL;
	struct pkt_probe *probe = NULL;

	eth_hdr = rte_pktmbuf_mtod(pkt, struct ether_hdr *);

//	LOG_INFO("PKT len %u", pkt->data_len);

	if (eth_hdr->ether_type != rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
//		LOG_INFO("Not IPv4");
		return -1;
	}

	ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct ipv4_hdr *,
					sizeof(struct ether_hdr));
	if (ip_hdr->next_proto_id != IPPROTO_UDP) {
//		LOG_INFO("Not UDP");
		return -1;
	}

	probe = rte_pktmbuf_mtod(pkt, struct pkt_probe *);
	if (probe->probe_magic != PKT_PROBE_MAGIC) {
//		LOG_INFO("Wrong magic %u %u", PKT_PROBE_MAGIC, probe->probe_magic);
		return -1;
	}

	*idx = probe->probe_idx;
	return 0;
}
