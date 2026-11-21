/* Link-time fault injection; never linked into the submitted OS executable. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

int __real_pthread_create (pthread_t *, const pthread_attr_t *,
                           void *(*) (void *), void *);

int
__wrap_pthread_create (pthread_t *thread, const pthread_attr_t *attributes,
                       void *(*routine) (void *), void *argument)
{
  static atomic_uint calls;
  const char *setting = getenv ("OS_TEST_FAIL_THREADS");
  unsigned int index = atomic_fetch_add (&calls, 1);
  unsigned long mask = setting == NULL ? 0 : strtoul (setting, NULL, 10);

  if (index < sizeof mask * 8 && (mask & (1UL << index)))
    return EAGAIN;
  return __real_pthread_create (thread, attributes, routine, argument);
}
