/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * LamiaAtrium release. Educational use for course CO2018; see LICENSE.
 */

#include "mm64.h"
#include "sysbus.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(MM64)

#define TABLE_ENTRIES 512

int
mm64_canonical (addr_t address)
{
  uint64_t upper = address >> 57;

  return upper == ((address & (UINT64_C (1) << 56)) ? 127 : 0);
}

int
get_pd_from_address (addr_t address, addr_t *pgd, addr_t *p4d, addr_t *pud,
                     addr_t *pmd, addr_t *pt)
{
  if (!mm64_canonical (address) || pgd == NULL || p4d == NULL || pud == NULL
      || pmd == NULL || pt == NULL)
    return -EINVAL;
  *pgd = (address >> 48) & 511;
  *p4d = (address >> 39) & 511;
  *pud = (address >> 30) & 511;
  *pmd = (address >> 21) & 511;
  *pt = (address >> 12) & 511;
  return 0;
}

int
get_pd_from_pagenum (addr_t page, addr_t *pgd, addr_t *p4d, addr_t *pud,
                     addr_t *pmd, addr_t *pt)
{
  if (page > (UINT64_MAX >> 12))
    return -EINVAL;
  return get_pd_from_address (page << 12, pgd, p4d, pud, pmd, pt);
}

/* Non-leaf entries hold host pointers to simulator tables. Leaf entries use
   the assignment's PRESENT/SWAPPED/FPN format; only FPN reaches the RAM bus.
 */
static addr_t *
pte_slot (struct mm_struct *mm, addr_t page, int create)
{
  addr_t indexes[5];
  addr_t *table;

  if (mm == NULL || mm->pgd == NULL
      || get_pd_from_pagenum (page, &indexes[0], &indexes[1], &indexes[2],
                              &indexes[3], &indexes[4])
             != 0)
    return NULL;
  table = mm->pgd;
  for (int level = 0; level < 4; level++)
    {
      mm->walk_reads++;
      if (table[indexes[level]] == 0)
        {
          addr_t *child;

          if (!create)
            return NULL;
          child = calloc (TABLE_ENTRIES, sizeof *child);
          if (child == NULL)
            return NULL;
          table[indexes[level]] = (uintptr_t) child;
          mm->table_pages++;
        }
      table = (addr_t *) (uintptr_t) table[indexes[level]];
    }
  mm->walk_reads++;
  return &table[indexes[4]];
}

static int
table_empty (addr_t *table)
{
  for (int i = 0; i < TABLE_ENTRIES; i++)
    if (table[i] != 0)
      return 0;
  return 1;
}

/* Prune empty branches after rollback/unmapping. The shared kernel P4D stays
   alive until kernel shutdown, since every process PGD references it. */
static void
prune_page (struct mm_struct *mm, addr_t page)
{
  addr_t indexes[5];
  addr_t *tables[5];
  int last = 0;

  if (get_pd_from_pagenum (page, &indexes[0], &indexes[1], &indexes[2],
                           &indexes[3], &indexes[4])
      != 0)
    return;
  tables[0] = mm->pgd;
  for (int i = 0; i < 4 && tables[i][indexes[i]] != 0; i++)
    {
      tables[i + 1] = (addr_t *) (uintptr_t) tables[i][indexes[i]];
      last++;
    }
  for (int i = last; i > 0; i--)
    {
      if ((i == 1 && indexes[0] >= 256) || !table_empty (tables[i]))
        break;
      free (tables[i]);
      tables[i - 1][indexes[i - 1]] = 0;
      mm->table_pages--;
    }
}

addr_t
pte_get_entry (struct mm_struct *mm, addr_t page, struct pcb_t *caller)
{
  addr_t *entry = pte_slot (mm, page, 0);

  (void) caller;
  return entry == NULL ? 0 : *entry;
}

int
pte_set_entry (struct mm_struct *mm, addr_t page, uint64_t value,
               struct pcb_t *caller)
{
  addr_t *entry = pte_slot (mm, page, value != 0);

