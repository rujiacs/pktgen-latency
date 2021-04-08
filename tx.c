#include <rte_ring.h>
#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_hash_crc.h>
#include <rte_random.h>

#include "util.h"
#include "control.h"
#include "tx.h"
#include "stat.h"
#include "pkt_seq.h"
#include "rate.h"

/**** TX ****/
/* - default tx rate: 1mbps */
#define TX_RATE_DEF "1M"

struct pkt_setup_param {
	struct pkt_seq_info *info;
	unsigned int type;
};

static struct tx_ctl tx_ctl = {
	.tx_type = TX_TYPE_SINGLE,
	.tx_mp = NULL,
	.trace = NULL,
	.tx_rate = {
		.rate_bps = 0,
		.cycle_per_byte = 0,
		.next_tx_cycle = 0,
	},
	.len = 0,
	.offset = 0,
	.mbuf_tbl = {NULL},
};

void tx_set_rate(const char *rate_str)
{
	rate_set_rate(rate_str, &tx_ctl.tx_rate);
}

static void __set_tx_pkt_info(struct pkt_seq_info *info)
{
	pkt_seq_set_default_mac();

	if (info == NULL) {
		pkt_seq_init(&tx_ctl.pkt_info);
	} else {
		tx_ctl.pkt_info.src_ip = info->src_ip;
		tx_ctl.pkt_info.dst_ip = info->dst_ip;
		tx_ctl.pkt_info.proto = info->proto;
		tx_ctl.pkt_info.src_port = info->src_port;
		tx_ctl.pkt_info.dst_port = info->dst_port;
		tx_ctl.pkt_info.pkt_len = info->pkt_len;
	}
}

static inline void __pkt_setup(struct rte_mbuf *m, unsigned tx_type,
				struct pkt_seq_info *info)
{
	if (tx_type == TX_TYPE_RANDOM) {
		uint64_t val = 0;

		val = rte_rand();
		info->src_ip = val & 0xffffffff;
		info->dst_ip = (val >> 32) &0xffffffff;
	}

	pkt_seq_fill_mbuf(m, info);
}

static bool __tx_init(unsigned tx_type, struct rte_mempool *mp,
				struct pkt_seq_info *seq, const char *filename)
{
	if (tx_type >= TX_TYPE_MAX) {
		LOG_ERROR("Wrong TX type %u", tx_type);
		return false;
	}
	tx_ctl.tx_type = tx_type;

	if (tx_ctl.tx_rate.rate_bps == 0)
		tx_set_rate(TX_RATE_DEF);

	__set_tx_pkt_info(seq);

	if (tx_type == TX_TYPE_RANDOM)
		rte_srand(rte_get_tsc_cycles());

	tx_ctl.tx_mp = mp;

	if (tx_type == TX_TYPE_SINGLE || tx_type == TX_TYPE_RANDOM) {
//		/* Set default packets */
//		param.info = &tx_ctl.pkt_info;
//		param.type = tx_type;
//		rte_mempool_obj_iter(tx_ctl.tx_mp, __pkt_setup, &param);

	} else if (tx_type == TX_TYPE_5TUPLE_TRACE) {
		LOG_INFO("TODO: load file %s", filename);
		return false;
	}

	return true;
}

static inline void __pktmbuf_reset(struct rte_mbuf *m)
{
	m->next = NULL;
	m->nb_segs = 1;
	m->port = 0xff;

	m->data_off = (RTE_PKTMBUF_HEADROOM <= m->buf_len) ?
		RTE_PKTMBUF_HEADROOM : m->buf_len;
}

/**
 * Allocate a bulk of mbufs, initialize refcnt and reset the fields to default
 * values.
 */
static inline int
__pktmbuf_alloc_bulk(struct rte_mempool *pool,
		      struct rte_mbuf **mbufs, unsigned count)
{
	unsigned idx = 0;
	int rc;

	rc = rte_mempool_get_bulk(pool, (void * *)mbufs, count);
	if (unlikely(rc))
		return rc;

