#ifndef _PKTGEN_CONTROL_H_
#define _PKTGEN_CONTROL_H_

enum {
	STATE_INITED = 0,
	STATE_UNINIT,
	STATE_STOPPED,
	STATE_ERROR
};

enum {
	WORKER_STAT = 0,
	WORKER_RX = 1,
	WORKER_TX = 2,
	WORKER_MAX = 3
};

struct ctl_worker {
	unsigned state;
	unsigned lcoreid;
};

bool ctl_is_stop(void);

void ctl_signal_handler(int signo);

unsigned ctl_get_state(unsigned worker);

void ctl_set_state(unsigned worker, unsigned state);

void ctl_set_lcore(unsigned worker, unsigned core);

unsigned ctl_get_workerid(unsigned lcoreid);

#endif /* _PKTGEN_CONTROL_H_ */
