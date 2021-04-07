#include "util.h"
#include "measure.h"
#include "pkt_seq.h"
#include "stat.h"
#include "rate.h"

#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_hash_crc.h>
#include <rte_malloc.h>

static struct rate_ctl probe_rate = {
	.rate_bps = 0,
	.cycle_per_byte = 0,
	.next_tx_cycle = 0,
};

static uint32_t probe_iter = 0;
static struct pkt_probe *probe_pkt = NULL;
static int probe_pkt_len = PKT_SEQ_PROBE_PKT_LEN;

static void __prepare_probe_mbuf(struct rte_mbuf **buf,
				struct rte_mempool *mp)
{
	struct rte_mbuf *pkt = NULL;
	uint32_t crc = 0;

	pkt = rte_mbuf_raw_alloc(mp);
	if (pkt == NULL) {
		LOG_ERROR("No available mbuf in mempool");
		*buf = NULL;
		return;
	}

	pkt->pkt_len = probe_pkt_len + ETH_CRC_LEN;
	pkt->data_len = probe_pkt_len + ETH_CRC_LEN;
	/* the number of packet segments */
	pkt->nb_segs = 1;

	/* construct probe packet */
	probe_pkt->probe_idx = probe_iter;
	probe_pkt->send_cycle = rte_get_tsc_cycles();
	if (!copy_buf_to_pkt(probe_pkt, sizeof(struct pkt_probe),
							pkt, 0)) {
		LOG_ERROR("Failed to copy probe packet into mbuf");
		goto close_free_mbuf;
	}

	/* calculate ethernet frame checksum */
	crc = rte_hash_crc(rte_pktmbuf_mtod(pkt, void *),
					probe_pkt_len, PKT_PROBE_INITVAL);

	if (!copy_buf_to_pkt(&crc, sizeof(uint32_t),
							pkt, probe_pkt_len)) {
		LOG_ERROR("Failed to copy FCS into mbuf");
		goto close_free_mbuf;
	}

	/* complete packet mbuf */
	pkt->ol_flags = 0;
	pkt->vlan_tci = 0;
	pkt->vlan_tci_outer = 0;
	pkt->l2_len = sizeof(struct ether_hdr);
	pkt->l3_len = sizeof(struct ipv4_hdr);
	*buf = pkt;
	return;

close_free_mbuf:
	rte_pktmbuf_free(pkt);
	*buf = NULL;
	return;
}

static int __process_tx(int portid, struct rte_mempool *mp)
{
	uint16_t nb_tx = 0;
	uint64_t start_cyc = 0;
	struct rte_mbuf *pkt = NULL;

	start_cyc = rte_get_tsc_cycles();

	if (probe_pkt == NULL) {
		LOG_INFO("create probe");
		probe_pkt = pkt_seq_create_probe();
		if (probe_pkt == NULL) {
			LOG_ERROR("Failed to create template of probe pkt");
			return -ENOMEM;
		}
	}

	/* check if send now */
	if (start_cyc < probe_rate.next_tx_cycle) {
		return 0;
	}

	/* construct mbuf for probe packet */
//	LOG_ERROR("TX start");
	__prepare_probe_mbuf(&pkt, mp);

	if (pkt == NULL)
		return -EAGAIN;

	/* send probe packet */
	nb_tx = rte_eth_tx_burst(portid, 0, &pkt, 1);
	if (nb_tx < 1) {
		LOG_ERROR("Failed to send probe packet %u", probe_pkt->probe_idx);
		return -EAGAIN;
	}
//	LOG_INFO("TX packet %u on %lu", probe_iter, start_cyc);

	/* update TX statistics */
	stat_update_tx_probe(probe_pkt->probe_idx,
					pkt->pkt_len, probe_pkt->send_cycle);

	/* update tx seq state */
	probe_iter++;

	/* calculate the next time to TX (and sleep) */
	rate_set_next_cycle(&probe_rate, probe_pkt->send_cycle, pkt->pkt_len);
//	if (is_sleep) {
//		rate_wait_for_time(&probe_rate);
//	}

	return 0;
}

void measure_thread_run(struct measure_param *param)
{
	uint64_t start_cyc = 0, next_cycle = 0;
	int sender = param->sender;
	struct rte_mempool *mp = param->mp;
	int ret = 0;
	bool is_err = false;

	if (sender < 0 || mp == NULL || !stat_init()) {
		LOG_ERROR("Failed to initialize probe thread");
		return;
	}

	start_cyc = rte_get_tsc_cycles();
	probe_iter = 0;
	rate_set_rate(PROBE_RATE_DEF, &probe_rate);

	LOG_INFO("Probe packet send to port %d", sender);

	while(!stat_is_stop()) {
		/* TX */
		if (!is_err) {
			ret = __process_tx(sender, mp);
			if (ret == -ENOMEM) {
				LOG_ERROR("Probe packet TX error!");
				is_err = true;
			}
		}

		next_cycle = stat_processing();
		if (next_cycle > probe_rate.next_tx_cycle) {
			rate_wait_for_time(probe_rate.next_tx_cycle);
		} else {
			rate_wait_for_time(next_cycle);
		}
	}

	if (probe_pkt != NULL) {
		rte_free(probe_pkt);
		probe_pkt = NULL;
	}

	stat_finish(start_cyc);
}
