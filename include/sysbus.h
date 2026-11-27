 




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

 

memory_bank_t memory_bank_init (size_t size_bytes);
int memory_bank_cleanup (void);
int sysbus_bind_ram (struct memphy_struct *ram);
size_t memory_bank_get_size (void);
int memory_bank_is_available (void);

 
void *memory_bank_get_ptr (uint64_t offset);

 

int sysbus_cpu_read (struct memphy_struct *ram, uint64_t address,
                     unsigned char *value);
int sysbus_cpu_write (struct memphy_struct *ram, uint64_t address,
                      unsigned char value);
int sysbus_range_pinned (struct memphy_struct *ram, uint64_t address,
                         uint64_t length);

 
int sysbus_dma_reserve (uint64_t source, uint64_t destination,
                       uint64_t length);
void sysbus_dma_release (void);
int sysbus_dma_error (void);
int sysbus_get_access_counts (uint64_t *cpu_reads, uint64_t *cpu_writes,
                               uint64_t *dma_reads, uint64_t *dma_writes);
void sysbus_reset_access_counts (void);

 
int dpi_bank_read32_legacy (long long offset);
int dpi_bank_write32_legacy (long long offset, int value);

#ifdef __cplusplus
}
#endif

#endif
