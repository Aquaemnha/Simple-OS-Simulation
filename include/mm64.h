#ifndef MM64_H
#define MM64_H

#include "mm.h"

#define MM64_BITS_PER_LONG 64
#define PAGING64_CPU_BUS_WIDTH 57
#define PAGING64_PAGESZ 4096
#define PAGING64_TABLE_ENTRIES 512
#define PAGING64_DIR_BYTES (PAGING64_TABLE_ENTRIES * sizeof(addr_t))
#define PAGING64_PT_BYTES (PAGING64_TABLE_ENTRIES * sizeof(uint32_t))
#define PAGING64_PAGE_MASK UINT64_C(0xfffffffffffff000)
#define PAGING64_INDEX_MASK UINT64_C(0x1ff)
#define PAGING64_ADDR_OFFST_HIBIT 11
#define PAGING64_ADDR_OFFST_LOBIT 0
#define PAGING64_ADDR_PT_HIBIT 20
#define PAGING64_ADDR_PT_LOBIT 12
#define PAGING64_ADDR_PT_SHIFT 12
#define PAGING64_ADDR_PMD_HIBIT 29
#define PAGING64_ADDR_PMD_LOBIT 21
#define PAGING64_ADDR_PUD_HIBIT 38
#define PAGING64_ADDR_PUD_LOBIT 30
#define PAGING64_ADDR_P4D_HIBIT 47
#define PAGING64_ADDR_P4D_LOBIT 39
#define PAGING64_ADDR_PGD_HIBIT 56
#define PAGING64_ADDR_PGD_LOBIT 48
#define PAGING64_ADDR_OFFST_MASK UINT64_C(0xfff)
#define PAGING64_ADDR_PT_MASK (PAGING64_INDEX_MASK << PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD_MASK (PAGING64_INDEX_MASK << PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD_MASK (PAGING64_INDEX_MASK << PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D_MASK (PAGING64_INDEX_MASK << PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD_MASK (PAGING64_INDEX_MASK << PAGING64_ADDR_PGD_LOBIT)
#define PAGING64_ADDR_OFFST(addr) ((addr) & PAGING64_ADDR_OFFST_MASK)
#define PAGING64_ADDR_PT(addr) (((addr) >> PAGING64_ADDR_PT_LOBIT) & PAGING64_INDEX_MASK)
#define PAGING64_ADDR_PMD(addr) (((addr) >> PAGING64_ADDR_PMD_LOBIT) & PAGING64_INDEX_MASK)
#define PAGING64_ADDR_PUD(addr) (((addr) >> PAGING64_ADDR_PUD_LOBIT) & PAGING64_INDEX_MASK)
#define PAGING64_ADDR_P4D(addr) (((addr) >> PAGING64_ADDR_P4D_LOBIT) & PAGING64_INDEX_MASK)
#define PAGING64_ADDR_PGD(addr) (((addr) >> PAGING64_ADDR_PGD_LOBIT) & PAGING64_INDEX_MASK)
#define PAGING64_MAX_PGN (UINT64_MAX >> 12)
#define PAGING64_PAGE_ALIGNSZ(size) (((size) + PAGING64_PAGESZ - 1) & PAGING64_PAGE_MASK)
#define MM64_KERNEL_BASE UINT64_C(0xffff800000000000)
#define MM64_KERNEL_PGD_INDEX ((MM64_KERNEL_BASE >> 48) & PAGING64_INDEX_MASK)
#define MM64_USER_LIMIT (UINT64_C(1) << 56)
#define MM64_USER_MIN UINT64_C(0x1000)
#define MM64_VMA_SMALL 0
#define MM64_VMA_MEDIUM 1
#define MM64_VMA_LARGE 2
#define MM64_VMA_KERNEL 3
#define MM64_SMALL_MAX_SIZE UINT64_C(1024)
#define MM64_MEDIUM_MAX_SIZE UINT64_C(16384)

int mm64_canonical (addr_t address);
int mm64_translate (struct mm_struct *mm, struct memphy_struct *ram,
                    addr_t address, addr_t *physical);
int mm64_translate_pin (struct mm_struct *mm, struct memphy_struct *ram,
                        addr_t address, addr_t *physical, addr_t *frame);
void mm64_destroy (struct mm_struct *mm);
int mm64_release_pages (struct mm_struct *mm, struct memphy_struct *ram);
int mm64_map_contiguous (struct pcb_t *caller, addr_t address, addr_t size);
int mm64_prepare_vma (struct pcb_t *caller, int vmaid);
int mm64_swap_out_one (struct krnl_t *kernel);
int mm64_swap_out_frame (struct krnl_t *kernel, addr_t frame, int swap_type,
                         addr_t swap_offset);
void mm64_reset_stats (void);
void mm64_print_system_stats (struct krnl_t *kernel);

#endif
