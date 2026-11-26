/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

/* Hàm kiểm tra địa chỉ thuộc không gian User Space hợp lệ (ngăn User truy cập Kernel Space) */
static inline int is_valid_user_address(int vmaid, addr_t addr)
{
  if (vmaid == MACOS_VMA_KERNEL)
    return 0;

#ifdef MM64
  if (!IS_CANONICAL_USER_ADDR(addr))
    return 0;
  if (addr < MACOS_PAGEZERO_END || addr >= MACOS_VMA_LARGE_END)
    return 0;
#endif
  return 1;
}

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  /* Tìm đúng VMA chứa vùng trống theo rg_elmt->vmaid (thay vì luôn đẩy vào VMA 0) */
  struct vm_area_struct *target_vma = get_vma_by_num(mm, rg_elmt->vmaid);
  if (target_vma == NULL)
    target_vma = mm->mmap;

  struct vm_rg_struct *rg_node = target_vma->vm_freerg_list;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  /* Chèn node trống theo thứ tự địa chỉ tăng dần và tự động gộp (coalesce) các vùng liền kề */
  rg_elmt->rg_next = NULL;
  struct vm_rg_struct *curr = rg_node;
  struct vm_rg_struct *prev = NULL;

  while (curr != NULL && curr->rg_start < rg_elmt->rg_start)
  {
    prev = curr;
    curr = curr->rg_next;
  }

  /* Enlist the new region */
  rg_elmt->rg_next = curr;
  if (prev == NULL)
    target_vma->vm_freerg_list = rg_elmt;
  else
    prev->rg_next = rg_elmt;

  /* Gộp với vùng trống kế sau nếu liền kề */
  if (rg_elmt->rg_next != NULL && rg_elmt->rg_end == rg_elmt->rg_next->rg_start)
  {
    struct vm_rg_struct *next_node = rg_elmt->rg_next;
    rg_elmt->rg_end = next_node->rg_end;
    rg_elmt->rg_next = next_node->rg_next;
    free(next_node);
  }

  /* Gộp với vùng trống phía trước nếu liền kề */
  if (prev != NULL && prev->rg_end == rg_elmt->rg_start)
  {
    prev->rg_end = rg_elmt->rg_end;
    prev->rg_next = rg_elmt->rg_next;
    free(rg_elmt);
  }

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);

  /* Đăng ký PID vào bảng ánh xạ của Kernel và đảm bảo layout macOS-style đã sẵn sàng */
  register_krnl_proc(caller);
  init_macos_vm_layout(caller->mm);

  /* Nếu thanh ghi rgid đang giữ vùng nhớ cũ chưa giải phóng, thu hồi vào freerg_list trước */
  if (caller->mm->symrgtbl[rgid].rg_start != 0 || caller->mm->symrgtbl[rgid].rg_end != 0)
  {
    struct vm_rg_struct *old_free = malloc(sizeof(struct vm_rg_struct));
    if (old_free != NULL)
    {
      old_free->vmaid = caller->mm->symrgtbl[rgid].vmaid;
      old_free->rg_start = caller->mm->symrgtbl[rgid].rg_start;
      old_free->rg_end = caller->mm->symrgtbl[rgid].rg_end;
      old_free->rg_next = NULL;
      enlist_vm_freerg_list(caller->mm, old_free);
    }
    caller->mm->symrgtbl[rgid].rg_start = 0;
    caller->mm->symrgtbl[rgid].rg_end = 0;
  }

  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  addr_t inc_sz = 0;

  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->mm->symrgtbl[rgid].vmaid = vmaid;
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
    caller->regs[rgid] = rgnode.rg_start;
 
    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/

  /*Attempt to increate limit to get space */
#ifdef MM64
  inc_sz = PAGING64_PAGE_ALIGNSZ(size);
#else
  inc_sz = PAGING_PAGE_ALIGNSZ(size);
#endif
  addr_t old_sbrk;

  old_sbrk = cur_vma->sbrk;

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
#ifdef MM64
  regs.a3 = inc_sz;
#else
  regs.a3 = PAGING_PAGE_ALIGNSZ(size);
