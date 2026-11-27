 





#include "common.h"
#include "sysbus.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

 

static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *storage;
static size_t capacity;
static struct memphy_struct *bound_ram;
static int owns_storage;
static int reserved;
static uint64_t dma_source;
static uint64_t dma_destination;
static uint64_t dma_length;
static int bus_error;
static uint64_t cpu_read_bytes;
static uint64_t cpu_write_bytes;
static uint64_t dma_read_bytes;
static uint64_t dma_write_bytes;

 
static int
range_valid (uint64_t address, uint64_t length)
{
  return storage != NULL && address <= capacity
         && length <= capacity - address;
}

 
static int
overlaps (uint64_t a, uint64_t n, uint64_t b, uint64_t m)
{
  if (n == 0 || m == 0)
    return 0;
  return a <= b ? b - a < n : a - b < m;
}

memory_bank_t
memory_bank_init (size_t size_bytes)
{
  unsigned char *data;

  if (size_bytes == 0)
    return MEMORY_BANK_INVALID;
  data = calloc (size_bytes, 1);
  if (data == NULL)
    return MEMORY_BANK_INVALID;
  pthread_mutex_lock (&memory_lock);
  if (storage != NULL)
    {
      pthread_mutex_unlock (&memory_lock);
      free (data);
      return MEMORY_BANK_INVALID;
    }
  storage = data;
  capacity = size_bytes;
  owns_storage = 1;
  pthread_mutex_unlock (&memory_lock);
  return 0;
}

int
sysbus_bind_ram (struct memphy_struct *ram)
{
  int status = 0;

  if (ram == NULL || ram->storage == NULL || ram->maxsz <= 0)
    return -EINVAL;
  pthread_mutex_lock (&memory_lock);
  if (storage != NULL)
    status = -EBUSY;
  else
    {
      storage = (unsigned char *) ram->storage;
      capacity = (size_t) ram->maxsz;
      bound_ram = ram;
      owns_storage = 0;
    }
  pthread_mutex_unlock (&memory_lock);
  return status;
}

int
memory_bank_cleanup (void)
{
  pthread_mutex_lock (&memory_lock);
  if (reserved)
    {
      pthread_mutex_unlock (&memory_lock);
      return -EBUSY;
    }
  if (owns_storage)
    free (storage);
  storage = NULL;
  capacity = 0;
  bound_ram = NULL;
  owns_storage = 0;
  pthread_mutex_unlock (&memory_lock);
  return 0;
}

int
memory_bank_is_available (void)
{
  int available;

  pthread_mutex_lock (&memory_lock);
  available = storage != NULL;
  pthread_mutex_unlock (&memory_lock);
  return available;
}

size_t
memory_bank_get_size (void)
{
  size_t size;

  pthread_mutex_lock (&memory_lock);
  size = capacity;
  pthread_mutex_unlock (&memory_lock);
  return size;
}

void *
memory_bank_get_ptr (uint64_t offset)
{
  void *pointer = NULL;

  pthread_mutex_lock (&memory_lock);
  if (range_valid (offset, 1) && !reserved)
    pointer = storage + offset;
  pthread_mutex_unlock (&memory_lock);
  return pointer;
}

int
sysbus_dma_reserve (uint64_t source, uint64_t destination, uint64_t length)
{
  int status = 0;

  pthread_mutex_lock (&memory_lock);
  if (reserved)
    status = -EBUSY;
  else if (length == 0 || !range_valid (source, length)
           || !range_valid (destination, length))
    status = -EFAULT;
  else if (overlaps (source, length, destination, length))
    status = -EINVAL;
  else
    {
      reserved = 1;
      dma_source = source;
      dma_destination = destination;
      dma_length = length;
      bus_error = 0;
    }
  pthread_mutex_unlock (&memory_lock);
  return status;
}

void
sysbus_dma_release (void)
{
  pthread_mutex_lock (&memory_lock);
  reserved = 0;
  pthread_mutex_unlock (&memory_lock);
}

