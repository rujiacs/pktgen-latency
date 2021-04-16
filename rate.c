#include "util.h"
#include "rate.h"
#include "pkt_seq.h"

#include <rte_cycles.h>

#define USEC_PER_SEC	1000000

/* - Cycles per second */
static uint64_t cycle_per_sec = 0;

static uint64_t __get_cycle_per_pkt(uint64_t tx_bps)
{
	if (cycle_per_sec == 0) {
		cycle_per_sec = rte_get_tsc_hz();
	}

    uint64_t pps = tx_bps / (8 * PKT_SEQ_PKT_LEN);

    if (pps == 0) pps = 1;

	return (cycle_per_sec / pps);
}

/* Format: e.g 1000k => 1000 kbps, 2m => 2 mbps,
 * 			   128 => 128 bps
 */
bool rate_set_rate(const char *rate_str, 
						struct rate_ctl *rate)
{
	long val = 0;
	char *unit = NULL;
	uint64_t tx_rate = 0;

	memset(rate, 0, sizeof(struct rate_ctl));

	val = strtol(rate_str, &unit, 10);
	if (errno == EINVAL || errno == ERANGE
					|| unit == rate_str) {
		LOG_ERROR("Failed to parse TX rate %s", rate_str);
		return false;
	}

	if (val < 0) {
		LOG_ERROR("Wrong rate value %ld", val);
		return false;
	}

	switch(*unit) {
		case 'k':	case 'K':
			tx_rate = val << 10;
			break;
		case 'm':	case 'M':
			tx_rate = val << 20;
			break;
		case 'g':	case 'G':
			tx_rate = val << 30;
			break;
		default:
			tx_rate = val;
	}

	rate->rate_bps = tx_rate;
    rate->cycle_per_pkt = __get_cycle_per_pkt(tx_rate);
//	rate->cycle_per_byte = __get_cycle_per_byte(tx_rate);
	rate->next_tx_cycle = 0;
	LOG_INFO("bps %lu, hz %lu, cycle_per_pkt %lu", tx_rate,
					cycle_per_sec, rate->cycle_per_pkt);
	return true;
}

void rate_set_next_cycle(struct rate_ctl *rate,
				uint64_t cur_cycle, unsigned nb_pkt)
{
	rate->next_tx_cycle =  cur_cycle + rate->cycle_per_pkt * nb_pkt;
}

void rate_wait_for_time(uint64_t next_cycle)
{
	unsigned long time = 0;
	uint64_t cur = 0;

	cur = rte_get_tsc_cycles();
	if (cur >= next_cycle)
		return;

	if (cycle_per_sec == 0) {
		cycle_per_sec = rte_get_tsc_hz();
	}
	time = (next_cycle - cur) / (cycle_per_sec / USEC_PER_SEC);
	usleep(time);
}
