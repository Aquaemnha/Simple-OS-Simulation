/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * demacOprx release. Educational use for course CO2018; see LICENSE.
 */

#include "cpu.h"
#include "dma.h"
#include "libmem.h"
#include "loader.h"
#include "mm64.h"
#include "queue.h"
#include "sched.h"
#include "sysbus.h"
#include "timer.h"
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static unsigned int time_slice, cpu_count, process_count;
static addr_t ram_size, swap_sizes[PAGING_MAX_MMSWP];
static atomic_int loader_done;
static atomic_int failed;
static struct krnl_t os;

struct launch
{
  struct pcb_t *process;
  uint64_t time;
};
static struct launch launches[MAX_QUEUE_SIZE];

static int
read_number (FILE *file, uint64_t *value)
{
  char token[100], *end;

  if (fscanf (file, "%99s", token) != 1 || token[0] < '0' || token[0] > '9')
    return -EINVAL;
  errno = 0;
  *value = strtoull (token, &end, 10);
  return errno != 0 || *end != '\0' ? -EINVAL : 0;
}

struct cpu_args
{
  struct timer_id_t *timer;
  unsigned int id;
  int started;
};

/* The retained DPI completion is handled at an instruction boundary. */
static void
handle_dma_interrupt (void)
{
  struct dma_result result;

  if (dma_take_completion (&result) == 1)
    {
      printf ("DMA interrupt: PID %u status %d bytes %u callbacks %u "
              "CPU instructions during DMA=%" PRIu64 "\n",
              result.pid, result.status, result.bytes_transferred,
              result.interrupts, result.cpu_instructions);
      if (result.status != 0)
        atomic_store (&failed, 1);
    }
}

static void
release_process (struct pcb_t *process)
{
  int status;

  print_pgtbl (process, 0, 0);
  status = free_pcb_memph (process);
  finish_proc (process);
  if (status != 0)
    {
      fprintf (stderr, "Cannot release PID %u: %d\n", process->pid, status);
      atomic_store (&failed, 1);
      return;
    }
  free (process->mm);
  unload (process);
}

static void
requeue_process (struct pcb_t *process)
{
  int status = put_proc (process);

  if (status != 0)
    {
      fprintf (stderr, "Cannot requeue PID %u: %d\n", process->pid, status);
      finish_proc (process);
      atomic_store (&failed, 1);
      /* The PID registry retains ownership for shutdown cleanup. */
    }
}

static void *
cpu_routine (void *argument)
{
  struct cpu_args *args = argument;
  struct pcb_t *process = NULL;
  unsigned int remaining = 0;

  for (;;)
    {
      int status = 0;

      handle_dma_interrupt ();
      if (process != NULL && process->pc == process->code->size)
        {
          if (dma_pid_busy (process->pid))
            requeue_process (process);
          else
            {
              printf ("CPU %u: Process %u finished\n", args->id, process->pid);
              release_process (process);
            }
          process = NULL;
        }
      if (process != NULL && remaining == 0)
        {
          requeue_process (process);
          process = NULL;
        }
      if (process == NULL)
        {
          process = get_proc ();
          remaining = time_slice;
          if (process != NULL)
            printf ("CPU %u: Dispatched PID %u at slot %" PRIu64 "\n",
                    args->id, process->pid, current_time ());
        }
      if (process == NULL && atomic_load (&loader_done))
        break;
      if (process != NULL && process->pc < process->code->size)
        {
          status = run (process);
          if (status != 0 && status != -EBUSY && status != -EAGAIN)
            {
              fprintf (stderr, "PID %u instruction %u failed: %d\n",
                       process->pid, process->pc, status);
              process->pc = process->code->size;
              atomic_store (&failed, 1);
            }
          remaining--;
          if (status == -EBUSY || status == -EAGAIN)
            remaining = 0;
        }
      sched_yield ();
      next_slot (args->timer);
    }
  printf ("CPU %u stopped\n", args->id);
  detach_event (args->timer);
  return NULL;
}

