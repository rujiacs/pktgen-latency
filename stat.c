#include "util.h"
#include "control.h"
#include "stat.h"
#include "rate.h"

#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_ring.h>
#include <rte_malloc.h>

static struct stat_ctl stat_ctl = {
	.port_stat = {
		{
			.last_bytes = 0,
			.last_pkts = 0,
			.stat_bytes = 0,
			.stat_pkts = 0,
			.last_cycle = 0,
		}
	},
	.cycle_per_sec = 0,
	.next_dump_cycle = 0,
	.dump_interval = 0,
	.is_latency = false,
	.lat_output = NULL,
	.lat_pages = NULL,
	.free_pages = NULL,
	.full_pages = NULL,
	.cur_page = NULL,
};

bool stat_set_output(const char *prefix)
{
	char outputfile[FILEPATH_MAX] = {'\0'};

	if (strlen(prefix) == 0)
		snprintf(outputfile, FILEPATH_MAX, "tmp.lat");
	else
		snprintf(outputfile, FILEPATH_MAX, "%s", prefix);

	stat_ctl.lat_output = fopen(outputfile, "w");
	if (!stat_ctl.lat_output) {
		LOG_ERROR("Failed to latency record file %s", outputfile);
		return false;
	}
	stat_ctl.is_latency = true;
	return true;
}

static void __update_stat(struct stat_info *stat, uint64_t byte)
{
	stat->stat_bytes += byte;
	stat->stat_pkts ++;
}

void stat_update_rx(uint64_t bytes)
{
	__update_stat(&stat_ctl.port_stat[STAT_IDX_RX], bytes);
}

void stat_update_rx_latency(uint64_t id, uint64_t tx, uint64_t rx)
{
	struct stat_ctl *ctl = &stat_ctl;
	void *tmp = NULL;

	if (ctl->cur_page == NULL) {
		if (rte_ring_dequeue(ctl->free_pages, &tmp) < 0) {
			LOG_ERROR("No free pages, drop record(%lu, %lu, %lu).",
						id, tx, rx);
			return;
		}
		ctl->cur_page = (struct stat_lat_page *)tmp;
	}
	else if (ctl->cur_page->nb_record == STAT_LAT_PAGE_SIZE) {
		rte_ring_enqueue(ctl->full_pages, ctl->cur_page);
		if (rte_ring_dequeue(ctl->free_pages, &tmp) < 0) {
			LOG_ERROR("No free pages, drop record(%lu, %lu, %lu).",
						id, tx, rx);
			ctl->cur_page = NULL;
			return;
		}
		ctl->cur_page = (struct stat_lat_page *)tmp;
	}
	ctl->cur_page->record[ctl->cur_page->nb_record].pkt_id = id;
	ctl->cur_page->record[ctl->cur_page->nb_record].tx_ts = tx;
	ctl->cur_page->record[ctl->cur_page->nb_record].rx_ts = rx;
	ctl->cur_page->nb_record ++;
}

void stat_update_tx(uint64_t bytes, unsigned int pkts)
{
	stat_ctl.port_stat[STAT_IDX_TX].stat_bytes += bytes;
	stat_ctl.port_stat[STAT_IDX_TX].stat_pkts += pkts;
}

static inline void __process_stat(struct stat_info *stat,
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
	sec = (double)(cur_cycle - stat->last_cycle) / stat_ctl.cycle_per_sec;
	stat->last_cycle = cur_cycle;

	*bps = (bytes - last_b) * 8 / (sec * 1024);
	*pps = (pkts - last_p) / (sec * 1024);
}

static void __summary_stat(uint64_t cycles)
{
	double sec = 0;
	uint64_t rx_bytes, rx_pkts, tx_bytes, tx_pkts;

	sec = (double)cycles / stat_ctl.cycle_per_sec;
	rx_bytes = stat_ctl.port_stat[STAT_IDX_RX].stat_bytes;
	rx_pkts = stat_ctl.port_stat[STAT_IDX_RX].stat_pkts;
	tx_bytes = stat_ctl.port_stat[STAT_IDX_TX].stat_bytes;
	tx_pkts = stat_ctl.port_stat[STAT_IDX_TX].stat_pkts;

	LOG_INFO("Running %lf seconds.", sec);
	LOG_INFO("\tRX %lu bytes (%lf kbps), %lu packets (%lf pps)",
					rx_bytes, (rx_bytes * 8 / (sec * 1024)),
					rx_pkts, (rx_pkts / sec));
	LOG_INFO("\tTX %lu bytes (%lf kbps), %lu packets (%lf pps)",
					tx_bytes, (tx_bytes * 8 / (sec * 1024)),
					tx_pkts, (tx_pkts / sec));
}

