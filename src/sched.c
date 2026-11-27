/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * LamiaAtrium release. Educational use for course CO2018; see LICENSE.
 */

#include "sched.h"
#include "queue.h"
#include <errno.h>
#include <pthread.h>
#include <string.h>

static struct queue_t ready_queue;
static struct queue_t running_list;
static struct queue_t mlq_ready_queue[MAX_PRIO];
/* queue_lock bảo vệ các hàng đợi MLQ khi có nhiều CPU truy cập đồng thời */
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int priority;
static unsigned int remaining;

void
init_scheduler (void)
{
  pthread_mutex_lock (&queue_lock);
  memset (&ready_queue, 0, sizeof ready_queue);
  memset (&running_list, 0, sizeof running_list);
  memset (mlq_ready_queue, 0, sizeof mlq_ready_queue);
  priority = 0;
  remaining = MAX_PRIO;
  pthread_mutex_unlock (&queue_lock);
}

void
finish_scheduler (void)
{
  init_scheduler ();
}

int
queue_empty (void)
{
  int result = 1;

  pthread_mutex_lock (&queue_lock);
  for (int i = 0; i < MAX_PRIO; i++)
    if (!empty (&mlq_ready_queue[i]))
      result = 0;
  pthread_mutex_unlock (&queue_lock);
  return result;
}

struct pcb_t *
get_proc (void)
{
  struct pcb_t *process = NULL;

  pthread_mutex_lock (&queue_lock);
  for (int visited = 0; visited < MAX_PRIO; visited++)
    {
      if (!empty (&mlq_ready_queue[priority])
          && running_list.size < MAX_QUEUE_SIZE)
        {
          process = dequeue (&mlq_ready_queue[priority]);
          enqueue (&running_list, process);
          remaining--;
          if (remaining != 0)
            break;
        }
      priority = (priority + 1) % MAX_PRIO;
      remaining = MAX_PRIO - priority;
      if (process != NULL)
        break;
    }
  pthread_mutex_unlock (&queue_lock);
  return process;
}

static int
queue_contains (struct queue_t *queue, struct pcb_t *process)
{
  for (int i = 0; i < queue->size; i++)
    if (queue->proc[i] == process)
      return 1;
  return 0;
}

static int
make_ready (struct pcb_t *process, int running)
{
  int status;

  if (process == NULL || process->krnl == NULL || process->prio >= MAX_PRIO)
    return -EINVAL;
  pthread_mutex_lock (&queue_lock);
  if (queue_contains (&mlq_ready_queue[process->prio], process)
      || queue_contains (&running_list, process) != running)
    status = -EEXIST;
  else
    {
      status = enqueue (&mlq_ready_queue[process->prio], process);
      if (status == 0)
        {
          if (running)
            purgequeue (&running_list, process);
          process->krnl->ready_queue = &ready_queue;
          process->krnl->mlq_ready_queue = mlq_ready_queue;
          process->krnl->running_list = &running_list;
        }
    }
  pthread_mutex_unlock (&queue_lock);
  return status;
}

int
put_proc (struct pcb_t *process)
{
  return make_ready (process, 1);
}

int
add_proc (struct pcb_t *process)
{
  return make_ready (process, 0);
}

void
finish_proc (struct pcb_t *process)
{
  pthread_mutex_lock (&queue_lock);
  purgequeue (&running_list, process);
  pthread_mutex_unlock (&queue_lock);
}
