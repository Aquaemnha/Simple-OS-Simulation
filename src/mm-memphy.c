/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 * Caitoa release. Educational use for course CO2018; see LICENSE.
 */

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
  unsigned char byte;
  int status;

  if (value == NULL)
    return -EFAULT;
  status = sysbus_cpu_read (mp, address, &byte);
  if (status == 0)
    {
      *value = (BYTE) byte;
      if (!mp->rdmflg)
        mp->cursor = (int) address;
    }
  return status;
}

int
MEMPHY_write (struct memphy_struct *mp, addr_t address, BYTE value)
{
  int status = sysbus_cpu_write (mp, address, (unsigned char) value);

  if (status == 0 && !mp->rdmflg)
    mp->cursor = (int) address;
  return status;
}

/* Frame list operations are serialized by mmvm_lock in the memory manager.
   Move existing nodes between the lists so freeing a frame cannot need malloc.
 */
int
MEMPHY_takefp (struct memphy_struct *mp, addr_t number)
{
  struct framephy_struct **link;
  struct framephy_struct *frame;

  if (mp == NULL)
    return -EINVAL;
  for (link = &mp->free_fp_list; *link != NULL; link = &(*link)->fp_next)
    if ((*link)->fpn == number)
      {
        frame = *link;
        *link = frame->fp_next;
        frame->fp_next = mp->used_fp_list;
        mp->used_fp_list = frame;
        return 0;
      }
  return -ENOMEM;
}

int
MEMPHY_get_freefp (struct memphy_struct *mp, addr_t *number)
{
  if (mp == NULL || number == NULL)
    return -EINVAL;
  if (mp->free_fp_list == NULL)
    return -ENOMEM;
  *number = mp->free_fp_list->fpn;
  return MEMPHY_takefp (mp, *number);
}

int
MEMPHY_put_freefp (struct memphy_struct *mp, addr_t number)
{
  struct framephy_struct **link, **position;
  struct framephy_struct *frame;

  if (mp == NULL || number > UINT64_MAX / FRAME_SIZE)
    return -EINVAL;
  if (sysbus_range_pinned (mp, number * FRAME_SIZE, FRAME_SIZE))
    return -EBUSY;
  if (number >= (addr_t) mp->maxsz / FRAME_SIZE)
    return -EINVAL;
  for (link = &mp->used_fp_list; *link != NULL; link = &(*link)->fp_next)
    if ((*link)->fpn == number)
      {
        frame = *link;
        *link = frame->fp_next;
        frame->owner = NULL;
        for (position = &mp->free_fp_list;
             *position != NULL && (*position)->fpn < number;
             position = &(*position)->fp_next)
          ;
        frame->fp_next = *position;
        *position = frame;
        return 0;
      }
  return -EINVAL; /* Includes double free. */
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
  memset (mp, 0, sizeof *mp);
}

int
init_memphy (struct memphy_struct *mp, addr_t size, int random_access)
{
  struct framephy_struct **tail;

  if (mp == NULL || size > INT_MAX)
    return -EINVAL;
  memset (mp, 0, sizeof *mp);
  mp->rdmflg = random_access != 0;
  if (size == 0)
    return 0; /* An unused SWAP device. */
  mp->storage = calloc ((size_t) size, 1);
  if (mp->storage == NULL)
    return -ENOMEM;
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
  if (mp == NULL)
    return -EINVAL;
  for (int i = 0; i < mp->maxsz; i++)
    if (mp->storage[i] != 0)
      printf ("RAM[%d]=%u\n", i, (unsigned char) mp->storage[i]);
  return 0;
}
