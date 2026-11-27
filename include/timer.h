#ifndef TIMER_H
#define TIMER_H

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  struct timer_id_t
  {
    int done;
    int fsh;
  };

  int start_timer (void);

  void stop_timer ();

  struct timer_id_t *attach_event ();

  void detach_event (struct timer_id_t *event);

  void next_slot (struct timer_id_t *timer_id);

  uint64_t current_time ();

#ifdef __cplusplus
}
#endif

#endif
