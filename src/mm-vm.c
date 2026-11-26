/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

//#ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Virtual memory module mm/mm-vm.c
 */

#include "string.h"
#include "mm64.h"
#include "common.h"
#include "queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

/* Bảng ánh xạ nội bộ PID -> PCB nằm trong Kernel Space.
 * Giúp __sys_memmap tra cứu tiến trình chỉ bằng PID và struct krnl_t,
 * tuyệt đối không truyền trực tiếp con trỏ PCB từ User Space qua syscall. */
#define MAX_TRACKED_PROCS 1024
static struct pcb_t *krnl_pid_map[MAX_TRACKED_PROCS] = {NULL};
static pthread_mutex_t krnl_pid_lock = PTHREAD_MUTEX_INITIALIZER;

/* Đăng ký PCB vào bảng quản lý PID của Kernel khi tiến trình khởi tạo/cấp phát */
void register_krnl_proc(struct pcb_t *proc)
{
  if (proc == NULL || proc->pid >= MAX_TRACKED_PROCS)
    return;

  pthread_mutex_lock(&krnl_pid_lock);
  krnl_pid_map[proc->pid] = proc;
  pthread_mutex_unlock(&krnl_pid_lock);
}

/* Hàm tra cứu PCB phía Kernel thông qua krnl_t và PID (running_list -> ready_queue -> mlq -> pid_map) */
struct pcb_t *find_proc_by_pid_krnl(struct krnl_t *krnl, uint32_t pid)
{
  if (krnl == NULL)
    return NULL;

  if (krnl->running_list != NULL)
  {
    for (int i = 0; i < krnl->running_list->size; i++)
    {
      if (krnl->running_list->proc[i] != NULL &&
          krnl->running_list->proc[i]->pid == pid)
      {
        return krnl->running_list->proc[i];
      }
    }
  }

  if (krnl->ready_queue != NULL)
  {
    for (int i = 0; i < krnl->ready_queue->size; i++)
    {
      if (krnl->ready_queue->proc[i] != NULL &&
          krnl->ready_queue->proc[i]->pid == pid)
      {
        return krnl->ready_queue->proc[i];
      }
    }
  }

#ifdef MLQ_SCHED
  if (krnl->mlq_ready_queue != NULL)
  {
    for (int prio = 0; prio < MAX_PRIO; prio++)
    {
      for (int i = 0; i < krnl->mlq_ready_queue[prio].size; i++)
      {
        if (krnl->mlq_ready_queue[prio].proc[i] != NULL &&
            krnl->mlq_ready_queue[prio].proc[i]->pid == pid)
        {
          return krnl->mlq_ready_queue[prio].proc[i];
        }
      }
    }
  }
#endif

  if (pid < MAX_TRACKED_PROCS)
  {
    pthread_mutex_lock(&krnl_pid_lock);
    struct pcb_t *p = krnl_pid_map[pid];
    pthread_mutex_unlock(&krnl_pid_lock);
    if (p != NULL && p->krnl == krnl)
      return p;
  }

  return NULL;
}

/* Phân loại kích thước vùng nhớ cần cấp phát theo kiểu macOS */
int macos_select_vmaid(addr_t size)
{
  if (size <= MACOS_SMALL_MAX_SZ)
    return MACOS_VMA_SMALL;   /* VMA 0: Small zone (<= 1KB) */
  else if (size <= MACOS_MEDIUM_MAX_SZ)
    return MACOS_VMA_MEDIUM;  /* VMA 1: Medium zone (1KB - 16KB) */
  else
    return MACOS_VMA_LARGE;   /* VMA 2: Large DMA-aligned zone (> 16KB) */
}

/* Hàm nội bộ khởi tạo một node vm_area_struct */
static struct vm_area_struct *create_vma_node(unsigned long id, addr_t start, addr_t end, struct mm_struct *mm)
{
  struct vm_area_struct *vma = (struct vm_area_struct *)malloc(sizeof(struct vm_area_struct));
  if (vma == NULL)
    return NULL;

  vma->vm_id = id;
  vma->vm_start = start;
  vma->vm_end = end;
  vma->sbrk = start;
  vma->vm_mm = mm;
  vma->vm_freerg_list = NULL;
  vma->vm_next = NULL;
  return vma;
}

