#include "mm64.h"
#include "sysbus.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(MM64)

static uint64_t total_translations;
static uint64_t total_walk_reads;
static uint64_t total_swap_outs;
static uint64_t total_swap_ins;
static uint64_t total_table_bytes_allocated;
static uint64_t current_table_bytes;
static uint64_t total_table_bytes_by_level[5];
static uint64_t current_table_bytes_by_level[5];
static uint64_t total_walk_reads_by_level[5];
static uint64_t total_page_table_writes;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

static void
account_table_allocation (struct mm_struct *mm, uint64_t bytes, int level)
{
  if (level < 0 || level >= 5)
    return;
  mm->table_pages++;
  mm->table_bytes_current += bytes;
  mm->table_bytes_allocated += bytes;
  mm->table_bytes_by_level[level] += bytes;
  pthread_mutex_lock (&stats_lock);
  current_table_bytes += bytes;
  total_table_bytes_allocated += bytes;
  current_table_bytes_by_level[level] += bytes;
  total_table_bytes_by_level[level] += bytes;
  pthread_mutex_unlock (&stats_lock);
}

static void
account_table_release (struct mm_struct *mm, uint64_t bytes, int level)
{
  if (level < 0 || level >= 5)
    return;
  if (mm->table_pages != 0)
    mm->table_pages--;
  if (mm->table_bytes_current >= bytes)
    mm->table_bytes_current -= bytes;
  else
    mm->table_bytes_current = 0;
  if (mm->table_bytes_by_level[level] >= bytes)
    mm->table_bytes_by_level[level] -= bytes;
  else
    mm->table_bytes_by_level[level] = 0;
  pthread_mutex_lock (&stats_lock);
  if (current_table_bytes >= bytes)
    current_table_bytes -= bytes;
  else
    current_table_bytes = 0;
  if (current_table_bytes_by_level[level] >= bytes)
    current_table_bytes_by_level[level] -= bytes;
  else
    current_table_bytes_by_level[level] = 0;
  pthread_mutex_unlock (&stats_lock);
}

static void
account_walk_read (struct mm_struct *mm, int level)
{
  mm->walk_reads++;
  pthread_mutex_lock (&stats_lock);
  total_walk_reads++;
  total_walk_reads_by_level[level]++;
  pthread_mutex_unlock (&stats_lock);
}

int
mm64_canonical (addr_t address)
{
  uint64_t upper = address >> 57;
  uint64_t expected = (address & (UINT64_C (1) << 56)) != 0 ? 127 : 0;

  return upper == expected;
}