	switch (count % 4) {
	case 0:
		while (idx != count) {
#ifdef RTE_ASSERT
			RTE_ASSERT(rte_mbuf_refcnt_read(mbufs[idx]) == 0);
#else
			RTE_VERIFY(rte_mbuf_refcnt_read(mbufs[idx]) == 0);
#endif
			rte_mbuf_refcnt_set(mbufs[idx], 1);
			__pktmbuf_reset(mbufs[idx]);
			idx++;
			/* fall-through */
		case 3:
#ifdef RTE_ASSERT
			RTE_ASSERT(rte_mbuf_refcnt_read(mbufs[idx]) == 0);
#else
			RTE_VERIFY(rte_mbuf_refcnt_read(mbufs[idx]) == 0);
#endif
			rte_mbuf_refcnt_set(mbufs[idx], 1);
			__pktmbuf_reset(mbufs[idx]);
			idx++;
			/* fall-through */
		case 2:
#ifdef RTE_ASSERT
			RTE_ASSERT(rte_mbuf_refcnt_read(mbufs[idx]) == 0);
#else
			RTE_VERIFY(rte_mbuf_refcnt_read(mbufs[idx]) == 0);
#endif
			rte_mbuf_refcnt_set(mbufs[idx], 1);
			__pktmbuf_reset(mbufs[idx]);
			idx++;
			/* fall-through */
		case 1:
#ifdef RTE_ASSERT
			RTE_ASSERT(rte_mbuf_refcnt_read(mbufs[idx]) == 0);
#else
			RTE_VERIFY(rte_mbuf_refcnt_read(mbufs[idx]) == 0);
#endif
			rte_mbuf_refcnt_set(mbufs[idx], 1);
			__pktmbuf_reset(mbufs[idx]);
			idx++;
		}
	}
	return 0;
}

static int __process_tx(int portid __rte_unused, struct tx_ctl *ctl)
{
	int ret = 0;
	struct rte_mbuf **pkts = NULL;
	struct rate_ctl *rate = &ctl->tx_rate;
	unsigned int cnt = 0, i = 0;
	unsigned int sum = 0;
	uint64_t start_cyc = 0;

	start_cyc = rte_get_tsc_cycles();
	if (start_cyc < rate->next_tx_cycle) {
		return 0;
	}

	if (ctl->len <= 0) {
		ret = __pktmbuf_alloc_bulk(ctl->tx_mp, ctl->mbuf_tbl, TX_BURST);
		if (ret == 0) {
			pkts = ctl->mbuf_tbl;
			cnt = TX_BURST;

			for (i = 0; i < cnt; i++) {
				__pkt_setup(pkts[i], ctl->tx_type, &ctl->pkt_info);
			}

			ctl->len = TX_BURST;
			ctl->offset = 0;

		} else {
			ctl->len = 0;
			ctl->offset = 0;
			return -ENOMEM;
		}
	}

	pkts = &ctl->mbuf_tbl[ctl->offset];
	ret = rte_eth_tx_burst(portid, 0, pkts, ctl->len);
	ctl->len -= ret;
	ctl->offset += ret;

	sum = (ctl->pkt_info.pkt_len + ETH_CRC_LEN) * ret;
	stat_update_tx(sum, ret);
	rate_set_next_cycle(&ctl->tx_rate, start_cyc, sum);
	return 0;
}

void tx_thread_run_tx(int portid,
				struct rte_mempool *mp, unsigned tx_type,
				struct pkt_seq_info *seq, const char *filename)
{
//	int ret = 0;
//	unsigned int tx_retry = 0;

	/* waiting for stat thread */
	while (ctl_get_state(WORKER_STAT) == STATE_UNINIT && !ctl_is_stop(WORKER_TX)) {}

	if (ctl_get_state(WORKER_STAT) == STATE_STOPPED
					|| ctl_get_state(WORKER_STAT) == STATE_ERROR)
		return;

	if (portid < 0 || mp == NULL || tx_type >= TX_TYPE_MAX) {
		LOG_ERROR("Invalid parameters, portid %d, tx type %u",
						portid, tx_type);
		ctl_set_state(WORKER_TX, STATE_ERROR);
		return;
	}

	if (!__tx_init(tx_type, mp, seq, filename)) {
		LOG_ERROR("Failed to initialize TX");
		ctl_set_state(WORKER_TX, STATE_ERROR);
		return;
	}

	LOG_INFO("tx running on lcore %u", rte_lcore_id());

//	tx_seq_iter = 0;

	ctl_set_state(WORKER_TX, STATE_INITED);

	while (!ctl_is_stop(WORKER_TX)) {
		/* TX */
		if (__process_tx(portid, &tx_ctl) < 0) {
			LOG_ERROR("TX error!");
			break;
		}
	}

	if (tx_ctl.trace != NULL)
		fclose(tx_ctl.trace);

	ctl_set_state(WORKER_TX, STATE_STOPPED);
}