int
sysbus_dma_error (void)
{
  int status;

  pthread_mutex_lock (&memory_lock);
  status = bus_error;
  pthread_mutex_unlock (&memory_lock);
  return status;
}

int
sysbus_range_pinned (struct memphy_struct *ram, uint64_t address,
                     uint64_t length)
{
  int pinned;

  pthread_mutex_lock (&memory_lock);
  pinned = ram == bound_ram && reserved
           && (overlaps (address, length, dma_source, dma_length)
               || overlaps (address, length, dma_destination, dma_length));
  pthread_mutex_unlock (&memory_lock);
  return pinned;
}

int
sysbus_cpu_read (struct memphy_struct *ram, uint64_t address,
                 unsigned char *value)
{
  int status = 0;

  if (ram == NULL || value == NULL || ram->storage == NULL || ram->maxsz <= 0
      || address >= (uint64_t) ram->maxsz)
    return -EFAULT;
  pthread_mutex_lock (&memory_lock);
  if (ram == bound_ram && reserved
      && overlaps (address, 1, dma_destination, dma_length))
    status = -EBUSY;
  else
    {
      *value = (unsigned char) ram->storage[address];
      cpu_read_bytes++;
    }
  pthread_mutex_unlock (&memory_lock);
  return status;
}

int
sysbus_cpu_write (struct memphy_struct *ram, uint64_t address,
                  unsigned char value)
{
  int status = 0;

  if (ram == NULL || ram->storage == NULL || ram->maxsz <= 0
      || address >= (uint64_t) ram->maxsz)
    return -EFAULT;
  pthread_mutex_lock (&memory_lock);
  if (ram == bound_ram && reserved
      && (overlaps (address, 1, dma_source, dma_length)
          || overlaps (address, 1, dma_destination, dma_length)))
    status = -EBUSY;
  else
    {
      ram->storage[address] = (char) value;
      cpu_write_bytes++;
    }
  pthread_mutex_unlock (&memory_lock);
  return status;
}

int
dpi_bank_read32_legacy (long long offset)
{
  uint32_t word = 0;
  int result;

  pthread_mutex_lock (&memory_lock);
  if (offset < 0 || !range_valid ((uint64_t) offset, 4))
    bus_error = -EFAULT;
  else
    {
      for (unsigned int i = 0; i < 4; i++)
        word |= (uint32_t) storage[(uint64_t) offset + i] << (8 * i);
      dma_read_bytes += 4;
    }
  pthread_mutex_unlock (&memory_lock);
   
  memcpy (&result, &word, sizeof result);
  return result;
}

int
dpi_bank_write32_legacy (long long offset, int value)
{
  int status = 0;
  uint32_t word = (uint32_t) value;

  pthread_mutex_lock (&memory_lock);
  if (offset < 0 || !range_valid ((uint64_t) offset, 4))
    status = bus_error = -EFAULT;
  else
    {
      for (unsigned int i = 0; i < 4; i++)
        storage[(uint64_t) offset + i] = (unsigned char) (word >> (8 * i));
      dma_write_bytes += 4;
    }
  pthread_mutex_unlock (&memory_lock);
  return status;
}

int
sysbus_get_access_counts (uint64_t *cpu_reads, uint64_t *cpu_writes,
                          uint64_t *dma_reads, uint64_t *dma_writes)
{
  if (cpu_reads == NULL || cpu_writes == NULL || dma_reads == NULL
      || dma_writes == NULL)
    return -EINVAL;
  pthread_mutex_lock (&memory_lock);
  *cpu_reads = cpu_read_bytes;
  *cpu_writes = cpu_write_bytes;
  *dma_reads = dma_read_bytes;
  *dma_writes = dma_write_bytes;
  pthread_mutex_unlock (&memory_lock);
  return 0;
}

void
sysbus_reset_access_counts (void)
{
  pthread_mutex_lock (&memory_lock);
  cpu_read_bytes = 0;
  cpu_write_bytes = 0;
  dma_read_bytes = 0;
  dma_write_bytes = 0;
  pthread_mutex_unlock (&memory_lock);
}
