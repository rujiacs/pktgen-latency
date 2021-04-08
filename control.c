#include "util.h"
#include "control.h"

#include <signal.h>

static bool force_quit = false;

static struct ctl_worker worker_state[WORKER_MAX] = {
	{ .state = STATE_UNINIT, .lcoreid = UINT_MAX }
};

bool ctl_is_stop(void)
{
	return force_quit;
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