/* Phân loại kích thước vùng nhớ cần cấp phát theo kiểu macOS */
int init_macos_vm_layout(struct mm_struct *mm)
{
  if (mm == NULL)
    return -1;

  if (mm->mmap != NULL &&
      mm->mmap->vm_id == MACOS_VMA_SMALL &&
      mm->mmap->vm_start == MACOS_VMA_SMALL_START &&
      mm->mmap->vm_next != NULL)
  {
    return 0;
  }

  struct vm_area_struct *vma0 = mm->mmap;
  if (vma0 == NULL)
  {
    vma0 = create_vma_node(MACOS_VMA_SMALL, MACOS_VMA_SMALL_START, MACOS_VMA_SMALL_END, mm);
    if (vma0 == NULL)
      return -1;
    mm->mmap = vma0;
  }
  else
  {
    vma0->vm_id = MACOS_VMA_SMALL;
    vma0->vm_start = MACOS_VMA_SMALL_START;
    vma0->vm_end = MACOS_VMA_SMALL_END;
    vma0->sbrk = MACOS_VMA_SMALL_START;
    vma0->vm_mm = mm;
    vma0->vm_freerg_list = NULL;
  }

  struct vm_area_struct *vma1 = create_vma_node(MACOS_VMA_MEDIUM, MACOS_VMA_MEDIUM_START, MACOS_VMA_MEDIUM_END, mm);
  struct vm_area_struct *vma2 = create_vma_node(MACOS_VMA_LARGE, MACOS_VMA_LARGE_START, MACOS_VMA_LARGE_END, mm);
  struct vm_area_struct *vma3 = create_vma_node(MACOS_VMA_KERNEL, MACOS_VMA_KERNEL_START, MACOS_VMA_KERNEL_END, mm);

  if (!vma1 || !vma2 || !vma3)
    return -1;

  vma0->vm_next = vma1;
  vma1->vm_next = vma2;
  vma2->vm_next = vma3;
  vma3->vm_next = NULL;

  for (int i = 0; i < PAGING_MAX_SYMTBL_SZ; i++)
  {
    mm->symrgtbl[i].vmaid = -1;
    mm->symrgtbl[i].rg_start = 0;
    mm->symrgtbl[i].rg_end = 0;
    mm->symrgtbl[i].rg_next = NULL;
  }

  return 0;
}

/*get_vma_by_num - get vm area by numID
 *@mm: memory region
 *@vmaid: ID vm area to alloc memory region
 *
 */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  if (mm == NULL)
    return NULL;

  /* Đảm bảo layout macOS-style đã được khởi tạo trước khi duyệt danh sách VMA */
  init_macos_vm_layout(mm);

  struct vm_area_struct *pvma = mm->mmap;

  if (mm->mmap == NULL)
    return NULL;

  int vmait = pvma->vm_id;

  while (vmait < vmaid)
  {
    if (pvma == NULL)
      return NULL;

    pvma = pvma->vm_next;
    /* Kiểm tra pvma != NULL trước khi truy xuất pvma->vm_id để tránh lỗi Segfault */
    if (pvma == NULL)
      return NULL;
    vmait = pvma->vm_id;
  }

  return (vmait == vmaid) ? pvma : NULL;
}

int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
    /* Kiểm tra con trỏ hợp lệ trước khi chép trang giữa MEMRAM và active_mswp */
    if (caller == NULL || caller->krnl == NULL)
        return -1;
    __swap_cp_page(caller->krnl->mram, vicfpn, caller->krnl->active_mswp, swpfpn);
    return 0;
}

/*get_vm_area_node - get vm area for a number of pages
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  struct vm_rg_struct * newrg;
  /* TODO retrive current vma to obtain newrg, current comment out due to compiler redundant warning*/
  //struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  //newrg = malloc(sizeof(struct vm_rg_struct));

  /* TODO: update the newrg boundary
  // newrg->rg_start = ...
  // newrg->rg_end = ...
  */
  // struct vm_area_struct *cur_vma=....

  /* Lấy cur_vma từ caller->mm (không gian của tiến trình) và tạo node vùng nhớ mới tại đỉnh sbrk */
  if (caller == NULL || caller->mm == NULL)
    return NULL;

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
    return NULL;

  newrg = malloc(sizeof(struct vm_rg_struct));
  if (newrg == NULL)
    return NULL;

  newrg->vmaid = vmaid;
  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end = cur_vma->sbrk + alignedsz;
  newrg->rg_next = NULL;

  /* END TODO */

  return newrg;
}

/*validate_overlap_vm_area
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct vm_area_struct *vma = caller->mm->mmap;

  /* TODO validate the planned memory area is not overlapped */
  if (vmastart >= vmaend)
  {
    return -1;
  }

  if (vma == NULL)
  {
    return -1;
  }

#ifdef MM64
  /* Kiểm tra địa chỉ mở rộng phải tuân thủ chuẩn Canonical Address 64-bit */
  if (!IS_CANONICAL_ADDR(vmastart) || !IS_CANONICAL_ADDR(vmaend - 1))
  {
    return -1;
  }