int
get_pd_from_address (addr_t address, addr_t *pgd, addr_t *p4d, addr_t *pud,
                     addr_t *pmd, addr_t *pt)
{
  if (!mm64_canonical (address) || pgd == NULL || p4d == NULL || pud == NULL
      || pmd == NULL || pt == NULL)
    return -EINVAL;
  *pgd = PAGING64_ADDR_PGD (address);
  *p4d = PAGING64_ADDR_P4D (address);
  *pud = PAGING64_ADDR_PUD (address);
  *pmd = PAGING64_ADDR_PMD (address);
  *pt = PAGING64_ADDR_PT (address);
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

static uint32_t *
pte_slot (struct mm_struct *mm, addr_t page, int create)
{
  addr_t indexes[5];
  addr_t *directory;

  if (mm == NULL || mm->pgd == NULL
      || get_pd_from_pagenum (page, &indexes[0], &indexes[1], &indexes[2],
                              &indexes[3], &indexes[4])
             != 0)
    return NULL;
  directory = mm->pgd;
  for (int level = 0; level < 4; level++)
    {
      addr_t child_address;

      account_walk_read (mm, level);
      child_address = directory[indexes[level]];
      if (child_address == 0)
        {
          void *child;
          size_t entry_size = level == 3 ? sizeof (uint32_t) : sizeof (addr_t);
          uint64_t table_bytes = level == 3 ? PAGING64_PT_BYTES
                                            : PAGING64_DIR_BYTES;

          if (!create)
            return NULL;
          child = calloc (PAGING64_TABLE_ENTRIES, entry_size);
          if (child == NULL)
            return NULL;
          directory[indexes[level]] = (addr_t) (uintptr_t) child;
          account_table_allocation (mm, table_bytes, level + 1);
          child_address = (addr_t) (uintptr_t) child;
        }
      if (level == 3)
        {
          uint32_t *page_table = (uint32_t *) (uintptr_t) child_address;

          account_walk_read (mm, 4);
          return &page_table[indexes[4]];
        }
      directory = (addr_t *) (uintptr_t) child_address;
    }
  return NULL;
}

static int
page_directory_empty (addr_t *directory)
{
  for (size_t i = 0; i < PAGING64_TABLE_ENTRIES; i++)
    if (directory[i] != 0)
      return 0;
  return 1;
}

static int
page_table_empty (uint32_t *page_table)
{
  for (size_t i = 0; i < PAGING64_TABLE_ENTRIES; i++)
    if (page_table[i] != 0)
      return 0;
  return 1;
}

static void
prune_page (struct mm_struct *mm, addr_t page)
{
  addr_t indexes[5];
  addr_t *directories[4];
  void *children[4] = { NULL, NULL, NULL, NULL };

  if (mm == NULL || mm->pgd == NULL
      || get_pd_from_pagenum (page, &indexes[0], &indexes[1], &indexes[2],
                              &indexes[3], &indexes[4])
             != 0)
    return;
  directories[0] = mm->pgd;
  for (int level = 0; level < 4; level++)
    {
      addr_t child = directories[level][indexes[level]];

      if (child == 0)
        return;
      children[level] = (void *) (uintptr_t) child;
      if (level < 3)
        directories[level + 1] = (addr_t *) children[level];
    }
  for (int level = 3; level >= 0; level--)
    {
      int empty = level == 3
                      ? page_table_empty ((uint32_t *) children[level])
                      : page_directory_empty ((addr_t *) children[level]);
      uint64_t bytes = level == 3 ? PAGING64_PT_BYTES : PAGING64_DIR_BYTES;

      if (!empty || (level == 0 && indexes[0] == MM64_KERNEL_PGD_INDEX && !mm->kernel_space))
        break;
      free (children[level]);
      directories[level][indexes[level]] = 0;
      account_table_release (mm, bytes, level + 1);
    }
}

addr_t
pte_get_entry (struct mm_struct *mm, addr_t page, struct pcb_t *caller)
{
  uint32_t *entry;
  addr_t value;

  (void) caller;
  if (mm == NULL)
    return 0;
  if (mm->page_table_lock_initialized)
    pthread_mutex_lock (&mm->page_table_lock);
  entry = pte_slot (mm, page, 0);
  value = entry == NULL ? 0 : (addr_t) *entry;
  if (mm->page_table_lock_initialized)
    pthread_mutex_unlock (&mm->page_table_lock);
  return value;
}

int
pte_set_entry (struct mm_struct *mm, addr_t page, uint64_t value,
               struct pcb_t *caller)
{
  uint32_t *entry;
  int status = 0;

  (void) caller;
  if (mm == NULL || value > UINT32_MAX)
    return -EINVAL;
  if (mm->page_table_lock_initialized)
    pthread_mutex_lock (&mm->page_table_lock);
  entry = pte_slot (mm, page, value != 0);
  if (entry == NULL)
    status = value == 0 ? 0 : -ENOMEM;
  else
    {
      *entry = (uint32_t) value;
      pthread_mutex_lock (&stats_lock);
      total_page_table_writes++;
      pthread_mutex_unlock (&stats_lock);
      if (value == 0)
        prune_page (mm, page);
    }
  if (mm->page_table_lock_initialized)
    pthread_mutex_unlock (&mm->page_table_lock);
  return status;
}

int
init_pte (addr_t *pte, int present, addr_t frame, int dirty, int swapped,
          int swap_type, addr_t swap_offset)
{
  uint32_t value = 0;

  if (pte == NULL || (present && swapped) || frame > PAGING_PTE_FPN_MASK
      || swap_type < 0 || swap_type > 31
      || swap_offset > (PAGING_PTE_SWPOFF_MASK >> PAGING_PTE_SWPOFF_LOBIT))
    return -EINVAL;
  if (present)
    value |= (uint32_t) PAGING_PTE_PRESENT_MASK | (uint32_t) frame;
  if (swapped)
    value |= (uint32_t) PAGING_PTE_SWAPPED_MASK | (uint32_t) swap_type
             | (uint32_t) (swap_offset << PAGING_PTE_SWPOFF_LOBIT);
  if (dirty)
    value |= (uint32_t) PAGING_PTE_DIRTY_MASK;
  *pte = value;
  return 0;
}

int
pte_set_fpn (struct mm_struct *mm, addr_t page, addr_t frame,
             struct pcb_t *caller)
{
  addr_t entry;
  uint32_t user_number;
  int status = init_pte (&entry, 1, frame, 0, 0, 0, 0);

  if (status != 0)
    return status;
  user_number = mm->kernel_space
                    ? 0
                    : caller != NULL ? caller->pid : mm->owner_pid;
  entry |= (addr_t) (user_number & 0x1fffU) << PAGING_PTE_USRNUM_LOBIT;
  return pte_set_entry (mm, page, entry, caller);
}

int
pte_set_swap (struct pcb_t *caller, addr_t page, int type, addr_t offset)
{
  addr_t entry;
  int status;

  if (caller == NULL || caller->mm == NULL)
    return -EINVAL;
  status = init_pte (&entry, 0, 0, 0, 1, type, offset);
  return status == 0 ? pte_set_entry (caller->mm, page, entry, caller)
                     : status;
}

static int
swap_in_page (struct mm_struct *mm, struct memphy_struct *ram, addr_t address)
{
  struct krnl_t *kernel;
  struct memphy_struct *swap;
  addr_t page = address >> 12;
  addr_t entry = pte_get_entry (mm, page, NULL);
  addr_t swap_type, swap_offset, frame;
  int status;

  if (!(entry & PAGING_PTE_SWAPPED_MASK) || mm == NULL || ram == NULL
      || mm->kernel == NULL)
    return -EFAULT;
  kernel = mm->kernel;
  swap_type = entry & PAGING_PTE_SWPTYP_MASK;
  swap_offset = PAGING_PTE_SWP (entry);
  if (swap_type >= PAGING_MAX_MMSWP || kernel->mswp == NULL
      || kernel->mswp[swap_type] == NULL)
    return -ENODEV;
  swap = kernel->mswp[swap_type];
  if (swap_offset >= (addr_t) swap->maxsz / PAGING64_PAGESZ)
    return -EFAULT;

  status = MEMPHY_get_freefp (ram, &frame);
  if (status != 0)
    {
      status = mm64_swap_out_one (kernel);
      if (status != 0)
        return -ENOMEM;
      status = MEMPHY_get_freefp (ram, &frame);
    }
  if (status != 0)
    return status;

  status = __swap_cp_page (swap, swap_offset, ram, frame);
  if (status != 0)
    {
      MEMPHY_put_freefp (ram, frame);
      return status;
    }
  status = pte_set_fpn (mm, page, frame, NULL);
  if (status == 0 && (entry & PAGING_PTE_DIRTY_MASK) != 0)
    {
      addr_t present_entry = pte_get_entry (mm, page, NULL);

      status = pte_set_entry (mm, page,
                              present_entry | PAGING_PTE_DIRTY_MASK, NULL);
    }
  if (status == 0)
    status = MEMPHY_set_owner (ram, frame, mm, page << 12);
  if (status == 0)
    status = MEMPHY_put_freefp (swap, swap_offset);
  if (status != 0)
    {
      pte_set_entry (mm, page, entry, NULL);
      MEMPHY_put_freefp (ram, frame);
      return status;
    }
  pthread_mutex_lock (&stats_lock);
  mm->swap_ins++;
  total_swap_ins++;
  pthread_mutex_unlock (&stats_lock);
  return 0;
}

int
mm64_translate (struct mm_struct *mm, struct memphy_struct *ram,
                addr_t address, addr_t *physical)
{
  addr_t entry, frame, offset = PAGING64_ADDR_OFFST (address);
  int status;

  if (mm == NULL || ram == NULL || physical == NULL)
    return -EINVAL;
  if (!mm64_canonical (address))
    return -EFAULT;
  if ((!mm->kernel_space && address >= MM64_USER_LIMIT)
      || (mm->kernel_space && address < MM64_KERNEL_BASE))
    return -EACCES;
  pthread_mutex_lock (&stats_lock);
  mm->translations++;
  total_translations++;
  pthread_mutex_unlock (&stats_lock);
  entry = pte_get_entry (mm, address >> 12, NULL);
  if (entry & PAGING_PTE_SWAPPED_MASK)
    {
      status = swap_in_page (mm, ram, address);
      if (status != 0)
        return status;
      entry = pte_get_entry (mm, address >> 12, NULL);
    }
  if (!(entry & PAGING_PTE_PRESENT_MASK)
      || (entry & PAGING_PTE_SWAPPED_MASK))
    return -EFAULT;
  frame = PAGING_PTE_FPN (entry);
  if (ram->maxsz <= 0
      || frame > UINT64_MAX / PAGING64_PAGESZ
      || frame * PAGING64_PAGESZ + offset >= (addr_t) ram->maxsz)
    return -EFAULT;
  *physical = frame * PAGING64_PAGESZ + offset;
  return 0;
}

int
mm64_translate_pin (struct mm_struct *mm, struct memphy_struct *ram,
                    addr_t address, addr_t *physical, addr_t *pinned_frame)
{
  int status = -EAGAIN;

  if (physical == NULL || pinned_frame == NULL)
    return -EINVAL;
  for (int attempt = 0; attempt < 16; attempt++)
    {
      addr_t frame, entry;

      status = mm64_translate (mm, ram, address, physical);
      if (status != 0)
        return status;
      frame = *physical / PAGING64_PAGESZ;
      status = MEMPHY_pin_frame (ram, frame);
      if (status != 0)
        {
          if (status == -EBUSY || status == -ENOENT)
            continue;
          return status;
        }
      entry = pte_get_entry (mm, address >> 12, NULL);
      if ((entry & PAGING_PTE_PRESENT_MASK) != 0
          && (entry & PAGING_PTE_SWAPPED_MASK) == 0
          && PAGING_PTE_FPN (entry) == frame)
        {
          *pinned_frame = frame;
          return 0;
        }
      MEMPHY_unpin_frame (ram, frame);
      status = -EAGAIN;
    }
  return status;
}

static int
zero_frame (struct memphy_struct *ram, addr_t frame)
{
  addr_t base;

  if (ram == NULL || frame > UINT64_MAX / PAGING64_PAGESZ)
    return -EINVAL;
  base = frame * PAGING64_PAGESZ;
  if (base > (addr_t) ram->maxsz
      || PAGING64_PAGESZ > (addr_t) ram->maxsz - base)
    return -EFAULT;
  for (addr_t offset = 0; offset < PAGING64_PAGESZ; offset++)
    {
      int status = MEMPHY_write (ram, base + offset, 0);

      if (status != 0)
        return status;
    }
  return 0;
}

addr_t
alloc_pages_range (struct pcb_t *caller, int count,
                   struct framephy_struct **frames)
{
  struct framephy_struct **tail;
  struct memphy_struct *ram;

  if (caller == NULL || caller->krnl == NULL || caller->krnl->mram == NULL
      || frames == NULL || count <= 0)
    return (addr_t) -EINVAL;
  ram = caller->krnl->mram;
  *frames = NULL;
  tail = frames;
  for (int i = 0; i < count; i++)
    {
      struct framephy_struct *node = calloc (1, sizeof *node);
      int status;

      if (node == NULL)
        goto rollback;
      status = MEMPHY_get_freefp (ram, &node->fpn);
      while (status != 0)
        {
          if (mm64_swap_out_one (caller->krnl) != 0)
            break;
          status = MEMPHY_get_freefp (ram, &node->fpn);
        }
      if (status != 0)
        {
          free (node);
          goto rollback;
        }
      *tail = node;
      tail = &node->fp_next;
    }
  return 0;

rollback:
  while (*frames != NULL)
    {
      struct framephy_struct *node = *frames;

      *frames = node->fp_next;
      MEMPHY_put_freefp (ram, node->fpn);
      free (node);
    }
  return (addr_t) -ENOMEM;
}

addr_t
vmap_page_range (struct pcb_t *caller, addr_t address, int count,
                 struct framephy_struct *frames, struct vm_rg_struct *region)
{
  struct mm_struct *mm;
  struct framephy_struct *frame = frames;
  int mapped = 0;

  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL
      || caller->mm == NULL || region == NULL || count <= 0
      || address % PAGING64_PAGESZ != 0)
    return (addr_t) -EINVAL;
  mm = address >= MM64_KERNEL_BASE ? caller->krnl->mm : caller->mm;
  for (; mapped < count && frame != NULL; mapped++, frame = frame->fp_next)
    {
      addr_t page = (address >> 12) + (addr_t) mapped;
      int status;

      if (pte_get_entry (mm, page, caller) != 0)
        break;
      status = pte_set_fpn (mm, page, frame->fpn, caller);
      if (status == 0)
        {
          status = MEMPHY_set_owner (caller->krnl->mram, frame->fpn, mm,
                                     page << 12);
          if (status != 0)
            pte_set_entry (mm, page, 0, caller);
        }
      if (status != 0)
        break;
    }
  if (mapped != count)
    {
      struct framephy_struct *rollback = frames;

      for (int i = 0; i < mapped; i++)
        {
          addr_t page = (address >> 12) + (addr_t) i;

          pte_set_entry (mm, page, 0, caller);
          if (rollback != NULL)
            {
              MEMPHY_set_owner (caller->krnl->mram, rollback->fpn, NULL, 0);
              rollback = rollback->fp_next;
            }
        }
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
  struct framephy_struct *frames = NULL;
  struct framephy_struct *frame;
  addr_t size, status;

  if (caller == NULL || count <= 0 || start > end
      || address % PAGING64_PAGESZ != 0 || address < start || address > end
      || (addr_t) count > (end - address) / PAGING64_PAGESZ
      || (addr_t) count > UINT64_MAX / PAGING64_PAGESZ)
    return (addr_t) -EINVAL;
  size = (addr_t) count * PAGING64_PAGESZ;
  status = alloc_pages_range (caller, count, &frames);
  if (status != 0)
    return status;
  for (frame = frames; frame != NULL; frame = frame->fp_next)
    {
      status = zero_frame (caller->krnl->mram, frame->fpn);
      if (status != 0)
        break;
    }
  if (status == 0)
    status = vmap_page_range (caller, address, count, frames, region);
  while ((frame = frames) != NULL)
    {
      frames = frame->fp_next;
      if (status != 0)
        MEMPHY_put_freefp (caller->krnl->mram, frame->fpn);
      free (frame);
    }
  if (status == 0)
    region->rg_end = address + size;
  return status;
}

int
mm64_map_contiguous (struct pcb_t *caller, addr_t address, addr_t size)
{
  struct memphy_struct *ram;
  struct mm_struct *mm;
  addr_t count, base = 0;
  int status;

  if (caller == NULL || caller->krnl == NULL || caller->krnl->mram == NULL
      || caller->krnl->mm == NULL || address < MM64_KERNEL_BASE
      || !mm64_canonical (address) || size == 0
      || size % PAGING64_PAGESZ != 0 || address % PAGING64_PAGESZ != 0
      || size > UINT64_MAX - address
      || !mm64_canonical (address + size - 1))
    return -EINVAL;
  ram = caller->krnl->mram;
  mm = caller->krnl->mm;
  count = size / PAGING64_PAGESZ;
  if (count == 0 || count > INT_MAX)
    return -EINVAL;
  status = MEMPHY_get_contiguousfp (ram, count, &base);
  while (status != 0 && mm64_swap_out_one (caller->krnl) == 0)
    status = MEMPHY_get_contiguousfp (ram, count, &base);
  if (status != 0)
    return status;

  for (addr_t i = 0; i < count; i++)
    {
      addr_t page = (address >> 12) + i;

      status = pte_get_entry (mm, page, caller) == 0
                   ? pte_set_fpn (mm, page, base + i, caller)
                   : -EEXIST;
      if (status == 0)
        status = MEMPHY_set_owner (ram, base + i, mm, page << 12);
      if (status != 0)
        break;
    }
  if (status == 0)
    for (addr_t i = 0; i < count; i++)
      {
        status = zero_frame (ram, base + i);
        if (status != 0)
          break;
      }
  if (status != 0)
    {
      for (addr_t i = 0; i < count; i++)
        {
          pte_set_entry (mm, (address >> 12) + i, 0, caller);
          MEMPHY_put_freefp (ram, base + i);
        }
      return status;
    }
  return 0;
}

static void
destroy_table (addr_t *table, int level, int limit)
{
  if (table == NULL)
    return;
  if (level > 1)
    {
      for (int i = 0; i < limit; i++)
        if (table[i] != 0)
          destroy_table ((addr_t *) (uintptr_t) table[i], level - 1,
                         PAGING64_TABLE_ENTRIES);
    }
  else if (level == 1)
    {
      for (int i = 0; i < limit; i++)
        if (table[i] != 0)
          free ((void *) (uintptr_t) table[i]);
    }
  free (table);
}

void
mm64_destroy (struct mm_struct *mm)
{
  struct vm_area_struct *area;

  if (mm == NULL)
    return;
  if (mm->pgd != NULL)
    destroy_table (mm->pgd, 4,
                   mm->kernel_space ? PAGING64_TABLE_ENTRIES : 256);
  pthread_mutex_lock (&stats_lock);
  if (current_table_bytes >= mm->table_bytes_current)
    current_table_bytes -= mm->table_bytes_current;
  else
    current_table_bytes = 0;
  for (int level = 0; level < 5; level++)
    {
      if (current_table_bytes_by_level[level] >= mm->table_bytes_by_level[level])
        current_table_bytes_by_level[level] -= mm->table_bytes_by_level[level];
      else
        current_table_bytes_by_level[level] = 0;
    }
  pthread_mutex_unlock (&stats_lock);
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
  if (mm->page_table_lock_initialized)
    pthread_mutex_destroy (&mm->page_table_lock);
  memset (mm, 0, sizeof *mm);
}

static int
release_table (addr_t *table, int level, int limit, struct mm_struct *mm,
               struct memphy_struct *ram)
{
  for (int i = 0; i < limit; i++)
    if (table[i] != 0)
      {
        int status = 0;

        if (level > 1)
          status = release_table ((addr_t *) (uintptr_t) table[i], level - 1,
                                  PAGING64_TABLE_ENTRIES, mm, ram);
        else if (level == 1)
          {
            uint32_t *page_table = (uint32_t *) (uintptr_t) table[i];

            for (int j = 0; j < PAGING64_TABLE_ENTRIES; j++)
              {
                uint32_t entry = page_table[j];

                if (entry & PAGING_PTE_PRESENT_MASK)
                  status = MEMPHY_put_freefp (ram, PAGING_PTE_FPN (entry));
                else if (entry & PAGING_PTE_SWAPPED_MASK)
                  {
                    addr_t type = entry & PAGING_PTE_SWPTYP_MASK;
                    addr_t offset = PAGING_PTE_SWP (entry);

                    if (mm->kernel == NULL || mm->kernel->mswp == NULL
                        || type >= PAGING_MAX_MMSWP
                        || mm->kernel->mswp[type] == NULL)
                      status = -ENODEV;
                    else
                      status = MEMPHY_put_freefp (mm->kernel->mswp[type],
                                                 offset);
                  }
                if (status != 0)
                  return status;
              }
          }
      }
  return 0;
}

int
mm64_release_pages (struct mm_struct *mm, struct memphy_struct *ram)
{
  if (mm == NULL || mm->pgd == NULL || ram == NULL)
    return -EINVAL;
  return release_table (mm->pgd, 4,
                        mm->kernel_space ? PAGING64_TABLE_ENTRIES : 256, mm,
                        ram);
}

static struct vm_area_struct *
create_area (unsigned long id, addr_t start, struct mm_struct *mm)
{
  struct vm_area_struct *area = calloc (1, sizeof *area);

  if (area == NULL)
    return NULL;
  area->vm_id = id;
  area->vm_start = start;
  area->vm_end = start;
  area->sbrk = start;
  area->vm_mm = mm;
  return area;
}

int
init_mm (struct mm_struct *mm, struct pcb_t *caller)
{
  struct vm_area_struct **tail;

  if (mm == NULL)
    return -EINVAL;
  memset (mm, 0, sizeof *mm);
  mm->kernel_space = caller == NULL;
  mm->kernel = caller == NULL ? NULL : caller->krnl;
  mm->owner_pid = caller == NULL ? 0 : caller->pid;
  int lock_status = pthread_mutex_init (&mm->page_table_lock, NULL);

  if (lock_status != 0)
    return -lock_status;
  mm->page_table_lock_initialized = 1;
  mm->pgd = calloc (PAGING64_TABLE_ENTRIES, sizeof *mm->pgd);
  if (mm->pgd == NULL)
    {
      mm64_destroy (mm);
      return -ENOMEM;
    }
  account_table_allocation (mm, PAGING64_DIR_BYTES, 0);

  if (caller == NULL)
    {
      mm->pgd[MM64_KERNEL_PGD_INDEX] = (addr_t) (uintptr_t)
          calloc (PAGING64_TABLE_ENTRIES, sizeof (addr_t));
      if (mm->pgd[MM64_KERNEL_PGD_INDEX] == 0)
        goto fail;
      account_table_allocation (mm, PAGING64_DIR_BYTES, 1);
    }
  else if (caller->krnl != NULL && caller->krnl->mm != NULL
           && caller->krnl->mm->pgd != NULL)
    mm->pgd[MM64_KERNEL_PGD_INDEX]
        = caller->krnl->mm->pgd[MM64_KERNEL_PGD_INDEX];

  tail = &mm->mmap;
  if (caller == NULL)
    {
      *tail = create_area (MM64_VMA_KERNEL, MM64_KERNEL_BASE, mm);
      if (*tail == NULL)
        goto fail;
    }
  else
    for (int id = MM64_VMA_SMALL; id <= MM64_VMA_LARGE; id++)
      {
        *tail = create_area ((unsigned long) id, 0, mm);
        if (*tail == NULL)
          goto fail;
        tail = &(*tail)->vm_next;
      }
  for (int i = 0; i < PAGING_MAX_SYMTBL_SZ; i++)
    mm->symrgtbl[i].vmaid = -1;
  return 0;

fail:
  mm64_destroy (mm);
  return -ENOMEM;
}

struct vm_rg_struct *
init_vm_rg (addr_t start, addr_t end)
{
  struct vm_rg_struct *region;

  if (start > end)
    return NULL;
  region = calloc (1, sizeof *region);
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
  if (list == NULL || region == NULL)
    return -EINVAL;
  region->rg_next = *list;
  *list = region;
  return 0;
}

int
enlist_pgn_node (struct pgn_t **list, addr_t page)
{
  struct pgn_t *node;

  if (list == NULL)
    return -EINVAL;
  node = malloc (sizeof *node);
  if (node == NULL)
    return -ENOMEM;
  node->pgn = page;
  node->pg_next = *list;
  *list = node;
  return 0;
}

static int
swap_out_frame_reserved (struct krnl_t *kernel, addr_t frame, int swap_type,
                     addr_t swap_offset)
{
  struct mm_struct *owner;
  struct memphy_struct *ram, *swap;
  addr_t virtual_address, page, old_entry, new_entry;
  int status;

  if (kernel == NULL || kernel->mram == NULL || kernel->mswp == NULL
      || swap_type < 0 || swap_type >= PAGING_MAX_MMSWP
      || kernel->mswp[swap_type] == NULL)
    return -EINVAL;
  ram = kernel->mram;
  swap = kernel->mswp[swap_type];
  if (swap_offset >= (addr_t) swap->maxsz / PAGING64_PAGESZ
      || sysbus_range_pinned (ram, frame * PAGING64_PAGESZ,
                              PAGING64_PAGESZ))
    return -EBUSY;
  status = MEMPHY_get_frame_info (ram, frame, &owner, &virtual_address);
  if (status != 0 || owner == NULL || owner->kernel_space
      || owner->kernel != kernel || !mm64_canonical (virtual_address))
    return -EINVAL;
  page = virtual_address >> 12;
  old_entry = pte_get_entry (owner, page, NULL);
  if (!(old_entry & PAGING_PTE_PRESENT_MASK)
      || (old_entry & PAGING_PTE_SWAPPED_MASK)
      || PAGING_PTE_FPN (old_entry) != frame)
    return -EFAULT;
  status = __swap_cp_page (ram, frame, swap, swap_offset);
  if (status != 0)
    return status;
  status = init_pte (&new_entry, 0, 0,
                     (old_entry & PAGING_PTE_DIRTY_MASK) != 0, 1, swap_type,
                     swap_offset);
  if (status == 0)
    status = pte_set_entry (owner, page, new_entry, NULL);
  if (status == 0)
    status = MEMPHY_put_freefp (ram, frame);
  if (status != 0)
    {
      pte_set_entry (owner, page, old_entry, NULL);
      return status;
    }
  pthread_mutex_lock (&stats_lock);
  owner->swap_outs++;
  total_swap_outs++;
  pthread_mutex_unlock (&stats_lock);
  return 0;
}


int
mm64_swap_out_frame (struct krnl_t *kernel, addr_t frame, int swap_type,
                     addr_t swap_offset)
{
  int status;

  if (kernel == NULL || kernel->mram == NULL)
    return -EINVAL;
  status = MEMPHY_reserve_victim (kernel->mram, frame);
  if (status != 0)
    return status;
  status = swap_out_frame_reserved (kernel, frame, swap_type, swap_offset);
  if (status != 0)
    MEMPHY_cancel_victim (kernel->mram, frame);
  return status;
}

int
mm64_swap_out_one (struct krnl_t *kernel)
{
  addr_t frame, virtual_address, swap_offset;
  struct mm_struct *owner;
  int status;

  if (kernel == NULL || kernel->mram == NULL || kernel->mswp == NULL)
    return -EINVAL;
  status = MEMPHY_find_victim (kernel->mram, &frame, &owner,
                               &virtual_address);
  if (status != 0)
    return status;
  (void) owner;
  (void) virtual_address;
  for (int type = 0; type < PAGING_MAX_MMSWP; type++)
    {
      struct memphy_struct *swap = kernel->mswp[type];

      if (swap == NULL || swap->maxsz < (int) PAGING64_PAGESZ)
        continue;
      status = MEMPHY_get_freefp (swap, &swap_offset);
      if (status != 0)
        continue;
      status = swap_out_frame_reserved (kernel, frame, type, swap_offset);
      if (status == 0)
        {
          kernel->active_mswp = swap;
          kernel->active_mswp_id = (uint32_t) type;
          return 0;
        }
      MEMPHY_put_freefp (swap, swap_offset);
      if (status != -EBUSY && status != -EFAULT)
        {
          MEMPHY_cancel_victim (kernel->mram, frame);
          return status;
        }
    }
  MEMPHY_cancel_victim (kernel->mram, frame);
  return -ENOSPC;
}

void
mm64_reset_stats (void)
{
  pthread_mutex_lock (&stats_lock);
  total_translations = 0;
  total_walk_reads = 0;
  total_swap_outs = 0;
  total_swap_ins = 0;
  total_table_bytes_allocated = 0;
  current_table_bytes = 0;
  total_page_table_writes = 0;
  for (int level = 0; level < 5; level++)
    {
      total_table_bytes_by_level[level] = 0;
      current_table_bytes_by_level[level] = 0;
      total_walk_reads_by_level[level] = 0;
    }
  pthread_mutex_unlock (&stats_lock);
  sysbus_reset_access_counts ();
}

void
mm64_print_system_stats (struct krnl_t *kernel)
{
  uint64_t cpu_reads = 0;
  uint64_t cpu_writes = 0;
  uint64_t dma_reads = 0;
  uint64_t dma_writes = 0;
  uint64_t translations, walk_reads, swap_outs, swap_ins;
  uint64_t table_bytes_allocated, table_bytes_current, page_table_writes;
  uint64_t table_bytes_by_level[5], live_table_bytes_by_level[5];
  uint64_t reads_by_level[5], total_accesses;

  pthread_mutex_lock (&stats_lock);
  translations = total_translations;
  walk_reads = total_walk_reads;
  swap_outs = total_swap_outs;
  swap_ins = total_swap_ins;
  table_bytes_allocated = total_table_bytes_allocated;
  table_bytes_current = current_table_bytes;
  page_table_writes = total_page_table_writes;
  for (int level = 0; level < 5; level++)
    {
      table_bytes_by_level[level] = total_table_bytes_by_level[level];
      live_table_bytes_by_level[level] = current_table_bytes_by_level[level];
      reads_by_level[level] = total_walk_reads_by_level[level];
    }
  pthread_mutex_unlock (&stats_lock);
  sysbus_get_access_counts (&cpu_reads, &cpu_writes, &dma_reads, &dma_writes);
  total_accesses = walk_reads + page_table_writes + cpu_reads + cpu_writes
                   + dma_reads + dma_writes;
  printf ("\n=== MEMORY STATISTICS ===\n");
  printf ("64-bit translations: %" PRIu64 "\n", translations);
  printf ("Page-table entry reads: %" PRIu64 "\n", walk_reads);
  printf ("Page-table entry writes: %" PRIu64 "\n", page_table_writes);
  for (int level = 0; level < 5; level++)
    printf ("Level %d entry reads: %" PRIu64 "\n", 5 - level,
            reads_by_level[level]);
  printf ("RAM/SWAP byte reads: %" PRIu64 "\n", cpu_reads);
  printf ("RAM/SWAP byte writes: %" PRIu64 "\n", cpu_writes);
  printf ("DMA bytes read: %" PRIu64 "\n", dma_reads);
  printf ("DMA bytes written: %" PRIu64 "\n", dma_writes);
  printf ("Total memory accesses: %" PRIu64 "\n", total_accesses);
  printf ("Page-table bytes allocated cumulatively: %" PRIu64 "\n",
          table_bytes_allocated);
  printf ("Page-table bytes still allocated: %" PRIu64 "\n",
          table_bytes_current);
  for (int level = 0; level < 5; level++)
    printf ("Level %d table bytes cumulative/current: %" PRIu64 "/%" PRIu64
            "\n", 5 - level, table_bytes_by_level[level],
            live_table_bytes_by_level[level]);
  printf ("Swap out pages: %" PRIu64 "\n", swap_outs);
  printf ("Swap in pages: %" PRIu64 "\n", swap_ins);
  if (kernel != NULL && kernel->mram != NULL)
    printf ("RAM capacity: %d bytes\n", kernel->mram->maxsz);
  printf ("=========================\n");
}

int
print_pgtbl (struct pcb_t *caller, addr_t start, addr_t end)
{
  struct vm_area_struct *area;

  if (caller == NULL || caller->mm == NULL)
    return -EINVAL;
  (void) start;
  (void) end;
  printf ("MMU PID %u: translations=%" PRIu64 " walk_reads=%" PRIu64
          " table_bytes_current=%" PRIu64 " table_bytes_allocated=%" PRIu64
          " swap_out=%" PRIu64 " swap_in=%" PRIu64 "\n",
          caller->pid, caller->mm->translations, caller->mm->walk_reads,
          caller->mm->table_bytes_current,
          caller->mm->table_bytes_allocated, caller->mm->swap_outs,
          caller->mm->swap_ins);
  for (area = caller->mm->mmap; area != NULL; area = area->vm_next)
    printf ("VMA[%lu] [0x%016" PRIx64 ",0x%016" PRIx64 ") sbrk=0x%016" PRIx64 "\n",
            area->vm_id, (uint64_t) area->vm_start, (uint64_t) area->vm_end,
            (uint64_t) area->sbrk);
  return 0;
}

#endif
