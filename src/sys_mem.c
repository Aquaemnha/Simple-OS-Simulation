#include "libmem.h"
#include "mm64.h"
#include "syscall.h"
#include <errno.h>
#include <limits.h>

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
  pthread_mutex_unlock (&mmvm_lock);
  if (caller == NULL)
    status = -ESRCH;
  else
    switch (regs->a1)
      {
      case SYSMEM_MAP_OP:
        if (regs->a3 <= INT_MAX)
          status = vmap_pgd_memset (caller, regs->a2, (int) regs->a3);
        break;
      case SYSMEM_INC_OP:
        if (regs->a2 <= INT_MAX)
          status = inc_vma_limit (caller, (int) regs->a2, regs->a3);
        break;
      case SYSMEM_SWP_OP:
        pthread_mutex_lock (&mmvm_lock);
        status = __mm_swap_page (caller, regs->a2, regs->a3);
        pthread_mutex_unlock (&mmvm_lock);
        break;
      case SYSMEM_ALLOC_OP:
        if (regs->a2 < PAGING_MAX_SYMTBL_SZ)
          {
            int vmaid = regs->a3 <= MM64_SMALL_MAX_SIZE
                            ? MM64_VMA_SMALL
                            : regs->a3 <= MM64_MEDIUM_MAX_SIZE
                                  ? MM64_VMA_MEDIUM
                                  : MM64_VMA_LARGE;

            status = __alloc (caller, vmaid, (int) regs->a2, regs->a3,
                              &address);
            if (status == 0 && regs->a2 < 10)
              caller->regs[regs->a2] = address;
          }
        break;
      case SYSMEM_KALLOC_OP:
        if (regs->a2 < PAGING_MAX_SYMTBL_SZ)
          {
            pthread_mutex_lock (&mmvm_lock);
            status = (int) __kmalloc (caller, 0, (int) regs->a2, regs->a3,
                                      &address);
            if (status == 0 && regs->a2 < 10)
              caller->regs[regs->a2] = address;
            pthread_mutex_unlock (&mmvm_lock);
          }
        break;
      case SYSMEM_FREE_OP:
        if (regs->a2 < PAGING_MAX_SYMTBL_SZ)
          {
            pthread_mutex_lock (&mmvm_lock);
            status = __free (caller, 0, (int) regs->a2);
            pthread_mutex_unlock (&mmvm_lock);
          }
        break;
      case SYSMEM_IO_READ:
        if (regs->a2 < PAGING_MAX_SYMTBL_SZ)
          {
            status = __read (caller, 0, (int) regs->a2, regs->a3, &value);
            if (status == 0)
              regs->a4 = (unsigned char) value;
          }
        break;
      case SYSMEM_IO_WRITE:
        if (regs->a2 < PAGING_MAX_SYMTBL_SZ && regs->a4 <= 255)
          {
            status = __write (caller, 0, (int) regs->a2, regs->a3,
                              (BYTE) regs->a4);
          }
        break;
      case SYSMEM_COPY_FROM_USER:
      case SYSMEM_COPY_TO_USER:
        if (regs->a2 < PAGING_MAX_SYMTBL_SZ
            && regs->a3 < PAGING_MAX_SYMTBL_SZ)
          status = mm_copy_regions (caller, (uint32_t) regs->a2,
                                    (uint32_t) regs->a3, regs->a4, regs->a5,
                                    regs->a1 == SYSMEM_COPY_FROM_USER);
        break;
      default:
        status = -ENOTSUP;
      }
  return status;
}
