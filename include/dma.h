/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * demacOprx release. Educational use for course CO2018; see LICENSE.
 */

#ifndef DMA_H
#define DMA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  struct dma_result
  {
    uint32_t pid;
    uint64_t source;
    uint64_t destination;
    uint64_t length;
    uint32_t bytes_transferred;
    unsigned int interrupts;
    uint64_t cpu_instructions;
    int status;
  };

  /* One channel, with one completion slot. Errors are negative errno values.
     Start/stop belong to the OS main thread. Bind RAM before starting. */
  int dma_start (void);
  void dma_shutdown (void);

  /* Kernel interface: addresses are RAM bus offsets, never VA or host
     pointers. Return immediately after accepting the request, or -EBUSY.
     Buffers must remain allocated until completion.  Length must be a nonzero
     multiple of 4, at most UINT32_MAX. */
  int dma_submit (uint32_t pid, uint64_t source, uint64_t destination,
                  uint64_t length);
  int dma_pid_busy (uint32_t pid);
  int dma_is_busy (void);
  void dma_record_cpu_instruction (void);

  /* CPU interrupt handling: 1 = consumed, 0 = no interrupt, negative = error.
     Consuming the result acknowledges it and makes the channel available. */
  int dma_take_completion (struct dma_result *result);

  /* Called by the supplied model on the rising edge of ch_done. */
  void dma_done_callback (void);

#ifdef __cplusplus
}
#endif

#endif
