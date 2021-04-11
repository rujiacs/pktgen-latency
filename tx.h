#ifndef _PKTGEN_TX_H_
#define _PKTGEN_TX_H_

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
//	TX_TYPE_PCAP,
	TX_TYPE_MAX,
};



#define MBUF_SIZE (RTE_MBUF_DEFAULT_BUF_SIZE + DEFAULT_PRIV_SIZE)
#define TUPLE_TRACE_MAX 65535

struct tx_ctl {
	unsigned int tx_type;

	struct rte_mempool *tx_mp;

	struct pkt_seq_info pkt_info;

	struct rate_ctl tx_rate;

	unsigned tx_burst;

	/* fot 5-tuple trace */
	unsigned nb_trace;
	unsigned trace_iter;
	struct pkt_seq_info trace[TUPLE_TRACE_MAX];

	/* for latency measurement */
	bool is_latency;
	char latency_file[FILEPATH_MAX];

	unsigned int len;
	unsigned int offset;
	struct rte_mbuf *mbuf_tbl[TX_BURST];
};

void tx_thread_run_tx(int portid,
				struct rte_mempool *mp, unsigned tx_type,
				struct pkt_seq_info *seq, const char *filename);

void tx_set_rate(const char *rate_str);

void tx_set_burst(int burst);

void tx_enable_latency(void);

#endif /* _PKTGEN_TX_H_ */