#endif  
  /* Giao tiếp User -> Kernel qua SYSCALL 17 chỉ truyền caller->krnl và caller->pid */
  if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0 || cur_vma->sbrk <= old_sbrk)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /*Successful increase limit */
  caller->mm->symrgtbl[rgid].vmaid = vmaid;
  caller->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->mm->symrgtbl[rgid].rg_end = old_sbrk + size;
  caller->regs[rgid] = old_sbrk;

  *alloc_addr = old_sbrk;

  /* Đưa phần khoảng trống còn dư [old_sbrk + size, cur_vma->sbrk) vào freerg_list để tái sử dụng */
  if (old_sbrk + size < cur_vma->sbrk)
  {
    struct vm_rg_struct *rem_rg = malloc(sizeof(struct vm_rg_struct));
    if (rem_rg != NULL)
    {
      rem_rg->vmaid = vmaid;
      rem_rg->rg_start = old_sbrk + size;
      rem_rg->rg_end = cur_vma->sbrk;
      rem_rg->rg_next = NULL;
      enlist_vm_freerg_list(caller->mm, rem_rg);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;

}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO: Manage the collect freed region to freerg_list */
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode == NULL || (rgnode->rg_start == 0 && rgnode->rg_end == 0))
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* Ngăn chặn tiến trình user dùng lệnh free thông thường để giải phóng vùng Kernel Space */
  int target_vmaid = (rgnode->vmaid >= 0) ? rgnode->vmaid : vmaid;
  if (target_vmaid == MACOS_VMA_KERNEL)
  {
    printf("[PROTECTION FAULT] PID=%u cannot free kernel memory region reg=%d\n", caller->pid, rgid);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  freerg_node->vmaid = target_vmaid;
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->rg_next = NULL;

  rgnode->vmaid = -1;
  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;
  caller->regs[rgid] = 0;

  /*enlist the obsoleted memory region */
  enlist_vm_freerg_list(caller->mm, freerg_node);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  if (proc == NULL || proc->mm == NULL)
    return -1;

  addr_t addr = 0;
  int vmaid = macos_select_vmaid(size);
  int val = __alloc(proc, vmaid, (int)reg_index, size, &addr);
  if (val == -1)
    return -1;

#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif

  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  if (proc == NULL || proc->mm == NULL || reg_index >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  int vmaid = (proc->mm->symrgtbl[reg_index].vmaid >= 0)
                  ? proc->mm->symrgtbl[reg_index].vmaid
                  : MACOS_VMA_SMALL;

  int val = __free(proc, vmaid, (int)reg_index);
  if (val == -1)
    return -1;

#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif

  return 0;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */
int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller)
{

  uint64_t pte = pte_get_entry(mm, pgn, caller);

  if (!PAGING_PAGE_PRESENT(pte))
  { /* Page is not online, make it actively living */
    addr_t vicpgn;
    addr_t vicpte;
    addr_t vicfpn, swpfpn;
    struct sc_regs regs;

    /* TODO Initialize the target frame storing our variable */
//  addr_t tgtfpn 
    addr_t tgtfpn = 0;
    int was_swapped = (pte & PAGING_PTE_SWAPPED_MASK) ? 1 : 0;
    addr_t old_swpfpn = PAGING_SWP(pte);

    /* TODO: Play with your paging theory here */
    /* Ưu tiên xin frame trống trực tiếp từ MEMRAM; nếu đầy mới chọn trang nạn nhân để swap-out */
    if (MEMPHY_get_freefp(caller->krnl->mram, &tgtfpn) != 0)
    {
      /* Find victim page */
      if (find_victim_page(mm, &vicpgn) == -1)
      {
        return -1;
      }

      vicpte = pte_get_entry(mm, vicpgn, caller);
      vicfpn = PAGING_FPN(vicpte);

      /* Get free frame in MEMSWP */
      if (MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == -1)
      {
        return -1;
      }

      /* TODO: Implement swap frame from MEMRAM to MEMSWP and vice versa*/

      /* TODO copy victim frame to swap 
       * SWP(vicfpn <--> swpfpn)
       * SYSCALL 1 sys_memmap
       */
      regs.a1 = SYSMEM_SWP_OP;
      regs.a2 = vicfpn;
      regs.a3 = swpfpn;
      _syscall(caller->krnl, caller->pid, 17, &regs);

      /* Update page table */
      //pte_set_swap(...);
      pte_set_swap(caller, vicpgn, 0, swpfpn);
      tgtfpn = vicfpn;
    }

    /* Nếu trang mục tiêu từng bị đẩy ra SWAP trước đó, chép ngược từ SWAP về MEMRAM (swap-in) */
    if (was_swapped && caller->krnl->active_mswp != NULL)
    {
      __swap_cp_page(caller->krnl->active_mswp, old_swpfpn, caller->krnl->mram, tgtfpn);
      MEMPHY_put_freefp(caller->krnl->active_mswp, old_swpfpn);
    }

    /* Update its online status of the target page */
    //pte_set_fpn(...);
    pte_set_fpn(mm, pgn, tgtfpn, caller);

    enlist_pgn_node(&mm->fifo_pgn, pgn);
  }

  *fpn = PAGING_FPN(pte_get_entry(mm, pgn, caller));

  return 0;
}

/* Helper hỗ trợ địa chỉ ảo 64-bit đầy đủ cho thao tác đọc byte qua SYSCALL 17 */
static int pg_getval64(struct mm_struct *mm, addr_t addr, BYTE *data, struct pcb_t *caller)
{
  addr_t pgn = VM_PGN(addr);
  addr_t off = VM_OFFST(addr);
  int fpn;

  if (pg_getpage(mm, (int)pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  addr_t phyaddr = VM_PHYADDR(fpn, off);

  struct sc_regs regs;
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  regs.a3 = 0;

  if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
    return -1;

  *data = (BYTE)(regs.a3 & 0xFF);
  return 0;
}

/* Helper hỗ trợ địa chỉ ảo 64-bit đầy đủ cho thao tác ghi byte qua SYSCALL 17 */
static int pg_setval64(struct mm_struct *mm, addr_t addr, BYTE value, struct pcb_t *caller)
{
  addr_t pgn = VM_PGN(addr);
  addr_t off = VM_OFFST(addr);
  int fpn;

  if (pg_getpage(mm, (int)pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  addr_t phyaddr = VM_PHYADDR(fpn, off);

  struct sc_regs regs;
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = (arg_t)(unsigned char)value;

  if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
    return -1;

  return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, int addr, BYTE *data, struct pcb_t *caller)
{
  int pgn = PAGING_PGN(addr);
//int off = ...
  int fpn;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

//int phyaddr = ...

  /* TODO 
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */
  return pg_getval64(mm, (addr_t)(uint32_t)addr, data, caller);
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, int addr, BYTE value, struct pcb_t *caller)
{
  int pgn = PAGING_PGN(addr);
//int off = ...
  int fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */


  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */
  return pg_setval64(mm, (addr_t)(uint32_t)addr, value, caller);
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  pthread_mutex_lock(&mmvm_lock);

  /* Chặn truy cập nếu địa chỉ trong thanh ghi nằm ở Kernel Space (>= 0xFF00000000000000) */
  if (rgid >= 0 && rgid < 10 && caller->regs[rgid] >= CANONICAL_KERNEL_BASE)
  {
    printf("[PROTECTION FAULT] PID=%u illegal READ to Kernel Space address 0x%016llx\n",
           caller->pid, (unsigned long long)(caller->regs[rgid] + offset));
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);

//struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);
  int actual_vmaid = (currg != NULL && currg->vmaid >= 0) ? currg->vmaid : vmaid;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, actual_vmaid);

  /* TODO Invalid memory identify */
  if (currg == NULL || cur_vma == NULL || (currg->rg_start == 0 && currg->rg_end == 0))
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  addr_t target_addr = currg->rg_start + offset;

  /* Kiểm tra quyền User Space và giới hạn biên vùng nhớ [rg_start, rg_end) */
  if (!is_valid_user_address(actual_vmaid, target_addr) || target_addr >= currg->rg_end || target_addr >= cur_vma->sbrk)
  {
    printf("[PROTECTION FAULT] PID=%u out-of-bounds or kernel READ at 0x%016llx\n",
           caller->pid, (unsigned long long)target_addr);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* Đọc từ không gian bộ nhớ của tiến trình caller->mm (không đọc nhầm sang krnl->mm) */
  int ret = pg_getval64(caller->mm, target_addr, data, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc,
    uint32_t source,
    addr_t offset,
    uint32_t *destination)
{
  if (proc == NULL || proc->mm == NULL || destination == NULL)
    return -1;

  BYTE data = 0;
  int vmaid = (source < PAGING_MAX_SYMTBL_SZ && proc->mm->symrgtbl[source].vmaid >= 0)
                  ? proc->mm->symrgtbl[source].vmaid
                  : MACOS_VMA_SMALL;

  int val = __read(proc, vmaid, (int)source, offset, &data);
  if (val != 0)
    return -1;

  *destination = (uint32_t)(unsigned char)data;

#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  pthread_mutex_lock(&mmvm_lock);

  /* Chặn truy cập ghi trái phép vào Kernel Space từ User Space */
  if (rgid >= 0 && rgid < 10 && caller->regs[rgid] >= CANONICAL_KERNEL_BASE)
  {
    printf("[PROTECTION FAULT] PID=%u illegal WRITE to Kernel Space address 0x%016llx\n",
           caller->pid, (unsigned long long)(caller->regs[rgid] + offset));
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* Truy xuất symrgtbl và VMA thuộc không gian tiến trình caller->mm */
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  int actual_vmaid = (currg != NULL && currg->vmaid >= 0) ? currg->vmaid : vmaid;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, actual_vmaid);

  if (currg == NULL || cur_vma == NULL || (currg->rg_start == 0 && currg->rg_end == 0)) /* Invalid memory identify */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  addr_t target_addr = currg->rg_start + offset;

  /* Kiểm tra địa chỉ User hợp lệ và không vượt quá rg_end / sbrk */
  if (!is_valid_user_address(actual_vmaid, target_addr) || target_addr >= currg->rg_end || target_addr >= cur_vma->sbrk)
  {
    printf("[PROTECTION FAULT] PID=%u out-of-bounds or kernel WRITE at 0x%016llx\n",
           caller->pid, (unsigned long long)target_addr);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  int ret = pg_setval64(caller->mm, target_addr, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,
    BYTE data,
    uint32_t destination,
    addr_t offset)
{
  if (proc == NULL || proc->mm == NULL)
    return -1;

  int vmaid = (destination < PAGING_MAX_SYMTBL_SZ && proc->mm->symrgtbl[destination].vmaid >= 0)
                  ? proc->mm->symrgtbl[destination].vmaid
                  : MACOS_VMA_SMALL;

  int val = __write(proc, vmaid, (int)destination, offset, data);
  if (val == -1)
    return -1;

#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1);
#endif

  return val;
}

/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  /* 
   * TODO: provide OS level management
   *       and forward the request to helper
   */
//addr_t  addr;
//int val = ...
  addr_t addr = 0;
  addr_t ret = __kmalloc(caller, MACOS_VMA_KERNEL, (int)reg_index, (addr_t)size, &addr);

  /* 
   * TODO: provide OS kmem allocation validation
   */
  if (ret == (addr_t)-1 || !IS_CANONICAL_KERNEL_ADDR(addr))
    return -1;

  caller->regs[reg_index] = addr;
  return 0;
}


/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /* TODO: provide OS kernel memory allocation
   *       update krnl_pgd for OS kernel level management */

  //struct krnl_t *krnl = caller->krnl;
  //krnl->symrgtbl...
  //krnl->krnl_pgd ...
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL || size == 0)
    return (addr_t)-1;

  pthread_mutex_lock(&kmm_lock);

  struct krnl_t *krnl = caller->krnl;
  init_macos_vm_layout(krnl->mm);

  struct vm_area_struct *kvma = get_vma_by_num(krnl->mm, vmaid);
  if (kvma == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
  {
    pthread_mutex_unlock(&kmm_lock);
    return (addr_t)-1;
  }

  addr_t alignedsz = VM_PAGE_ALIGNSZ(size);
  int incnumpage = (int)(alignedsz / VM_PAGESZ);

  if (kvma->sbrk + alignedsz > kvma->vm_end)
  {
    pthread_mutex_unlock(&kmm_lock);
    return (addr_t)-1;
  }

  addr_t start_addr = kvma->sbrk;
  struct vm_rg_struct ret_rg;
  memset(&ret_rg, 0, sizeof(struct vm_rg_struct));

  /* Ánh xạ vùng nhớ liên tục vào không gian Kernel (krnl->mm) */
  struct pcb_t kproxy = *caller;
  kproxy.mm = krnl->mm;

  if (vm_map_ram(&kproxy, start_addr, start_addr + alignedsz, start_addr, incnumpage, &ret_rg) == (addr_t)-1)
  {
    pthread_mutex_unlock(&kmm_lock);
    return (addr_t)-1;
  }

  kvma->sbrk += alignedsz;
  krnl->mm->symrgtbl[rgid].vmaid = MACOS_VMA_KERNEL;
  krnl->mm->symrgtbl[rgid].rg_start = start_addr;
  krnl->mm->symrgtbl[rgid].rg_end = start_addr + size;

  if (alloc_addr != NULL)
    *alloc_addr = start_addr;

  pthread_mutex_unlock(&kmm_lock);
  return start_addr;

}



/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  /* TODO: provide DMA mempy */

  //struct krnl_t *krnl = caller->krnl;

  /*
   *  TODO: provide asynchronous dma memcpy
   *        handling the dma_done_callback
   */
  //do_dma(caller, (uint64_t) src, (uint64_t) dst, (uint64_t) size);
  //krnl->...
  //krnl->...
  if (caller == NULL || caller->mm == NULL)
    return -1;

  struct vm_rg_struct *src_rg = get_symrg_byid(caller->mm, (int)src);
  struct vm_rg_struct *dst_rg = get_symrg_byid(caller->mm, (int)dst);
  if (src_rg == NULL || dst_rg == NULL)
    return -1;

  if (src_rg->rg_start + offset + size > src_rg->rg_end ||
      dst_rg->rg_start + offset + size > dst_rg->rg_end)
    return -1;

  if (!is_valid_user_address(src_rg->vmaid, src_rg->rg_start + offset) ||
      !is_valid_user_address(dst_rg->vmaid, dst_rg->rg_start + offset))
    return -1;

  for (uint32_t i = 0; i < size; i++)
  {
    BYTE b = 0;
    if (__read(caller, src_rg->vmaid, (int)src, (addr_t)(offset + i), &b) != 0)
      return -1;
    if (__write(caller, dst_rg->vmaid, (int)dst, (addr_t)(offset + i), b) != 0)
      return -1;
  }

  return 0;
}


int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_user_mem(...)
  //__write_kernel_mem(...);
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL)
    return -1;

  int src_vmaid = (source < PAGING_MAX_SYMTBL_SZ) ? caller->mm->symrgtbl[source].vmaid : 0;
  for (uint32_t i = 0; i < size; i++)
  {
    BYTE val = 0;
    if (__read_user_mem(caller, src_vmaid, (int)source, (addr_t)(offset + i), &val) != 0)
      return -1;
    if (__write_kernel_mem(caller, MACOS_VMA_KERNEL, (int)destination, (addr_t)(offset + i), val) != 0)
      return -1;
  }

  return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_kernel_mem(...)
  //__write_user_mem(...);
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL)
    return -1;

  int dst_vmaid = (destination < PAGING_MAX_SYMTBL_SZ) ? caller->mm->symrgtbl[destination].vmaid : 0;
  for (uint32_t i = 0; i < size; i++)
  {
    BYTE val = 0;
    if (__read_kernel_mem(caller, MACOS_VMA_KERNEL, (int)source, (addr_t)(offset + i), &val) != 0)
      return -1;
    if (__write_user_mem(caller, dst_vmaid, (int)destination, (addr_t)(offset + i), val) != 0)
      return -1;
  }

  return 0;
}


/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL || data == NULL)
    return -1;

  struct vm_rg_struct *krg = get_symrg_byid(caller->krnl->mm, rgid);
  if (krg == NULL || krg->vmaid != MACOS_VMA_KERNEL)
    return -1;

  addr_t addr = krg->rg_start + offset;
  if (!IS_CANONICAL_KERNEL_ADDR(addr) || addr >= krg->rg_end)
    return -1;

  struct pcb_t kproxy = *caller;
  kproxy.mm = caller->krnl->mm;
  return pg_getval64(caller->krnl->mm, addr, data, &kproxy);
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL)
    return -1;

  struct vm_rg_struct *krg = get_symrg_byid(caller->krnl->mm, rgid);
  if (krg == NULL || krg->vmaid != MACOS_VMA_KERNEL)
    return -1;

  addr_t addr = krg->rg_start + offset;
  if (!IS_CANONICAL_KERNEL_ADDR(addr) || addr >= krg->rg_end)
    return -1;

  struct pcb_t kproxy = *caller;
  kproxy.mm = caller->krnl->mm;
  return pg_setval64(caller->krnl->mm, addr, value, &kproxy);
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
  return __read(caller, vmaid, rgid, offset, data);
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
  return __write(caller, vmaid, rgid, offset, value);
}


/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint64_t pte;

  for (pagenum = 0; pagenum < PAGING_MAX_PGN; pagenum++)
  {
    pte = caller->mm->pgd[pagenum];
    if (pte == 0)
      continue;

    if (PAGING_PAGE_PRESENT(pte))
    {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else if (pte & PAGING_PTE_SWAPPED_MASK)
    {
      fpn = PAGING_SWP(pte);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg = mm->fifo_pgn;

  /* TODO: Implement the theorical mechanism to find the victim page */
  if (!pg)
  {
    return -1;
  }
  struct pgn_t *prev = NULL;
  while (pg->pg_next)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;

  /* Xử lý trường hợp danh sách fifo_pgn chỉ có đúng 1 phần tử (prev == NULL) để tránh NULL dereference */
  if (prev == NULL)
    mm->fifo_pgn = NULL;
  else
    prev->pg_next = NULL;

  free(pg);

  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
    return -1;

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == (addr_t)-1) // new region not found
    return -1;

  return 0;
}

// #endif