#include "loader.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint32_t next_pid = 1;

void
unload (struct pcb_t *process)
{
  if (process == NULL)
    return;
  if (process->code != NULL)
    {
      free (process->code->text);
      free (process->code);
    }
  free (process->page_table);
  free (process);
}

struct pcb_t *
load (const char *path)
{
  const char *names[] = { "calc",           "alloc",        "free",
                          "read",           "write",        "kmalloc",
                          "copy_from_user", "copy_to_user", "dma_memcpy",
                          "syscall" };
  const int arguments[] = { 0, 2, 1, 3, 3, 2, 4, 4, 4, 4 };
  struct pcb_t *process = NULL;
  FILE *file = fopen (path, "r");
  char line[256];
  char *token, *end;
  uint64_t header[2];

  if (file == NULL)
    return NULL;
  process = calloc (1, sizeof *process);
  if (process == NULL)
    goto fail;
  process->code = calloc (1, sizeof *process->code);
  if (process->code == NULL)
    goto fail;
  if (fgets (line, sizeof line, file) == NULL)
    goto fail;
  token = strtok (line, " \t\r\n");
  for (int i = 0; i < 2; i++)
    {
      if (token == NULL || token[0] < '0' || token[0] > '9')
        goto fail;
      errno = 0;
      header[i] = strtoull (token, &end, 10);
      if (errno != 0 || *end != '\0')
        goto fail;
      token = strtok (NULL, " \t\r\n");
    }
  if (token != NULL || header[0] >= MAX_PRIO || header[1] > 1000000)
    goto fail;
  process->priority = (uint32_t) header[0];
  process->code->size = (uint32_t) header[1];
  if (process->code->size != 0)
    {
      process->code->text
          = calloc (process->code->size, sizeof *process->code->text);
      if (process->code->text == NULL)
        goto fail;
    }
  for (uint32_t i = 0; i < process->code->size; i++)
    {
      char *token, *end;
      int opcode;
      arg_t values[4] = { 0 };
      struct inst_t *instruction = &process->code->text[i];

      if (fgets (line, sizeof line, file) == NULL)
        goto fail;
      token = strtok (line, " \t\r\n");
      if (token == NULL)
        goto fail;
      for (opcode = CALC; opcode <= SYSCALL; opcode++)
        if (strcmp (token, names[opcode]) == 0)
          break;
      if (opcode > SYSCALL)
        goto fail;
      instruction->opcode = opcode;
      for (int j = 0; j < arguments[opcode]; j++)
        {
          token = strtok (NULL, " \t\r\n");
          if (token == NULL && opcode == SYSCALL && j > 0)
            break;
          if (token == NULL || token[0] == '-')
            goto fail;
          errno = 0;
          values[j] = strtoull (token, &end, 10);
          if (errno != 0 || end == token || *end != '\0')
            goto fail;
        }
      if (strtok (NULL, " \t\r\n") != NULL)
        goto fail;
      instruction->arg_0 = values[0];
      instruction->arg_1 = values[1];
      instruction->arg_2 = values[2];
      instruction->arg_3 = values[3];
    }
  while (fgets (line, sizeof line, file) != NULL)
    if (strspn (line, " \t\r\n") != strlen (line))
      goto fail;
  fclose (file);
  process->pid = next_pid++;
  snprintf (process->path, sizeof process->path, "%s", path);
  return process;
fail:
  fprintf (stderr, "Cannot load process: %s\n", path);
  fclose (file);
  unload (process);
  return NULL;
}
