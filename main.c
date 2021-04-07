#include <rte_mbuf.h>
#include <rte_eal.h>
#include <rte_ring.h>
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_ethdev.h>
#include <rte_eth_ring.h>

#include <getopt.h>
#include <signal.h>

#include "util.h"
#include "stat.h"
#include "rxtx.h"
#include "control.h"
#include "pkt_seq.h"
#include "measure.h"

#define CLIENT_RXQ_NAME "dpdkr%u_tx"
#define CLIENT_TXQ_NAME "dpdkr%u_rx"
#define CLIENT_MP_NAME_PREFIX	"ovs_mp_2030_0"
#define CLIENT_MP_PREFIX_LEN 13

static int sender_id = -1;
static int receiver_id = -1;

static unsigned dev_type = 0;
static unsigned tx_type = TX_TYPE_SINGLE;

//static int portid = -1;

static struct rte_mempool *mp = NULL;


struct lcore_param {
	bool is_rx;
	bool is_tx;
	bool is_stat;
};

#define LCORE_MAX 3

static struct lcore_param lcore_param[RTE_MAX_LCORE] = {
	{
		.is_rx = false,
		.is_tx = false,
		.is_stat = false,
	},
};

static const char *__get_rxq_name(unsigned int id)
{
	static char buffer[RTE_RING_NAMESIZE];

	snprintf(buffer, sizeof(buffer), CLIENT_RXQ_NAME, id);
	return buffer;
}

static const char *__get_txq_name(unsigned int id)
{
	static char buffer[RTE_RING_NAMESIZE];

	snprintf(buffer, sizeof(buffer), CLIENT_TXQ_NAME, id);
	return buffer;
}

static int __parse_client_num(const char *client)
{
	if (str_to_int(client, 10, &sender_id)) {
		receiver_id = sender_id + 1;
		return 0;
	}
	return -1;
}

static void __usage(const char *progname)
{
	LOG_INFO("Usage: %s [<EAL args>] -- ", progname);
	LOG_INFO("\t\t-r <TX rate (default 0)>");
	LOG_INFO("\t\t-t <5-tuple trace file>");
	LOG_INFO("\t\t-o <output file prefix>");
	LOG_INFO("\t\t-R Random pakcets");
}

static int __parse_options(int argc, char *argv[])
{
	int opt = 0;
	char **argvopt = argv;
	const char *progname = NULL;

	progname = argv[0];

	while ((opt = getopt(argc, argvopt, "t:r:o:R")) != -1) {
		switch(opt) {
			case 't':
				if (strcmp(optarg, "eth") == 0) {
					dev_type = DEV_TYPE_ETH;
				} else if (strcmp(optarg, "dpdkr") == 0) {
					dev_type = DEV_TYPE_DPDKR;
				} else {
					LOG_ERROR("Wrong device type %s", optarg);
					__usage(progname);
					return -1;
				}
				break;
			case 'r':
				rxtx_set_rate(optarg);
				break;
			case 'o':
				stat_set_output(optarg);
				break;
			case 'R':
				tx_type = TX_TYPE_RANDOM;
				break;
			default:
				__usage(progname);
				return -1;
		}
	}
	return 0;
}

/* return value: if need to create stats thread */
static bool __set_lcore(void)
{
	unsigned core = 0;
	unsigned used_core[3] = {UINT_MAX}, used = 0;

	for (core = 0; core < RTE_MAX_LCORE; core++) {
		if (rte_lcore_is_enabled(core) == 0)
			continue;

		used_core[used] = core;
		used++;
		if (used >= 3)
			break;
	}

	if (used == 1) {
		lcore_param[used_core[0]].is_rx = true;
		lcore_param[used_core[0]].is_tx = true;
		return true;
	} else if (used == 2) {
		lcore_param[used_core[0]].is_rx = true;
		lcore_param[used_core[1]].is_tx = true;
		return true;
	} else if (used == 3) {
		lcore_param[used_core[0]].is_rx = true;
		lcore_param[used_core[1]].is_tx = true;
		lcore_param[used_core[2]].is_stat = true;
		return false;
	}
	return false;
}

static int __lcore_main(__attribute__((__unused__))void *arg)
{
	unsigned lcoreid;
	struct lcore_param *param;
	struct measure_param measure = {
		.sender = sender_id,
		.mp = mp,
	};

	lcoreid = rte_lcore_id();
//	if (lcoreid >= LCORE_MAX)
//		return 0;

	LOG_INFO("lcore %u started.", lcoreid);
	param = &lcore_param[lcoreid];

	if (param->is_stat) {
		measure_thread_run(&measure);
	} else if (param->is_rx && param->is_tx) {
		rxtx_thread_run_rxtx(sender_id, receiver_id, mp,
								tx_type, NULL, NULL);
	} else if (param->is_rx) {
		rxtx_thread_run_rx(receiver_id);
	} else if (param->is_tx) {
		rxtx_thread_run_tx(sender_id, mp, tx_type, NULL, NULL);
	}

	LOG_INFO("lcore %u finished.", lcoreid);
	return 0;
}

