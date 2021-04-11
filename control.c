#include "util.h"
#include "control.h"

#include <signal.h>

#include <rte_cycles.h>

static bool force_quit = false;
static uint64_t rx_quit_cycle = 0;

static struct ctl_worker worker_state[WORKER_MAX] = {
	{ .state = STATE_UNINIT, .lcoreid = UINT_MAX }
};

bool ctl_is_stop(unsigned workerid)
{
	if (workerid == WORKER_TX)
		return force_quit;
	if (workerid == WORKER_RX) {
		if (force_quit) {
			if (rx_quit_cycle == 0) {
				rx_quit_cycle = rte_get_tsc_cycles() +
						rte_get_tsc_hz() * RX_QUIT_DELAY / 1000;
			}

			if (rx_quit_cycle <= rte_get_tsc_cycles())
				return true;
		}
	}
	return false;
}

void ctl_quit(void)
{
	force_quit = true;
}

void ctl_signal_handler(int signo)
{
	if (signo == SIGINT || signo == SIGTERM) {
		LOG_INFO("Signal %d reveiced. Preparing to exit...",
						signo);
		force_quit = true;
	}
}

unsigned int ctl_get_state(unsigned worker)
{
	if (worker >= WORKER_MAX)
		return STATE_UNINIT;

	return worker_state[worker].state;
}

void ctl_set_state(unsigned worker, unsigned state)
{
	if (worker >= WORKER_MAX)
		return;
	worker_state[worker].state = state;
}

void ctl_set_lcore(unsigned worker, unsigned core)
{
	if (worker >= WORKER_MAX)
		return;
	worker_state[worker].lcoreid = core;
}

unsigned ctl_get_workerid(unsigned lcoreid)
{
	unsigned i = 0;

	for (i = 0; i < WORKER_MAX; i++) {
		if (worker_state[i].lcoreid == lcoreid)
			return i;
	}
	return WORKER_MAX;
}