static bool __init_latency(void)
{
	struct stat_ctl *ctl = &stat_ctl;
	size_t size = sizeof(struct stat_lat_page) * STAT_LAT_PAGE_NUM;
	struct rte_ring *ring = NULL;
	unsigned i = 0;

	ctl->lat_pages = (struct stat_lat_page *)rte_zmalloc(NULL, size, 0);
	if (!ctl->lat_pages) {
		LOG_ERROR("Failed to allocate latency record cache "
					"(%lu bytes)", size);
		return false;
	}

	ring = rte_ring_create("LAT_PAGE_FULL", STAT_LAT_PAGE_NUM,
							rte_socket_id(),
							RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (!ring) {
		LOG_ERROR("Faile to create full_pages ring buffer");
		goto free_lat_pages;
	}
	ctl->full_pages = ring;

	ring = rte_ring_create("LAT_PAGE_FREE", STAT_LAT_PAGE_NUM,
							rte_socket_id(),
							RING_F_SP_ENQ | RING_F_SC_DEQ);
	if (!ring) {
		LOG_ERROR("Faile to create free_pages ring buffer");
		goto free_full_pages;
	}
	ctl->free_pages = ring;

	/* Insert all initial pages expect the first one into free_pages */
	for (i = 1; i < STAT_LAT_PAGE_NUM; i++) {
		if (rte_ring_enqueue(ctl->free_pages, &(ctl->lat_pages[i])) < 0) {
			LOG_ERROR("Failed to enqueue free pages");
			goto free_free_pages;
		}
	}
	ctl->cur_page = &ctl->lat_pages[0];

	return true;

free_free_pages:
	rte_ring_free(ctl->free_pages);
	ctl->free_pages = NULL;

free_full_pages:
	rte_ring_free(ctl->full_pages);
	ctl->full_pages = NULL;

free_lat_pages:
	rte_free(ctl->lat_pages);
	ctl->lat_pages = NULL;

	return false;
}

bool stat_init(void)
{
	uint64_t cycle;
	int i = 0;
	bool ret = false;

	if (stat_ctl.is_latency) {
		ret = __init_latency();
		if (!ret) {
			LOG_ERROR("Failed to init latency stat");
			if (stat_ctl.lat_output) {
				fclose(stat_ctl.lat_output);
				stat_ctl.lat_output = NULL;
			}
			ctl_set_state(WORKER_STAT, STATE_ERROR);
			return false;
		}
	}

	/* Initialize timer */
	stat_ctl.cycle_per_sec = rte_get_tsc_hz();
	stat_ctl.dump_interval = STAT_PRINT_SEC * stat_ctl.cycle_per_sec;
	cycle = rte_get_tsc_cycles();
	for (i = 0; i < STAT_IDX_MAX; i++) {
		stat_ctl.port_stat[i].last_cycle = cycle;
	}
	stat_ctl.next_dump_cycle = cycle + stat_ctl.dump_interval;

	ctl_set_state(WORKER_STAT, STATE_INITED);
	return true;
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

	if (cur_cycle < stat_ctl.next_dump_cycle) {
		return stat_ctl.next_dump_cycle;
	}

	for (i = 0; i < STAT_IDX_MAX; i++) {
		__process_stat(&stat_ctl.port_stat[i], cur_cycle,
							&bps[i], &pps[i]);
	}

	LOG_INFO("TX speed %lf kbps, %lf pps",
					bps[STAT_IDX_TX], pps[STAT_IDX_TX]);
	LOG_INFO("RX speed %lf kbps, %lf pps",
					bps[STAT_IDX_RX], pps[STAT_IDX_RX]);

	stat_ctl.next_dump_cycle = cur_cycle + stat_ctl.dump_interval;
	return stat_ctl.next_dump_cycle;
}

void stat_finish(uint64_t start_cycle)
{
	__summary_stat(rte_get_tsc_cycles() - start_cycle);

	if (stat_ctl.is_latency) {
		if (stat_ctl.free_pages) {
			rte_ring_free(stat_ctl.free_pages);
			stat_ctl.free_pages = NULL;
		}
		if (stat_ctl.full_pages) {			
			unsigned pages = rte_ring_count(stat_ctl.full_pages);

			if (pages > 0) {
				LOG_INFO("Write back the %u pages in the queue", pages);
				struct stat_lat_page *page = NULL;
				void *tmp = NULL;

				while (rte_ring_dequeue(stat_ctl.full_pages, &tmp) == 0) {
					page = (struct stat_lat_page *)tmp;
					fwrite(page->record, sizeof(struct stat_lat),
							page->nb_record, stat_ctl.lat_output);
				}
			}

			rte_ring_free(stat_ctl.full_pages);
			stat_ctl.full_pages = NULL;
		}
		if (stat_ctl.cur_page) {
			LOG_INFO("Write back the last %u records",
						stat_ctl.cur_page->nb_record);
			fwrite(stat_ctl.cur_page->record, sizeof(struct stat_lat),
					stat_ctl.cur_page->nb_record, stat_ctl.lat_output);
			stat_ctl.cur_page = NULL;
		}

		if (stat_ctl.lat_pages) {
			rte_free(stat_ctl.lat_pages);
			stat_ctl.lat_pages = NULL;
		}

		if (stat_ctl.lat_output) {
			fclose(stat_ctl.lat_output);
			stat_ctl.lat_output = NULL;
		}
	}

	ctl_set_state(WORKER_STAT, STATE_STOPPED);
}

void stat_thread_run(void)
{
	if (!stat_init()) {
		LOG_ERROR("Failed to initialize stat thread");
		ctl_set_state(WORKER_STAT, STATE_ERROR);
		return;
	}

	uint64_t start_cyc = 0, next_cyc = 0;

	start_cyc = rte_get_tsc_cycles();

	LOG_INFO("Stat thread is running...");
	while (!stat_is_stop()) {
		next_cyc = stat_processing();

		if (stat_ctl.is_latency) {
			void *tmp = NULL;
			struct stat_lat_page *page = NULL;

			while (rte_ring_dequeue(stat_ctl.full_pages, &tmp) == 0) {
				page = (struct stat_lat_page *)tmp;
				fwrite(page->record, sizeof(struct stat_lat),
							page->nb_record, stat_ctl.lat_output);
				page->nb_record = 0;
				rte_ring_enqueue(stat_ctl.free_pages, page);
			}
		}
		else {
			rate_wait_for_time(next_cyc);
		}
	}

	stat_finish(start_cyc);
}