  (void) caller;
  if (entry == NULL)
    {
      if (mm != NULL && mm->pgd != NULL)
        prune_page (mm, page);
      return value == 0 ? 0 : -ENOMEM;
    }
  *entry = value;
  if (value == 0)
    prune_page (mm, page);
  return 0;
}

int
init_pte (addr_t *pte, int present, addr_t frame, int dirty, int swapped,
          int swap_type, addr_t swap_offset)
{
  if (pte == NULL || frame > PAGING_PTE_FPN_MASK || swap_type < 0
      || swap_type >= PAGING_MAX_MMSWP
      || swap_offset > (PAGING_PTE_SWPOFF_MASK >> PAGING_PTE_SWPOFF_LOBIT))
    return -EINVAL;
  *pte = 0;
  if (swapped)
    *pte = PAGING_PTE_SWAPPED_MASK | (addr_t) swap_type
           | (swap_offset << PAGING_PTE_SWPOFF_LOBIT);
  else if (present)
    *pte = PAGING_PTE_PRESENT_MASK | frame;
  if (dirty)
    *pte |= PAGING_PTE_DIRTY_MASK;
  return 0;
}

int
pte_set_fpn (struct mm_struct *mm, addr_t page, addr_t frame,
             struct pcb_t *caller)
{
  addr_t entry;
  int status = init_pte (&entry, 1, frame, 0, 0, 0, 0);

  return status == 0 ? pte_set_entry (mm, page, entry, caller) : status;
}

int
pte_set_swap (struct pcb_t *caller, addr_t page, int type, addr_t offset)
{
  addr_t entry;
  int status = init_pte (&entry, 0, 0, 0, 1, type, offset);

  if (caller == NULL)
    return -EINVAL;
  return status == 0 ? pte_set_entry (caller->mm, page, entry, caller)
                     : status;
}

int
mm64_translate (struct mm_struct *mm, struct memphy_struct *ram,
                addr_t address, addr_t *physical)
{
  addr_t entry, frame, offset = address & 4095;

  if (mm == NULL || ram == NULL || physical == NULL
      || !mm64_canonical (address))
    return -EFAULT;
  mm->translations++;
  entry = pte_get_entry (mm, address >> 12, NULL);
  if (!(entry & PAGING_PTE_PRESENT_MASK) || (entry & PAGING_PTE_SWAPPED_MASK))
    return -EFAULT;
  frame = PAGING_PTE_FPN (entry);
  if (ram->maxsz <= 0
      || frame * PAGING64_PAGESZ + offset >= (addr_t) ram->maxsz)
    return -EFAULT;
  *physical = frame * PAGING64_PAGESZ + offset;
  return 0;
}

/* Return every partially obtained frame on failure. RAM exhaustion is an
   error here; it must never start an unbounded eviction loop for pinned DMA.
 */
addr_t
alloc_pages_range (struct pcb_t *caller, int count,
                   struct framephy_struct **frames)
{
  struct framephy_struct **tail = frames;
  struct memphy_struct *ram = caller->krnl->mram;

  *frames = NULL;
  for (int i = 0; i < count; i++)
    {
      struct framephy_struct *node = calloc (1, sizeof *node);

      if (node == NULL || MEMPHY_get_freefp (ram, &node->fpn) != 0)
        {
          free (node);
          while (*frames != NULL)
            {
              node = *frames;
              *frames = node->fp_next;
              MEMPHY_put_freefp (ram, node->fpn);
              free (node);
            }
          return (addr_t) -ENOMEM;
        }
      *tail = node;
      tail = &node->fp_next;
    }
  return 0;
}

addr_t
vmap_page_range (struct pcb_t *caller, addr_t address, int count,
                 struct framephy_struct *frames, struct vm_rg_struct *region)
{
  struct mm_struct *mm
      = address >= MM64_KERNEL_BASE ? caller->krnl->mm : caller->mm;
  struct framephy_struct *frame = frames;
  int mapped = 0;

