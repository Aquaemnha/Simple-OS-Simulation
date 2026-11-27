#include "timer.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

struct timer_event
{
  struct timer_id_t id;
  struct timer_event *next;
};

/* One mutex protects slot generation and all device predicates. A device
   cannot lose a wakeup between declaring itself done and awaiting the tick. */
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct timer_event *events;
static pthread_t timer_thread;
static uint64_t ticks;
static int started;

static void *
timer_routine (void *unused)
{
  (void) unused;
  pthread_mutex_lock (&timer_lock);
  for (;;)
    {
      struct timer_event *event;
      int waiting = 0, active = 0;

      for (event = events; event != NULL; event = event->next)
        if (!event->id.fsh)
          {
            active++;
            if (!event->id.done)
              waiting++;
          }
      if (active == 0)
        break;
      if (waiting != 0)
        {
          pthread_cond_wait (&changed, &timer_lock);
          continue;
        }
      ticks++;
      printf ("Time slot %3" PRIu64 "\n", ticks);
      for (event = events; event != NULL; event = event->next)
        event->id.done = 0;
      pthread_cond_broadcast (&changed);
    }
  pthread_mutex_unlock (&timer_lock);
  return NULL;
}

void
next_slot (struct timer_id_t *id)
{
  uint64_t previous;

  pthread_mutex_lock (&timer_lock);
  previous = ticks;
  id->done = 1;
  pthread_cond_broadcast (&changed);
  while (ticks == previous)
    pthread_cond_wait (&changed, &timer_lock);
  pthread_mutex_unlock (&timer_lock);
}

uint64_t
current_time (void)
{
  uint64_t value;

  pthread_mutex_lock (&timer_lock);
  value = ticks;
  pthread_mutex_unlock (&timer_lock);
  return value;
}

int
start_timer (void)
{
  int status;

  if (started || events == NULL)
    return -EINVAL;
  ticks = 0;
  status = pthread_create (&timer_thread, NULL, timer_routine, NULL);
  if (status == 0)
    started = 1;
  return -status;
}

void
detach_event (struct timer_id_t *id)
{
  pthread_mutex_lock (&timer_lock);
  id->fsh = 1;
  pthread_cond_broadcast (&changed);
  pthread_mutex_unlock (&timer_lock);
}

struct timer_id_t *
attach_event (void)
{
  struct timer_event *event;

  if (started)
    return NULL;
  event = calloc (1, sizeof *event);
  if (event == NULL)
    return NULL;
  event->next = events;
  events = event;
  return &event->id;
}

void
stop_timer (void)
{
  struct timer_event *event;

  if (started)
    pthread_join (timer_thread, NULL);
  started = 0;
  while ((event = events) != NULL)
    {
      events = event->next;
      free (event);
    }
}
