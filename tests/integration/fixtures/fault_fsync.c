/* LD_PRELOAD fault-injection fixture for tests/integration/test_faults.py (not part of the product).
 * While the file named by $COFFEE_FAULT_FILE exists, directory fsync() calls are faulted according to its content:
 *   "eio"      directory fsync below .../artifacts/outputs fails with EIO
 *   "eio-all"  every directory fsync fails with EIO
 *   "eio-inputs" directory fsync below .../artifacts/inputs fails with EIO (persistent while the file exists)
 *   "slow:N"   directory fsync below .../artifacts/outputs sleeps N ms, then really syncs
 * Everything else forwards to the real fsync. Each injection prints a marker to stderr (the process log). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int fsync(int fd) {
  const char* trigger = getenv("COFFEE_FAULT_FILE");
  struct stat st;
  if (trigger && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    FILE* f = fopen(trigger, "r");
    if (f) {
      char mode[64] = {0};
      if (!fgets(mode, sizeof mode, f)) mode[0] = 0;
      fclose(f);
      char link[64], path[1024] = {0};
      snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
      ssize_t n = readlink(link, path, sizeof path - 1);
      int in_outputs = n > 0 && strstr(path, "/artifacts/outputs") != NULL;
      int in_inputs = n > 0 && strstr(path, "/artifacts/inputs") != NULL;
      if (!strncmp(mode, "eio-inputs", 10)) {
        if (in_inputs) {
          fprintf(stderr, "FAULT-INJECTED directory fsync EIO %s\n", path);
          errno = EIO;
          return -1;
        }
        return (int)syscall(SYS_fsync, fd);
      }
      if (!strncmp(mode, "eio-all", 7) || (!strncmp(mode, "eio", 3) && in_outputs)) {
        fprintf(stderr, "FAULT-INJECTED directory fsync EIO %s\n", path);
        errno = EIO;
        return -1;
      }
      if (!strncmp(mode, "slow:", 5) && in_outputs) {
        fprintf(stderr, "FAULT-INJECTED slow directory fsync %s\n", path);
        usleep((useconds_t)atoi(mode + 5) * 1000);
      }
    }
  }
  return (int)syscall(SYS_fsync, fd);
}
