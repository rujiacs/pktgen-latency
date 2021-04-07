#ifndef _PKTGEN_RATE_H_
#define _PKTGEN_RATE_H_

#include <stdint.h>
#include <stdbool.h>

struct rate_ctl {
	uint64_t rate_bps;
	uint64_t cycle_per_byte;
	uint64_t next_tx_cycle;
};

bool rate_set_rate(const char *rate_str, struct rate_ctl *rate);

void rate_set_next_cycle(struct rate_ctl *rate,
				uint64_t cur_cycle, uint16_t pkt_len);

void rate_wait_for_time(uint64_t next_cycle);

#endif
