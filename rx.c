#include <rte_ring.h>
#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_hash_crc.h>
#include <rte_random.h>

#include <pcap.h>

#include "util.h"
#include "control.h"
#include "rx.h"
#include "stat.h"
#include "pkt_seq.h"

static struct rx_ctl rx_ctl = {
	.dump_to_pcap = false,
	.pcapfile = {'\0'},
	.is_latency = false,
	.rx_buf = {NULL}
};

void rx_enable_latency(void)
{
	rx_ctl.is_latency = true;
}

void rx_set_pcap_output(const char *filename)
{
	if (strlen(filename) == 0) {
		snprintf(rx_ctl.pcapfile, FILEPATH_MAX, "rx.pcap");
	}
	else {
		snprintf(rx_ctl.pcapfile, FILEPATH_MAX, "%s", filename);
	}
	rx_ctl.dump_to_pcap = true;
}

static void __rx_stat_latency(struct rte_mbuf *pkt, uint64_t recv_cyc)
{
	struct pkt_latency *lat = NULL;

	lat = pkt_seq_get_latency(pkt);
	if (!lat)
		return;

	stat_update_rx_latency(lat->id, lat->timestamp, recv_cyc);
}

static void __pcap_dump_pkt(pcap_dumper_t *out,
							const u_char *pkt, int len,
							time_t tv_sec, suseconds_t tv_usec)
{
    struct pcap_pkthdr hdr;
    hdr.ts.tv_sec = tv_sec;
    hdr.ts.tv_usec = tv_usec;
    hdr.caplen = len;
    hdr.len = len; 

    pcap_dump((u_char*)out, &hdr, pkt); 
}

static int __process_rx(int portid, pcap_dumper_t *pcapout)
{
	uint16_t nb_rx, i = 0;
	struct timeval tv;
	uint64_t recv_cyc = 0;

//	recv_cyc = rte_get_tsc_cycles();
	nb_rx = rte_eth_rx_burst(portid, 0, rx_ctl.rx_buf, RX_BURST);
	if (nb_rx == 0)
		return 0;

	if (pcapout)
		gettimeofday(&tv, NULL);
	if (rx_ctl.is_latency)
		recv_cyc = rte_get_tsc_cycles();

	for (i = 0; i < nb_rx; i++) {
		struct rte_mbuf *pkt = rx_ctl.rx_buf[i];

		stat_update_rx(pkt->data_len);

		if (rx_ctl.is_latency)
			__rx_stat_latency(pkt, recv_cyc);

		if (pcapout) {
			 char *pktbuf = rte_pktmbuf_mtod(pkt, char *);

			__pcap_dump_pkt(pcapout, (const u_char*)pktbuf, pkt->data_len,
								tv.tv_sec, tv.tv_usec + i);
		}

		rte_pktmbuf_free(pkt);
	}
	return 0;
}

void rx_thread_run_rx(int portid)
{
	/* waiting for stat thread */
	while (ctl_get_state(WORKER_STAT) == STATE_UNINIT && !ctl_is_stop(WORKER_RX)) {}

	if (ctl_get_state(WORKER_STAT) == STATE_STOPPED
					|| ctl_get_state(WORKER_STAT) == STATE_ERROR)
		return;

	if (portid < 0) {
		LOG_ERROR("Invalid parameters, portid %d", portid);
		ctl_set_state(WORKER_RX, STATE_ERROR);
		return;
	}

	pcap_dumper_t *pcapout =NULL;

	if (rx_ctl.dump_to_pcap) {
		pcapout = pcap_dump_open(pcap_open_dead(DLT_EN10MB, 1600), rx_ctl.pcapfile);
		if (!pcapout) {
			LOG_ERROR("Failed to open output pcap file %s", rx_ctl.pcapfile);
			ctl_set_state(WORKER_RX, STATE_ERROR);
			return;
		}
		LOG_INFO("All packet will be written to pcap file %s", rx_ctl.pcapfile);
	}

	LOG_INFO("rx running on lcore %u", rte_lcore_id());		

	ctl_set_state(WORKER_RX, STATE_INITED);

	while (!ctl_is_stop(WORKER_RX)) {
		if (__process_rx(portid, pcapout) < 0) {
			LOG_ERROR("RX error!");
			break;
		}
	}

	if (pcapout) {
		pcap_dump_close(pcapout);
		pcapout = NULL;
	}

	LOG_INFO("RX thread quit");
	ctl_set_state(WORKER_RX, STATE_STOPPED);
}
