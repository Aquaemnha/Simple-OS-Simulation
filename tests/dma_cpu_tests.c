/* Exercise the real CPU/libmem/syscall/DMA path. The page table below is a
   test fixture, not a replacement for the group's unfinished MM64 module. */

#include "cpu.h"
#include "dma.h"
#include "libmem.h"
#include "mm.h"
#include "sysbus.h"
#include "syscall.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_PAGE_SIZE UINT64_C (4096)
#define TEST_LENGTH UINT64_C (262144)

struct page_mapping
{
  uint64_t virtual_page;
  uint64_t frame;
  int writable;
};

static struct pcb_t process;
static struct page_mapping pages[128];

/* Reject holes, writes to read-only pages and physically scattered spans. */
static int
translate_span (uint64_t address, uint64_t length, int write, uint64_t *bus)
{
  uint64_t consumed = 0;

  while (consumed < length)
    {
      uint64_t current = address + consumed;
      uint64_t offset = current % TEST_PAGE_SIZE;
      uint64_t chunk = TEST_PAGE_SIZE - offset;
      size_t i;

      for (i = 0; i < sizeof pages / sizeof *pages; i++)
        if (pages[i].virtual_page == current / TEST_PAGE_SIZE)
          break;
      if (i == sizeof pages / sizeof *pages || (write && !pages[i].writable))
        return -EFAULT;
      uint64_t physical = pages[i].frame * TEST_PAGE_SIZE + offset;
      if (consumed == 0)
        *bus = physical;
      else if (physical != *bus + consumed)
        return -ENOTSUP;
      if (chunk > length - consumed)
        chunk = length - consumed;
      consumed += chunk;
    }
  return 0;
}

/* Test-owned kernel registry: look up by PID before touching its MM state. */
static int
map_buffers (struct krnl_t *krnl, uint32_t pid, uint32_t source_region,
             uint32_t destination_region, uint64_t offset, uint64_t length,
             uint64_t *source, uint64_t *destination)
{
  struct vm_rg_struct *src, *dst;
  int status;

  if (pid != process.pid || krnl != process.krnl)
    return -ESRCH;
  src = &process.mm->symrgtbl[source_region];
  dst = &process.mm->symrgtbl[destination_region];
  if (src->rg_end <= src->rg_start || dst->rg_end <= dst->rg_start
      || offset > src->rg_end - src->rg_start
      || length > src->rg_end - src->rg_start - offset
      || offset > dst->rg_end - dst->rg_start
      || length > dst->rg_end - dst->rg_start - offset)
    return -EFAULT;
  status = translate_span (src->rg_start + offset, length, 0, source);
  if (status == 0)
    status = translate_span (dst->rg_start + offset, length, 1, destination);
  return status;
}

int
main (void)
{
  const uint64_t source_va = UINT64_C (0x100001000);
  const uint64_t destination_va = UINT64_C (0x200003000);
  struct mm_struct mm = {0};
  struct krnl_t kernel = {0};
  struct memphy_struct ram = {0};
  struct dma_result result;
  struct sc_regs regs = {.a1 = 1, .a2 = 2, .a4 = 4};
  struct inst_t *instructions = calloc (65537, sizeof *instructions);
  struct code_seg_t code = {.text = instructions, .size = 65537};
  unsigned char *bytes = calloc (1024 * 1024, 1);
  struct timespec begin, now;
  uint64_t calculations = 0, checksum = 0;

  assert (bytes != NULL && instructions != NULL);
  instructions[0] = (struct inst_t) {.opcode = DMA_MEMCPY,
                                    .arg_0 = 1, .arg_1 = 2,
                                    .arg_2 = 0, .arg_3 = TEST_LENGTH};
  ram.storage = (BYTE *) bytes;
  ram.maxsz = 1024 * 1024;
  ram.rdmflg = 1;
  kernel.mram = &ram;
  process.pid = 41;
  process.krnl = &kernel;
  process.mm = &mm;
  process.code = &code;
  mm.symrgtbl[1].rg_start = source_va;
  mm.symrgtbl[1].rg_end = source_va + TEST_LENGTH;
  mm.symrgtbl[2].rg_start = destination_va;
  mm.symrgtbl[2].rg_end = destination_va + TEST_LENGTH;
  for (unsigned int i = 0; i < 64; i++)
    {
      pages[i] = (struct page_mapping) {source_va / TEST_PAGE_SIZE + i, i, 0};
      pages[64 + i] = (struct page_mapping) {
        destination_va / TEST_PAGE_SIZE + i, 128 + i, 1
      };
    }
  for (unsigned int i = 1; i < code.size; i++)
    instructions[i].opcode = CALC;
  for (uint64_t i = 0; i < TEST_LENGTH; i++)
    assert (MEMPHY_write (&ram, i, (BYTE) (i * 37 + 0x81)) == 0);
  assert (sysbus_bind_ram (&ram) == 0);
  assert (dma_start () == 0);

  /* Missing MM adapter must fail explicitly, not reinterpret region IDs. */
  assert (libmem_dma_memcpy (NULL, 1, 2, 0, 4) == -EINVAL);
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 4) == -ENOSYS);
  kernel.dma_map_buffers = map_buffers;
  assert (_syscall (&kernel, process.pid + 1, SYS_DMA_MEMCPY, &regs)
          == -ESRCH);
  assert (libmem_dma_memcpy (&process, 30, 2, 0, 4) == -EINVAL);
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 3) == -EINVAL);
  assert (libmem_dma_memcpy (&process, 1, 2, UINT64_MAX, 4) == -EFAULT);
