/* CPU/syscall/MM64 integration against the supplied RTL model. */

#include "cpu.h"
#include "dma.h"
#include "libmem.h"
#include "mm64.h"
#include "sysbus.h"
#include "syscall.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COPY_SIZE UINT64_C (262144)

static unsigned int
free_frames (struct memphy_struct *ram)
{
  struct framephy_struct *frame;
  unsigned int count = 0;

  for (frame = ram->free_fp_list; frame != NULL; frame = frame->fp_next)
    count++;
  return count;
}

int
main (void)
{
  struct krnl_t kernel = { 0 };
  struct mm_struct kernel_mm, mm, other_mm;
  struct pcb_t process = { .pid = 41, .krnl = &kernel, .mm = &mm };
  struct pcb_t other = { .pid = 42, .krnl = &kernel, .mm = &other_mm };
  struct memphy_struct ram = { 0 };
  struct dma_result result;
  struct sc_regs regs = { .a1 = 1, .a2 = 2, .a4 = 4 };
  struct inst_t *instructions = calloc (65537, sizeof *instructions);
  struct code_seg_t code = { .text = instructions, .size = 65537 };
  struct timespec begin, now;
  uint64_t checksum = 0, iterations = 0;
  addr_t src, dst, page, saved, physical, indexes[5];
  uint32_t value;

  assert (instructions != NULL);
  assert (init_memphy (&ram, 4 * 1024 * 1024, 1) == 0);
  assert (init_mm (&kernel_mm, NULL) == 0);
  kernel.mm = &kernel_mm;
  kernel.mram = &ram;
  assert (init_mm (&mm, &process) == 0);
  assert (init_mm (&other_mm, &other) == 0);
  assert (mm_register_process (&process) == 0);
  assert (mm_register_process (&other) == 0);
  assert (mm_register_process (&process) == -EEXIST);
  process.code = &code;

  /* Actual five-level indices, canonical limits and private process tables. */
  assert (get_pd_from_address (UINT64_C (0x39ffe1d9c9000), &indexes[0],
                               &indexes[1], &indexes[2], &indexes[3],
                               &indexes[4])
          == 0);
  assert (indexes[0] == 3 && indexes[1] == 0x13f && indexes[2] == 0x1f8
          && indexes[3] == 0xec && indexes[4] == 0x1c9);
  assert (mm64_canonical (MM64_USER_LIMIT - 1));
  assert (!mm64_canonical (MM64_USER_LIMIT));
  assert (mm64_canonical (UINT64_C (0xff00000000000000)));
  assert (!mm64_canonical (UINT64_C (0xfeffffffffffffff)));
  assert (mm64_canonical (UINT64_MAX));
  assert (liballoc (&process, COPY_SIZE, 1) == 0);
  assert (liballoc (&process, COPY_SIZE, 2) == 0);
  assert (liballoc (&process, 4, 3) == 0);
  assert (liballoc (&other, 4, 1) == 0);
  assert (libwrite (&process, (BYTE) 0x81, 3, 0) == 0);
  assert (libwrite (&other, (BYTE) 0xa5, 1, 0) == 0);
  assert (libread (&process, 3, 0, &value) == 0 && value == 0x81);
  assert (libread (&other, 1, 0, &value) == 0 && value == 0xa5);
  assert (libread (&process, 3, 4, &value) == -EFAULT);
  assert (libread (&process, 30, 0, &value) == -EINVAL);
  assert (mm64_translate (&mm, &ram, MM64_USER_LIMIT, &physical) == -EFAULT);
  assert (get_pd_from_pagenum (UINT64_MAX, &indexes[0], &indexes[1],
                               &indexes[2], &indexes[3], &indexes[4])
          == -EINVAL);
  assert (pte_set_fpn (&mm, (MM64_USER_LIMIT - 4096) >> 12, 0, &process) == 0);
  assert (mm64_translate (&mm, &ram, MM64_USER_LIMIT - 1, &physical) == 0
          && physical == 4095);
  assert (pte_set_entry (&mm, (MM64_USER_LIMIT - 4096) >> 12, 0, &process)
          == 0);

  /* Kernel mappings are shared; user instructions still cannot access them. */
  assert (libkmem_malloc (&process, 8192, 4) == 0);
  assert (mm.pgd[511] == other_mm.pgd[511]
          && mm.pgd[511] == kernel_mm.pgd[511]);
  assert (libread (&process, 4, 0, &value) == -EACCES);
  assert (libwrite (&process, 7, 4, 0) == -EACCES);
  assert (libkmem_copy_from_user (&process, 3, 4, 0, 4) == 0);
  assert (libkmem_copy_to_user (&process, 4, 3, 0, 4) == 0);
  assert (libread (&process, 3, 0, &value) == 0 && value == 0x81);
  assert (libkmem_copy_from_user (&process, 4, 3, 0, 4) == -EACCES);
  assert (libkmem_copy_to_user (&process, 4, 3, UINT64_MAX, 4) == -EFAULT);
  assert (mm64_translate (&mm, &ram, process.regs[4], &physical) == 0);
  assert (mm64_translate (&other_mm, &ram, process.regs[4], &src) == 0
          && src == physical);
  assert (mm64_translate (&kernel_mm, &ram, process.regs[4] + 4096, &src) == 0
          && src == physical + 4096);

  /* RAM OOM rolls back frames, symbols and VMA break without looping. */
  unsigned int before = free_frames (&ram);
  uint64_t tables = mm.table_pages;
  addr_t old_break = get_vma_by_num (&mm, 2)->sbrk;
  assert (liballoc (&process, ram.maxsz, 5) == -ENOMEM);
  assert (free_frames (&ram) == before && mm.table_pages == tables);
  assert (get_vma_by_num (&mm, 2)->sbrk == old_break);
  assert (mm.symrgtbl[5].rg_start == mm.symrgtbl[5].rg_end);
  assert (liballoc (&process, 4, 5) == 0);
  addr_t reused = mm.symrgtbl[5].rg_start;
  assert (libfree (&process, 5) == 0);
  assert (libfree (&process, 5) == -EINVAL);
  assert (liballoc (&process, 4, 5) == 0 && mm.symrgtbl[5].rg_start == reused);
  assert (MEMPHY_put_freefp (&ram, 1000) == -EINVAL);

  assert (sysbus_bind_ram (&ram) == 0);
  assert (dma_start () == 0);
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 4) == -ENOSYS);
  kernel.dma_map_buffers = mm_dma_map_buffers;
  assert (_syscall (&kernel, 999, SYS_DMA_MEMCPY, &regs) == -ESRCH);
  assert (libmem_dma_memcpy (&process, 30, 2, 0, 4) == -EINVAL);
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 3) == -EINVAL);
  assert (libmem_dma_memcpy (&process, 1, 2, UINT64_MAX, 4) == -EFAULT);
  assert (libmem_dma_memcpy (&process, UINT64_C (1) << 32, 2, 0, 4)
          == -EINVAL);
  assert (libmem_dma_memcpy (&process, 1, 2, COPY_SIZE - 1, 4) == -EFAULT);
  assert (libmem_dma_memcpy (&other, 1, 2, 0, 4) == -EFAULT);

  /* Do not linearize scattered, swapped or unmapped pages as if they were RAM.
   */
  page = (mm.symrgtbl[1].rg_start >> 12) + 1;
  saved = pte_get_entry (&mm, page, &process);
  assert (pte_set_fpn (&mm, page, 900, &process) == 0);
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 8192) == -ENOTSUP);
  assert (pte_set_entry (&mm, page, PAGING_PTE_SWAPPED_MASK, &process) == 0);
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 8192) == -EFAULT);
  assert (pte_set_entry (&mm, page, 0, &process) == 0);
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 8192) == -EFAULT);
  assert (pte_set_entry (&mm, page, saved, &process) == 0);
  assert (!dma_is_busy ());

  assert (
      mm_dma_map_buffers (&kernel, process.pid, 1, 2, 0, COPY_SIZE, &src, &dst)
      == 0);
  assert (src != process.regs[1] && dst != process.regs[2]);
  for (addr_t i = 0; i < COPY_SIZE; i++)
    assert (libwrite (&process, (BYTE) (i * 37 + 0x81), 1, i) == 0);

  /* Deterministic pinning checks, independent of how quickly RTL finishes. */
  assert (sysbus_dma_reserve (src, dst, COPY_SIZE) == 0);
  assert (libfree (&process, 1) == -EBUSY);
  assert (liballoc (&process, 4, 6) == 0);
  instructions[0] = (struct inst_t) { .opcode = READ, .arg_0 = 2, .arg_2 = 9 };
  assert (run (&process) == -EBUSY && process.pc == 0);
  assert (MEMPHY_put_freefp (&ram, src / 4096) == -EBUSY);
  sysbus_dma_release ();
  instructions[0] = (struct inst_t) { .opcode = DMA_MEMCPY,
                                      .arg_0 = 1,
                                      .arg_1 = 2,
                                      .arg_3 = (UINT64_C (1) << 32) + 4 };
  assert (run (&process) == -EINVAL && process.pc == 0);
  instructions[0].arg_3 = COPY_SIZE;
  for (unsigned int i = 1; i < code.size; i++)
    instructions[i].opcode = CALC;
  assert (run (&process) == 0 && process.pc == 1);
  assert (free_pcb_memph (&process) == -EBUSY);

  clock_gettime (CLOCK_MONOTONIC, &begin);
  do
    {
      if (process.pc < code.size)
        assert (run (&process) == 0);
      assert (libwrite (&process, (BYTE) iterations, 3, 0) == 0);
      assert (libread (&process, 3, 0, &value) == 0);
      assert (value == (unsigned char) iterations);
      checksum += value;
      iterations++;
      sched_yield ();
      clock_gettime (CLOCK_MONOTONIC, &now);
      assert (now.tv_sec - begin.tv_sec < 30);
    }
  while (dma_take_completion (&result) == 0);

  assert (result.status == 0 && result.interrupts == 1);
  assert (result.source == src && result.destination == dst);
  assert (result.bytes_transferred == COPY_SIZE
          && result.cpu_instructions > 0);
  assert (dma_take_completion (&result) == 0);
  for (addr_t i = 0; i < COPY_SIZE; i++)
    {
      assert (libread (&process, 2, i, &value) == 0);
      assert (value == (unsigned char) (i * 37 + 0x81));
      assert ((unsigned char) ram.storage[src + i] == value);
    }
  process.pc = 0;
  instructions[0] = (struct inst_t) { .opcode = READ, .arg_0 = 2, .arg_2 = 9 };
  assert (run (&process) == 0 && process.regs[9] == 0x81);
  dma_shutdown ();
  assert (memory_bank_cleanup () == 0);
  print_pgtbl (&process, 0, 0);
  printf ("PASS: CPU -> PID syscall -> five-level MMU -> RTL -> RAM, "
          "bytes=%u callbacks=%u CPU instructions during DMA=%" PRIu64 "\n",
          result.bytes_transferred, result.interrupts,
          result.cpu_instructions);
  printf ("Independent MMU read/write iterations=%" PRIu64 " checksum=%" PRIu64
          "\n",
          iterations, checksum);
  assert (free_pcb_memph (&process) == 0);
  assert (free_pcb_memph (&other) == 0);
  assert (kernel.processes == NULL);
  assert (free_frames (&ram) == (unsigned int) ram.maxsz / 4096);
  mm64_destroy (&kernel_mm);
  destroy_memphy (&ram);
  free (instructions);
  return 0;
}
