#include "mm64.h"
#include "sysbus.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
  struct memphy_struct ram = {0};
  struct memphy_struct swap0 = {0};
  struct memphy_struct *swaps[PAGING_MAX_MMSWP] = {0};
  struct mm_struct kernel_mm = {0};
  struct mm_struct user_mm = {0};
  struct krnl_t kernel = {0};
  struct pcb_t process = {0};
  struct vm_area_struct *area;
  struct vm_rg_struct mapped = {0};
  addr_t paddr, idx[5];
  BYTE value;
  int status;

  mm64_reset_stats();
  assert(init_memphy(&ram, 3 * PAGING64_PAGESZ, 1) == 0);
  assert(init_memphy(&swap0, 16 * PAGING64_PAGESZ, 1) == 0);
  swaps[0] = &swap0;
  kernel.mm = &kernel_mm;
  kernel.mram = &ram;
  kernel.mswp = swaps;
  kernel.active_mswp = &swap0;
  kernel.active_mswp_id = 0;
  process.pid = 42;
  process.krnl = &kernel;
  assert(init_mm(&kernel_mm, NULL) == 0);
  assert(sysbus_bind_ram(&ram) == 0);
  assert(init_mm(&user_mm, &process) == 0);
  process.mm = &user_mm;
  assert(mm64_prepare_vma(&process, MM64_VMA_MEDIUM) == 0);
  area = get_vma_by_num(&user_mm, MM64_VMA_MEDIUM);
  assert(area != NULL);
  assert(area->vm_start != 0);
  assert(mm64_canonical(area->vm_start));
  assert(area->vm_start < MM64_USER_LIMIT);
  assert(get_pd_from_address(area->vm_start, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]) == 0);
  assert(vm_map_ram(&process, area->vm_start, area->vm_start + 3 * PAGING64_PAGESZ,
                    area->vm_start, 3, &mapped) == 0);
  for (int i = 0; i < 3; i++) {
    assert(mm64_translate(&user_mm, &ram, area->vm_start + (addr_t)i * PAGING64_PAGESZ,
                          &paddr) == 0);
    assert(MEMPHY_write(&ram, paddr, (BYTE)(70 + i)) == 0);
  }
  mapped.rg_start = 0;
  mapped.rg_end = 0;
  status = vm_map_ram(&process, area->vm_start,
                      area->vm_start + 4 * PAGING64_PAGESZ,
                      area->vm_start + 3 * PAGING64_PAGESZ, 1, &mapped);
  assert(status == 0);
  assert((pte_get_entry(&user_mm, (area->vm_start >> 12) + 2, &process)
          & PAGING_PTE_SWAPPED_MASK) != 0);
  assert(mm64_translate(&user_mm, &ram,
                        area->vm_start + 2 * PAGING64_PAGESZ, &paddr) == 0);
  assert(MEMPHY_read(&ram, paddr, &value) == 0);
  assert((unsigned char)value == 72);
  assert(mm64_translate(&user_mm, &ram, area->vm_start, &paddr) == 0);
  assert(MEMPHY_read(&ram, paddr, &value) == 0);
  assert((unsigned char)value == 70);
  assert(mm64_release_pages(&user_mm, &ram) == 0);
  mm64_destroy(&user_mm);
  mm64_destroy(&kernel_mm);
  mm64_print_system_stats(&kernel);
  assert(memory_bank_cleanup() == 0);
  destroy_memphy(&ram);
  destroy_memphy(&swap0);
  puts("MM64 member-3 smoke test passed");
  return 0;
}
