#include "common.h"
                                                            

#include "dma.h"
#include "dpi_rtl.h"
#include "sysbus.h"
#include <errno.h>
#include <pthread.h>

enum channel_state
{
  DMA_IDLE,
  DMA_QUEUED,
  DMA_RUNNING,
  DMA_COMPLETE
};

                                                                         
                                                                              
static pthread_mutex_t channel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t channel_changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static enum channel_state state = DMA_IDLE;
static struct dma_result request;
static int started;
static int stopping;
static int ready;
static int init_status;

                                                                              
static void *
dma_worker (void *unused)
{
  int status;

  (void) unused;
  status = rtl_dma_init_simulation ();
  pthread_mutex_lock (&channel_lock);
  init_status = status;
  ready = 1;
  pthread_cond_broadcast (&channel_changed);
  if (status != 0)
    {
      pthread_mutex_unlock (&channel_lock);
      return NULL;
    }

  for (;;)
    {
      struct dma_result current;

      while (state != DMA_QUEUED && !stopping)
        pthread_cond_wait (&channel_changed, &channel_lock);
      if (state != DMA_QUEUED && stopping)
        break;

      current = request;
      state = DMA_RUNNING;
      pthread_mutex_unlock (&channel_lock);

                                                                              
                                                                             
      status = dpi_rtl_dma_copy (current.source, current.destination,
                                 current.length);
      current.bytes_transferred = dpi_rtl_dma_bytes_transferred ();

      pthread_mutex_lock (&channel_lock);
      current.interrupts = request.interrupts;
      current.cpu_instructions = request.cpu_instructions;
      if (status == 0
          && (current.interrupts != 1
              || current.bytes_transferred != current.length))
        status = -EIO;
      current.status = status;
      sysbus_dma_release ();
      request = current;
      state = DMA_COMPLETE;
    }

  pthread_mutex_unlock (&channel_lock);
  rtl_dma_cleanup_simulation ();
  return NULL;
}

int
dma_start (void)
{
  int status;

  pthread_mutex_lock (&channel_lock);
  if (started || state != DMA_IDLE)
    {
      pthread_mutex_unlock (&channel_lock);
      return -EBUSY;
    }
  if (!memory_bank_is_available ())
    {
      pthread_mutex_unlock (&channel_lock);
      return -ENODEV;
    }
  stopping = 0;
  ready = 0;
  status = pthread_create (&worker, NULL, dma_worker, NULL);
  if (status != 0)
    {
      pthread_mutex_unlock (&channel_lock);
      return -status;
    }
  started = 1;
  while (!ready)
    pthread_cond_wait (&channel_changed, &channel_lock);
  status = init_status;
  pthread_mutex_unlock (&channel_lock);
  if (status != 0)
    {
      pthread_join (worker, NULL);
      pthread_mutex_lock (&channel_lock);
      started = 0;
      pthread_mutex_unlock (&channel_lock);
    }
  return status;
}

static int
dma_submit_internal (uint32_t pid, uint64_t source, uint64_t destination,
                     uint64_t length, int already_reserved)
{
  int status;

  if (length == 0 || length > UINT32_MAX || length % 4 != 0)
    return -EINVAL;
  pthread_mutex_lock (&channel_lock);
  if (!started || stopping)
    status = -ENODEV;
  else if (state != DMA_IDLE)
    status = -EBUSY;
  else
    {
      status = already_reserved
                   ? 0
                   : sysbus_dma_reserve (source, destination, length);
      if (status == 0)
        {
          request = (struct dma_result) { .pid = pid,
                                          .source = source,
                                          .destination = destination,
                                          .length = length };
          state = DMA_QUEUED;
          pthread_cond_signal (&channel_changed);
        }
    }
  pthread_mutex_unlock (&channel_lock);
  return status;
}

int
dma_submit (uint32_t pid, uint64_t source, uint64_t destination,
            uint64_t length)
{
  return dma_submit_internal (pid, source, destination, length, 0);
}

int
dma_submit_reserved (uint32_t pid, uint64_t source, uint64_t destination,
                      uint64_t length)
{
  return dma_submit_internal (pid, source, destination, length, 1);
}

void
dma_done_callback (void)
{
  pthread_mutex_lock (&channel_lock);
  if (state == DMA_RUNNING)
    request.interrupts++;
  pthread_mutex_unlock (&channel_lock);
}

int
dma_take_completion (struct dma_result *result)
{
  int available;

  if (result == NULL)
    return -EINVAL;
  pthread_mutex_lock (&channel_lock);
  available = state == DMA_COMPLETE;
  if (available)
    {
      *result = request;
      state = DMA_IDLE;
    }
  pthread_mutex_unlock (&channel_lock);
  return available;
}

int
dma_pid_busy (uint32_t pid)
{
  int busy;

  pthread_mutex_lock (&channel_lock);
  busy = state != DMA_IDLE && request.pid == pid;
  pthread_mutex_unlock (&channel_lock);
  return busy;
}

int
dma_is_busy (void)
{
  int busy;

  pthread_mutex_lock (&channel_lock);
  busy = state != DMA_IDLE;
  pthread_mutex_unlock (&channel_lock);
  return busy;
}

                                                                          
                                                                      
void
dma_record_cpu_instruction (void)
{
  pthread_mutex_lock (&channel_lock);
  if (state == DMA_RUNNING && request.interrupts == 0)
    request.cpu_instructions++;
  pthread_mutex_unlock (&channel_lock);
}

void
dma_shutdown (void)
{
  pthread_mutex_lock (&channel_lock);
  if (!started)
    {
      pthread_mutex_unlock (&channel_lock);
      return;
    }
  stopping = 1;
  pthread_cond_signal (&channel_changed);
  pthread_mutex_unlock (&channel_lock);

                                                                         
                                                                               
  pthread_join (worker, NULL);
  pthread_mutex_lock (&channel_lock);
  started = 0;
  pthread_mutex_unlock (&channel_lock);
}
