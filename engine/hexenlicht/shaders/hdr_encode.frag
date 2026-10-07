/* hdr_encode.frag -- HDR output (story 7.3, vk_hdr.c): the frame image
 * into the swapchain image (drawn with fullscreen.vert over the whole of
 * it)
 *
 * The frame image holds the 8-bit color values as the SDR swapchain would
 * (the composite, GL's view blend, gamma and the 2D drawn into it as in
 * SDR), above 1 kept. They become linear light by the engine's transfer
 * (transfer.glsl: a 2.2 power, the sRGB curve with r_srgb 1) or, with
 * r_hdr_decode 1, by the sRGB curve up to 1, as Windows shows an SDR
 * program on an HDR desktop; then times the paper white, at most the peak, in the
 * swapchain's units: scRGB's (1 = 80 nits, BT.709's primaries) or, for
 * HDR10, BT.2020's primaries in ST 2084's PQ (1 = 10000 nits).
 *
 * Copyright (C) 2026  Hexenlicht contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#version 460
#extension GL_GOOGLE_include_directive : require

#include "transfer.glsl"

layout (set = 0, binding = 0) uniform sampler2D frame;

layout (location = 0) out vec4 out_color;

/* vk_hdr.c's encode_push_t */
layout (push_constant) uniform push_constants
{
	float	scale;		/* linear light (1: the paper white) to the swapchain's units */
	float	max_value;	/* the peak in the same units */
	uint	srgb;		/* decode up to 1 by the sRGB curve, else the 2.2 power */
	uint	srgb_above;	/* the same above 1: the engine's transfer, the composite's encode */
	uint	pq;		/* HDR10 */
} push;

/* BT.709's primaries to BT.2020's, linear light (ITU-R BT.2087), column by column */
const mat3 BT709_TO_BT2020 = mat3 (
	0.627403896, 0.069097289, 0.016391439,
	0.329283039, 0.919540395, 0.088013308,
	0.043313065, 0.011362316, 0.895595253);

/* SMPTE ST 2084's inverse EOTF: light (1 = 10000 nits) as a PQ value */
vec3 pq_encode (vec3 y)
{
	const float	m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
	const float	c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
	vec3		p = pow (clamp (y, 0.0, 1.0), vec3 (m1));

	return pow ((c1 + c2 * p) / (1.0 + c3 * p), vec3 (m2));
}

void main ()
{
	vec3	c = texelFetch (frame, ivec2 (gl_FragCoord.xy), 0).rgb;
	/* above 1 (the tone mapper's shoulder) by the curve that encoded it,
	 * so r_hdr_decode 1 meets the peak where the shoulder does; both
	 * curves give 1 at 1 */
	vec3	lin = mix (color_to_linear (c, push.srgb), color_to_linear (c, push.srgb_above), greaterThan (c, vec3 (1.0)));
	vec3	light = min (lin * push.scale, vec3 (push.max_value));

	if (push.pq != 0u)
		light = pq_encode (BT709_TO_BT2020 * light);
	out_color = vec4 (light, 1.0);
}
