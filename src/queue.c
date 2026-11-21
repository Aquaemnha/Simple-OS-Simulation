#include "queue.h"
#include <errno.h>
#include <string.h>

int
empty (struct queue_t *queue)
{
  return queue == NULL || queue->size == 0;
}

int
enqueue (struct queue_t *queue, struct pcb_t *process)
{
  if (queue == NULL || process == NULL)
    return -EINVAL;
  if (queue->size == MAX_QUEUE_SIZE)
    return -ENOSPC;
  queue->proc[queue->size++] = process;
  return 0;
}

struct pcb_t *
dequeue (struct queue_t *queue)
{
  struct pcb_t *process;

  if (empty (queue))
    return NULL;
  process = queue->proc[0];
  memmove (queue->proc, queue->proc + 1,
           (size_t) --queue->size * sizeof *queue->proc);
  return process;
}

struct pcb_t *
purgequeue (struct queue_t *queue, struct pcb_t *process)
{
  if (queue == NULL)
    return NULL;
  for (int i = 0; i < queue->size; i++)
    if (queue->proc[i] == process)
      {
        memmove (&queue->proc[i], &queue->proc[i + 1],
                 (size_t) (queue->size - i - 1) * sizeof *queue->proc);
        queue->size--;
        return process;
      }
  return NULL;
}
