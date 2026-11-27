#include "mm64.h"
#include "sysbus.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef MM64
#define FRAME_SIZE PAGING64_PAGESZ
#else
#define FRAME_SIZE PAGING_PAGESZ
#endif

int
MEMPHY_read (struct memphy_struct *mp, addr_t address, BYTE *value)
{
  addr_t effective_address = address;
  int status;

  if (mp == NULL || value == NULL || mp->storage == NULL || mp->maxsz <= 0)
    return -EFAULT;
  if (mp->rdmflg)
    return sysbus_cpu_read (mp, effective_address, (unsigned char *) value);

  pthread_mutex_lock (&mp->cursor_lock);
  if (mp->cursor < 0 || mp->cursor >= mp->maxsz)
    status = -EFAULT;
  else
    {
      effective_address = (addr_t) mp->cursor;
      status = sysbus_cpu_read (mp, effective_address,
                                (unsigned char *) value);
      if (status == 0)
        mp->cursor++;
    }
  pthread_mutex_unlock (&mp->cursor_lock);
  return status;
}

int
MEMPHY_write (struct memphy_struct *mp, addr_t address, BYTE value)
{
  addr_t effective_address = address;
  int status;

  if (mp == NULL || mp->storage == NULL || mp->maxsz <= 0)
    return -EFAULT;
  if (mp->rdmflg)
    return sysbus_cpu_write (mp, effective_address, (unsigned char) value);

  pthread_mutex_lock (&mp->cursor_lock);
  if (mp->cursor < 0 || mp->cursor >= mp->maxsz)
    status = -EFAULT;
  else
    {
      effective_address = (addr_t) mp->cursor;
      status = sysbus_cpu_write (mp, effective_address, (unsigned char) value);
      if (status == 0)
        mp->cursor++;
    }
  pthread_mutex_unlock (&mp->cursor_lock);
  return status;
}

int
MEMPHY_takefp (struct memphy_struct *mp, addr_t number)
{
  struct framephy_struct **link;
  struct framephy_struct *frame;
  int status = -ENOMEM;

  if (mp == NULL || !mp->locks_initialized)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  for (link = &mp->free_fp_list; *link != NULL; link = &(*link)->fp_next)
    if ((*link)->fpn == number)
      {
        frame = *link;
        *link = frame->fp_next;
        frame->fp_next = mp->used_fp_list;
        frame->owner = NULL;
        frame->vaddr = 0;
        frame->pin_count = 0;
        frame->evicting = 0;
        mp->used_fp_list = frame;
        status = 0;
        break;
      }
  pthread_mutex_unlock (&mp->frame_lock);
  return status;
}

int
MEMPHY_get_freefp (struct memphy_struct *mp, addr_t *number)
{
  struct framephy_struct *frame;

  if (mp == NULL || number == NULL || !mp->locks_initialized)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  frame = mp->free_fp_list;
  if (frame == NULL)
    {
      pthread_mutex_unlock (&mp->frame_lock);
      return -ENOMEM;
    }
  mp->free_fp_list = frame->fp_next;
  frame->fp_next = mp->used_fp_list;
  frame->owner = NULL;
  frame->vaddr = 0;
  frame->pin_count = 0;
  frame->evicting = 0;
  mp->used_fp_list = frame;
  *number = frame->fpn;
  pthread_mutex_unlock (&mp->frame_lock);
  return 0;
}

int
MEMPHY_put_freefp (struct memphy_struct *mp, addr_t number)
{
  struct framephy_struct **link, **position;
  struct framephy_struct *frame;

  if (mp == NULL || !mp->locks_initialized || mp->maxsz < 0
      || number > UINT64_MAX / FRAME_SIZE
      || number >= (addr_t) mp->maxsz / FRAME_SIZE)
    return -EINVAL;
  if (sysbus_range_pinned (mp, number * FRAME_SIZE, FRAME_SIZE))
    return -EBUSY;
  pthread_mutex_lock (&mp->frame_lock);
  for (link = &mp->used_fp_list; *link != NULL; link = &(*link)->fp_next)
    if ((*link)->fpn == number)
      {
        frame = *link;
        if (frame->pin_count != 0)
          {
            pthread_mutex_unlock (&mp->frame_lock);
            return -EBUSY;
          }
        *link = frame->fp_next;
        frame->owner = NULL;
        frame->vaddr = 0;
        frame->evicting = 0;
        for (position = &mp->free_fp_list;
             *position != NULL && (*position)->fpn < number;
             position = &(*position)->fp_next)
          ;
        frame->fp_next = *position;
        *position = frame;
        pthread_mutex_unlock (&mp->frame_lock);
        return 0;
      }
  pthread_mutex_unlock (&mp->frame_lock);
  return -EINVAL;
}

