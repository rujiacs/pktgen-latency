#ifndef _PERF_PKT_SEQ_H_
#define _PERF_PKT_SEQ_H_

#include <stdio.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_mbuf.h>

enum {
	DEV_TYPE_DPDKR = 0,
	DEV_TYPE_ETH,
};

struct pkt_seq_info {
	/* IPv4 info */
	uint32_t src_ip;
	uint32_t dst_ip;
	uint8_t proto;

	/* L3 info */
	uint16_t src_port;
	uint16_t dst_port;

	uint16_t pkt_len;
};

struct pkt_latency {
	uint64_t id;
	uint64_t timestamp;
} __attribute__((__packed__));

struct tcpip_hdr {
	struct rte_ipv4_hdr ip;
	struct rte_tcp_hdr tcp;
};

struct udpip_hdr {
	struct rte_ipv4_hdr ip;
	struct rte_udp_hdr udp;
};

#define IPv4(a, b, c, d)   ((uint32_t)(((a) & 0xff) << 24) |   \
			    (((b) & 0xff) << 16) |	\
			    (((c) & 0xff) << 8)  |	\
			    ((d) & 0xff))

/* Default packet sequence configuration */
#define PKT_SEQ_MAC_SRC "0c:42:a1:75:5f:b4"
#define PKT_SEQ_MAC_DST "0c:42:a1:5c:02:08"
#define PKT_SEQ_IP_SRC IPv4(192,68,0,12)
#define PKT_SEQ_IP_DST IPv4(192,68,0,21)

#define PKT_SEQ_PKT_LEN 128
#define PKT_SEQ_PROTO IPPROTO_UDP
#define PKT_SEQ_PORT_SRC 9312
#define PKT_SEQ_PORT_DST 9321
#define PKT_SEQ_TCP_SEQ 0x12345678
#define PKT_SEQ_TCP_ACK 0x12345690
#define PKT_SEQ_TCP_FLAGS RTE_TCP_ACK_FLAG
#define PKT_SEQ_TCP_WINDOW 8192

#define PKT_SEQ_LATENCY_PKTID 30712
#define PKT_SEQ_LATENCY_MINSIZE 72

void pkt_seq_set_default_mac(void);
void pkt_seq_set_src_mac(uint16_t portid);
void pkt_seq_set_dst_mac(uint16_t portid);

void pkt_seq_init(struct pkt_seq_info *info);

void pkt_seq_setup_udpip(struct pkt_seq_info *info,
				struct udpip_hdr *udpip, bool is_latency);

void pkt_seq_setup_tcpip(struct pkt_seq_info *info,
				struct tcpip_hdr *tcpip, bool is_latency);

void pkt_seq_fill_mbuf(struct rte_mbuf *mbuf,
				struct pkt_seq_info *info, bool latency);

struct pkt_latency *pkt_seq_get_latency(struct rte_mbuf *mbuf);

#define ETH_CRC_LEN 4

static inline bool copy_buf_to_pkt(void *buf, unsigned len,
				struct rte_mbuf *pkt, unsigned offset)
{
	if (offset + len <= pkt->data_len) {
		rte_memcpy(rte_pktmbuf_mtod_offset(pkt, char *, offset),
						buf, (size_t)len);
		return true;
	}
//	LOG_ERROR("Out of range, data_len %u, requested area [%u,%u]",
//					pkt->data_len, offset, offset + len);
	return false;
}

#endif /* _PERF_PKT_SEQ_H_ */
