/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * Caitoa release. Educational use for course CO2018; see LICENSE.
 */

#include "libmem.h"
#include "mm64.h"
#include "syscall.h"
#include <errno.h>

int
__sys_memmap (struct krnl_t *kernel, uint32_t pid, struct sc_regs *regs)
{
  struct pcb_t *caller;
  addr_t address;
  BYTE value;
  int status = -EINVAL;

  if (kernel == NULL || regs == NULL)
    return -EINVAL;
  pthread_mutex_lock (&mmvm_lock);
  caller = mm_find_process (kernel, pid);
  if (caller == NULL)
    status = -ESRCH;
  else if (regs->a2 >= PAGING_MAX_SYMTBL_SZ)
    status = -EINVAL;
  else
    switch (regs->a1)
      {
      case SYSMEM_ALLOC_OP:
        /* Separate similarly sized user objects into small/medium/large VMAs.
         */
        status = __alloc (caller,
                          regs->a3 <= 1024   ? 0
                          : regs->a3 <= 4096 ? 1
                                             : 2,
                          (int) regs->a2, regs->a3, &address);
        if (status == 0 && regs->a2 < 10)
          caller->regs[regs->a2] = address;
        break;
      case SYSMEM_KALLOC_OP:
        status
            = (int) __kmalloc (caller, 0, (int) regs->a2, regs->a3, &address);
        if (status == 0 && regs->a2 < 10)
          caller->regs[regs->a2] = address;
        break;
      case SYSMEM_FREE_OP:
        status = __free (caller, 0, (int) regs->a2);
        break;
      case SYSMEM_IO_READ:
        status = __read (caller, 0, (int) regs->a2, regs->a3, &value);
        if (status == 0)
          regs->a4 = (unsigned char) value;
        break;
      case SYSMEM_IO_WRITE:
        if (regs->a4 <= 255)
          status
              = __write (caller, 0, (int) regs->a2, regs->a3, (BYTE) regs->a4);
        break;
      case SYSMEM_COPY_FROM_USER:
      case SYSMEM_COPY_TO_USER:
        if (regs->a3 < PAGING_MAX_SYMTBL_SZ)
          status = mm_copy_regions (caller, (uint32_t) regs->a2,
                                    (uint32_t) regs->a3, regs->a4, regs->a5,
                                    regs->a1 == SYSMEM_COPY_FROM_USER);
        break;
      default:
        status = -ENOTSUP;
      }
  pthread_mutex_unlock (&mmvm_lock);
  return status;
}
