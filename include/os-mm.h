#ifndef OSMM_H
#define OSMM_H

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>

#ifndef MM_PAGING
#define MM_PAGING 1
#endif

#define PAGING_MAX_MMSWP 4
#define PAGING_MAX_SYMTBL_SZ 30

#ifdef MM64
#define ADDR_TYPE uint64_t
#else
#define ADDR_TYPE uint32_t
#endif

typedef char BYTE;
typedef ADDR_TYPE addr_t;

#ifdef MM64
#define FORMAT_ADDR "%" PRIu64
#define FORMATX_ADDR "%016" PRIx64
#else
#define FORMAT_ADDR "%" PRIu32
#define FORMATX_ADDR "%08" PRIx32
#endif

struct mm_struct;
struct krnl_t;

struct pgn_t
{
  addr_t pgn;
  struct pgn_t *pg_next;
};

struct vm_rg_struct
{
  int vmaid;
  addr_t rg_start;
  addr_t rg_end;
  int mode_bit;
  struct vm_rg_struct *rg_next;
};

struct vm_area_struct
{
  unsigned long vm_id;
  addr_t vm_start;
  addr_t vm_end;
  addr_t sbrk;
  struct mm_struct *vm_mm;
  struct vm_rg_struct *vm_freerg_list;
  struct vm_area_struct *vm_next;
};

struct kcache_pool_struct
{
  int size;
  int align;
#ifdef MM64
  addr_t storage;
#else
  uint32_t storage;
#endif
};

struct mm_struct
{
#ifdef MM64
  addr_t *pgd;
  addr_t *p4d;
  addr_t *pud;
  addr_t *pmd;
  addr_t *pt;
#else
  uint32_t *pgd;
#endif
  struct vm_area_struct *mmap;
  struct vm_rg_struct symrgtbl[PAGING_MAX_SYMTBL_SZ];
  struct pgn_t *fifo_pgn;
  struct kcache_pool_struct *kcpooltbl;
  struct krnl_t *kernel;
  uint32_t owner_pid;
  uint64_t table_pages;
  uint64_t table_bytes_current;
  uint64_t table_bytes_allocated;
  uint64_t table_bytes_by_level[5];
  uint64_t translations;
  uint64_t walk_reads;
  uint64_t swap_outs;
  uint64_t swap_ins;
  int kernel_space;
  pthread_mutex_t page_table_lock;
  int page_table_lock_initialized;
};

struct framephy_struct
{
  addr_t fpn;
  struct framephy_struct *fp_next;
  struct mm_struct *owner;
  addr_t vaddr;
  unsigned int pin_count;
  int evicting;
};

struct memphy_struct
{
  BYTE *storage;
  int maxsz;
  int rdmflg;
  int cursor;
  struct framephy_struct *free_fp_list;
  struct framephy_struct *used_fp_list;
  pthread_mutex_t frame_lock;
  pthread_mutex_t cursor_lock;
  int locks_initialized;
};

#endif
