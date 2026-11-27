#include "mm64.h"
#include "libmem.h"
#include "sysbus.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int
main (void)
{
  struct memphy_struct ram = { 0 }, swap = { 0 };
  struct memphy_struct *swaps[PAGING_MAX_MMSWP] = { 0 };
  struct mm_struct kernel_mm = { 0 }, user_mm = { 0 };
  struct krnl_t kernel = { 0 };
  struct pcb_t process = { 0 };
  addr_t source_address, destination_address;
  uint64_t source_bus, destination_bus;
  BYTE value = 0x5a;

  assert (init_memphy (&ram, 8 * PAGING64_PAGESZ, 1) == 0);
  assert (init_memphy (&swap, 32 * PAGING64_PAGESZ, 1) == 0);
  swaps[0] = &swap;
  kernel.mm = &kernel_mm;
  kernel.mram = &ram;
  kernel.mswp = swaps;
  process.pid = 91;
  process.krnl = &kernel;
  assert (init_mm (&kernel_mm, NULL) == 0);
  assert (sysbus_bind_ram (&ram) == 0);
  assert (init_mm (&user_mm, &process) == 0);
  process.mm = &user_mm;
  assert (__alloc (&process, MM64_VMA_SMALL, 0, PAGING64_PAGESZ,
                   &source_address) == 0);
  assert (__alloc (&process, MM64_VMA_SMALL, 1, PAGING64_PAGESZ,
                   &destination_address) == 0);
  assert (__write (&process, 0, 0, 0, value) == 0);
  assert (__write (&process, 0, 1, 0, 0) == 0);
  assert (mm_register_process (&process) == 0);
  assert (mm_dma_map_buffers (&kernel, process.pid, 0, 1, 0,
                              PAGING64_PAGESZ, &source_bus,
                              &destination_bus) == 0);
  assert (sysbus_range_pinned (&ram, source_bus, PAGING64_PAGESZ));
  assert (sysbus_range_pinned (&ram, destination_bus, PAGING64_PAGESZ));
  assert (MEMPHY_write (&ram, source_bus, 1) == -EBUSY);
  assert (MEMPHY_write (&ram, destination_bus, 1) == -EBUSY);
  assert (MEMPHY_read (&ram, destination_bus, &value) == -EBUSY);
  sysbus_dma_release ();
  assert (MEMPHY_write (&ram, source_bus, 1) == 0);
  assert (mm_unregister_process (&process) == 0);
  assert (mm64_release_pages (&user_mm, &ram) == 0);
  mm64_destroy (&user_mm);
  mm64_destroy (&kernel_mm);
  assert (memory_bank_cleanup () == 0);
  destroy_memphy (&ram);
  destroy_memphy (&swap);
  puts ("MM64 DMA buffer reservation test passed");
  return 0;
}
