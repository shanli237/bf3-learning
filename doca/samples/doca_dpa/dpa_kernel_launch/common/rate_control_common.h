#ifndef RATE_CONTROL_COMMON_H
#define RATE_CONTROL_COMMON_H

#include <stdint.h>

enum rate_action {
	RATE_ACTION_HOLD = 0,
	RATE_ACTION_INCREASE = 1,
	RATE_ACTION_DECREASE = 2,
};

struct rate_state {
	uint64_t flow_id;
	uint64_t current_rate_mbps;
	uint64_t measured_rate_mbps;
	uint64_t min_rate_mbps;
	uint64_t max_rate_mbps;
	uint64_t step_mbps;
	uint64_t update_count;
	uint64_t last_action;
};

_Static_assert(sizeof(struct rate_state) == 64,
	       "rate_state must occupy one cache line");

#endif