 




#include "libmem.h"
#include "dma.h"
#include "mm64.h"
#include "sysbus.h"
#include "syscall.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;

 

struct pcb_t *
mm_find_process (struct krnl_t *kernel, uint32_t pid)
{
  struct pcb_t *process;

  if (kernel == NULL)
    return NULL;
  for (process = kernel->processes; process != NULL;
       process = process->next_process)
    if (process->pid == pid)
      return process;
  return NULL;
}

int
mm_register_process (struct pcb_t *process)
{
  if (process == NULL || process->krnl == NULL || process->mm == NULL)
    return -EINVAL;
  if (mm_find_process (process->krnl, process->pid) != NULL)
    return -EEXIST;
  process->next_process = process->krnl->processes;
  process->krnl->processes = process;
  return 0;
}

int
mm_unregister_process (struct pcb_t *process)
{
  struct pcb_t **link;

  if (process == NULL || process->krnl == NULL)
    return -EINVAL;
  for (link = &process->krnl->processes; *link != NULL;
       link = &(*link)->next_process)
    if (*link == process)
      {
        *link = process->next_process;
        process->next_process = NULL;
        return 0;
      }
  return -ESRCH;
}

struct vm_rg_struct *
get_symrg_byid (struct mm_struct *mm, int id)
{
  return mm != NULL && id >= 0 && id < PAGING_MAX_SYMTBL_SZ ? &mm->symrgtbl[id]
                                                            : NULL;
}

int
mm_region_span (struct pcb_t *process, uint32_t id, addr_t offset,
                addr_t length, struct vm_rg_struct **result)
{
  struct vm_rg_struct *region;

  if (process == NULL || process->mm == NULL || result == NULL
      || id >= PAGING_MAX_SYMTBL_SZ)
    return -EINVAL;
  region = get_symrg_byid (process->mm, (int) id);
  if (region == NULL || region->rg_end <= region->rg_start
      || !mm64_canonical (region->rg_start)
      || !mm64_canonical (region->rg_end - 1)
      || (region->mode_bit && region->rg_end > MM64_USER_LIMIT)
      || (!region->mode_bit && region->rg_start < MM64_KERNEL_BASE)
      || offset > region->rg_end - region->rg_start
      || length > region->rg_end - region->rg_start - offset)
    return -EFAULT;
  *result = region;
  return 0;
}

 
int
get_free_vmrg_area (struct pcb_t *caller, int id, int size,
                    struct vm_rg_struct *result)
{
  struct vm_area_struct *area;
  struct vm_rg_struct **link, *region;

  if (caller == NULL || caller->mm == NULL || result == NULL || size <= 0)
    return -EINVAL;
  area = get_vma_by_num (caller->mm, id);
  if (area == NULL)
    return -EINVAL;
  for (link = &area->vm_freerg_list; *link != NULL; link = &(*link)->rg_next)
    if ((addr_t) size <= (*link)->rg_end - (*link)->rg_start)
      {
        region = *link;
        *result = *region;
        result->rg_end = result->rg_start + size;
        result->rg_next = NULL;
        region->rg_start += size;
        if (region->rg_start == region->rg_end)
          {
            *link = region->rg_next;
            free (region);
          }
        return 0;
      }
  return -ENOMEM;
}

