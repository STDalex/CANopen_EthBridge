#include <rt_sys.h>
#include <time.h>

/* The REST formatter uses the full Arm C library. Keep that library from
   issuing semihosting BKPT instructions when no debugger is attached. */
__asm(".global __use_no_semihosting\n");

#define RETARGET_STANDARD_HANDLE 0x100

const char __stdin_name[] = "STDIN";
const char __stdout_name[] = "STDOUT";
const char __stderr_name[] = "STDERR";

FILEHANDLE _sys_open(const char *name, int openmode)
{
  (void)name;
  (void)openmode;
  return RETARGET_STANDARD_HANDLE;
}

int _sys_close(FILEHANDLE handle)
{
  (void)handle;
  return 0;
}

int _sys_write(FILEHANDLE handle, const unsigned char *buffer,
               unsigned length, int mode)
{
  (void)handle;
  (void)buffer;
  (void)length;
  (void)mode;
  return 0;
}

int _sys_read(FILEHANDLE handle, unsigned char *buffer,
              unsigned length, int mode)
{
  (void)handle;
  (void)buffer;
  (void)mode;
  return (int)length;
}

void _ttywrch(int character)
{
  (void)character;
}

int _sys_istty(FILEHANDLE handle)
{
  (void)handle;
  return 1;
}

int _sys_seek(FILEHANDLE handle, long position)
{
  (void)handle;
  (void)position;
  return -1;
}

int _sys_ensure(FILEHANDLE handle)
{
  (void)handle;
  return 0;
}

long _sys_flen(FILEHANDLE handle)
{
  (void)handle;
  return 0L;
}

int _sys_tmpnam2(char *name, int signature, unsigned maximum_length)
{
  (void)name;
  (void)signature;
  (void)maximum_length;
  return -1;
}

char *_sys_command_string(char *command, int length)
{
  if ((command != NULL) && (length > 0))
  {
    command[0] = '\0';
  }
  return command;
}

void _sys_exit(int return_code)
{
  (void)return_code;
  for (;;)
  {
  }
}

time_t time(time_t *result)
{
  time_t value = (time_t)-1;
  if (result != NULL)
  {
    *result = value;
  }
  return value;
}
