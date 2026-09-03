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

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include <doca_error.h>
#include <doca_log.h>
#include "../common/rate_control_common.h"
#include "dpa_common.h"

DOCA_LOG_REGISTER(KERNEL_LAUNCH::SAMPLE);

/* Kernel function declaration */
extern doca_dpa_func_t hello_world;

/*
 * Waits for 5 seconds and then update a given event.
 * This function is made to be used by a pthread, hence it needs to return void*, and it will always return NULL.
 *
 * @event [in]: event to be updated
 * @return: NULL (dummy return)
 */
static void *wait_event_update_thread(void *event)
{
	doca_error_t result;

	sleep(5);
	result = doca_sync_event_update_set((struct doca_sync_event *)event, 5);
	if (result != DOCA_SUCCESS)
		DOCA_LOG_ERR("Failed to update DOCA sync event: %s", doca_error_get_descr(result));

	/* Dummy return */
	return NULL;
}

/*
 * Run kernel_launch sample
 *
 * @resources [in]: DOCA DPA resources that the DPA sample will use
 * @return: DOCA_SUCCESS on success and DOCA_ERROR otherwise
 */

static const char *
rate_action_name(uint64_t action)
{
	switch (action) {
	case RATE_ACTION_INCREASE:
		return "INCREASE";
	case RATE_ACTION_DECREASE:
		return "DECREASE";
	default:
		return "HOLD";
	}
}


enum {
	NUM_FLOWS = 4,
};

_Alignas(64) struct rate_state states_host[NUM_FLOWS] = {
	{
		.flow_id = 100,
		.current_rate_mbps = 5000,
		.measured_rate_mbps = 4900,
		.min_rate_mbps = 1000,
		.max_rate_mbps = 25000,
		.step_mbps = 5000,
	},
	{
		.flow_id = 101,
		.current_rate_mbps = 10000,
		.measured_rate_mbps = 4000,
		.min_rate_mbps = 1000,
		.max_rate_mbps = 25000,
		.step_mbps = 5000,
	},
	{
		.flow_id = 102,
		.current_rate_mbps = 15000,
		.measured_rate_mbps = 10000,
		.min_rate_mbps = 1000,
		.max_rate_mbps = 25000,
		.step_mbps = 5000,
	},
	{
		.flow_id = 103,
		.current_rate_mbps = 25000,
		.measured_rate_mbps = 24800,
		.min_rate_mbps = 1000,
		.max_rate_mbps = 25000,
		.step_mbps = 5000,
	},
};

