/****************************************************************************
 * apps/face/face_main.c
 *
 * The robot face, all vendor art: NEUTRAL.AVI loops as the idle state
 * and S2 cuts straight to the next expression clip -- pressed at any
 * moment, including mid-clip.  Every frame is a 320x160 MJPEG from the
 * SD card, software-decoded (TJpgDec) and split down the middle onto
 * the two 160x160 panels.  S3 quits.
 *
 * Usage: face [expression-name]   e.g. "face HAPPY" plays one clip;
 *        plain "face" runs the interactive demo.
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/video/fb.h>
#include <nuttx/input/buttons.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tjpgd.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EYE_XRES   160
#define EYE_YRES   160
#define AVI_W      320

#define CHUNK_MAX  (64 * 1024)
#define TJPGD_POOL 16384

#define BTN_S2     (1u << 1)
#define BTN_S3     (1u << 2)

/* play_avi() outcomes */

#define PLAY_END   0                   /* clip finished naturally */
#define PLAY_QUIT  1                   /* S3: leave the program */
#define PLAY_NEXT  2                   /* S2: cut to the next clip */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct eye_fb_s
{
  int fd;
  uint16_t *mem;
  size_t len;
  int stride_px;
};

struct jpeg_src_s
{
  const uint8_t *data;
  size_t size;
  size_t pos;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *g_exprs[] =
{
  "HAPPY", "CURIOUS", "LOVE", "SURPRISE", "THINKING",
  "SAD", "SLEEPY", "ANGRY"
};

#define NEXPRS (sizeof(g_exprs) / sizeof(g_exprs[0]))

static struct eye_fb_s g_eye[2];
static btn_buttonset_t g_btn_last;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int eye_open(struct eye_fb_s *e, const char *path)
{
  struct fb_planeinfo_s pinfo;

  e->fd = open(path, O_RDWR);
  if (e->fd < 0)
    {
      printf("face: cannot open %s\n", path);
      return -1;
    }

  if (ioctl(e->fd, FBIOGET_PLANEINFO, (unsigned long)(uintptr_t)&pinfo) < 0)
    {
      close(e->fd);
      return -1;
    }

  e->len = pinfo.fblen;
  e->stride_px = pinfo.stride / 2;
  e->mem = mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FILE, e->fd, 0);
  return (e->mem == MAP_FAILED) ? -1 : 0;
}

static void eye_flush_full(struct eye_fb_s *e)
{
  struct fb_area_s area;

  area.x = 0;
  area.y = 0;
  area.w = EYE_XRES;
  area.h = EYE_YRES;
  ioctl(e->fd, FBIO_UPDATE, (unsigned long)(uintptr_t)&area);
}

/****************************************************************************
 * Name: btn_event
 *
 * Description:
 *   Poll the buttons and report rising edges: PLAY_QUIT for S3,
 *   PLAY_NEXT for S2, 0 for nothing new.  Kept as the single reader of
 *   button state so no press is ever swallowed between modes.
 *
 ****************************************************************************/

static int btn_event(int btnfd)
{
  btn_buttonset_t btn = 0;
  int ev = 0;

  if (btnfd < 0 ||
      read(btnfd, &btn, sizeof(btn)) != sizeof(btn))
    {
      return 0;
    }

  if ((btn & BTN_S3) != 0 && (g_btn_last & BTN_S3) == 0)
    {
      ev = PLAY_QUIT;
    }
  else if ((btn & BTN_S2) != 0 && (g_btn_last & BTN_S2) == 0)
    {
      ev = PLAY_NEXT;
    }

  g_btn_last = btn;
  return ev;
}

/****************************************************************************
 * MJPEG decode plumbing
 ****************************************************************************/

static size_t jpeg_in(JDEC *jd, uint8_t *buf, size_t len)
{
  struct jpeg_src_s *src = (struct jpeg_src_s *)jd->device;

  if (src->pos + len > src->size)
    {
      len = src->size - src->pos;
    }

  if (buf != NULL)
    {
      memcpy(buf, src->data + src->pos, len);
    }

  src->pos += len;
  return len;
}

/* Frame columns 0-159 land on the viewer-left panel (fb1), 160-319 on
 * the viewer-right one (fb0).
 */

static int jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
  const uint16_t *src = (const uint16_t *)bitmap;
  int w = rect->right - rect->left + 1;
  int y;
  int x;

  for (y = rect->top; y <= rect->bottom && y < EYE_YRES; y++)
    {
      for (x = rect->left; x <= rect->right && x < AVI_W; x++)
        {
          uint16_t px = src[(y - rect->top) * w + (x - rect->left)];

          if (x < EYE_XRES)
            {
              g_eye[1].mem[y * g_eye[1].stride_px + x] = px;
            }
          else
            {
              g_eye[0].mem[y * g_eye[0].stride_px + (x - EYE_XRES)] = px;
            }
        }
    }

  return 1;
}

/****************************************************************************
 * Name: play_avi
 *
 * Description:
 *   Stream one MJPEG AVI onto both panels, checking the buttons after
 *   every frame.  Returns PLAY_END / PLAY_QUIT / PLAY_NEXT, or a
 *   negative value when the file cannot be played at all.
 *
 ****************************************************************************/