int
MEMPHY_get_contiguousfp (struct memphy_struct *mp, addr_t count,
                         addr_t *first_fpn)
{
  struct framephy_struct *candidate;
  int status = -ENOMEM;

  if (mp == NULL || first_fpn == NULL || count == 0
      || !mp->locks_initialized)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  for (candidate = mp->free_fp_list; candidate != NULL;
       candidate = candidate->fp_next)
    {
      struct framephy_struct *scan = candidate;
      addr_t expected = candidate->fpn;
      addr_t found = 0;

      while (scan != NULL && found < count && scan->fpn == expected)
        {
          found++;
          expected++;
          scan = scan->fp_next;
        }
      if (found != count)
        continue;
      *first_fpn = candidate->fpn;
      for (addr_t i = 0; i < count; i++)
        {
          struct framephy_struct **link = &mp->free_fp_list;
          struct framephy_struct *frame;

          while (*link != NULL && (*link)->fpn != *first_fpn + i)
            link = &(*link)->fp_next;
          if (*link == NULL)
            {
              pthread_mutex_unlock (&mp->frame_lock);
              return -EFAULT;
            }
          frame = *link;
          *link = frame->fp_next;
          frame->owner = NULL;
          frame->vaddr = 0;
          frame->pin_count = 0;
          frame->evicting = 0;
          frame->fp_next = mp->used_fp_list;
          mp->used_fp_list = frame;
        }
      status = 0;
      break;
    }
  pthread_mutex_unlock (&mp->frame_lock);
  return status;
}

int
MEMPHY_set_owner (struct memphy_struct *mp, addr_t number,
                  struct mm_struct *owner, addr_t vaddr)
{
  struct framephy_struct *frame;

  if (mp == NULL || !mp->locks_initialized)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  for (frame = mp->used_fp_list; frame != NULL; frame = frame->fp_next)
    if (frame->fpn == number)
      {
        frame->owner = owner;
        frame->vaddr = vaddr;
        pthread_mutex_unlock (&mp->frame_lock);
        return 0;
      }
  pthread_mutex_unlock (&mp->frame_lock);
  return -ENOENT;
}

int
MEMPHY_get_frame_info (struct memphy_struct *mp, addr_t number,
                       struct mm_struct **owner, addr_t *vaddr)
{
  struct framephy_struct *frame;

  if (mp == NULL || owner == NULL || vaddr == NULL || !mp->locks_initialized)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  for (frame = mp->used_fp_list; frame != NULL; frame = frame->fp_next)
    if (frame->fpn == number)
      {
        *owner = frame->owner;
        *vaddr = frame->vaddr;
        pthread_mutex_unlock (&mp->frame_lock);
        return 0;
      }
  pthread_mutex_unlock (&mp->frame_lock);
  return -ENOENT;
}

int
MEMPHY_find_victim (struct memphy_struct *mp, addr_t *number,
                    struct mm_struct **owner, addr_t *vaddr)
{
  struct framephy_struct *frame;
  addr_t *candidates;
  size_t capacity, count = 0;

  if (mp == NULL || number == NULL || owner == NULL || vaddr == NULL
      || !mp->locks_initialized || mp->maxsz <= 0)
    return -EINVAL;
  capacity = (size_t) mp->maxsz / FRAME_SIZE;
  if (capacity == 0 || capacity > SIZE_MAX / sizeof *candidates)
    return -ENOMEM;
  candidates = calloc (capacity, sizeof *candidates);
  if (candidates == NULL)
    return -ENOMEM;
  pthread_mutex_lock (&mp->frame_lock);
  for (frame = mp->used_fp_list; frame != NULL && count < capacity;
       frame = frame->fp_next)
    if (frame->owner != NULL && frame->pin_count == 0 && !frame->evicting
        && !frame->owner->kernel_space)
      candidates[count++] = frame->fpn;
  pthread_mutex_unlock (&mp->frame_lock);
  for (size_t i = 0; i < count; i++)
    {
      addr_t candidate = candidates[i];
      int pinned = sysbus_range_pinned (mp, candidate * FRAME_SIZE,
                                        FRAME_SIZE);

      if (pinned)
        continue;
      pthread_mutex_lock (&mp->frame_lock);
      for (frame = mp->used_fp_list; frame != NULL; frame = frame->fp_next)
        if (frame->fpn == candidate)
          break;
      if (frame != NULL && frame->owner != NULL && frame->pin_count == 0
          && !frame->evicting && !frame->owner->kernel_space)
        {
          frame->evicting = 1;
          *number = frame->fpn;
          *owner = frame->owner;
          *vaddr = frame->vaddr;
          pthread_mutex_unlock (&mp->frame_lock);
          free (candidates);
          return 0;
        }
      pthread_mutex_unlock (&mp->frame_lock);
    }
  free (candidates);
  return -ENOMEM;
}

int
MEMPHY_cancel_victim (struct memphy_struct *mp, addr_t number)
{
  struct framephy_struct *frame;

  if (mp == NULL || !mp->locks_initialized)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  for (frame = mp->used_fp_list; frame != NULL; frame = frame->fp_next)
    if (frame->fpn == number)
      {
        frame->evicting = 0;
        pthread_mutex_unlock (&mp->frame_lock);
        return 0;
      }
  pthread_mutex_unlock (&mp->frame_lock);
  return -ENOENT;
}