  for (; mapped < count && frame != NULL; mapped++, frame = frame->fp_next)
    {
      addr_t page = (address >> 12) + mapped;

      if (pte_get_entry (mm, page, caller) != 0
          || pte_set_fpn (mm, page, frame->fpn, caller) != 0)
        break;
    }
  if (mapped != count)
    {
      for (int i = 0; i < mapped; i++)
        pte_set_entry (mm, (address >> 12) + i, 0, caller);
      return (addr_t) -ENOMEM;
    }
  region->rg_start = address;
  region->rg_end = address + (addr_t) count * PAGING64_PAGESZ;
  return 0;
}

addr_t
vm_map_ram (struct pcb_t *caller, addr_t start, addr_t end, addr_t address,
            int count, struct vm_rg_struct *region)
{
  struct framephy_struct *frames, *frame;
  addr_t status;

  if (count <= 0 || address % PAGING64_PAGESZ != 0 || address < start
      || address > end || (addr_t) count > (end - address) / PAGING64_PAGESZ)
    return (addr_t) -EINVAL;
  status = alloc_pages_range (caller, count, &frames);
  if (status != 0)
    return status;
  status = vmap_page_range (caller, address, count, frames, region);
  while ((frame = frames) != NULL)
    {
      frames = frame->fp_next;
      if (status != 0)
        MEMPHY_put_freefp (caller->krnl->mram, frame->fpn);
      else
        memset (caller->krnl->mram->storage + frame->fpn * PAGING64_PAGESZ, 0,
                PAGING64_PAGESZ);
      free (frame);
    }
  return status;
}

int
mm64_map_contiguous (struct pcb_t *caller, addr_t address, addr_t size)
{
  struct memphy_struct *ram = caller->krnl->mram;
  struct mm_struct *mm = caller->krnl->mm;
  struct framephy_struct *first, *next;
  addr_t count = size / PAGING64_PAGESZ, run = 0, base = 0, mapped = 0;

  for (first = ram->free_fp_list; first != NULL; first = first->fp_next)
    {
      if (run == 0 || first->fpn != base + run)
        {
          base = first->fpn;
          run = 0;
        }
      run++;
      if (run == count)
        break;
    }
  if (run != count || count == 0)
    return -ENOMEM;
  /* Reserve all frames before publishing any of the mappings. */
  for (addr_t i = 0; i < count; i++)
    MEMPHY_takefp (ram, base + i);
  for (; mapped < count; mapped++)
    if (pte_set_fpn (mm, (address >> 12) + mapped, base + mapped, caller) != 0)
      break;
  if (mapped != count)
    {
      for (addr_t i = 0; i < mapped; i++)
        pte_set_entry (mm, (address >> 12) + i, 0, caller);
      for (addr_t i = 0; i < count; i++)
        MEMPHY_put_freefp (ram, base + i);
      return -ENOMEM;
    }
  memset (ram->storage + base * PAGING64_PAGESZ, 0, (size_t) size);
  /* Assign ownership to the shared kernel MM. */
  for (next = ram->used_fp_list; next != NULL; next = next->fp_next)
    if (next->fpn >= base && next->fpn - base < count)
      next->owner = mm;
  return 0;
}

static void
destroy_table (addr_t *table, int level, int limit)
{
  if (level > 0)
    for (int i = 0; i < limit; i++)
      if (table[i] != 0)
        destroy_table ((addr_t *) (uintptr_t) table[i], level - 1,
                       TABLE_ENTRIES);
  free (table);
}

void
mm64_destroy (struct mm_struct *mm)
{
  struct vm_area_struct *area;

  if (mm == NULL)
    return;
  if (mm->pgd != NULL)
    destroy_table (mm->pgd, 4, mm->kernel_space ? TABLE_ENTRIES : 256);
  while ((area = mm->mmap) != NULL)
    {
      struct vm_rg_struct *region;

      mm->mmap = area->vm_next;
      while ((region = area->vm_freerg_list) != NULL)
        {
          area->vm_freerg_list = region->rg_next;
          free (region);
        }
      free (area);
    }
  while (mm->fifo_pgn != NULL)
    {
      struct pgn_t *page = mm->fifo_pgn;

      mm->fifo_pgn = page->pg_next;
      free (page);
    }
  memset (mm, 0, sizeof *mm);
}

