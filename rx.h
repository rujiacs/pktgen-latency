#ifndef _PKTGEN_RX_H_
#define _PKTGEN_RX_H_

#include <stdbool.h>
#include "util.h"

struct rx_ctl {
	bool dump_to_pcap;
	char pcapfile[FILEPATH_MAX];
	struct rte_mbuf *rx_buf[RX_BURST];
};

void rx_set_pcap_output(const char *filename);

void rx_thread_run_rx(int portid);

#endif /* _PKTGEN_RX_H_ */
