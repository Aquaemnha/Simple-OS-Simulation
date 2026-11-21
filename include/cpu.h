#ifndef CPU_H
#define CPU_H

#include "common.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Successful instructions advance PC. -EBUSY/-EAGAIN leave PC unchanged so
     the scheduler can run another process and retry later. Other errors fail.
   */
  int run (struct pcb_t *proc);

#ifdef __cplusplus
}
#endif

#endif