int
MEMPHY_reserve_victim (struct memphy_struct *mp, addr_t number)
{
  struct framephy_struct *frame;

  if (mp == NULL || !mp->locks_initialized
      || number >= (addr_t) mp->maxsz / FRAME_SIZE)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  for (frame = mp->used_fp_list; frame != NULL; frame = frame->fp_next)
    if (frame->fpn == number)
      break;
  if (frame == NULL || frame->owner == NULL || frame->owner->kernel_space
      || frame->pin_count != 0 || frame->evicting)
    {
      pthread_mutex_unlock (&mp->frame_lock);
      return -EBUSY;
    }
  frame->evicting = 1;
  pthread_mutex_unlock (&mp->frame_lock);
  if (sysbus_range_pinned (mp, number * FRAME_SIZE, FRAME_SIZE))
    {
      MEMPHY_cancel_victim (mp, number);
      return -EBUSY;
    }
  return 0;
}

int
MEMPHY_pin_frame (struct memphy_struct *mp, addr_t number)
{
  struct framephy_struct *frame;

  if (mp == NULL || !mp->locks_initialized)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  for (frame = mp->used_fp_list; frame != NULL; frame = frame->fp_next)
    if (frame->fpn == number)
      {
        if (frame->evicting)
          {
            pthread_mutex_unlock (&mp->frame_lock);
            return -EBUSY;
          }
        if (frame->pin_count == UINT_MAX)
          {
            pthread_mutex_unlock (&mp->frame_lock);
            return -EOVERFLOW;
          }
        frame->pin_count++;
        pthread_mutex_unlock (&mp->frame_lock);
        return 0;
      }
  pthread_mutex_unlock (&mp->frame_lock);
  return -ENOENT;
}

int
MEMPHY_unpin_frame (struct memphy_struct *mp, addr_t number)
{
  struct framephy_struct *frame;

  if (mp == NULL || !mp->locks_initialized)
    return -EINVAL;
  pthread_mutex_lock (&mp->frame_lock);
  for (frame = mp->used_fp_list; frame != NULL; frame = frame->fp_next)
    if (frame->fpn == number)
      {
        if (frame->pin_count == 0)
          {
            pthread_mutex_unlock (&mp->frame_lock);
            return -EINVAL;
          }
        frame->pin_count--;
        pthread_mutex_unlock (&mp->frame_lock);
        return 0;
      }
  pthread_mutex_unlock (&mp->frame_lock);
  return -ENOENT;
}

void
destroy_memphy (struct memphy_struct *mp)
{
  struct framephy_struct *frame;

  if (mp == NULL)
    return;
  while ((frame = mp->free_fp_list) != NULL)
    {
      mp->free_fp_list = frame->fp_next;
      free (frame);
    }
  while ((frame = mp->used_fp_list) != NULL)
    {
      mp->used_fp_list = frame->fp_next;
      free (frame);
    }
  free (mp->storage);
  if (mp->locks_initialized)
    {
      pthread_mutex_destroy (&mp->cursor_lock);
      pthread_mutex_destroy (&mp->frame_lock);
    }
  memset (mp, 0, sizeof *mp);
}

int
init_memphy (struct memphy_struct *mp, addr_t size, int random_access)
{
  struct framephy_struct **tail;
  int status;

  if (mp == NULL || size > INT_MAX)
    return -EINVAL;
  memset (mp, 0, sizeof *mp);
  status = pthread_mutex_init (&mp->frame_lock, NULL);
  if (status != 0)
    return -status;
  status = pthread_mutex_init (&mp->cursor_lock, NULL);
  if (status != 0)
    {
      pthread_mutex_destroy (&mp->frame_lock);
      return -status;
    }
  mp->locks_initialized = 1;
  mp->rdmflg = random_access != 0;
  if (size == 0)
    return 0;
  mp->storage = calloc ((size_t) size, 1);
  if (mp->storage == NULL)
    {
      destroy_memphy (mp);
      return -ENOMEM;
    }
  mp->maxsz = (int) size;
  tail = &mp->free_fp_list;
  for (addr_t i = 0; i < size / FRAME_SIZE; i++)
    {
      *tail = calloc (1, sizeof **tail);
      if (*tail == NULL)
        {
          destroy_memphy (mp);
          return -ENOMEM;
        }
      (*tail)->fpn = i;
      tail = &(*tail)->fp_next;
    }
  return 0;
}

int
MEMPHY_dump (struct memphy_struct *mp)
{
  if (mp == NULL || mp->storage == NULL || mp->maxsz < 0)
    return -EINVAL;
  for (int i = 0; i < mp->maxsz; i++)
    {
      BYTE value;

      if (MEMPHY_read (mp, (addr_t) i, &value) != 0)
        return -EFAULT;
      if (value != 0)
        printf ("MEMPHY[%d]=%u\n", i, (unsigned char) value);
    }
  return 0;
}
