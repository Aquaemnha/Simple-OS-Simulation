                                                                          

#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include "sysbus.h"
#include <errno.h>

int
__sys_dma_memcpy (struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs)
{
  uint64_t source, destination;
  int status;

  if (krnl == NULL || regs == NULL)
    return -EINVAL;
  if (regs->a1 >= PAGING_MAX_SYMTBL_SZ || regs->a2 >= PAGING_MAX_SYMTBL_SZ
      || regs->a4 == 0 || regs->a4 > UINT32_MAX || regs->a4 % 4 != 0)
    return -EINVAL;
  if (krnl->dma_map_buffers == NULL)
    return -ENOSYS;

  pthread_mutex_lock (&mmvm_lock);
  status = krnl->dma_map_buffers (krnl, pid, (uint32_t) regs->a1,
                                  (uint32_t) regs->a2, regs->a3, regs->a4,
                                  &source, &destination);
  if (status == 0)
    {
      status = dma_submit_reserved (pid, source, destination, regs->a4);
      if (status != 0)
        sysbus_dma_release ();
    }
  pthread_mutex_unlock (&mmvm_lock);
  return status;
}