#ifdef MM64
  assert (libmem_dma_memcpy (&process, UINT64_C (1) << 32, 2, 0, 4)
          == -EINVAL);
  instructions[0].arg_3 = (UINT64_C (1) << 32) + 4;
  /* CPU must not truncate 64-bit arguments. */
  assert (run (&process) == -EINVAL);
  process.pc = 0;
  instructions[0].arg_3 = TEST_LENGTH;
#endif
  assert (libmem_dma_memcpy (&process, 1, 2, TEST_LENGTH - 1, 4) == -EFAULT);
  pages[64].writable = 0;
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 4) == -EFAULT);
  pages[64].writable = 1;
  pages[1].frame = 50;
  assert (libmem_dma_memcpy (&process, 1, 2, 0, 8192) == -ENOTSUP);
  pages[1].frame = 1;
  assert (!dma_is_busy ());

  assert (run (&process) == 0);
  assert (process.pc == 1 && dma_pid_busy (process.pid));
  assert (libfree (&process, 1) == -EBUSY);
  assert (free_pcb_memph (&process) == -EBUSY);
  assert (liballoc (&process, 4, 3) == -EBUSY);

  clock_gettime (CLOCK_MONOTONIC, &begin);
  /* Execute subsequent CPU instructions and independent RAM work. There is
     no join/wait between the DMA instruction and these CALC instructions. */
  do
    {
      if (process.pc < code.size)
        {
          assert (run (&process) == 0);
          calculations++;
        }
      BYTE value;
      assert (MEMPHY_write (&ram, 900000, (BYTE) calculations) == 0);
      assert (MEMPHY_read (&ram, 900000, &value) == 0);
      checksum += (unsigned char) value;
      sched_yield ();
      clock_gettime (CLOCK_MONOTONIC, &now);
      assert (now.tv_sec - begin.tv_sec < 30);
    }
  while (dma_take_completion (&result) == 0);

  assert (calculations > 0);
  assert (result.pid == process.pid && result.source == 0);
  assert (result.destination == 524288 && result.length == TEST_LENGTH);
  assert (result.status == 0 && result.interrupts == 1);
  assert (result.bytes_transferred == TEST_LENGTH);
  assert (memcmp (bytes, bytes + 524288, TEST_LENGTH) == 0);
  for (uint64_t i = 0; i < TEST_LENGTH; i++)
    assert (bytes[i] == (unsigned char) (i * 37 + 0x81));
  assert (!dma_pid_busy (process.pid));
  dma_shutdown ();
  assert (memory_bank_cleanup () == 0);
  free (bytes);
  free (instructions);
  printf ("PASS: real CPU -> libmem -> PID syscall "
          "-> test MM mapping -> RTL -> RAM\n"
          "CALC instructions=%" PRIu64 ", independent RAM checksum=%"
          PRIu64 "\n",
          calculations, checksum);
  return 0;
}
