#include "mm64.h"
#include <errno.h>
#include <stdint.h>
#include <limits.h>

#define MM64_VMA_RESERVE UINT64_C(0x10000000000)
#define MM64_VMA_ALIGN UINT64_C(0x200000)
#define MM64_VMA_PROBE_LIMIT 4096

static uint64_t
mix_address_seed (uint64_t value)
{
  value += UINT64_C(0x9e3779b97f4a7c15);
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

struct vm_area_struct *
get_vma_by_num (struct mm_struct *mm, int id)
{
  struct vm_area_struct *area;

  if (mm == NULL || id < 0)
    return NULL;
  for (area = mm->mmap; area != NULL; area = area->vm_next)
    if (area->vm_id == (unsigned long) id)
      return area;
  return NULL;
}

int
validate_overlap_vm_area (struct pcb_t *caller, int id, addr_t start,
                          addr_t end)
{
  struct vm_area_struct *area;

  if (caller == NULL || caller->mm == NULL || id < 0 || start < MM64_USER_MIN
      || start >= end || end > MM64_USER_LIMIT
      || !mm64_canonical (start) || !mm64_canonical (end - 1))
    return -EINVAL;
  for (area = caller->mm->mmap; area != NULL; area = area->vm_next)
    if (area->vm_id != (unsigned long) id && area->vm_start != 0
        && area->vm_end > area->vm_start
        && OVERLAP (start, end, area->vm_start, area->vm_end))
      return -EEXIST;
  return 0;
}

int
mm64_prepare_vma (struct pcb_t *caller, int id)
{
  struct vm_area_struct *area;
  uint64_t range, seed, candidate, step;

  if (caller == NULL || caller->mm == NULL || id < MM64_VMA_SMALL
      || id > MM64_VMA_LARGE)
    return -EINVAL;
  area = get_vma_by_num (caller->mm, id);
  if (area == NULL)
    return -ENOENT;
  if (area->vm_start != 0)
    return 0;
  if (MM64_USER_LIMIT <= MM64_USER_MIN + MM64_VMA_RESERVE)
    return -ENOMEM;
  range = MM64_USER_LIMIT - MM64_USER_MIN - MM64_VMA_RESERVE;
  range &= ~(MM64_VMA_ALIGN - 1);
  seed = ((uint64_t) caller->pid << 32) ^ ((uint64_t) id << 17)
         ^ UINT64_C(0x6d61634f53564d);
  candidate = MM64_USER_MIN
              + (mix_address_seed (seed) % (range / MM64_VMA_ALIGN))
                    * MM64_VMA_ALIGN;
  step = (mix_address_seed (seed ^ UINT64_C(0xa5a5a5a5))
          % (range / MM64_VMA_ALIGN - 1) + 1)
         * MM64_VMA_ALIGN;
  for (unsigned int attempt = 0; attempt < MM64_VMA_PROBE_LIMIT; attempt++)
    {
      addr_t end = candidate + MM64_VMA_RESERVE;
      struct vm_area_struct *other;
      int available = 1;

      if (candidate < MM64_USER_MIN || end > MM64_USER_LIMIT
          || !mm64_canonical (candidate) || !mm64_canonical (end - 1))
        available = 0;
      for (other = caller->mm->mmap; available && other != NULL;
           other = other->vm_next)
        if (other != area && other->vm_start != 0)
          {
            addr_t other_end;

            if (other->vm_start > MM64_USER_LIMIT - MM64_VMA_RESERVE)
              other_end = MM64_USER_LIMIT;
            else
              other_end = other->vm_start + MM64_VMA_RESERVE;
            if (OVERLAP (candidate, end, other->vm_start, other_end))
              available = 0;
          }
      if (available)
        {
          area->vm_start = candidate;
          area->vm_end = candidate;
          area->sbrk = candidate;
          return 0;
        }
      candidate += step;
      if (candidate >= MM64_USER_LIMIT - MM64_VMA_RESERVE)
        candidate = MM64_USER_MIN
                    + (candidate % range & ~(MM64_VMA_ALIGN - 1));
    }
  return -ENOMEM;
}

int
inc_vma_limit (struct pcb_t *caller, int id, addr_t increment)
{
  struct vm_area_struct *area;
  struct vm_rg_struct mapped = { 0 };
  addr_t size, end;
  int status;

  if (caller == NULL || caller->mm == NULL || increment == 0
      || increment > UINT64_MAX - (PAGING64_PAGESZ - 1))
    return -EINVAL;
  status = mm64_prepare_vma (caller, id);
  if (status != 0)
    return status;
  area = get_vma_by_num (caller->mm, id);
  if (area == NULL || area->vm_end > MM64_USER_LIMIT)
    return -EINVAL;
  size = (increment + PAGING64_PAGESZ - 1)
         & ~(addr_t) (PAGING64_PAGESZ - 1);
  if (size > MM64_USER_LIMIT - area->vm_end
      || size / PAGING64_PAGESZ > INT_MAX)
    return -ENOMEM;
  end = area->vm_end + size;
  status = validate_overlap_vm_area (caller, id, area->vm_start, end);
  if (status == 0)
    status = (int) vm_map_ram (caller, area->vm_start, end, area->vm_end,
                               (int) (size / PAGING64_PAGESZ), &mapped);
  if (status == 0)
    area->vm_end = end;
  return status;
}

int
__mm_swap_page (struct pcb_t *caller, addr_t victim_fpn, addr_t swap_fpn)
{
  struct mm_struct *owner;
  struct memphy_struct *swap;
  addr_t virtual_address;
  int status;

  if (caller == NULL || caller->krnl == NULL || caller->mm == NULL
      || caller->krnl->mram == NULL || caller->krnl->mswp == NULL
      || caller->krnl->active_mswp == NULL
      || caller->krnl->active_mswp_id >= PAGING_MAX_MMSWP
      || caller->krnl->mswp[caller->krnl->active_mswp_id] == NULL
      || caller->krnl->active_mswp
             != caller->krnl->mswp[caller->krnl->active_mswp_id])
    return -EINVAL;
  swap = caller->krnl->active_mswp;
  status = MEMPHY_get_frame_info (caller->krnl->mram, victim_fpn, &owner,
                                  &virtual_address);
  if (status != 0 || owner != caller->mm)
    return -EACCES;
  status = MEMPHY_takefp (swap, swap_fpn);
  if (status != 0)
    return status;
  status = mm64_swap_out_frame (caller->krnl, victim_fpn,
                                (int) caller->krnl->active_mswp_id, swap_fpn);
  if (status != 0)
    MEMPHY_put_freefp (swap, swap_fpn);
  return status;
}
