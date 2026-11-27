/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * Caitoa release. Educational use for course CO2018; see LICENSE.
 */

#include "mm64.h"
#include <errno.h>

struct vm_area_struct *
get_vma_by_num (struct mm_struct *mm, int id)
{
  struct vm_area_struct *area;

  for (area = mm == NULL ? NULL : mm->mmap; area != NULL; area = area->vm_next)
    if (area->vm_id == (unsigned long) id)
      return area;
  return NULL;
}

int
validate_overlap_vm_area (struct pcb_t *caller, int id, addr_t start,
                          addr_t end)
{
  struct vm_area_struct *area;

  if (start >= end || end > MM64_USER_LIMIT)
    return -EINVAL;
  for (area = caller->mm->mmap; area != NULL; area = area->vm_next)
    if (area->vm_id != (unsigned long) id
        && OVERLAP (start, end, area->vm_start, area->vm_end))
      return -EINVAL;
  return 0;
}

int
inc_vma_limit (struct pcb_t *caller, int id, addr_t increment)
{
  struct vm_area_struct *area = get_vma_by_num (caller->mm, id);
  struct vm_rg_struct mapped = { 0 };
  addr_t size, end;
  int status;

  if (area == NULL || increment == 0 || increment > MM64_USER_LIMIT - 4095)
    return -EINVAL;
  size = (increment + 4095) & ~UINT64_C (4095);
  if (size > MM64_USER_LIMIT - area->vm_end
      || size / PAGING64_PAGESZ > INT32_MAX)
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
