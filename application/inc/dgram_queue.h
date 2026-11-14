/*
 * Single-producer / single-consumer queue of fixed-size datagram slots.
 * Lock free: exactly one thread may produce and one thread may consume.
 * Slots are filled and drained in place (no copy through the queue).
 */
#ifndef DGRAM_QUEUE_H
#define DGRAM_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

#define DGRAM_FLAG_ECHO 0x01u /* the far board sends this datagram straight back over the radio */

typedef struct
{
  uint16_t len;
  uint8_t flags;
  uint8_t data[APP_DGRAM_MAX];
} dgram_t;

typedef struct
{
  dgram_t slots[APP_DGRAM_QUEUE_DEPTH];
  volatile uint32_t head; /* written by producer */
  volatile uint32_t tail; /* written by consumer */
  volatile uint32_t drops;
} dgram_queue_t;

/* Producer: get the free slot to fill (NULL if full), then commit it. */
dgram_t *dq_alloc(dgram_queue_t *q);
void dq_commit(dgram_queue_t *q);

/* Consumer: get the oldest datagram (NULL if empty), then release it. */
dgram_t *dq_peek(dgram_queue_t *q);
void dq_release(dgram_queue_t *q);

uint32_t dq_count(const dgram_queue_t *q);

#endif /* DGRAM_QUEUE_H */