int
__alloc (struct pcb_t *caller, int id, int symbol, addr_t size,
         addr_t *address)
{
  struct vm_rg_struct *region;
  struct vm_area_struct *area;
  struct vm_rg_struct allocated = { 0 };
  addr_t end;
  int status;

  if (caller == NULL || caller->mm == NULL || address == NULL
      || symbol < 0 || symbol >= PAGING_MAX_SYMTBL_SZ || size == 0
      || size > INT32_MAX || id < MM64_VMA_SMALL || id > MM64_VMA_LARGE)
    return -EINVAL;
  region = get_symrg_byid (caller->mm, symbol);
  if (region == NULL)
    return -EINVAL;
  if (region->rg_end > region->rg_start)
    return -EEXIST;
  status = mm64_prepare_vma (caller, id);
  if (status != 0)
    return status;
  area = get_vma_by_num (caller->mm, id);
  if (area == NULL)
    return -ENOENT;
  status = get_free_vmrg_area (caller, id, (int) size, &allocated);
  if (status != 0)
    {
      if (area->sbrk > MM64_USER_LIMIT || size > MM64_USER_LIMIT - area->sbrk)
        return -ENOMEM;
      end = area->sbrk + size;
      if (end > area->vm_end)
        {
          status = inc_vma_limit (caller, id, end - area->vm_end);
          if (status != 0)
            return status;
        }
      allocated.rg_start = area->sbrk;
      allocated.rg_end = end;
      area->sbrk = end;
    }
  allocated.vmaid = id;
  allocated.mode_bit = 1;
  allocated.rg_next = NULL;
  *region = allocated;
  *address = allocated.rg_start;
  return 0;
}

static int
region_pinned (struct pcb_t *caller, struct vm_rg_struct *region)
{
  struct mm_struct *mm;

  if (caller == NULL || caller->krnl == NULL || region == NULL)
    return -EINVAL;
  mm = region->mode_bit ? caller->mm : caller->krnl->mm;
  if (mm == NULL)
    return -EINVAL;
  for (addr_t address = region->rg_start; address < region->rg_end;)
    {
      addr_t step = PAGING64_PAGESZ - (address & (PAGING64_PAGESZ - 1));
      addr_t entry = pte_get_entry (mm, address >> 12, caller);

      if ((entry & PAGING_PTE_SWAPPED_MASK) == 0)
        {
          addr_t physical;

          if ((entry & PAGING_PTE_PRESENT_MASK) == 0)
            return -EFAULT;
          physical = PAGING_PTE_FPN (entry) * PAGING64_PAGESZ
                     + (address & (PAGING64_PAGESZ - 1));
          if (sysbus_range_pinned (caller->krnl->mram, physical,
                                   step < region->rg_end - address
                                       ? step
                                       : region->rg_end - address))
            return -EBUSY;
        }
      address += step < region->rg_end - address
                     ? step
                     : region->rg_end - address;
    }
  return 0;
}

int
__free (struct pcb_t *caller, int id, int symbol)
{
  struct vm_rg_struct *region;
  struct vm_rg_struct *free_region;
  struct vm_area_struct *area;
  int status;

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL
      || caller->krnl->mram == NULL || symbol < 0
      || symbol >= PAGING_MAX_SYMTBL_SZ)
    return -EINVAL;
  region = get_symrg_byid (caller->mm, symbol);
  (void) id;
  if (region == NULL || region->rg_end <= region->rg_start)
    return -EINVAL;
  status = region_pinned (caller, region);
  if (status != 0)
    return status;
  if (!region->mode_bit)
    {
      addr_t size
          = (region->rg_end - region->rg_start + 4095) & ~UINT64_C (4095);

      for (addr_t offset = 0; offset < size; offset += PAGING64_PAGESZ)
        {
          addr_t page = (region->rg_start + offset) >> 12;
          addr_t entry = pte_get_entry (caller->krnl->mm, page, caller);

          status
              = MEMPHY_put_freefp (caller->krnl->mram, PAGING_PTE_FPN (entry));
          if (status != 0)
            return status;
          pte_set_entry (caller->krnl->mm, page, 0, caller);
        }
      memset (region, 0, sizeof *region);
      return 0;
    }
  free_region = malloc (sizeof *free_region);
  if (free_region == NULL)
    return -ENOMEM;
  *free_region = *region;
  area = get_vma_by_num (caller->mm, region->vmaid);
  if (area == NULL)
    {
      free (free_region);
      return -EINVAL;
    }
  status = enlist_vm_rg_node (&area->vm_freerg_list, free_region);
  if (status != 0)
    {
      free (free_region);
      return status;
    }
  memset (region, 0, sizeof *region);
  return 0;
}

