/*
Copyright (C) 2007-2010 Christian Kothe

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef FREESURROUND_CHANNELS_H
#define FREESURROUND_CHANNELS_H

// Trimmed from Cog's Audio/ThirdParty/fsurround/freesurround_decoder.h.
//
// Only the two enumerations are kept. The decoder class they used to accompany
// is reimplemented in core/src/audio/FreeSurround.cpp against XPCog's own FFT,
// so declaring it here as well would leave a type that nothing defines. The
// enums stay because channelmaps.cpp is keyed on them and is verbatim.

/**
 * Identifiers for the supported output channels (from front to back, left to right).
 * The ordering here also determines the ordering of interleaved samples in the output signal.
 */
typedef enum channel_id {
	ci_none = 0,
	ci_front_left = 1 << 1,
	ci_front_center_left = 1 << 2,
	ci_front_center = 1 << 3,
	ci_front_center_right = 1 << 4,
	ci_front_right = 1 << 5,
	ci_side_front_left = 1 << 6,
	ci_side_front_right = 1 << 7,
	ci_side_center_left = 1 << 8,
	ci_side_center_right = 1 << 9,
	ci_side_back_left = 1 << 10,
	ci_side_back_right = 1 << 11,
	ci_back_left = 1 << 12,
	ci_back_center_left = 1 << 13,
	ci_back_center = 1 << 14,
	ci_back_center_right = 1 << 15,
	ci_back_right = 1 << 16,
	ci_lfe = 1 << 31
} channel_id;

/**
 * The supported output channel setups.
 * A channel setup is defined by the set of channels that are present. Here is a graphic
 * of the cs_5point1 setup: http://en.wikipedia.org/wiki/File:5_1_channels_(surround_sound)_label.svg
 */
typedef enum channel_setup {
	cs_stereo = ci_front_left | ci_front_right | ci_lfe,
	cs_3stereo = ci_front_left | ci_front_center | ci_front_right | ci_lfe,
	cs_5stereo = ci_front_left | ci_front_center_left | ci_front_center | ci_front_center_right | ci_front_right | ci_lfe,
	cs_4point1 = ci_front_left | ci_front_right | ci_back_left | ci_back_right | ci_lfe,
	cs_5point1 = ci_front_left | ci_front_center | ci_front_right | ci_back_left | ci_back_right | ci_lfe,
	cs_6point1 = ci_front_left | ci_front_center | ci_front_right | ci_side_center_left | ci_side_center_right | ci_back_center | ci_lfe,
	cs_7point1 = ci_front_left | ci_front_center | ci_front_right | ci_side_center_left | ci_side_center_right | ci_back_left | ci_back_right | ci_lfe,
	cs_7point1_panorama = ci_front_left | ci_front_center_left | ci_front_center | ci_front_center_right | ci_front_right |
	                      ci_side_center_left | ci_side_center_right | ci_lfe,
	cs_7point1_tricenter = ci_front_left | ci_front_center_left | ci_front_center | ci_front_center_right | ci_front_right |
	                       ci_back_left | ci_back_right | ci_lfe,
	cs_8point1 = ci_front_left | ci_front_center | ci_front_right | ci_side_center_left | ci_side_center_right |
	             ci_back_left | ci_back_center | ci_back_right | ci_lfe,
	cs_9point1_densepanorama = ci_front_left | ci_front_center_left | ci_front_center | ci_front_center_right | ci_front_right |
	                           ci_side_front_left | ci_side_front_right | ci_side_center_left | ci_side_center_right | ci_lfe,
	cs_9point1_wrap = ci_front_left | ci_front_center_left | ci_front_center | ci_front_center_right | ci_front_right |
	                  ci_side_center_left | ci_side_center_right | ci_back_left | ci_back_right | ci_lfe,
	cs_11point1_densewrap = ci_front_left | ci_front_center_left | ci_front_center | ci_front_center_right | ci_front_right |
	                        ci_side_front_left | ci_side_front_right | ci_side_center_left | ci_side_center_right |
	                        ci_side_back_left | ci_side_back_right | ci_lfe,
	cs_13point1_totalwrap = ci_front_left | ci_front_center_left | ci_front_center | ci_front_center_right | ci_front_right |
	                        ci_side_front_left | ci_side_front_right | ci_side_center_left | ci_side_center_right |
	                        ci_side_back_left | ci_side_back_right | ci_back_left | ci_back_right | ci_lfe,
	cs_16point1 = ci_front_left | ci_front_center_left | ci_front_center | ci_front_center_right | ci_front_right |
	              ci_side_front_left | ci_side_front_right | ci_side_center_left | ci_side_center_right | ci_side_back_left |
	              ci_side_back_right | ci_back_left | ci_back_center_left | ci_back_center | ci_back_center_right | ci_back_right | ci_lfe,
	cs_legacy = 0 // same channels as cs_5point1 but different upmixing transform; does not support the focus control
} channel_setup;


#endif
