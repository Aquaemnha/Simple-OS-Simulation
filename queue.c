#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

int empty(struct queue_t *q)
{
    if (q == NULL)
        return 1;
    return (q->size == 0);
}

void enqueue(struct queue_t *q, struct pcb_t *proc)
{
    /* TODO: put a new process to queue [q] */
    if (q == NULL || proc == NULL)
        return;
    if (q->size >= MAX_QUEUE_SIZE)
        return;

    q->proc[q->size] = proc;
    q->size++;
}

struct pcb_t *dequeue(struct queue_t *q)
{
    if (q == NULL || empty(q))
        return NULL;

    int best = 0;
    int i;
#ifndef MLQ_SCHED
    for (i = 1; i < q->size; i++)
        if (q->proc[i]->priority < q->proc[best]->priority)
            best = i;
#endif
    struct pcb_t *proc = q->proc[best];
    for (i = best; i < q->size - 1; i++)
        q->proc[i] = q->proc[i + 1];

    q->size--;
    q->proc[q->size] = NULL;
    return proc;
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc)
{
    /* TODO: remove a specific item from queue
     * */
    if (q == NULL || proc == NULL || empty(q))
        return NULL;

    int idx = -1;
    int i;
    for (i = 0; i < q->size; i++) {
        if (q->proc[i] == proc) {
            idx = i;
            break;
        }
    }
    if (idx == -1)
        return NULL;

    struct pcb_t *found = q->proc[idx];
    for (i = idx; i < q->size - 1; i++)
        q->proc[i] = q->proc[i + 1];

    q->size--;
    q->proc[q->size] = NULL;
    return found;
}