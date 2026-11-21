/*
 * Copyright (C) 2025 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Sierra release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "common.h"
#include <pthread.h>

/* Serialize allocation/free, DMA buffer mapping and reservation. */
extern pthread_mutex_t mmvm_lock;

#define SYSMEM_MAP_OP 1
#define SYSMEM_INC_OP 2
#define SYSMEM_SWP_OP 3
#define SYSMEM_IO_READ 4
#define SYSMEM_IO_WRITE 5
#define SYSMEM_ALLOC_OP 6
#define SYSMEM_FREE_OP 7
#define SYSMEM_KALLOC_OP 8
#define SYSMEM_COPY_FROM_USER 9
#define SYSMEM_COPY_TO_USER 10

/* Kernel registry operations require mmvm_lock. */
struct pcb_t *mm_find_process (struct krnl_t *, uint32_t);
int mm_register_process (struct pcb_t *);
int mm_unregister_process (struct pcb_t *);
int mm_dma_map_buffers (struct krnl_t *, uint32_t, uint32_t, uint32_t,
                        uint64_t, uint64_t, uint64_t *, uint64_t *);
int mm_region_span (struct pcb_t *, uint32_t, addr_t, addr_t,
                    struct vm_rg_struct **);
int mm_copy_regions (struct pcb_t *, uint32_t, uint32_t, addr_t, addr_t, int);

extern struct vm_area_struct *get_vma_by_num (struct mm_struct *mm, int vmaid);
int liballoc (struct pcb_t *, addr_t, uint32_t);
int libfree (struct pcb_t *, uint32_t);
int free_pcb_memph (struct pcb_t *);
int libread (struct pcb_t *, uint32_t, addr_t, uint32_t *);
int libwrite (struct pcb_t *, BYTE, uint32_t, addr_t);
int libkmem_malloc (struct pcb_t *, addr_t, uint32_t);
int libkmem_copy_from_user (struct pcb_t *, uint32_t, uint32_t, addr_t,
                            addr_t);
int libkmem_copy_to_user (struct pcb_t *, uint32_t, uint32_t, addr_t, addr_t);
int libmem_dma_memcpy (struct pcb_t *, arg_t, arg_t, arg_t, arg_t);
