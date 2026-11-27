#include "mm64.h"
#include "libmem.h"
#include "sysbus.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void setup(struct memphy_struct *ram, struct memphy_struct *swap,
                  struct memphy_struct **swaps, struct mm_struct *kernel_mm,
                  struct mm_struct *user_mm, struct krnl_t *kernel,
                  struct pcb_t *process, addr_t ram_size)
{
  assert(init_memphy(ram, ram_size, 1) == 0);
  assert(init_memphy(swap, 64 * PAGING64_PAGESZ, 1) == 0);
  swaps[0] = swap;
  kernel->mm = kernel_mm;
  kernel->mram = ram;
  kernel->mswp = swaps;
  kernel->active_mswp = swap;
  kernel->active_mswp_id = 0;
  process->pid = 77;
  process->krnl = kernel;
  assert(init_mm(kernel_mm, NULL) == 0);
  assert(sysbus_bind_ram(ram) == 0);
  assert(init_mm(user_mm, process) == 0);
  process->mm = user_mm;
}

static void teardown(struct memphy_struct *ram, struct memphy_struct *swap,
                     struct mm_struct *user_mm, struct mm_struct *kernel_mm)
{
  assert(mm64_release_pages(user_mm, ram) == 0);
  mm64_destroy(user_mm);
  mm64_destroy(kernel_mm);
  assert(memory_bank_cleanup() == 0);
  destroy_memphy(ram);
  destroy_memphy(swap);
}

int main(void)
{
  struct memphy_struct ram = {0}, swap = {0};
  struct memphy_struct *swaps[PAGING_MAX_MMSWP] = {0};
  struct mm_struct kernel_mm = {0}, user_mm = {0};
  struct krnl_t kernel = {0};
  struct pcb_t process = {0};
  BYTE value;
  addr_t small, medium, large;

  mm64_reset_stats();
  setup(&ram, &swap, swaps, &kernel_mm, &user_mm, &kernel, &process,
        64 * PAGING64_PAGESZ);
  assert(__alloc(&process, MM64_VMA_SMALL, 0, 256, &small) == 0);
  assert(__alloc(&process, MM64_VMA_MEDIUM, 2, 8192, &medium) == 0);
  assert(__alloc(&process, MM64_VMA_LARGE, 4, 32768, &large) == 0);
  assert(small != medium && medium != large && small != large);
  assert(mm64_canonical(small) && mm64_canonical(medium) && mm64_canonical(large));
  assert(__write(&process, 0, 0, 0, 17) == 0);
  assert(__write(&process, 0, 2, 4096, 28) == 0);
  assert(__write(&process, 0, 4, 16384, 39) == 0);
  assert(__read(&process, 0, 0, 0, &value) == 0 && (unsigned char)value == 17);
  assert(__read(&process, 0, 2, 4096, &value) == 0 && (unsigned char)value == 28);
  assert(__read(&process, 0, 4, 16384, &value) == 0 && (unsigned char)value == 39);
  teardown(&ram, &swap, &user_mm, &kernel_mm);

  memset(&ram, 0, sizeof ram);
  memset(&swap, 0, sizeof swap);
  memset(&kernel_mm, 0, sizeof kernel_mm);
  memset(&user_mm, 0, sizeof user_mm);
  memset(&kernel, 0, sizeof kernel);
  memset(&process, 0, sizeof process);
  memset(swaps, 0, sizeof swaps);
  setup(&ram, &swap, swaps, &kernel_mm, &user_mm, &kernel, &process,
        3 * PAGING64_PAGESZ);
  assert(__alloc(&process, MM64_VMA_MEDIUM, 0, 12288, &medium) == 0);
  assert(__write(&process, 0, 0, 0, 51) == 0);
  assert(__write(&process, 0, 0, 4096, 52) == 0);
  assert(__write(&process, 0, 0, 8192, 53) == 0);
  assert(__alloc(&process, MM64_VMA_MEDIUM, 1, 4096, &large) == 0);
  assert(process.mm->swap_outs > 0);
  assert(__write(&process, 0, 1, 0, 64) == 0);
  assert(__read(&process, 0, 0, 0, &value) == 0 && (unsigned char)value == 51);
  assert(__read(&process, 0, 0, 4096, &value) == 0 && (unsigned char)value == 52);
  assert(__read(&process, 0, 0, 8192, &value) == 0 && (unsigned char)value == 53);
  assert(__read(&process, 0, 1, 0, &value) == 0 && (unsigned char)value == 64);
  assert(process.mm->swap_ins > 0);
  teardown(&ram, &swap, &user_mm, &kernel_mm);
  puts("MM64 multi-region allocation and constrained-RAM swap tests passed");
  return 0;
}
