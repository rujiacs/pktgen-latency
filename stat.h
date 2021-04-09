#ifndef _PKTGEN_STAT_H_
#define _PKTGEN_STAT_H_

#include <stdint.h>

struct stat_info {
	uint64_t last_bytes;
	uint64_t last_pkts;
	uint64_t stat_bytes;
	uint64_t stat_pkts;
	uint64_t last_cycle;
};

enum {
	STAT_IDX_RX = 0,
	STAT_IDX_TX,
//	STAT_IDX_TX_PROBE,
	STAT_IDX_MAX
};

struct stat_lat {
	uint64_t pkt_id;
	uint64_t tx_ts;
	uint64_t rx_ts;
};

#define STAT_LAT_PAGE_SIZE 1024
#define STAT_LAT_PAGE_NUM 128

struct stat_lat_page {
	struct stat_lat record[STAT_LAT_PAGE_SIZE];
	uint16_t nb_record;
};

struct rte_ring;

struct stat_ctl {
	struct stat_info port_stat[STAT_IDX_MAX];

	uint64_t cycle_per_sec;
	uint64_t next_dump_cycle ;
	uint64_t dump_interval;

	bool is_latency;
	FILE *lat_output;
	struct stat_lat_page *lat_pages;
	struct rte_ring *free_pages;
	struct rte_ring *full_pages;
	/* used by rx thread */
	struct stat_lat_page *cur_page;
};

#define STAT_PRINT_SEC	1

bool stat_init(void);

bool stat_is_stop(void);

uint64_t stat_processing(void);

void stat_finish(uint64_t start_cycle);

void stat_update_rx(uint64_t bytes);

void stat_update_rx_latency(uint64_t id, uint64_t tx, uint64_t rx);

void stat_update_tx(uint64_t bytes, unsigned int pkts);

bool stat_set_output(const char *prefix);

void stat_thread_run(void);

#endif /* _PKTGEN_STAT_H_ */
