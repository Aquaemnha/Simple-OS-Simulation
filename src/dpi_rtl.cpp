/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * demacOprx release. Educational use for course CO2018; see LICENSE.
 * This wrapper is used only by the DMA worker thread (context-based time).
 */

#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vdma_top.h"
#include "dpi_rtl.h"
#include <cerrno>
#include <cstdlib>
#include <new>

static Vdma_top *top;
static VerilatedContext *context;
static VerilatedVcdC *tracer;
static uint32_t last_bytes;

/* Advance one clock edge using the worker's simulation context. */
static void
tick ()
{
  top->clk ^= 1;
  top->eval ();
  if (tracer != nullptr)
    tracer->dump (context->time ());
  context->timeInc (1);
}

extern "C" void
rtl_dma_cleanup_simulation (void)
{
  if (top != nullptr)
    top->final ();
  if (tracer != nullptr)
    {
      tracer->close ();
      delete tracer;
      tracer = nullptr;
    }
  delete top;
  top = nullptr;
  delete context;
  context = nullptr;
}

extern "C" int
rtl_dma_init_simulation (void)
{
  const char *trace_path = std::getenv ("DMA_TRACE");

  if (top != nullptr)
    return -EBUSY;
  try
    {
      context = new VerilatedContext;
      context->threads (1);
      context->traceEverOn (trace_path != nullptr && trace_path[0] != '\0');
      top = new Vdma_top (context, "dma_top");
      if (trace_path != nullptr && trace_path[0] != '\0')
        {
          tracer = new VerilatedVcdC;
          top->trace (tracer, 99);
          tracer->open (trace_path);
          if (!tracer->isOpen ())
            {
              rtl_dma_cleanup_simulation ();
              return -EIO;
            }
        }
    }
  catch (const std::bad_alloc &)
    {
      rtl_dma_cleanup_simulation ();
      return -ENOMEM;
    }
  last_bytes = 0;
  return 0;
}

extern "C" int
dpi_rtl_dma_copy (uint64_t source, uint64_t destination, uint64_t length)
{
  int status;
  uint64_t half_cycles = 0;
  uint64_t limit;

  last_bytes = 0;
  if (top == nullptr)
    return -ENODEV;
  if (length == 0 || length > UINT32_MAX || length % 4 != 0)
    return -EINVAL;

  top->clk = 0;
  top->rst_n = 0;
  top->desc_valid = 0;
  top->ch_enable = 0;
  top->ch_pause = 0;
  top->eval ();
  tick ();
  tick ();
  top->rst_n = 1;
  top->desc_data_0 = source;
  top->desc_data_1 = destination;
  top->desc_data_2 = length;
  top->desc_data_3 = 0;
  top->desc_valid = 1;
  top->ch_enable = 1;

  /* The model needs read/write handshakes for every word. Scale the timeout
     with the transfer rather than truncating large copies at 100000 ticks. */
  limit = 64 + 32 * (length / 4);
  while (!top->ch_done && !top->ch_error && !context->gotFinish ()
         && half_cycles < limit)
    {
      tick ();
      half_cycles++;
    }
  status = top->ch_done ? 0 : -ETIMEDOUT;
  if (top->ch_error || context->gotFinish ())
    status = -EIO;
  if (status == 0)
    {
      /* The supplied ch_done edge detector calls DPI on the next rising edge.
         Its irq pin is disabled by intr_en=0 in the generated model. */
      tick ();
      tick ();
    }
  last_bytes = top->ch_bytes_transferred;
  if (sysbus_dma_error () != 0)
    status = sysbus_dma_error ();

  /* Quiesce the bus before returning, including the timeout/error path. */
  top->ch_enable = 0;
  top->desc_valid = 0;
  top->rst_n = 0;
  top->eval ();
  tick ();
  tick ();
  return status;
}

extern "C" uint32_t
dpi_rtl_dma_bytes_transferred (void)
{
  return last_bytes;
}

extern "C" int
dpi_rtl_dma_is_available (void)
{
  return top != nullptr;
}
