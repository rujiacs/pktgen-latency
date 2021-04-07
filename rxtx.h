#ifndef _PKTGEN_RXTX_H_
#define _PKTGEN_RXTX_H_

#include <stdbool.h>

#include "pkt_seq.h"
#include "rate.h"

struct rte_mempool;
struct pkt_seq_info;
struct rate_ctl;

enum {
	TX_TYPE_SINGLE = 0,
	TX_TYPE_RANDOM,
	TX_TYPE_5TUPLE_TRACE,
	TX_TYPE_PCAP,
	TX_TYPE_MAX,
};

#define DEFAULT_PKT_BURST 32
#define RX_BURST DEFAULT_PKT_BURST
#define TX_BURST DEFAULT_PKT_BURST
#define MAX_MBUF_PER_PORT 2048
#define DEFAULT_PRIV_SIZE 0
#define MBUF_SIZE (RTE_MBUF_DEFAULT_BUF_SIZE + DEFAULT_PRIV_SIZE)

struct tx_ctl {
	unsigned int tx_type;

	struct rte_mempool *tx_mp;

	struct pkt_seq_info pkt_info;

	/* fot 5-tuple trace */
	FILE *trace;

	struct rate_ctl tx_rate;

	unsigned int len;
	unsigned int offset;
	struct rte_mbuf *mbuf_tbl[TX_BURST];
};

void rxtx_thread_run_rxtx(int sender, int recv,
				struct rte_mempool *mp, unsigned tx_type,
				struct pkt_seq_info *seq, const char *filename);

void rxtx_thread_run_rx(int portid);

void rxtx_thread_run_tx(int portid,
				struct rte_mempool *mp, unsigned tx_type,
				struct pkt_seq_info *seq, const char *filename);

void rxtx_set_rate(const char *rate_str);

#endif /* _PKTGEN_RXTX_H_ */