addr_t
__kmalloc (struct pcb_t *caller, int id, int symbol, addr_t size,
           addr_t *address)
{
  struct vm_rg_struct *region;
  struct vm_area_struct *area;
  addr_t aligned;
  int status;

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL
      || caller->krnl->mm == NULL || address == NULL || symbol < 0
      || symbol >= PAGING_MAX_SYMTBL_SZ)
    return (addr_t) -EINVAL;
  region = get_symrg_byid (caller->mm, symbol);
  area = caller->krnl->mm->mmap;
  (void) id;
  if (region == NULL || area == NULL || size == 0 || size > INT32_MAX)
    return (addr_t) -EINVAL;
  if (region->rg_end != region->rg_start)
    return (addr_t) -EEXIST;
  aligned = (size + 4095) & ~UINT64_C (4095);
  if (aligned > UINT64_MAX - area->sbrk)
    return (addr_t) -ENOMEM;
  status = mm64_map_contiguous (caller, area->sbrk, aligned);
  if (status != 0)
    return (addr_t) status;
  *region = (struct vm_rg_struct) { .rg_start = area->sbrk,
                                    .rg_end = area->sbrk + size,
                                    .mode_bit = 0 };
  *address = area->sbrk;
  area->sbrk += aligned;
  area->vm_end = area->sbrk;
  return 0;
}

static int
access_region (struct pcb_t *caller, int symbol, addr_t offset, int mode,
               int write, BYTE *value)
{
  struct vm_rg_struct *region;
  struct mm_struct *mm;
  addr_t physical, pinned_frame;
  int status;

  if (caller == NULL || caller->krnl == NULL || caller->krnl->mram == NULL
      || value == NULL)
    return -EINVAL;
  status = mm_region_span (caller, symbol, offset, 1, &region);
  if (status != 0)
    return status;
  if (region->mode_bit != mode)
    return -EACCES;
  mm = mode ? caller->mm : caller->krnl->mm;
  status = mm64_translate_pin (mm, caller->krnl->mram,
                               region->rg_start + offset, &physical,
                               &pinned_frame);
  if (status != 0)
    return status;
  status = write ? MEMPHY_write (caller->krnl->mram, physical, *value)
                 : MEMPHY_read (caller->krnl->mram, physical, value);
  if (status == 0 && write)
    {
      addr_t page = (region->rg_start + offset) >> 12;
      addr_t entry = pte_get_entry (mm, page, caller);

      status = pte_set_entry (mm, page, entry | PAGING_PTE_DIRTY_MASK, caller);
    }
  MEMPHY_unpin_frame (caller->krnl->mram, pinned_frame);
  return status;
}

int
__read (struct pcb_t *caller, int id, int symbol, addr_t offset, BYTE *value)
{
  (void) id;
  return access_region (caller, symbol, offset, 1, 0, value);
}

int
__write (struct pcb_t *caller, int id, int symbol, addr_t offset, BYTE value)
{
  (void) id;
  return access_region (caller, symbol, offset, 1, 1, &value);
}

 

int
mm_copy_regions (struct pcb_t *caller, uint32_t source, uint32_t destination,
                 addr_t offset, addr_t length, int from_user)
{
  struct vm_rg_struct *src, *dst;

  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL
      || caller->krnl->mm == NULL || length == 0)
    return -EINVAL;
  int status = mm_region_span (caller, source, offset, length, &src);

  if (status == 0)
    status = mm_region_span (caller, destination, offset, length, &dst);
  if (status != 0)
    return status;
  if (src->mode_bit != from_user || dst->mode_bit == from_user)
    return -EACCES;
  status = region_pinned (caller, src);
  if (status == 0)
    status = region_pinned (caller, dst);
  if (status != 0)
    return status;
  for (addr_t i = 0; i < length; i++)
    {
      BYTE value;

      status
          = access_region (caller, source, offset + i, from_user, 0, &value);
      if (status == 0)
        status = access_region (caller, destination, offset + i, !from_user, 1,
                                &value);
      if (status != 0)
        return status;
    }
  return 0;
}