static void *
loader_routine (void *argument)
{
  struct timer_id_t *timer = argument;

  for (unsigned int i = 0; i < process_count; i++)
    {
      struct pcb_t *process = launches[i].process;
      int status;

      while (current_time () < launches[i].time)
        next_slot (timer);
      process->mm = calloc (1, sizeof *process->mm);
      pthread_mutex_lock (&mmvm_lock);
      status = process->mm == NULL ? -ENOMEM : init_mm (process->mm, process);
      if (status == 0)
        status = mm_register_process (process);
      pthread_mutex_unlock (&mmvm_lock);
      if (status == 0)
        {
          printf ("Loaded %s PID %u priority %u\n", process->path,
                  process->pid, process->prio);
          status = add_proc (process);
        }
      if (status != 0)
        {
          fprintf (stderr, "Cannot start PID %u: %d\n", process->pid, status);
          if (process->mm != NULL && process->mm->pgd != NULL)
            free_pcb_memph (process);
          free (process->mm);
          unload (process);
          atomic_store (&failed, 1);
        }
      launches[i].process = NULL;
      next_slot (timer);
    }
  atomic_store (&loader_done, 1);
  detach_event (timer);
  return NULL;
}

static int
read_config (const char *name)
{
  char path[256], program[100];
  FILE *file;
  unsigned int loaded = 0;
  uint64_t slice, cpus, processes;
  int status = -EINVAL;

  if (snprintf (path, sizeof path, "input/%s", name) >= (int) sizeof path)
    return -EINVAL;
  file = fopen (path, "r");
  if (file == NULL)
    return -ENOENT;
  if (read_number (file, &slice) != 0 || slice == 0 || slice > UINT_MAX
      || read_number (file, &cpus) != 0 || cpus == 0 || cpus > MAX_QUEUE_SIZE
      || read_number (file, &processes) != 0 || processes > MAX_QUEUE_SIZE
      || read_number (file, &ram_size) != 0 || ram_size < PAGING64_PAGESZ
      || ram_size > (PAGING_PTE_FPN_MASK + 1) * PAGING64_PAGESZ)
    goto finish;
  time_slice = (unsigned int) slice;
  cpu_count = (unsigned int) cpus;
  process_count = (unsigned int) processes;
  for (int i = 0; i < PAGING_MAX_MMSWP; i++)
    if (read_number (file, &swap_sizes[i]) != 0 || swap_sizes[i] > INT_MAX)
      goto finish;
  for (; loaded < process_count; loaded++)
    {
      struct pcb_t *process;
      uint64_t priority;

      if (read_number (file, &launches[loaded].time) != 0
          || fscanf (file, "%99s", program) != 1
          || read_number (file, &priority) != 0 || priority >= MAX_PRIO
          || (loaded != 0
              && launches[loaded].time < launches[loaded - 1].time))
        goto finish;
      if (snprintf (path, sizeof path, "input/proc/%s", program)
          >= (int) sizeof path)
        goto finish;
      process = load (path);
      if (process == NULL)
        goto finish;
      process->prio = (uint32_t) priority;
      process->krnl = &os;
      launches[loaded].process = process;
    }
  if (fscanf (file, " %99s", program) == 1)
    goto finish;
  status = 0;
finish:
  fclose (file);
  if (status != 0)
    {
      for (unsigned int i = 0; i < loaded; i++)
        unload (launches[i].process);
      process_count = 0;
    }
  return status;
}

