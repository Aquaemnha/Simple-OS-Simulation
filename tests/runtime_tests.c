#include "queue.h"
#include "sched.h"
#include "timer.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>

static struct krnl_t kernel;
static struct pcb_t processes[MAX_QUEUE_SIZE + 1];
static atomic_int executing[MAX_QUEUE_SIZE];

/* FIFO must survive a full queue and removal from its middle. */
static void
test_queue (void)
{
  struct queue_t queue = { 0 };

  assert (dequeue (&queue) == NULL);
  assert (enqueue (NULL, &processes[0]) == -EINVAL);
  for (int i = 0; i < MAX_QUEUE_SIZE; i++)
    assert (enqueue (&queue, &processes[i]) == 0);
  assert (enqueue (&queue, &processes[MAX_QUEUE_SIZE]) == -ENOSPC);
  assert (queue.size == MAX_QUEUE_SIZE);
  assert (purgequeue (&queue, &processes[17]) == &processes[17]);
  for (int i = 0; i < MAX_QUEUE_SIZE; i++)
    if (i != 17)
      assert (dequeue (&queue) == &processes[i]);
  assert (empty (&queue));
}

/* A low-priority process gets its slot after the high queue uses 140. */
static void
test_scheduler (void)
{
  init_scheduler ();
  processes[0].prio = 0;
  processes[1].prio = 139;
  assert (add_proc (&processes[0]) == 0);
  assert (add_proc (&processes[0]) == -EEXIST);
  assert (put_proc (&processes[0]) == -EEXIST);
  assert (add_proc (&processes[1]) == 0);
  for (int i = 0; i < 140; i++)
    {
      assert (get_proc () == &processes[0]);
      assert (add_proc (&processes[0]) == -EEXIST);
      assert (put_proc (&processes[0]) == 0);
    }
  assert (get_proc () == &processes[1]);
  finish_proc (&processes[1]);
  assert (get_proc () == &processes[0]);
  finish_proc (&processes[0]);
  assert (queue_empty ());

  processes[0].prio = processes[1].prio = 17;
  assert (add_proc (&processes[0]) == 0);
  assert (add_proc (&processes[1]) == 0);
  for (int i = 0; i < 280; i++)
    {
      struct pcb_t *process = get_proc ();
      assert (process == &processes[i % 2]);
      assert (put_proc (process) == 0);
    }
  finish_scheduler ();
}

static void *
dispatch_repeatedly (void *unused)
{
  (void) unused;
  for (int i = 0; i < 4000; i++)
    {
      struct pcb_t *process = get_proc ();
      int index;

      assert (process != NULL);
      index = (int) (process - processes);
      assert (atomic_exchange (&executing[index], 1) == 0);
      assert (atomic_exchange (&executing[index], 0) == 1);
      assert (put_proc (process) == 0);
    }
  return NULL;
}

/* Concurrent dispatch cannot give the same PCB to two CPUs. */
static void
test_parallel_dispatch (void)
{
  pthread_t threads[8];

  init_scheduler ();
  for (int i = 0; i < MAX_QUEUE_SIZE; i++)
    {
      processes[i].prio = i % MAX_PRIO;
      assert (add_proc (&processes[i]) == 0);
    }
  for (int i = 0; i < 8; i++)
    assert (pthread_create (&threads[i], NULL, dispatch_repeatedly, NULL)
            == 0);
  for (int i = 0; i < 8; i++)
    assert (pthread_join (threads[i], NULL) == 0);
  for (int i = 0; i < MAX_QUEUE_SIZE; i++)
    {
      struct pcb_t *process = get_proc ();
      assert (process != NULL);
      finish_proc (process);
    }
  assert (get_proc () == NULL && queue_empty ());
  finish_scheduler ();
}

struct tick_args
{
  struct timer_id_t *event;
  unsigned int count;
};

static void *
tick_repeatedly (void *argument)
{
  struct tick_args *args = argument;

  for (unsigned int i = 0; i < args->count; i++)
    {
      assert (current_time () == i);
      next_slot (args->event);
      assert (current_time () == i + 1);
    }
  detach_event (args->event);
  return NULL;
}

/* A departing device must neither lose a tick nor block remaining devices. */
static void
test_timer (void)
{
  pthread_t threads[3];
  struct tick_args args[3];

  for (int i = 0; i < 3; i++)
    {
      args[i].event = attach_event ();
      args[i].count = i == 0 ? 20 : 120;
      assert (args[i].event != NULL);
    }
  assert (start_timer () == 0);
  assert (start_timer () == -EINVAL && attach_event () == NULL);
  for (int i = 0; i < 3; i++)
    assert (pthread_create (&threads[i], NULL, tick_repeatedly, &args[i])
            == 0);
  for (int i = 0; i < 3; i++)
    assert (pthread_join (threads[i], NULL) == 0);
  stop_timer ();
  assert (current_time () == 120);
}

int
main (void)
{
  for (int i = 0; i <= MAX_QUEUE_SIZE; i++)
    processes[i].krnl = &kernel;
  test_queue ();
  test_scheduler ();
  test_parallel_dispatch ();
  test_timer ();
  test_timer ();
  puts ("PASS: queue bounds, weighted MLQ, round robin, parallel dispatch, "
        "timer restart");
  return 0;
}
