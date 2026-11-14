#include "fhss.h"

static uint8_t s_sequence[FHSS_CHANNEL_COUNT];

void fhss_init(uint32_t seed)
{
  for (uint32_t i = 0; i < FHSS_CHANNEL_COUNT; i++)
  {
    s_sequence[i] = (uint8_t)i;
  }

  /* Fisher-Yates shuffle with a simple LCG, deterministic for a given seed. */
  uint32_t state = seed;
  for (uint32_t i = FHSS_CHANNEL_COUNT - 1u; i > 0u; i--)
  {
    state = state * 1103515245u + 12345u;
    const uint32_t j = (state >> 16) % (i + 1u);
    const uint8_t tmp = s_sequence[i];
    s_sequence[i] = s_sequence[j];
    s_sequence[j] = tmp;
  }
}

uint8_t fhss_channel(uint8_t hop_index)
{
  return s_sequence[hop_index % FHSS_CHANNEL_COUNT];
}

uint32_t fhss_freq_hz(uint8_t hop_index)
{
  return FHSS_BASE_HZ + (uint32_t)s_sequence[hop_index % FHSS_CHANNEL_COUNT] * FHSS_SPACING_HZ;
}