int
main (int argc, char **argv)
{
  struct memphy_struct ram = { 0 }, swaps[PAGING_MAX_MMSWP] = { 0 };
  struct memphy_struct *swap_ptrs[PAGING_MAX_MMSWP];
  struct mm_struct kernel_mm = { 0 };
  struct cpu_args *args = NULL;
  pthread_t *cpus = NULL, loader;
  struct timer_id_t *loader_event = NULL;
  struct cpu_args fallback = { 0 };
  unsigned int started_cpus = 0;
  int status, loader_started = 0;

  if (argc != 2)
    {
      fprintf (stderr, "Usage: os [configuration in input/]\n");
      return 1;
    }
  status = read_config (argv[1]);
  if (status != 0)
    {
      fprintf (stderr, "Invalid configuration or process input: %d\n", status);
      return 1;
    }
  status = init_memphy (&ram, ram_size, 1);
  for (int i = 0; i < PAGING_MAX_MMSWP && status == 0; i++)
    {
      status = init_memphy (&swaps[i], swap_sizes[i], 1);
      swap_ptrs[i] = &swaps[i];
    }
  if (status == 0)
    status = init_mm (&kernel_mm, NULL);
  os.mm = &kernel_mm;
  os.krnl_pgd = kernel_mm.pgd;
  os.mram = &ram;
  os.mswp = swap_ptrs;
  os.active_mswp = &swaps[0];
  os.dma_map_buffers = mm_dma_map_buffers;
  if (status == 0)
    status = sysbus_bind_ram (&ram);
  if (status == 0)
    status = dma_start ();
  cpus = calloc (cpu_count, sizeof *cpus);
  args = calloc (cpu_count, sizeof *args);
  if (cpus == NULL || args == NULL)
    status = -ENOMEM;
  if (status == 0)
    {
      init_scheduler ();
      for (unsigned int i = 0; i < cpu_count; i++)
        {
          args[i].id = i;
          args[i].timer = attach_event ();
          if (args[i].timer == NULL)
            status = -ENOMEM;
        }
      loader_event = attach_event ();
      fallback.timer = attach_event ();
      if (loader_event == NULL || fallback.timer == NULL)
        status = -ENOMEM;
    }
  if (status == 0)
    status = start_timer ();
  if (status == 0)
    {
      int error = pthread_create (&loader, NULL, loader_routine, loader_event);

      if (error != 0)
        {
          status = -error;
          atomic_store (&loader_done, 1);
          detach_event (loader_event);
        }
      else
        loader_started = 1;
      for (unsigned int i = 0; i < cpu_count; i++)
        {
          error = loader_started
                      ? pthread_create (&cpus[i], NULL, cpu_routine, &args[i])
                      : EAGAIN;
          if (error != 0)
            {
              status = -error;
              detach_event (args[i].timer);
            }
          else
            {
              args[i].started = 1;
              started_cpus++;
            }
        }
      /* If thread creation failed for every CPU, drain on the main thread. */
      if (started_cpus == 0 && loader_started)
        cpu_routine (&fallback);
      else
        detach_event (fallback.timer);
      for (unsigned int i = 0; i < cpu_count; i++)
        if (args[i].started)
          pthread_join (cpus[i], NULL);
      if (loader_started)
        pthread_join (loader, NULL);
    }
  else if (args != NULL)
    {
      for (unsigned int i = 0; i < cpu_count; i++)
        if (args[i].timer != NULL)
          detach_event (args[i].timer);
      if (loader_event != NULL)
        detach_event (loader_event);
      if (fallback.timer != NULL)
        detach_event (fallback.timer);
    }
  dma_shutdown ();
  handle_dma_interrupt ();
  stop_timer ();
  while (os.processes != NULL)
    {
      struct pcb_t *process = os.processes;

      if (free_pcb_memph (process) != 0)
        {
          mm_unregister_process (process);
          mm64_destroy (process->mm);
          atomic_store (&failed, 1);
        }
      free (process->mm);
      unload (process);
    }
  finish_scheduler ();
  memory_bank_cleanup ();
  for (unsigned int i = 0; i < process_count; i++)
    unload (launches[i].process);
  mm64_destroy (&kernel_mm);
  destroy_memphy (&ram);
  for (int i = 0; i < PAGING_MAX_MMSWP; i++)
    destroy_memphy (&swaps[i]);
  free (cpus);
  free (args);
  if (status != 0)
    fprintf (stderr, "OS setup failed: %d\n", status);
  return status != 0 || atomic_load (&failed) ? 1 : 0;
}