static int play_avi(const char *name, int btnfd)
{
  char path[48];
  /* The decode pool must be SRAM: TJpgDec hammers it with byte-wise
   * random access, and a heap allocation that lands in PSRAM makes
   * every frame ~20x slower (2 s/frame, measured).
   */

  static uint8_t pool[TJPGD_POOL];

  uint8_t *chunk = NULL;
  int fd;
  int ret = PLAY_END;
  uint32_t hdr[3];

  snprintf(path, sizeof(path), "/mnt/%s.AVI", name);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -1;
    }

  chunk = malloc(CHUNK_MAX);
  if (chunk == NULL)
    {
      ret = -1;
      goto out;
    }

  if (read(fd, hdr, 12) != 12)
    {
      ret = -1;
      goto out;
    }

  for (; ; )
    {
      uint32_t fcc;
      uint32_t size;

      if (read(fd, hdr, 8) != 8)
        {
          goto out;                    /* natural end of clip */
        }

      fcc  = hdr[0];
      size = hdr[1];

      if (fcc == 0x5453494c)           /* "LIST" */
        {
          uint32_t ltype;

          if (read(fd, &ltype, 4) != 4)
            {
              goto out;
            }

          if (ltype == 0x69766f6d)     /* "movi": descend, do not skip */
            {
              continue;
            }

          lseek(fd, size - 4 + (size & 1), SEEK_CUR);
        }
      else if (fcc == 0x63643030)      /* "00dc": one MJPEG frame */
        {
          uint32_t t0 = now_ms();
          int ev;

          if (size > CHUNK_MAX)
            {
              lseek(fd, size + (size & 1), SEEK_CUR);
              continue;
            }

          if (read(fd, chunk, size) != (ssize_t)size)
            {
              goto out;
            }

          if ((size & 1) != 0)
            {
              lseek(fd, 1, SEEK_CUR);
            }

            {
              struct jpeg_src_s src =
              {
                .data = chunk, .size = size, .pos = 0
              };

              JDEC jd;

              if (jd_prepare(&jd, jpeg_in, pool, TJPGD_POOL, &src) == JDR_OK)
                {
                  jd_decomp(&jd, jpeg_out, 0);
                }
            }

          eye_flush_full(&g_eye[0]);
          eye_flush_full(&g_eye[1]);


          ev = btn_event(btnfd);
          if (ev != 0)
            {
              ret = ev;
              goto out;
            }

            {
              uint32_t spent = now_ms() - t0;

              if (spent < 50)
                {
                  usleep((50 - spent) * 1000);
                }
            }
        }
      else
        {
          lseek(fd, size + (size & 1), SEEK_CUR);
        }
    }

out:
  free(chunk);
  close(fd);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  int btnfd;
  int cur = -1;                        /* -1 = NEUTRAL idle loop */
  int i;

  if (eye_open(&g_eye[0], "/dev/fb0") < 0 ||
      eye_open(&g_eye[1], "/dev/fb1") < 0)
    {
      return 1;
    }

  mount("/dev/mmcsd0", "/mnt", "vfat", 0, NULL);

  btnfd = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
  g_btn_last = 0;

  if (argc > 1 && strcmp(argv[1], "BENCH") == 0)
    {
      /* Fetch-path discriminator: the same tight countdown loop timed
       * once running from XIP flash and once from a copy in SRAM.
       * subs r0,#1 ; bne .-2 ; bx lr
       */

      static uint16_t code[4] __attribute__((aligned(4))) =
      {
        0x3801, 0xd1fd, 0x4770, 0xbf00
      };

      uint32_t (*fn)(uint32_t);
      volatile uint32_t n;
      uint32_t t0;
      uint32_t t_flash;
      uint32_t t_sram;

      t0 = now_ms();
      for (n = 10000000; n > 0; n--)
        {
        }

      t_flash = now_ms() - t0;

      fn = (uint32_t (*)(uint32_t))((uintptr_t)code | 1);
      t0 = now_ms();
      fn(10000000);
      t_sram = now_ms() - t0;

      printf("face: bench flash-loop=%lums sram-loop=%lums\n",
             (unsigned long)t_flash, (unsigned long)t_sram);
      return 0;
    }

  if (argc > 1)
    {
      if (play_avi(argv[1], btnfd) < 0)
        {
          printf("face: cannot play %s\n", argv[1]);
        }

      return 0;
    }

  printf("face: NEUTRAL idle; S2 = next expression, S3 = quit\n");

  for (; ; )
    {
      const char *name = (cur < 0) ? "NEUTRAL" : g_exprs[cur];
      int r = play_avi(name, btnfd);

      if (r == PLAY_QUIT)
        {
          break;
        }
      else if (r == PLAY_NEXT)
        {
          cur = (cur < 0) ? 0 : (int)((cur + 1) % NEXPRS);
          printf("face: %s\n", g_exprs[cur]);
        }
      else if (r == PLAY_END)
        {
          if (cur >= 0)
            {
              cur = -1;                /* expression over: back to idle */
            }
        }
      else
        {
          printf("face: cannot play %s, quitting\n", name);
          break;
        }
    }

  for (i = 0; i < 2; i++)
    {
      munmap(g_eye[i].mem, g_eye[i].len);
      close(g_eye[i].fd);
    }

  if (btnfd >= 0)
    {
      close(btnfd);
    }

  return 0;
}
