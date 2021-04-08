#include <rte_mbuf.h>
#include <rte_eal.h>
#include <rte_ring.h>
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_ethdev.h>
#include <rte_eth_ring.h>

#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#include "util.h"
#include "stat.h"
#include "rx.h"
#include "tx.h"
#include "control.h"
#include "pkt_seq.h"
#include "measure.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

static unsigned tx_type = TX_TYPE_SINGLE;

//static int portid = -1;

static struct rte_mempool *mbuf_pool = NULL;

static char *trace_file = NULL;

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
	},
};

static void __usage(const char *progname)
{
	LOG_INFO("Usage: %s [<EAL args>] -- ", progname);
	LOG_INFO("\t\t-r <TX rate (default 0)>");
	LOG_INFO("\t\t-t <5-tuple trace file>");
	LOG_INFO("\t\t-o <output pcap file>");
	LOG_INFO("\t\t-l <latency file>");
	LOG_INFO("\t\t-R Random pakcets");
}

static int __parse_options(int argc, char *argv[])
{
	int opt = 0;
	char **argvopt = argv;
	const char *progname = NULL;
	bool is_trace = false, is_random = false;

	progname = argv[0];
	while ((opt = getopt(argc, argvopt, "t:r:l:o:R")) != -1) {
		switch(opt) {
			case 't':
				trace_file = strdup(optarg);
				// check if the file exists
				if((access(trace_file, F_OK)) == -1) {
					LOG_ERROR("Trace file %s doesn't exist", trace_file);
					zfree(trace_file);
				}
				LOG_INFO("Use 5-tuple trace file %s", trace_file);
				is_trace = true;
				break;
			case 'r':
				tx_set_rate(optarg);
				break;
			case 'l':
				stat_set_output(optarg);
				break;
			case 'o':
				rx_set_pcap_output(optarg);
				break;
			case 'R':
				is_random = true;
				break;
			default:
				__usage(progname);
				return -1;
		}
	}

	if (is_trace && is_random) {
		LOG_INFO("Both of 5tuple trace and random trace are selected, use 5-tuple trace");
		tx_type = TX_TYPE_5TUPLE_TRACE;
	}
	else if (is_random) {
		tx_type = TX_TYPE_RANDOM;
	}
	else if (is_trace) {
		tx_type = TX_TYPE_5TUPLE_TRACE;
	}
	else
		tx_type = TX_TYPE_SINGLE;

	return 0;
}

/* return value: if need to create stats thread */
static void __set_lcore(void)
{
	unsigned core = 0;
	unsigned rx_core = UINT_MAX, tx_core = UINT_MAX, master_core = UINT_MAX;

	for (core = 0; core < RTE_MAX_LCORE; core++) {
		if (rte_lcore_is_enabled(core) == 0)
			continue;
		if (master_core == UINT_MAX)
			master_core = core;
		else if (tx_core == UINT_MAX)
			tx_core = core;
		else if (rx_core == UINT_MAX)
			rx_core = core;
		else
			break;
	}
	ctl_set_lcore(WORKER_STAT, master_core);
	ctl_set_lcore(WORKER_RX, rx_core);
	ctl_set_lcore(WORKER_TX, tx_core);
	LOG_INFO("Lcore configuration: Master %u, TX %u, RX %u",
				master_core, tx_core, rx_core);
}

static inline int
__port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		LOG_ERROR("Error during getting device (port %u) info: %s",
				port, strerror(-retval));
		return retval;
	}

	LOG_INFO("TX_OFFLOAD_CAPA %lx", dev_info.tx_offload_capa);
	if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			DEV_TX_OFFLOAD_MBUF_FAST_FREE;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port);
	if (retval != 0)
		return retval;

	return 0;
}

static int __lcore_main(__attribute__((__unused__))void *arg)
{
	unsigned lcoreid, workerid;
//	struct measure_param measure = {
//		.sender = sender_id,
//		.mp = mp,
//	};

	lcoreid = rte_lcore_id();
	workerid = ctl_get_workerid(lcoreid);

	if (workerid == WORKER_MAX) {
		LOG_INFO("Lcore %u is unused", lcoreid);
		return;
	}

	LOG_INFO("lcore %u (worker %u) started.", lcoreid, workerid);

	if (workerid == WORKER_RX)
		rx_thread_run_rx(1);
	else if (workerid == WORKER_TX)
		tx_thread_run_tx(0, mbuf_pool, tx_type, NULL, trace_file);
	else {
		stat_thread_run();
	}

	LOG_INFO("lcore %u finished.", lcoreid);
	return 0;
}

int main(int argc, char *argv[])
{
	int retval = 0;
	int coreid = 0;
//	bool is_create_stat = false;
//	pthread_t tid;
//	struct measure_param param;
	unsigned nb_ports, portid;

	if ((retval = rte_eal_init(argc, argv)) < 0) {
		LOG_ERROR("Failed to initialize dpdk eal");
		return -1;
	}

	argc -= retval;
	argv += retval;

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports != 2)
		rte_exit(EXIT_FAILURE, "Error: number of ports must be 2\n");
	if (rte_lcore_count() < 3)
		rte_exit(EXIT_FAILURE, "Error: at least 3 cores are needed\n");
	if (rte_lcore_count() > 3)
		LOG_INFO("Only the first 3 cores will be used");
	__set_lcore();

//	pkt_seq_init(&pkt_seq);

	if (__parse_options(argc, argv) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid command-line arguments\n");
	}

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	signal(SIGINT, ctl_signal_handler);
	signal(SIGTERM, ctl_signal_handler);

	/* Initialize all ports. */
	RTE_ETH_FOREACH_DEV(portid)
		if (__port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
					portid);

//	if (is_create_stat) {
//		if (pthread_create(&tid, NULL, (void *)measure_thread_run, &param)) {
//			rte_exit(EXIT_FAILURE, "Cannot create statistics thread\n");
//		}
//	}

	retval = rte_eal_mp_remote_launch(__lcore_main, NULL, CALL_MASTER);
	if (retval < 0) {
		rte_exit(EXIT_FAILURE, "mp launch failed\n");
	}

	RTE_LCORE_FOREACH_SLAVE(coreid) {
		if (rte_eal_wait_lcore(coreid) < 0) {
			retval = -1;
			break;
		}
	}

//	if (is_create_stat) {
//		pthread_join(tid, NULL);
//	}

	RTE_ETH_FOREACH_DEV(portid) {
		LOG_INFO("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		LOG_INFO(" Done");
	}

	if (trace_file)
		zfree(trace_file);
		
	LOG_INFO("Bye...");

	return 0;
}
