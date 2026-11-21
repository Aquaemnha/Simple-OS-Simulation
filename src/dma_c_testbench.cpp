/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * demacOprx release. Educational use for course CO2018; see LICENSE.
 * Standalone driver. Test patterns belong here, not in the DMA service.
 */

#include "dma.h"
#include "sysbus.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <time.h>

#if DMA_MAIN
static int
parse_number (const char *text, uint64_t *value)
{
  char *end;

  if (text[0] == '\0' || text[0] == '-')
    return -1;
  errno = 0;
  *value = std::strtoull (text, &end, 0);
  return errno == 0 && *end == '\0' ? 0 : -1;
}

int
main (int argc, char **argv)
{
  const size_t bank_size = 1024 * 1024;
  uint64_t source = 0x1000;
  uint64_t destination = 0x2000;
  uint64_t length = 512;
  struct dma_result result;
  unsigned long calculations = 0;
  uint64_t checksum = 0;
  int status;

  if (argc != 1 && (argc != 4 || parse_number (argv[1], &source) != 0
                    || parse_number (argv[2], &destination) != 0
                    || parse_number (argv[3], &length) != 0))
    {
      std::fprintf (stderr, "Usage: %s [source destination length]\n", argv[0]);
      return 1;
    }
  if (length == 0 || length % 4 != 0 || source > bank_size
      || length > bank_size - source || destination > bank_size
      || length > bank_size - destination
      || (source <= destination ? destination - source < length
                                : source - destination < length))
    {
      std::fprintf (stderr, "Invalid DMA range or length (use disjoint buffers, 4-byte length).\n");
      return 1;
    }
  if (memory_bank_init (bank_size) == MEMORY_BANK_INVALID)
    return 1;
  auto *src = static_cast<unsigned char *> (memory_bank_get_ptr (source));
  auto *dst = static_cast<unsigned char *> (memory_bank_get_ptr (destination));
  for (uint64_t i = 0; i < length; i++)
    src[i] = static_cast<unsigned char> ((i * 37 + 0x81) & 0xff);
  std::memset (dst, 0, length);

  status = dma_start ();
  if (status == 0)
    status = dma_submit (1, source, destination, length);
  if (status != 0)
    {
      std::fprintf (stderr, "DMA submission failed: %d\n", status);
      dma_shutdown ();
      memory_bank_cleanup ();
      return 1;
    }
  std::printf ("submit: pid=1 bus=0x%llx -> 0x%llx bytes=%llu\n",
               (unsigned long long) source, (unsigned long long) destination,
               (unsigned long long) length);

  struct timespec begin, now;
  clock_gettime (CLOCK_MONOTONIC, &begin);
  while (dma_take_completion (&result) == 0)
    {
      /* Independent CPU work; no wait/join on the DMA instruction. */
      for (unsigned int i = 0; i < 256; i++)
        checksum += (uint64_t) calculations + i;
      calculations++;
      sched_yield ();
      clock_gettime (CLOCK_MONOTONIC, &now);
      if (now.tv_sec - begin.tv_sec > 30)
        {
          std::fprintf (stderr, "Timed out waiting for completion\n");
          dma_shutdown ();
          memory_bank_cleanup ();
          return 1;
        }
    }
  dma_shutdown ();
  status = result.status;
  for (uint64_t i = 0; i < length; i++)
    if (src[i] != ((i * 37 + 0x81) & 0xff) || dst[i] != src[i])
      status = -EIO;
  std::printf ("interrupt: pid=%u status=%d bytes=%u callbacks=%u\n",
               result.pid, result.status, result.bytes_transferred,
               result.interrupts);
  std::printf ("CPU work before completion: %lu batches, checksum=%llu\n",
               calculations, (unsigned long long) checksum);
  std::printf ("Full data and unchanged source: %s\n", status == 0 ? "PASS" : "FAIL");
  memory_bank_cleanup ();
  return status == 0 ? 0 : 1;
}
#endif