doca_error_t kernel_launch(struct dpa_resources *resources)
{
	struct doca_sync_event *wait_event = NULL;
	struct doca_sync_event *comp_event = NULL;
	pthread_t tid = 0;
	uint64_t wait_thresh = 4;
	uint64_t comp_event_val = 10;
	const unsigned int num_dpa_threads = NUM_FLOWS;

// -------------------------------------------------------

	const uint64_t measured_rates_mbps[] = {
		4900,
		9800,
		14500,
		8000,
		7800,
	};

	const unsigned int num_iterations =
		sizeof(measured_rates_mbps) /
		sizeof(measured_rates_mbps[0]);

	doca_dpa_dev_uintptr_t states_dpa_addr = 0;

// -------------------------------------------------------

	doca_error_t result, tmp_result;
	int res = 0;

	result = create_doca_dpa_wait_sync_event(resources->pf_dpa_ctx, resources->pf_doca_device, &wait_event);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA sync event for DPA kernel wait: %s", doca_error_get_descr(result));
		return result;
	}

	result = create_doca_dpa_completion_sync_event(resources->pf_dpa_ctx,
						       resources->pf_doca_device,
						       &comp_event,
						       NULL);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to create DOCA sync event for DPA kernel completion: %s",
			     doca_error_get_descr(result));
		goto destroy_wait_event;
	}

	result = doca_dpa_mem_alloc(
		resources->pf_dpa_ctx,
		sizeof(states_host),
		&states_dpa_addr);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to allocate DPA state: %s",
			     doca_error_get_descr(result));
		goto destroy_comp_event;
	}

	/*
	 * The wait event is initially 0.
	 * The DPA kernel waits until it becomes greater than 4.
	 */
	for (unsigned int iteration = 0;
	     iteration < num_iterations;
	     iteration++) {
		uint64_t iteration_comp_value;

		/*
		 * Update the input for this iteration.
		 * current_rate and update_count were written back
		 * by the previous iteration and must be preserved.
		 */

		/*
		 * Copy the complete state to DPA memory.
		 * This must happen before launching the kernel.
		 */
		result = doca_dpa_h2d_memcpy(
			resources->pf_dpa_ctx,
			states_dpa_addr,
			states_host,
			sizeof(states_host));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to copy flow states to DPA: %s",
				doca_error_get_descr(result));
			goto free_dpa_state;
		}

		iteration_comp_value =
			comp_event_val + iteration;

		result = doca_dpa_kernel_launch_update_set(
			resources->pf_dpa_ctx,
			wait_event,
			wait_thresh,
			comp_event,
			iteration_comp_value,
			num_dpa_threads,
			&hello_world,
			states_dpa_addr,
			(uint64_t)NUM_FLOWS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to launch rate kernel: %s",
				doca_error_get_descr(result));
			goto free_dpa_state;
		}

		/*
		 * Trigger the first kernel after five seconds.
		 * Later kernels see wait_event == 5 already.
		 */
		if (iteration == 0) {
			res = pthread_create(
				&tid,
				NULL,
				wait_event_update_thread,
				(void *)wait_event);
			if (res != 0) {
				DOCA_LOG_ERR(
					"Failed to create wait-event thread");
				result =
					DOCA_ERROR_OPERATING_SYSTEM;
				goto free_dpa_state;
			}

			res = pthread_detach(tid);
			if (res != 0) {
				DOCA_LOG_ERR(
					"pthread_detach() failed");
				result =
					DOCA_ERROR_OPERATING_SYSTEM;
				goto free_dpa_state;
			}
		}

		result = doca_sync_event_wait_gt(
			comp_event,
			iteration_comp_value - 1,
			SYNC_EVENT_MASK_FFS);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed waiting for iteration %u: %s",
				iteration + 1,
				doca_error_get_descr(result));
			goto free_dpa_state;
		}

		/*
		 * Copy the updated state back from DPA.
		 */
		result = doca_dpa_d2h_memcpy(
			resources->pf_dpa_ctx,
			states_host,
			states_dpa_addr,
			sizeof(states_host));
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR(
				"Failed to copy flow states from DPA: %s",
				doca_error_get_descr(result));
			goto free_dpa_state;
		}

		for (unsigned int i = 0; i < NUM_FLOWS; i++) {
			DOCA_LOG_INFO(
				"Flow %lu: measured=%lu Mbps, "
				"new_rate=%lu Mbps, action=%s, updates=%lu",
				(unsigned long)states_host[i].flow_id,
				(unsigned long)
					states_host[i].measured_rate_mbps,
				(unsigned long)
					states_host[i].current_rate_mbps,
				rate_action_name(
					states_host[i].last_action),
				(unsigned long)
					states_host[i].update_count);
		}
	}



free_dpa_state:
	if (states_dpa_addr != 0) {
		tmp_result = doca_dpa_mem_free(
			resources->pf_dpa_ctx,
			states_dpa_addr);

		if (tmp_result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to free DPA state: %s",
					doca_error_get_descr(tmp_result));
			DOCA_ERROR_PROPAGATE(result, tmp_result);
		}
	}
destroy_comp_event:
	/* Sleep 2 seconds before destroying the events*/
	sleep(2);
	tmp_result = doca_sync_event_destroy(comp_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy host completion event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
destroy_wait_event:
	tmp_result = doca_sync_event_destroy(wait_event);
	if (tmp_result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to destroy host wait event: %s", doca_error_get_descr(tmp_result));
		DOCA_ERROR_PROPAGATE(result, tmp_result);
	}
	return result;
}
