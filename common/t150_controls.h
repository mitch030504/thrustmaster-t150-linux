/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T150_CONTROLS_H
#define T150_CONTROLS_H

/* Inputs are signed evdev force values. Boost bends the curve toward
 * stronger small forces while preserving zero, sign, and saturation. */
static inline int t150_scale_force(int force, unsigned int percent, int boost)
{
	int magnitude = force < 0 ? -force : force;
	int adjusted = magnitude;

	if (boost)
		adjusted += magnitude * (32768 - magnitude) / 65536;
	adjusted = adjusted * (int)percent / 100;
	if (force < 0)
		return -adjusted;
	return adjusted > 32767 ? 32767 : adjusted;
}

/* On the T150 pedal report, released is 1023 and pressed is 0. */
static inline unsigned int t150_combined_axis(unsigned int throttle, unsigned int other)
{
	throttle &= 1023;
	other &= 1023;
	return (1023 + (int)throttle - (int)other) / 2;
}
#endif
