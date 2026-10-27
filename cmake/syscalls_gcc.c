#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

extern char __HeapBase;
extern char __HeapLimit;

void *_sbrk(ptrdiff_t increment)
{
  static char *current_break;
  char *previous_break;

  if (current_break == NULL)
  {
    current_break = &__HeapBase;
  }

  previous_break = current_break;
  if ((increment > 0) && (current_break + increment > &__HeapLimit))
  {
    errno = ENOMEM;
    return (void *)-1;
  }

  if ((increment < 0) && (current_break + increment < &__HeapBase))
  {
    errno = EINVAL;
    return (void *)-1;
  }

  current_break += increment;
  return previous_break;
}

int _write(int file, const char *buffer, int length)
{
  (void)file;
  (void)buffer;
  return length;
}

int _read(int file, char *buffer, int length)
{
  (void)file;
  (void)buffer;
  (void)length;
  return 0;
}

int _close(int file)
{
  (void)file;
  return -1;
}

int _fstat(int file, struct stat *status)
{
  (void)file;
  status->st_mode = S_IFCHR;
  return 0;
}

int _isatty(int file)
{
  (void)file;
  return 1;
}

int _lseek(int file, int offset, int origin)
{
  (void)file;
  (void)offset;
  (void)origin;
  return 0;
}

int _getpid(void)
{
  return 1;
}

int _kill(int pid, int signal)
{
  (void)pid;
  (void)signal;
  errno = EINVAL;
  return -1;
}

void _exit(int status)
{
  (void)status;
  for (;;)
  {
  }
}