static void __mempool_walk_func(struct rte_mempool *p,
				void *arg)
{
	struct rte_mempool **target = arg;

	LOG_INFO("mempool %s", p->name);
	if (strncmp(p->name, CLIENT_MP_NAME_PREFIX,
							strlen(CLIENT_MP_NAME_PREFIX)) == 0) {
		if (*target == NULL)
			*target = p;
	}
}

static struct rte_mempool *__lookup_mempool(void)
{
	struct rte_mempool *p = NULL;

	rte_mempool_walk(__mempool_walk_func, (void*)&p);
	if (p == NULL) {
		LOG_ERROR("Cannot find mempool");
		return NULL;
	}
	return p;
}

static int __get_ring_dev(unsigned int id)
{
	char buf[10] = {'\0'};
	struct rte_ring *tx_ring = NULL, *rx_ring = NULL;
	struct rte_eth_conf conf;
	int ret = 0;
	int ring_portid;

	rx_ring = rte_ring_lookup(__get_rxq_name(id));
	if (rx_ring == NULL) {
		LOG_ERROR("Cannot get RX ring for client %u", id);
		return -1;
	}

	tx_ring = rte_ring_lookup(__get_txq_name(id));
	if (tx_ring == NULL) {
		LOG_ERROR("Cannot get TX ring for client %u", id);
		return -1;
	}

	sprintf(buf, "dpdkr%d", id);
	ring_portid = rte_eth_from_rings(buf, &rx_ring, 1, &tx_ring, 1, 0);
	if (ring_portid < 0) {
		LOG_ERROR("Failed to create dev from ring %u", id);
		return -1;
	}

	/* Find mempool created by ovs */
	if (mp == NULL) {
		mp = __lookup_mempool();
		if (mp != NULL) {
			LOG_INFO("Found mempool %s", mp->name);
		} else {
			LOG_ERROR("Cannot find mempool for dpdkr%u", id);
			return -1;
		}
	}

	/* Setup interface */
	memset(&conf, 0, sizeof(conf));
	ret = rte_eth_dev_configure(ring_portid, 1, 1, &conf);
	if (ret) {
		LOG_ERROR("Failed to configure %s", buf);
		return -1;
	}

	/* Setup tx queue */
	ret = rte_eth_tx_queue_setup(ring_portid, 0, 2048, 0, NULL);
	if (ret) {
		LOG_ERROR("Failed to setup tx queue");
		return -1;
	}

	/* Setup rx queue */
	ret = rte_eth_rx_queue_setup(ring_portid, 0, 2048, 0,
						NULL, mp);
	if (ret) {
		LOG_ERROR("Failed to setup rx queue");
		return -1;
	}
	return ring_portid;
}

int main(int argc, char *argv[])
{
	int retval = 0;
	int coreid = 0;
	bool is_create_stat = false;
	pthread_t tid;
	struct measure_param param;
	unsigned nb_ports, nb_lcores;

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

//	pkt_seq_init(&pkt_seq);

	if (__parse_options(argc, argv) < 0) {
		rte_exit(EXIT_FAILURE, "Invalid command-line arguments\n");
	}

	signal(SIGINT, ctl_signal_handler);
	signal(SIGTERM, ctl_signal_handler);

	if (__get_ring_dev(sender_id) < 0) {
		rte_exit(EXIT_FAILURE, "Failed to get dpdkr device (sender)\n");
	}

	/* Start device */
	if (rte_eth_dev_start(sender_id) < 0) {
		rte_exit(EXIT_FAILURE, "Cannot start dpdkr device (sender)\n");
	}

	if (__get_ring_dev(receiver_id) < 0) {
		rte_exit(EXIT_FAILURE, "Failed to get dpdkr device (receiver)\n");
	}

	/* Start device */
	if (rte_eth_dev_start(receiver_id) < 0) {
		rte_exit(EXIT_FAILURE, "Cannot start dpdkr device (receiver)\n");
	}

	is_create_stat = __set_lcore();

	param.sender = sender_id;
	param.mp = mp;

	if (is_create_stat) {
		if (pthread_create(&tid, NULL, (void *)measure_thread_run, &param)) {
			rte_exit(EXIT_FAILURE, "Cannot create statistics thread\n");
		}
	}

	LOG_INFO("Processing client, sender %d, receiver %d",
					sender_id, receiver_id);

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

	if (is_create_stat) {
		pthread_join(tid, NULL);
	}

	rte_eth_dev_stop(sender_id);
	rte_eth_dev_stop(receiver_id);

	LOG_INFO("Done.");
	return 0;
}
