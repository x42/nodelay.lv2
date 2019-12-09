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

#define MAXDELAY (192001)
#define FADE_LEN (16)

typedef enum {
	NDL_DELAY   = 0,
	NDL_REPORT  = 1,
	NDL_LATENCY = 2,
	NDL_INPUT   = 3,
	NDL_OUTPUT  = 4
} PortIndex;

typedef struct {
	float* delay;
	float* report_latency;
	float* latency;
	float* input;
	float* output;

	float buffer[MAXDELAY];
	int   c_dly;
	int   w_ptr;
	int   r_ptr;

	/* settings from previous cycle */
	int p_delay;
	int p_mode;
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

	switch ((PortIndex)port) {
		case NDL_DELAY:
			self->delay = data;
			break;
		case NDL_REPORT:
			self->report_latency = data;
			break;
		case NDL_LATENCY:
			self->latency = data;
			break;
		case NDL_INPUT:
			self->input = data;
			break;
		case NDL_OUTPUT:
			self->output = data;
			break;
	}
}

#define INCREMENT_PTRS                        \
  self->r_ptr = (self->r_ptr + 1) % MAXDELAY; \
  self->w_ptr = (self->w_ptr + 1) % MAXDELAY;

#ifndef MAX
#  define MAX(A, B) ((A) > (B) ? (A) : (B))
#endif
#ifndef MIN
#  define MIN(A, B) ((A) < (B) ? (A) : (B))
#endif

static void
run (LV2_Handle instance, uint32_t n_samples)
{
	NoDelay* self = (NoDelay*)instance;

	uint32_t           pos        = 0;
	const float        delay_ctrl = MAX (0, MIN ((MAXDELAY - 1), *(self->delay)));
	int                mode       = rint (*self->report_latency);
	float const* const input      = self->input;
	float* const       output     = self->p_mode >= 2 ? NULL : self->output;
	int                delay      = self->p_delay;

	/* First report new latency to host. Only apply the change in the next cycle
	 * after the host has updated the laency */
	self->p_mode  = mode;
	self->p_delay = rintf (delay_ctrl);

	if (!output && self->input != self->output) {
		memcpy (self->output, self->input, sizeof (float) * n_samples);
	}

	if (self->c_dly != delay) {
		const int fade_len = (n_samples >= FADE_LEN) ? FADE_LEN : n_samples / 2;

		/* fade out */
		for (; pos < fade_len; ++pos) {
			const float gain          = (float)(fade_len - pos) / (float)fade_len;
			self->buffer[self->w_ptr] = input[pos];
			if (output) {
				output[pos] = self->buffer[self->r_ptr] * gain;
			}
			INCREMENT_PTRS;
		}

		/* update read pointer */
		self->r_ptr += self->c_dly - delay;
		if (self->r_ptr < 0) {
			self->r_ptr -= MAXDELAY * floor (self->r_ptr / (float)MAXDELAY);
		}
		self->r_ptr = self->r_ptr % MAXDELAY;
		self->c_dly = delay;

		/* fade in */
		for (; pos < 2 * fade_len; ++pos) {
			const float gain          = (float)(pos - fade_len) / (float)fade_len;
			self->buffer[self->w_ptr] = input[pos];
			if (output) {
				output[pos] = self->buffer[self->r_ptr] * gain;
			}
			INCREMENT_PTRS;
		}
	}

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

	for (; pos < n_samples; ++pos) {
		self->buffer[self->w_ptr] = input[pos];
		if (output) {
			output[pos] = self->buffer[self->r_ptr];
		}
		INCREMENT_PTRS;
	}
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

static const LV2_Descriptor descriptor = {
	NDL_URI,
	instantiate,
	connect_port,
	NULL,
	run,
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
			return &descriptor;
		default:
			return NULL;
	}
}
