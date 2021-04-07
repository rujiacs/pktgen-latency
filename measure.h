#ifndef _PKTGEN_MEASURE_H_
#define _PKTGEN_MEASURE_H_

struct measure_param {
	int sender;
	struct rte_mempool *mp;
};

#define PROBE_RATE_DEF "10k"

void measure_thread_run(struct measure_param *param);

#endif
