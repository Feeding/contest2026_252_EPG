/****************************************************************************
 * apps/snap/snap_main.c
 *
 * One-shot camera capture: grab a JPEG frame from the GC2145 and write
 * it to a file (default /mnt/photo.jpg -- mount the SD card first).
 *
 * Usage: snap [path]
 ****************************************************************************/

#include <nuttx/config.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define SNAP_BUFSIZE  (64 * 1024)

int main(int argc, char *argv[])
{
  extern int bk7258_camera_snap(uint8_t *buf, size_t maxlen);
  const char *path = (argc > 1) ? argv[1] : "/mnt/photo.jpg";
  uint8_t *buf;
  int len;
  int fd;
  ssize_t written;

  buf = memalign(32, SNAP_BUFSIZE);
  if (buf == NULL)
    {
      printf("snap: out of memory\n");
      return 1;
    }

  printf("snap: capturing...\n");
  len = bk7258_camera_snap(buf, SNAP_BUFSIZE);
  if (len < 0)
    {
      printf("snap: capture failed: %d\n", len);
      free(buf);
      return 1;
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      printf("snap: cannot open %s\n", path);
      free(buf);
      return 1;
    }

  written = write(fd, buf, len);
  close(fd);
  free(buf);

  if (written != len)
    {
      printf("snap: short write (%d of %d)\n", (int)written, len);
      return 1;
    }

  printf("snap: %d bytes -> %s\n", len, path);
  return 0;
}