static int
translate_span (struct pcb_t *caller, struct vm_rg_struct *region,
                addr_t offset, addr_t length, uint64_t *bus,
                addr_t **pinned_frames, size_t *pinned_count)
{
  struct mm_struct *mm = region->mode_bit ? caller->mm : caller->krnl->mm;
  addr_t address = region->rg_start + offset, consumed = 0;
  size_t page_capacity;
  addr_t *frames;
  size_t frame_count = 0;
  int status = 0;

  if (length == 0 || pinned_frames == NULL || pinned_count == NULL)
    return -EINVAL;
  page_capacity = (size_t) ((length + (address & 4095) + 4095) / 4096);
  if (page_capacity == 0 || page_capacity > SIZE_MAX / sizeof *frames)
    return -EINVAL;
  frames = calloc (page_capacity, sizeof *frames);
  if (frames == NULL)
    return -ENOMEM;
  while (consumed < length)
    {
      addr_t current = address + consumed;
      addr_t physical, step = PAGING64_PAGESZ - (current & 4095);
      addr_t page_entry = pte_get_entry (mm, current >> 12, caller);

      if (!(page_entry & PAGING_PTE_PRESENT_MASK)
          && !(page_entry & PAGING_PTE_SWAPPED_MASK))
        {
          status = -EFAULT;
          break;
        }
      addr_t pinned_frame;

      status = mm64_translate_pin (mm, caller->krnl->mram, current, &physical,
                                   &pinned_frame);
      if (status != 0)
        break;
      if (frame_count == 0)
        *bus = physical;
      else if (physical != *bus + consumed)
        {
          MEMPHY_unpin_frame (caller->krnl->mram, pinned_frame);
          status = -ENOTSUP;
          break;
        }
      frames[frame_count++] = pinned_frame;
      if ((step < length - consumed ? step : length - consumed)
          > (addr_t) caller->krnl->mram->maxsz - physical)
        {
          status = -EFAULT;
          break;
        }
      consumed += step < length - consumed ? step : length - consumed;
    }
  if (status != 0)
    {
      while (frame_count != 0)
        MEMPHY_unpin_frame (caller->krnl->mram, frames[--frame_count]);
      free (frames);
      return status;
    }
  *pinned_frames = frames;
  *pinned_count = frame_count;
  return 0;
}

int
mm_dma_map_buffers (struct krnl_t *kernel, uint32_t pid, uint32_t source,
                    uint32_t destination, uint64_t offset, uint64_t length,
                    uint64_t *source_bus, uint64_t *destination_bus)
{
  struct pcb_t *caller;
  struct vm_rg_struct *src, *dst;
  addr_t *source_frames = NULL, *destination_frames = NULL;
  size_t source_count = 0, destination_count = 0;
  int status;

  if (kernel == NULL || kernel->mram == NULL || source_bus == NULL
      || destination_bus == NULL || length == 0)
    return -EINVAL;
  caller = mm_find_process (kernel, pid);
  if (caller == NULL)
    return -ESRCH;
  status = mm_region_span (caller, source, offset, length, &src);
  if (status == 0)
    status = mm_region_span (caller, destination, offset, length, &dst);
  if (status == 0)
    status = translate_span (caller, src, offset, length, source_bus,
                             &source_frames, &source_count);
  if (status == 0)
    status = translate_span (caller, dst, offset, length, destination_bus,
                             &destination_frames, &destination_count);
  if (status == 0)
    status = sysbus_dma_reserve (*source_bus, *destination_bus, length);
  while (source_count != 0)
    MEMPHY_unpin_frame (kernel->mram, source_frames[--source_count]);
  while (destination_count != 0)
    MEMPHY_unpin_frame (kernel->mram, destination_frames[--destination_count]);
  free (source_frames);
  free (destination_frames);
  return status;
}

