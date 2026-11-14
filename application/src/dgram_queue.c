#include "dgram_queue.h"

#include <stddef.h>

#include "stm32h5xx.h"

dgram_t *dq_alloc(dgram_queue_t *q)
{
  if ((q->head - q->tail) >= APP_DGRAM_QUEUE_DEPTH)
  {
    q->drops++;
    return NULL;
  }
  return &q->slots[q->head % APP_DGRAM_QUEUE_DEPTH];
}

void dq_commit(dgram_queue_t *q)
{
  __DMB(); /* slot contents visible before the index update */
  q->head++;
}

dgram_t *dq_peek(dgram_queue_t *q)
{
  if (q->head == q->tail)
  {
    return NULL;
  }
  __DMB();
  return &q->slots[q->tail % APP_DGRAM_QUEUE_DEPTH];
}

void dq_release(dgram_queue_t *q)
{
  __DMB();
  q->tail++;
}

uint32_t dq_count(const dgram_queue_t *q)
{
  return q->head - q->tail;
}
