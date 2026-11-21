/* Real-model checks: boundaries, shared RAM, concurrency and shutdown. */

#include "common.h"
#include "dma.h"
#include "mm.h"
#include "sysbus.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t cpu_checksum;

static struct dma_result
finish_request (void)
{
  struct dma_result result;
  struct timespec begin, now;

  clock_gettime (CLOCK_MONOTONIC, &begin);
  while (dma_take_completion (&result) == 0)
    {
      cpu_checksum++;
      sched_yield ();
      clock_gettime (CLOCK_MONOTONIC, &now);
      assert (now.tv_sec - begin.tv_sec < 30);
    }
  assert (result.status == 0);
  assert (result.bytes_transferred == result.length);
  assert (result.interrupts == 1);
  assert (!dma_is_busy ());
  assert (dma_take_completion (&result) == 0);
  return result;
}

/* Invalid descriptors must leave the channel idle and RAM unchanged. */
static void
test_boundaries (void)
{
  unsigned char *bank;
  struct dma_result result;

  assert (dma_start () == -ENODEV);
  assert (dma_submit (1, 0, 16, 4) == -ENODEV);
  assert (dma_take_completion (NULL) == -EINVAL);
  dma_done_callback ();
  assert (dma_take_completion (&result) == 0);
  assert (memory_bank_init (0) == MEMORY_BANK_INVALID);
  assert (memory_bank_init (64) == 0);
  bank = memory_bank_get_ptr (0);
  memset (bank, 0x93, 64);
  assert (memory_bank_get_ptr (64) == NULL);
  assert (dpi_bank_write32_legacy (-1, 0) == -EFAULT);
  assert (dpi_bank_write32_legacy (61, 0) == -EFAULT);
  assert (dpi_bank_read32_legacy (INT64_MAX) == 0);
  for (int i = 0; i < 64; i++)
    assert (bank[i] == 0x93);

  assert (dma_start () == 0);
  assert (dma_start () == -EBUSY);
  const uint64_t invalid_lengths[] = {
    0, 1, 3, 5, UINT32_MAX, UINT64_C (1) << 32
  };
  for (size_t i = 0; i < sizeof invalid_lengths / sizeof *invalid_lengths; i++)
    assert (dma_submit (1, 0, 32, invalid_lengths[i]) == -EINVAL);
  assert (dma_submit (1, UINT64_MAX - 1, 32, 4) == -EFAULT);
  assert (dma_submit (1, UINT64_C (1) << 32, 32, 4) == -EFAULT);
  assert (dma_submit (1, 0, 61, 4) == -EFAULT);
  assert (dma_submit (1, 0, 0, 4) == -EINVAL);
  assert (dma_submit (1, 0, 3, 4) == -EINVAL);
  assert (!dma_is_busy ());

  /* Byte assembly preserves the high bit and does not require alignment. */
  bank[1] = 0x81;
  bank[2] = 0x23;
  bank[3] = 0x45;
  bank[4] = 0xf7;
  assert ((uint32_t) dpi_bank_read32_legacy (1) == UINT32_C (0xf7452381));
  assert (dma_submit (7, 1, 60, 4) == 0);
  assert (dma_pid_busy (7));
  assert (!dma_pid_busy (8));
  assert (dma_submit (8, 8, 32, 4) == -EBUSY);
  result = finish_request ();
  assert (result.pid == 7);
  assert (memcmp (bank + 1, bank + 60, 4) == 0);
  assert (bank[59] == 0x93);
  dma_shutdown ();
  dma_shutdown ();
  assert (memory_bank_cleanup () == 0);
  puts ("PASS: descriptor boundaries, endianness, "
        "unaligned and final RAM word");
}

/* Deterministic reservation checks: dependent memory operations return busy,
   while independent CPU memory operations remain usable. */
