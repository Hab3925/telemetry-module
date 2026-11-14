/*
 * Frequency hopping sequence.
 *
 * 40 channels on a 2 MHz grid, 2401..2479 MHz (78 MHz span = 93 % of the 2400-2483.5 MHz band;
 * EN 300 328 adaptive FHSS needs >= 15 hop frequencies and >= 70 % of the band).
 * The hop sequence is a pseudo-random permutation of all channels seeded by the bind phrase, so every
 * channel is used exactly once per cycle (frequency occupation requirement).
 */
#ifndef FHSS_H
#define FHSS_H

#include <stdint.h>

#define FHSS_CHANNEL_COUNT  40u
#define FHSS_BASE_HZ        2401000000u
#define FHSS_SPACING_HZ     2000000u

void fhss_init(uint32_t seed);
uint32_t fhss_freq_hz(uint8_t hop_index);  /* hop_index is taken modulo FHSS_CHANNEL_COUNT */
uint8_t fhss_channel(uint8_t hop_index);   /* channel number 0..39 (frequency = base + n * spacing) */

#endif /* FHSS_H */
