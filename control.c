#include "util.h"
#include "control.h"

#include <signal.h>

static bool force_quit = false;

static unsigned int worker_state[WORKER_MAX] = {STATE_UNINIT};

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

	return worker_state[worker];
}

void ctl_set_state(unsigned worker, unsigned state)
{
	if (worker >= WORKER_MAX)
		return;
	worker_state[worker] = state;
}
