/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * demacOprx release. Educational use for course CO2018; see LICENSE.
 */

#ifndef SYSTEM_BUS_H
#define SYSTEM_BUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct memphy_struct;
typedef int memory_bank_t;
#define MEMORY_BANK_INVALID (-1)

/* The supplied model addresses one RAM bank. The standalone bank owns its
   storage; binding OS RAM borrows it.  Rebinding/cleanup during DMA fails. */
memory_bank_t memory_bank_init (size_t size_bytes);
int memory_bank_cleanup (void);
int sysbus_bind_ram (struct memphy_struct *ram);
size_t memory_bank_get_size (void);
int memory_bank_is_available (void);

/* Only use the raw pointer before submission or after completion. */
void *memory_bank_get_ptr (uint64_t offset);

/* Shared CPU/DMA storage access. CPU accesses to a pending destination, and
   writes to either DMA buffer, return -EBUSY rather than waiting for DMA. */
int sysbus_cpu_read (struct memphy_struct *ram, uint64_t address,
                     unsigned char *value);
int sysbus_cpu_write (struct memphy_struct *ram, uint64_t address,
                      unsigned char value);
int sysbus_range_pinned (struct memphy_struct *ram, uint64_t address,
                         uint64_t length);

/* Internal channel reservation. Lock order: DMA state, then bus memory. */
int sysbus_dma_reserve (uint64_t source, uint64_t destination,
                       uint64_t length);
void sysbus_dma_release (void);
int sysbus_dma_error (void);

/* Generated DPI entry points: four little-endian bytes per access. */
int dpi_bank_read32_legacy (long long offset);
int dpi_bank_write32_legacy (long long offset, int value);

#ifdef __cplusplus
}
#endif

#endif