static void
test_shared_ram (void)
{
  unsigned char bytes[128] = {0};
  struct memphy_struct ram = {
    .storage = (BYTE *) bytes, .maxsz = sizeof bytes, .rdmflg = 1
  };
  BYTE value;

  assert (sysbus_bind_ram (NULL) == -EINVAL);
  assert (sysbus_bind_ram (&ram) == 0);
  assert (memory_bank_init (64) == MEMORY_BANK_INVALID);
  assert (MEMPHY_write (&ram, 1, (BYTE) 0x81) == 0);
  assert (sysbus_dma_reserve (1, 65, 4) == 0);
  assert (memory_bank_cleanup () == -EBUSY);
  assert (memory_bank_get_ptr (1) == NULL);
  assert (sysbus_bind_ram (&ram) == -EBUSY);
  assert (MEMPHY_write (&ram, 1, 7) == -EBUSY);
  assert (MEMPHY_write (&ram, 65, 7) == -EBUSY);
  assert (MEMPHY_read (&ram, 65, &value) == -EBUSY);
  assert (MEMPHY_read (&ram, 1, &value) == 0 && (unsigned char) value == 0x81);
  assert (MEMPHY_write (&ram, 120, 9) == 0);
  assert (MEMPHY_read (&ram, 120, &value) == 0 && value == 9);
  assert (MEMPHY_read (&ram, 128, &value) == -EFAULT);
  assert (MEMPHY_read (&ram, 1, NULL) == -EFAULT);
  assert (sysbus_range_pinned (&ram, 0, 4096));
  assert (MEMPHY_put_freefp (&ram, 0) == -EBUSY);
  sysbus_dma_release ();

  assert (dma_start () == 0);
  assert (dma_submit (9, 1, 65, 4) == 0);
  finish_request ();
  assert (MEMPHY_read (&ram, 65, &value) == 0
          && (unsigned char) value == 0x81);
  assert (bytes[1] == 0x81 && bytes[65] == 0x81);
  dma_shutdown ();
  assert (memory_bank_cleanup () == 0);
  bytes[0] = 3; /* Cleanup borrowed RAM must not free its storage. */
  puts ("PASS: CPU MEMPHY and DMA callbacks share RAM, "
        "buffer reservation and lifetime");
}

static pthread_barrier_t submit_barrier;
struct submitter
{
  uint32_t pid;
  int status;
};

static void *
submit_concurrently (void *argument)
{
  struct submitter *submitter = argument;

  pthread_barrier_wait (&submit_barrier);
  submitter->status = dma_submit (submitter->pid, 0, 512, 128);
  return NULL;
}

/* A completed request keeps its result until acknowledged, so simultaneous
   submitters cannot overwrite another PID's completion. */
static void
test_concurrency_and_shutdown (void)
{
  pthread_t threads[4];
  struct submitter submitters[4];
  struct dma_result result;
  int accepted = 0;
  uint32_t winner = 0;

  assert (memory_bank_init (1024 * 1024) == 0);
  unsigned char *bank = memory_bank_get_ptr (0);
  for (int i = 0; i < 1024 * 1024; i++)
    bank[i] = (unsigned char) (i * 37 + 0x81);
  assert (dma_start () == 0);
  assert (pthread_barrier_init (&submit_barrier, NULL, 5) == 0);
  for (int i = 0; i < 4; i++)
    {
      submitters[i].pid = 20 + i;
      assert (pthread_create (&threads[i], NULL, submit_concurrently,
                              &submitters[i]) == 0);
    }
  pthread_barrier_wait (&submit_barrier);
  for (int i = 0; i < 4; i++)
    {
      assert (pthread_join (threads[i], NULL) == 0);
      if (submitters[i].status == 0)
        {
          accepted++;
          winner = submitters[i].pid;
        }
      else
        assert (submitters[i].status == -EBUSY);
    }
  pthread_barrier_destroy (&submit_barrier);
  assert (accepted == 1);
  result = finish_request ();
  assert (result.pid == winner);

  for (int i = 0; i < 12; i++)
    {
      assert (dma_submit (30 + i, 1, 513, 64) == 0);
      result = finish_request ();
      assert (result.pid == (uint32_t) (30 + i));
      assert (memcmp (bank + 1, bank + 513, 64) == 0);
    }

  const uint64_t length = 262144;
  memset (bank + 524288, 0, length);
  assert (dma_submit (99, 0, 524288, length) == 0);
  dma_shutdown (); /* Must drain queued/running DMA, not discard it. */
  assert (dma_submit (100, 0, 512, 4) == -ENODEV);
  assert (dma_start () == -EBUSY); /* The old result still needs an ACK. */
  result = finish_request ();
  assert (result.pid == 99);
  assert (memcmp (bank, bank + 524288, length) == 0);
  assert (bank[524287] == (unsigned char) (524287 * 37 + 0x81));
  assert (bank[786432] == (unsigned char) (786432 * 37 + 0x81));
  assert (dma_start () == 0);
  assert (dma_submit (101, 0, 512, 4) == 0);
  finish_request ();
  dma_shutdown ();
  assert (memory_bank_cleanup () == 0);
  puts ("PASS: four submitters, retained completion, repeated requests, "
        "large copy, drain and restart");
}

int
main (void)
{
  test_boundaries ();
  test_shared_ram ();
  test_concurrency_and_shutdown ();
  printf ("All DMA checks passed; CPU loop iterations=%" PRIu64 "\n",
          cpu_checksum);
  return 0;
}
