#include "util.h"
#include "control.h"
#include "stat.h"

#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_ring.h>
#include <rte_malloc.h>

static struct stat_info port_stat[STAT_IDX_MAX];

#define PREFIX_MAX 100

static char output_prefix[PREFIX_MAX] = {'\0'};
static FILE *fout_rx = NULL;
static FILE *fout_tx = NULL;

static uint64_t cycle_per_sec = 0;
static uint64_t next_dump_cycle = 0;
static uint64_t dump_interval = 0;

void stat_set_output(const char *prefix)
{
	snprintf(output_prefix, PREFIX_MAX, "%s", prefix);
}

static void __update_stat(struct stat_info *stat, uint64_t byte)
{
	stat->stat_bytes += byte;
	stat->stat_pkts ++;
}

void stat_update_rx(uint64_t bytes)
{
	__update_stat(&port_stat[STAT_IDX_RX], bytes);
}

void stat_update_rx_probe(uint32_t idx, uint64_t bytes, uint64_t cycle)
{
	if (fout_rx != NULL)
		fprintf(fout_rx, "%u,%u,%lu\n", idx, RECORD_RX, cycle);

	LOG_DEBUG("RX probe packet %u at %lu", idx, (unsigned long)cycle);
	__update_stat(&port_stat[STAT_IDX_RX], bytes);
}

void stat_update_tx(uint64_t bytes, unsigned int pkts)
{
	port_stat[STAT_IDX_TX].stat_bytes += bytes;
	port_stat[STAT_IDX_TX].stat_pkts += pkts;
}

void stat_update_tx_probe(uint32_t idx, uint64_t bytes, uint64_t cycle)
{
	if (fout_tx != NULL)
		fprintf(fout_tx, "%u,%u,%lu\n", idx, RECORD_TX, cycle);

	__update_stat(&port_stat[STAT_IDX_TX_PROBE], bytes);
}

static inline void __process_stat(struct stat_info *stat,
//				double *bps, double *pps)
				uint64_t cur_cycle, double *bps, double *pps)
{
	uint64_t bytes = 0, pkts = 0;
	uint64_t last_b = 0, last_p = 0;
	double sec = 0;

	bytes = stat->stat_bytes;
	pkts = stat->stat_pkts;
	last_b = stat->last_bytes;
	last_p = stat->last_pkts;
	stat->last_bytes = bytes;
	stat->last_pkts = pkts;

//	sec = STAT_PRINT_SEC;
	sec = (double)(cur_cycle - stat->last_cycle) / cycle_per_sec;
	stat->last_cycle = cur_cycle;

	*bps = (bytes - last_b) * 8 / (sec * 1024);
	*pps = (pkts - last_p) / (sec * 1024);
}

static void __summary_stat(uint64_t cycles)
{
	double sec = 0;
	uint64_t rx_bytes, rx_pkts, tx_bytes, tx_pkts;

	sec = (double)cycles / cycle_per_sec;
	rx_bytes = port_stat[STAT_IDX_RX].stat_bytes;
	rx_pkts = port_stat[STAT_IDX_RX].stat_pkts;
	tx_bytes = port_stat[STAT_IDX_TX].stat_bytes
				+ port_stat[STAT_IDX_TX_PROBE].stat_bytes;
	tx_pkts = port_stat[STAT_IDX_TX].stat_pkts
				+ port_stat[STAT_IDX_TX_PROBE].stat_pkts;

	LOG_INFO("Running %lf seconds.", sec);
	LOG_INFO("\tRX %lu bytes (%lf kbps), %lu packets (%lf pps)",
					rx_bytes, (rx_bytes * 8 / (sec * 1024)),
					rx_pkts, (rx_pkts / sec));
	LOG_INFO("\tTX %lu bytes (%lf kbps), %lu packets (%lf pps)",
					tx_bytes, (tx_bytes * 8 / (sec * 1024)),
					tx_pkts, (tx_pkts / sec));
}

bool stat_init(void)
{
	uint64_t cycle;
	int i = 0;
	char buf[PREFIX_MAX + 4] = {'\0'};

	memset(port_stat, 0, sizeof(struct stat_info) * STAT_IDX_MAX);

	if (strlen(output_prefix) <= 0)
		sprintf(output_prefix, "probe");

	sprintf(buf, "%s.rx", output_prefix);
	fout_rx = fopen(buf, "w");
	if (fout_rx == NULL) {
		LOG_ERROR("Failed to open RX output file");
		goto close_set_error;
	}

	sprintf(buf, "%s.tx", output_prefix);
	fout_tx = fopen(buf, "w");
	if (fout_tx == NULL) {
		LOG_ERROR("Failed to open TX output file");
		goto close_free_rx;
	}

	/* Initialize timer */
	cycle_per_sec = rte_get_tsc_hz();
	dump_interval = STAT_PRINT_SEC * cycle_per_sec;
	cycle = rte_get_tsc_cycles();
	for (i = 0; i < STAT_IDX_MAX; i++) {
		port_stat[i].last_cycle = cycle;
	}
	next_dump_cycle = cycle + dump_interval;

	ctl_set_state(WORKER_STAT, STATE_INITED);
	return true;

close_free_rx:
	fclose(fout_rx);
	fout_rx = NULL;

close_set_error:
	ctl_set_state(WORKER_STAT, STATE_ERROR);
	return false;
}

bool stat_is_stop(void)
{
	unsigned int tx_state, rx_state;

	tx_state = ctl_get_state(WORKER_TX);
	rx_state = ctl_get_state(WORKER_RX);

	if ((tx_state == STATE_UNINIT || tx_state == STATE_INITED) &&
					(rx_state == STATE_UNINIT || rx_state == STATE_INITED))
		return false;
	return true;
}

uint64_t stat_processing(void)
{
	uint64_t cur_cycle = rte_get_tsc_cycles();
	double bps[STAT_IDX_MAX], pps[STAT_IDX_MAX];
	int i = 0;

	if (cur_cycle < next_dump_cycle) {
		return next_dump_cycle;
	}

	for (i = 0; i < STAT_IDX_MAX; i++) {
		__process_stat(&port_stat[i], cur_cycle, &bps[i], &pps[i]);
//		__process_stat(&port_stat[i], &bps[i], &pps[i]);
	}

	LOG_INFO("TX speed %lf kbps, %lf pps",
					bps[STAT_IDX_TX] + bps[STAT_IDX_TX_PROBE],
					pps[STAT_IDX_TX] + pps[STAT_IDX_TX_PROBE]);
	LOG_INFO("RX speed %lf kbps, %lf pps",
					bps[STAT_IDX_RX], pps[STAT_IDX_RX]);

	next_dump_cycle = cur_cycle + dump_interval;
	return next_dump_cycle;
}

void stat_finish(uint64_t start_cycle)
{
	__summary_stat(rte_get_tsc_cycles() - start_cycle);

	if (fout_tx != NULL)
		fclose(fout_tx);

	if (fout_rx != NULL)
		fclose(fout_rx);

	ctl_set_state(WORKER_STAT, STATE_STOPPED);
}
