/*
 * Copyright (c) 2022 NVIDIA CORPORATION AND AFFILIATES.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted
 * provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright notice, this list of
 *       conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name of the NVIDIA CORPORATION nor the names of its contributors may be used
 *       to endorse or promote products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NVIDIA CORPORATION BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TOR (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <doca_dpa_dev.h>
#include "../common/rate_control_common.h"
/*
 * Kernel function for kernel_launch sample
 */
// __dpa_global__ void hello_world(void)
// {
// 	DOCA_DPA_DEV_LOG_INFO("Hello from my first DPA program\n");
// }

__dpa_global__ void
hello_world(doca_dpa_dev_uintptr_t states_addr,
	    uint64_t num_flows)
{
	struct rate_state *states;
	struct rate_state *state;
	uint64_t increase_threshold;
	uint64_t next_rate;
	unsigned int rank;

	rank = doca_dpa_dev_thread_rank();

	/*
	 * 防御性检查。正常情况下Host启动的线程数等于flow数。
	 */
	if ((uint64_t)rank >= num_flows)
		return;

	states = (struct rate_state *)states_addr;
	state = &states[rank];

	increase_threshold =
		state->current_rate_mbps -
		state->current_rate_mbps / 10;

	if (state->measured_rate_mbps >= increase_threshold) {
		if (state->current_rate_mbps >=
		    state->max_rate_mbps) {
			state->current_rate_mbps =
				state->max_rate_mbps;
			state->last_action =
				RATE_ACTION_HOLD;
		} else {
			next_rate =
				state->current_rate_mbps +
				state->step_mbps;

			if (next_rate >
			    state->max_rate_mbps)
				next_rate =
					state->max_rate_mbps;

			state->current_rate_mbps = next_rate;
			state->last_action =
				RATE_ACTION_INCREASE;
		}
	} else if (state->measured_rate_mbps <
		   state->current_rate_mbps / 2) {
		if (state->measured_rate_mbps <
		    state->min_rate_mbps)
			state->current_rate_mbps =
				state->min_rate_mbps;
		else
			state->current_rate_mbps =
				state->measured_rate_mbps;

		state->last_action =
			RATE_ACTION_DECREASE;
	} else {
		state->last_action =
			RATE_ACTION_HOLD;
	}

	state->update_count++;
}
