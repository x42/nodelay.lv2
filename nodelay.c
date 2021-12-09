/* nodelay -- LV2 test & instrumentation tool
 *
 * simple delayline that also reports its delay as latency
 * it should be transparent for LV2 hosts that implement latency
 * compensation
 *
 * Copyright (C) 2013 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

#define NDL_URI "http://gareus.org/oss/lv2/nodelay"

#define DELAY_SIZE (262144) // 2^18
#define DELAY_MASK (262143)
#define FADE_LEN (32)

typedef struct {
	float* delay;
	float* report_latency;
	float* latency;
	float* input;
	float* output;

	float buffer[DELAY_SIZE];
	int   c_dly;
	int   w_ptr;
	int   r_ptr;

	/* settings from previous cycle */
	int p_delay;
	int p_mode;
	int p_fade;
} NoDelay;

static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	NoDelay* self = (NoDelay*)calloc (1, sizeof (NoDelay));

	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance,
              uint32_t   port,
              void*      data)
{
	NoDelay* self = (NoDelay*)instance;

	switch (port) {
		case 0:
			self->delay = data;
			break;
		case 1:
			self->report_latency = data;
			break;
		case 2:
			self->latency = data;
			break;
		case 3:
			self->input = data;
			break;
		case 4:
			self->output = data;
			break;
	}
}

static void
connect_port_micro (LV2_Handle instance,
                    uint32_t   port,
                    void*      data)
{
	NoDelay* self = (NoDelay*)instance;

	switch (port) {
		case 0:
			self->delay = data;
			break;
		case 1:
			self->latency = data;
			break;
		case 2:
			self->input = data;
			break;
		case 3:
			self->output = data;
			break;
	}
}

static void
connect_port_mega (LV2_Handle instance,
                    uint32_t   port,
                    void*      data)
{
	NoDelay* self = (NoDelay*)instance;

	switch (port) {
		case 0:
			self->delay = data;
			break;
		case 1:
			self->input = data;
			break;
		case 2:
			self->output = data;
			break;
	}
}

#define INCREMENT_PTRS                          \
  self->r_ptr = (self->r_ptr + 1) & DELAY_MASK; \
  self->w_ptr = (self->w_ptr + 1) & DELAY_MASK;

#ifndef MAX
#  define MAX(A, B) ((A) > (B) ? (A) : (B))
#endif
#ifndef MIN
#  define MIN(A, B) ((A) < (B) ? (A) : (B))
#endif

static void
no_delay (NoDelay* self, uint32_t n_samples)
{
	float const* const input = self->input;
	for (uint32_t pos = 0; pos < n_samples; ++pos) {
		self->buffer[self->w_ptr] = input[pos];
		INCREMENT_PTRS;
	}
	if (self->input != self->output) {
		memcpy (self->output, self->input, sizeof (float) * n_samples);
	}
}

static void
update_read_pointer (NoDelay* self, int new_delay)
{
	self->r_ptr = (self->r_ptr + DELAY_SIZE + self->c_dly - new_delay) & DELAY_MASK;
	self->c_dly = new_delay;
}

static void
run_delay (NoDelay* self, uint32_t n_samples, int delay)
{
	uint32_t           pos    = 0;
	float const* const input  = self->input;
	float* const       output = self->output;

	if (self->c_dly != delay) {
		const int fade_len = (n_samples >= (2 * FADE_LEN)) ? FADE_LEN : n_samples / 2;

		/* fade out */
		for (; pos < fade_len; ++pos) {
			self->buffer[self->w_ptr] = input[pos];
			/* when latency has changed last cycle, and this cycles
			 * adds a delay, there's alreay an active fade */
			if (self->p_fade) {
				output[pos] = 0;
			} else {
				const float gain = (float)(fade_len - pos) / (float)fade_len;
				output[pos]      = self->buffer[self->r_ptr] * gain;
			}
			INCREMENT_PTRS;
		}

		/* update read pointer */
		update_read_pointer (self, delay);

		/* fade in */
		for (; pos < 2 * fade_len; ++pos) {
			const float gain          = (float)(pos - fade_len) / (float)fade_len;
			self->buffer[self->w_ptr] = input[pos];
			output[pos]               = self->buffer[self->r_ptr] * gain;
			;
			INCREMENT_PTRS;
		}
		self->p_fade = 0;
	}

	for (; pos < n_samples; ++pos) {
		self->buffer[self->w_ptr] = input[pos];
		output[pos]               = self->buffer[self->r_ptr];
		INCREMENT_PTRS;
	}
}