int
free_pcb_memph (struct pcb_t *caller)
{
  int status = 0;

  if (caller == NULL || caller->krnl == NULL || caller->mm == NULL
      || caller->krnl->mram == NULL)
    return -EINVAL;
  pthread_mutex_lock (&mmvm_lock);
  if (dma_pid_busy (caller->pid))
    status = -EBUSY;
  else
    {
      for (int i = 0; i < PAGING_MAX_SYMTBL_SZ && status == 0; i++)
        if (caller->mm->symrgtbl[i].rg_end > caller->mm->symrgtbl[i].rg_start
            && !caller->mm->symrgtbl[i].mode_bit)
          status = __free (caller, 0, i);
      if (status == 0)
        status = mm64_release_pages (caller->mm, caller->krnl->mram);
      if (status == 0)
        {
          mm_unregister_process (caller);
          mm64_destroy (caller->mm);
        }
    }
  pthread_mutex_unlock (&mmvm_lock);
  return status;
}

static int
memory_call (struct pcb_t *caller, struct sc_regs *regs)
{
  if (caller == NULL || caller->krnl == NULL)
    return -EINVAL;
  return _syscall (caller->krnl, caller->pid, 17, regs);
}

int
liballoc (struct pcb_t *caller, addr_t size, uint32_t symbol)
{
  struct sc_regs regs = { .a1 = SYSMEM_ALLOC_OP, .a2 = symbol, .a3 = size };

  return memory_call (caller, &regs);
}

int
libfree (struct pcb_t *caller, uint32_t symbol)
{
  struct sc_regs regs = { .a1 = SYSMEM_FREE_OP, .a2 = symbol };

  return memory_call (caller, &regs);
}

int
libread (struct pcb_t *caller, uint32_t symbol, addr_t offset, uint32_t *value)
{
  struct sc_regs regs = { .a1 = SYSMEM_IO_READ, .a2 = symbol, .a3 = offset };
  int status;

  if (value == NULL)
    return -EINVAL;
  status = memory_call (caller, &regs);
  if (status == 0)
    *value = (uint32_t) regs.a4;
  return status;
}

int
libwrite (struct pcb_t *caller, BYTE value, uint32_t symbol, addr_t offset)
{
  struct sc_regs regs = { .a1 = SYSMEM_IO_WRITE,
                          .a2 = symbol,
                          .a3 = offset,
                          .a4 = (unsigned char) value };

  return memory_call (caller, &regs);
}

int
libkmem_malloc (struct pcb_t *caller, addr_t size, uint32_t symbol)
{
  struct sc_regs regs = { .a1 = SYSMEM_KALLOC_OP, .a2 = symbol, .a3 = size };

  return memory_call (caller, &regs);
}

int
libkmem_copy_from_user (struct pcb_t *caller, uint32_t source,
                        uint32_t destination, addr_t offset, addr_t size)
{
  struct sc_regs regs = { .a1 = SYSMEM_COPY_FROM_USER,
                          .a2 = source,
                          .a3 = destination,
                          .a4 = offset,
                          .a5 = size };

  return memory_call (caller, &regs);
}

int
libkmem_copy_to_user (struct pcb_t *caller, uint32_t source,
                      uint32_t destination, addr_t offset, addr_t size)
{
  struct sc_regs regs = { .a1 = SYSMEM_COPY_TO_USER,
                          .a2 = source,
                          .a3 = destination,
                          .a4 = offset,
                          .a5 = size };

  return memory_call (caller, &regs);
}

int
libmem_dma_memcpy (struct pcb_t *caller, arg_t source, arg_t destination,
                   arg_t offset, arg_t size)
{
  struct sc_regs regs
      = { .a1 = source, .a2 = destination, .a3 = offset, .a4 = size };

  if (caller == NULL || caller->krnl == NULL)
    return -EINVAL;
  return _syscall (caller->krnl, caller->pid, SYS_DMA_MEMCPY, &regs);
}
