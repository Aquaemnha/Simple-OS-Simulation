#include "mm64.h"
#include "sysbus.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

#ifdef MM64

int
__swap_cp_page (struct memphy_struct *source, addr_t source_frame,
                struct memphy_struct *destination, addr_t destination_frame)
{
  BYTE page[PAGING64_PAGESZ];
  addr_t source_base, destination_base;

  if (source == NULL || destination == NULL || source->storage == NULL
      || destination->storage == NULL || source->maxsz <= 0
      || destination->maxsz <= 0
      || source_frame >= (addr_t) source->maxsz / PAGING64_PAGESZ
      || destination_frame
             >= (addr_t) destination->maxsz / PAGING64_PAGESZ)
    return -EINVAL;
  source_base = source_frame * PAGING64_PAGESZ;
  destination_base = destination_frame * PAGING64_PAGESZ;
  for (addr_t offset = 0; offset < PAGING64_PAGESZ; offset++)
    {
      int status = sysbus_cpu_read (source, source_base + offset,
                                    (unsigned char *) &page[offset]);

      if (status != 0)
        return status;
    }
  for (addr_t offset = 0; offset < PAGING64_PAGESZ; offset++)
    {
      int status = sysbus_cpu_write (destination, destination_base + offset,
                                     (unsigned char) page[offset]);

      if (status != 0)
        return status;
    }
  return 0;
}

int
vmap_pgd_memset (struct pcb_t *caller, addr_t address, int page_count)
{
  struct vm_rg_struct region = { 0 };
  addr_t bytes;

  if (caller == NULL || caller->mm == NULL || page_count <= 0
      || address < MM64_USER_MIN || address >= MM64_USER_LIMIT
      || !mm64_canonical (address)
      || (addr_t) page_count > UINT64_MAX / PAGING64_PAGESZ)
    return -EINVAL;
  bytes = (addr_t) page_count * PAGING64_PAGESZ;
  if (bytes > MM64_USER_LIMIT - address
      || !mm64_canonical (address + bytes - 1))
    return -EINVAL;
  return (int) vm_map_ram (caller, address, address + bytes, address,
                           page_count, &region);
}

addr_t
vm_map_kernel (struct pcb_t *caller, addr_t area_start, addr_t area_end,
               addr_t map_start, int page_count, struct vm_rg_struct *region)
{
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL
      || map_start < MM64_KERNEL_BASE || area_start < MM64_KERNEL_BASE
      || area_start > area_end || map_start < area_start || map_start > area_end
      || !mm64_canonical (map_start) || !mm64_canonical (area_end - 1))
    return (addr_t) -EINVAL;
  return vm_map_ram (caller, area_start, area_end, map_start, page_count,
                     region);
}

static int
find_present_page (addr_t *table, int level, addr_t prefix, addr_t *page)
{
  for (addr_t index = 0; index < PAGING64_TABLE_ENTRIES; index++)
    if (table[index] != 0)
      {
        addr_t next_prefix = prefix | (index << (12 + 9 * level));

        if (level == 1)
          {
            uint32_t *entries = (uint32_t *) (uintptr_t) table[index];

            for (addr_t leaf = 0; leaf < PAGING64_TABLE_ENTRIES; leaf++)
              if ((entries[leaf] & PAGING_PTE_PRESENT_MASK) != 0
                  && (entries[leaf] & PAGING_PTE_SWAPPED_MASK) == 0)
                {
                  *page = (next_prefix | (leaf << 12)) >> 12;
                  return 0;
                }
          }
        else if (find_present_page ((addr_t *) (uintptr_t) table[index],
                                    level - 1, next_prefix, page) == 0)
          return 0;
      }
  return -ENOENT;
}

int
find_victim_page (struct mm_struct *mm, addr_t *page)
{
  if (mm == NULL || mm->pgd == NULL || page == NULL)
    return -EINVAL;
  return find_present_page (mm->pgd, 4, 0, page);
}

int
print_list_fp (struct framephy_struct *frame)
{
  for (; frame != NULL; frame = frame->fp_next)
    printf ("%" PRIu64 " ", (uint64_t) frame->fpn);
  printf ("\n");
  return 0;
}

int
print_list_rg (struct vm_rg_struct *region)
{
  for (; region != NULL; region = region->rg_next)
    printf ("[0x%016" PRIx64 ",0x%016" PRIx64 ") ",
            (uint64_t) region->rg_start, (uint64_t) region->rg_end);
  printf ("\n");
  return 0;
}

int
print_list_vma (struct vm_area_struct *area)
{
  for (; area != NULL; area = area->vm_next)
    printf ("%lu:[0x%016" PRIx64 ",0x%016" PRIx64 ") ", area->vm_id,
            (uint64_t) area->vm_start, (uint64_t) area->vm_end);
  printf ("\n");
  return 0;
}

int
print_list_pgn (struct pgn_t *page)
{
  for (; page != NULL; page = page->pg_next)
    printf ("%" PRIu64 " ", (uint64_t) page->pgn);
  printf ("\n");
  return 0;
}

#else

int
__swap_cp_page (struct memphy_struct *source, addr_t source_frame,
                struct memphy_struct *destination, addr_t destination_frame)
{
  (void) source;
  (void) source_frame;
  (void) destination;
  (void) destination_frame;
  return -ENOTSUP;
}

#endif