static void
process (NoDelay* self, uint32_t n_samples, int delay)
{
	if (delay > 0 || self->c_dly > 0) {
		run_delay (self, n_samples, delay);
	} else {
		no_delay (self, n_samples);
		if (self->c_dly != delay) {
			update_read_pointer (self, delay);
		}
	}
}

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	NoDelay* self = (NoDelay*)instance;

	const float delay_ctrl = MAX (0, MIN (DELAY_MASK, *(self->delay)));
	int         mode       = rint (*self->report_latency);
	int         delay      = self->p_mode < 2 ? self->p_delay : 0;

	/* First report new latency to host. Only apply the change in the next cycle
	 * after the host has updated the latency */
	self->p_mode  = mode;
	self->p_delay = rintf (delay_ctrl);

	/* process fade c_dly -> delay */
	process (self, n_samples, delay);

	/* report latency */
	switch (mode) {
		case 3:
		case 0:
			*(self->latency) = 0.f;
			break;
		case 2:
			*(self->latency) = delay_ctrl;
			break;
		default:
			*(self->latency) = (float)self->c_dly;
			break;
	}
}

static void
run_micro (LV2_Handle instance, uint32_t n_samples)
{
	NoDelay* self = (NoDelay*)instance;

	const int delay_ctrl = MAX (-10000, MIN (DELAY_MASK, rintf (*(self->delay))));
	const int delay      = delay_ctrl >= 0 ? delay_ctrl : 0;

	process (self, n_samples, delay);

	/* fade in from previous cycle's fade-out (after a latency change) */
	if (self->p_fade && delay == self->c_dly) {
		float* const output   = self->output;
		const int    fade_len = (n_samples >= (2 * FADE_LEN)) ? FADE_LEN : n_samples / 2;
		for (uint32_t pos = 0; pos < fade_len; ++pos) {
			const float gain = pos / (float)fade_len;
			output[pos] *= gain;
		}
	}

	self->p_fade = 0;

	/* latency changes, fade out at end, fade-in next cycle */
	if (delay_ctrl != self->p_delay && delay_ctrl < 0) {
		float* const output   = self->output;
		const int    fade_len = (n_samples >= FADE_LEN) ? FADE_LEN : n_samples;
		uint32_t     pos      = n_samples - fade_len;
		for (uint32_t cnt = 0; cnt < fade_len; ++pos, ++cnt) {
			const float gain = (float)(fade_len - cnt) / (float)fade_len;
			output[pos] *= gain;
		}
		self->p_fade = 1;
	}

	self->p_delay = delay_ctrl;

	/* report latency */
	if (delay_ctrl >= 0) {
		*(self->latency) = 0.f;
	} else {
		*(self->latency) = -delay_ctrl;
	}
}

static void
run_mega (LV2_Handle instance, uint32_t n_samples)
{
	NoDelay* self = (NoDelay*)instance;

	const int delay = MAX (0, MIN (DELAY_MASK, rintf (*(self->delay))));
	process (self, n_samples, delay);
}

static void
cleanup (LV2_Handle instance)
{
	free (instance);
}

static const void*
extension_data (const char* uri)
{
	return NULL;
}

static const LV2_Descriptor descriptor0 = {
	NDL_URI,
	instantiate,
	connect_port,
	NULL,
	run,
	NULL,
	cleanup,
	extension_data
};

static const LV2_Descriptor descriptor1 = {
	NDL_URI "#micro",
	instantiate,
	connect_port_micro,
	NULL,
	run_micro,
	NULL,
	cleanup,
	extension_data
};

static const LV2_Descriptor descriptor2 = {
	NDL_URI "#mega",
	instantiate,
	connect_port_mega,
	NULL,
	run_mega,
	NULL,
	cleanup,
	extension_data
};

#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
#  define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
#  define LV2_SYMBOL_EXPORT __attribute__ ((visibility ("default")))
#endif
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	switch (index) {
		case 0:
			return &descriptor0;
		case 1:
			return &descriptor1;
		case 2:
			return &descriptor2;
		default:
			return NULL;
	}
}