static int
release_table (addr_t *table, int level, int limit, struct memphy_struct *ram)
{
  for (int i = 0; i < limit; i++)
    if (table[i] != 0)
      {
        int status;

        if (level > 0)
          status = release_table ((addr_t *) (uintptr_t) table[i], level - 1,
                                  TABLE_ENTRIES, ram);
        else if (table[i] & PAGING_PTE_PRESENT_MASK)
          status = MEMPHY_put_freefp (ram, PAGING_PTE_FPN (table[i]));
        else
          return -ENOTSUP;
        if (status != 0)
          return status;
      }
  return 0;
}

int
mm64_release_pages (struct mm_struct *mm, struct memphy_struct *ram)
{
  return release_table (mm->pgd, 4, mm->kernel_space ? TABLE_ENTRIES : 256,
                        ram);
}

int
init_mm (struct mm_struct *mm, struct pcb_t *caller)
{
  const addr_t bases[] = { UINT64_C (0x100000000), UINT64_C (0x200000000),
                           UINT64_C (0x400000000) };
  struct vm_area_struct **tail;
  int kernel = caller == NULL;

  memset (mm, 0, sizeof *mm);
  mm->kernel_space = kernel;
  mm->pgd = calloc (TABLE_ENTRIES, sizeof *mm->pgd);
  if (mm->pgd == NULL)
    return -ENOMEM;
  mm->table_pages = 1;
  if (kernel)
    {
      addr_t *shared = calloc (TABLE_ENTRIES, sizeof *shared);

      if (shared == NULL)
        goto fail;
      mm->pgd[511] = (uintptr_t) shared;
      mm->table_pages++;
    }
  else if (caller->krnl != NULL && caller->krnl->mm != NULL)
    mm->pgd[511] = caller->krnl->mm->pgd[511];
  tail = &mm->mmap;
  for (int i = 0; i < (kernel ? 1 : 3); i++)
    {
      *tail = calloc (1, sizeof **tail);
      if (*tail == NULL)
        goto fail;
      (*tail)->vm_id = i;
      (*tail)->vm_start = kernel ? MM64_KERNEL_BASE : bases[i];
      (*tail)->vm_end = (*tail)->sbrk = (*tail)->vm_start;
      (*tail)->vm_mm = mm;
      tail = &(*tail)->vm_next;
    }
  return 0;
fail:
  mm64_destroy (mm);
  return -ENOMEM;
}

struct vm_rg_struct *
init_vm_rg (addr_t start, addr_t end)
{
  struct vm_rg_struct *region = calloc (1, sizeof *region);

  if (region != NULL)
    {
      region->rg_start = start;
      region->rg_end = end;
    }
  return region;
}

int
enlist_vm_rg_node (struct vm_rg_struct **list, struct vm_rg_struct *region)
{
  region->rg_next = *list;
  *list = region;
  return 0;
}

int
enlist_pgn_node (struct pgn_t **list, addr_t page)
{
  struct pgn_t *node = malloc (sizeof *node);

  if (node == NULL)
    return -ENOMEM;
  *node = (struct pgn_t) { .pgn = page, .pg_next = *list };
  *list = node;
  return 0;
}

int
print_pgtbl (struct pcb_t *caller, addr_t start, addr_t end)
{
  (void) start;
  (void) end;
  printf ("MMU PID %u: translations=%" PRIu64 " walk_reads=%" PRIu64
          " table_bytes=%" PRIu64 "\n",
          caller->pid, caller->mm->translations, caller->mm->walk_reads,
          caller->mm->table_pages * PAGING64_PAGESZ);
  return 0;
}

#endif