#endif

  /* TODO validate the planned memory area is not overlapped */

  struct vm_area_struct *cur_area = get_vma_by_num(caller->mm, vmaid);
  if (cur_area == NULL)
  {
    return -1;
  }

  /* Vùng dự kiến mở rộng [vmastart, vmaend] phải nằm gọn trong biên [vm_start, vm_end] của cur_area */
  if (!INCLUDE(cur_area->vm_start, cur_area->vm_end, vmastart, vmaend))
  {
    return -1;
  }

  /* Kiểm tra khoảng mở rộng mới [vmastart, vmaend] không chồng lấn với bất kỳ VMA nào khác */
  while (vma != NULL)
  {
    if (vma != cur_area && OVERLAP(vmastart, vmaend, vma->vm_start, vma->vm_end))
    {
      return -1;
    }
    vma = vma->vm_next;
  }
  /* End TODO*/

  return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@inc_sz: increment size
 *
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  /* TOTO with new address scheme, the size need tobe aligned 
   *      the raw inc_sz maybe not fit pagesize
   */ 
  /* Căn lề kích thước tăng thêm theo bội số kích thước trang (4KB với MM64, 256B với 32-bit) */
  if (caller == NULL || caller->mm == NULL || inc_sz == 0)
    return -1;

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
    return -1;

  addr_t alignedsz = VM_PAGE_ALIGNSZ(inc_sz);
  int incnumpage = (int)(alignedsz / VM_PAGESZ);

  struct vm_rg_struct *area = get_vm_area_node_at_brk(caller, vmaid, alignedsz);
  if (area == NULL)
    return -1;

  /* 
   * TODO Validate overlap of obtained region 
   *if (validate_overlap_vm_area(caller, vmaid, area->rg_start, area->rg_end) < 0)
   *   return -1;  //Overlap and failed allocation 
   */
  if (validate_overlap_vm_area(caller, vmaid, area->rg_start, area->rg_end) < 0)
  {
    free(area);
    return -1; /* Overlap and failed allocation */
  }

  /* TODO: Obtain the new vm area based on vmaid */
  /* Ánh xạ các trang ảo mới vào khung trang vật lý trên MEMRAM và cập nhật đỉnh sbrk */
  struct vm_rg_struct ret_rg;
  memset(&ret_rg, 0, sizeof(struct vm_rg_struct));

  if (vm_map_ram(caller, area->rg_start, area->rg_end,
                 cur_vma->sbrk, incnumpage, &ret_rg) == (addr_t)-1)
  {
    free(area);
    return -1;
  }

  cur_vma->sbrk += alignedsz;
  free(area);

  return 0;
}

/* Hàm tiện ích in sơ đồ trạng thái các VMA, sbrk, symrgtbl và freerg_list phục vụ Demo & Báo cáo */
int print_vm_allocation_state(struct pcb_t *proc, const char *tag)
{
  if (proc == NULL || proc->mm == NULL)
    return -1;

  init_macos_vm_layout(proc->mm);

  const char *vma_names[] = {
    "MACOS_SMALL  (<=1KB)   ",
    "MACOS_MEDIUM (1-16KB) ",
    "MACOS_LARGE  (>16KB)  ",
    "KERNEL_SPACE (Priv)   "
  };

  printf("\n=== [VM STATE: %s] PID=%u ===\n", tag ? tag : "SNAPSHOT", proc->pid);
  struct vm_area_struct *vma = proc->mm->mmap;
  while (vma != NULL)
  {
    const char *label = (vma->vm_id < 4) ? vma_names[vma->vm_id] : "CUSTOM_VMA";
    printf("  VMA[%lu] %s | [0x%016llx - 0x%016llx) | sbrk=0x%016llx\n",
           vma->vm_id, label,
           (unsigned long long)vma->vm_start,
           (unsigned long long)vma->vm_end,
           (unsigned long long)vma->sbrk);

    struct vm_rg_struct *frg = vma->vm_freerg_list;
    if (frg != NULL)
    {
      printf("    Free regions: ");
      while (frg != NULL)
      {
        printf("[0x%llx - 0x%llx) ",
               (unsigned long long)frg->rg_start,
               (unsigned long long)frg->rg_end);
        frg = frg->rg_next;
      }
      printf("\n");
    }
    vma = vma->vm_next;
  }

  printf("  Active Symbol Table (symrgtbl):\n");
  for (int i = 0; i < PAGING_MAX_SYMTBL_SZ; i++)
  {
    struct vm_rg_struct *rg = &proc->mm->symrgtbl[i];
    if (rg->rg_start != 0 || rg->rg_end != 0)
    {
      printf("    reg[%2d] -> VMA[%d] [0x%016llx - 0x%016llx) (size=%llu B)\n",
             i, rg->vmaid,
             (unsigned long long)rg->rg_start,
             (unsigned long long)rg->rg_end,
             (unsigned long long)(rg->rg_end - rg->rg_start));
    }
  }
  printf("=================================================\n");
  return 0;
}

// #endif