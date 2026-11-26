/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "os-mm.h"
#include "syscall.h"
#include "libmem.h"
#include "queue.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef MM64
#include "mm64.h"
#else
#include "mm.h"
#endif

//typedef char BYTE;

int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs* regs)
{
   if (krnl == NULL || regs == NULL)
      return -1;

   int memop = regs->a1;
   BYTE value = 0;
   /* 
    * TODO THIS DUMMY CREATE EMPTY PROC TO AVOID COMPILER NOTIFY 
    *      need to be eliminated
	*/
   /* Đã loại bỏ việc malloc caller giả; tra cứu PCB thật trong Kernel Space thông qua krnl và PID */
   //struct pcb_t *caller = malloc(sizeof(struct pcb_t));
   //caller->krnl = malloc(sizeof(struct krnl_t));

   /*
    * @bksysnet: Please note in the dual spacing design
    *            syscall implementations are in kernel space.
    */

   /* TODO: Traverse proclist to terminate the proc
    *       stcmp to check the process match proc_name
    */

    /* TODO Maching and marking the process */
    /* user process are not allowed to access directly pcb in kernel space of syscall */
    //....
   struct pcb_t *caller = find_proc_by_pid_krnl(krnl, pid);
   if (caller == NULL)
   {
      printf("[SYSCALL ERROR] __sys_memmap: Cannot resolve PID=%u in kernel space\n", pid);
      return -1;
   }

   switch (memop) {
   case SYSMEM_MAP_OP:
            /* Reserved process case*/
			vmap_pgd_memset(caller, regs->a2, regs->a3);
            break;
   case SYSMEM_INC_OP:
            return inc_vma_limit(caller, regs->a2, regs->a3);
   case SYSMEM_SWP_OP:
            return __mm_swap_page(caller, regs->a2, regs->a3);
   case SYSMEM_IO_READ:
            /* Kiểm tra địa chỉ vật lý nằm trong phạm vi MEMRAM trước khi đọc */
            if (krnl->mram == NULL || regs->a2 >= (addr_t)krnl->mram->maxsz)
               return -1;
            if (MEMPHY_read(krnl->mram, regs->a2, &value) != 0)
               return -1;
            regs->a3 = value;
            break;
   case SYSMEM_IO_WRITE:
            /* Kiểm tra địa chỉ vật lý nằm trong phạm vi MEMRAM trước khi ghi (tránh SIGSEGV) */
            if (krnl->mram == NULL || regs->a2 >= (addr_t)krnl->mram->maxsz)
               return -1;
            if (MEMPHY_write(krnl->mram, regs->a2, regs->a3) != 0)
               return -1;
            break;
   default:
            printf("Memop code: %d\n", memop);
            return -1;
   }
   
   return 0;
}