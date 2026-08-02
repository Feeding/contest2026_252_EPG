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
#include <nuttx/input/buttons.h>

#include <sys/mount.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>



/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EYE_XRES   160
#define EYE_YRES   160
#define AVI_W      320

#define CHUNK_MAX  (64 * 1024)

#define BTN_S2     (1u << 1)
#define BTN_S3     (1u << 2)

/* play_avi() outcomes */

#define PLAY_END   0                   /* clip finished naturally */
#define PLAY_QUIT  1                   /* S3: leave the program */
#define PLAY_NEXT  2                   /* S2: cut to the next clip */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *g_exprs[] =
{
  "HAPPY", "CURIOUS", "LOVE", "SURPRISE", "THINKING",
  "SAD", "SLEEPY", "ANGRY"
};

#define NEXPRS (sizeof(g_exprs) / sizeof(g_exprs[0]))

static btn_buttonset_t g_btn_last;
static uint8_t *g_yuyv;
static uint8_t *g_rgbl;
static uint8_t *g_rgbr;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
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
  /* Raw GPIO poll (the KEYS-probe path, proven on hardware; the
   * /dev/buttons route silently delivered nothing during playback).
   * Debounced by consecutive frames: a single glitched sample can
   * neither switch nor -- worse -- quit.  power (GPIO12) = next
   * expression at 2 frames; >> (GPIO8) = quit at 3 frames (~135 ms).
   */

  extern bool bk7258_gpio_read(int pin);
  static int s2_cnt;
  static int s3_cnt;
  int ev = 0;

  if (!bk7258_gpio_read(8))
    {
      if (++s3_cnt == 3)
        {
          ev = PLAY_QUIT;
        }
    }
  else
    {
      s3_cnt = 0;
    }

  if (!bk7258_gpio_read(12))
    {
      if (++s2_cnt == 2 && ev == 0)
        {
          ev = PLAY_NEXT;
        }
    }
  else
    {
      s2_cnt = 0;
    }

  return ev;
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
              extern int bk7258_jpegdec_decode(const uint8_t *jpg,
                                               size_t len, uint8_t *out,
                                               int width, int height);

              extern int bk7258_dma2d_split(const void *src, void *dstl,
                                            void *dstr);
              extern int bk7258_gc9d01_blast_pair(const void *right,
                                                  const void *left);

              if (bk7258_jpegdec_decode(chunk, size, g_yuyv,
                                        AVI_W, EYE_YRES) == 0 &&
                  bk7258_dma2d_split(g_yuyv, g_rgbl, g_rgbr) == 0)
                {
                  bk7258_gc9d01_blast_pair(g_rgbr, g_rgbl);
                }
            }

          ev = btn_event(btnfd);
          if (ev != 0)
            {
              ret = ev;
              goto out;
            }

            {
              uint32_t spent = now_ms() - t0;

              if (spent < 33)
                {
                  usleep((33 - spent) * 1000);
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

  mount("/dev/mmcsd0", "/mnt", "vfat", 0, NULL);

  g_yuyv = malloc(AVI_W * EYE_YRES * 2 + 64);
  g_rgbl = memalign(4, EYE_XRES * EYE_YRES * 2);
  g_rgbr = memalign(4, EYE_XRES * EYE_YRES * 2);
  if (g_yuyv == NULL || g_rgbl == NULL || g_rgbr == NULL)
    {
      return 1;
    }

  btnfd = open("/dev/buttons", O_RDONLY | O_NONBLOCK);
  g_btn_last = 0;

    {
      extern void bk7258_gpio_config(int pin, bool output, bool pullup,
                                     bool pulldown);

      bk7258_gpio_config(12, false, true, false);
      bk7258_gpio_config(8, false, true, false);
      printf("face: btnfd=%d (diagnostic; raw GPIO poll is primary)\n",
             btnfd);
    }

  if (argc > 1 && strcmp(argv[1], "CAL") == 0)
    {
      /* Color calibration: decode NEUTRAL frame 1 (host knows its
       * ground truth: cream background RGB565 ~0xde96), then run the
       * DMA2D with every input-format interpretation and print sample
       * pixels.  The right combo identifies itself.
       */

      extern int bk7258_jpegdec_decode(const uint8_t *jpg, size_t len,
                                       uint8_t *out, int w, int h);
      extern int bk7258_dma2d_split(const void *src, void *dstl,
                                    void *dstr);
      extern void bk7258_dma2d_set_fgcfg(uint32_t fmt2, int reve);

      uint8_t *chunk = malloc(CHUNK_MAX);
      int fd = open("/mnt/NEUTRAL.AVI", O_RDONLY);
      uint32_t hdr[3];
      uint32_t fmt;
      int reve;

      if (fd < 0 || chunk == NULL)
        {
          return 1;
        }

      read(fd, hdr, 12);
      for (; ; )
        {
          if (read(fd, hdr, 8) != 8) return 1;
          if (hdr[0] == 0x5453494c)
            {
              uint32_t lt; read(fd, &lt, 4);
              if (lt == 0x69766f6d) continue;
              lseek(fd, hdr[1] - 4 + (hdr[1] & 1), SEEK_CUR);
            }
          else if (hdr[0] == 0x63643030)
            {
              read(fd, chunk, hdr[1]);
              break;
            }
          else
            {
              lseek(fd, hdr[1] + (hdr[1] & 1), SEEK_CUR);
            }
        }
      close(fd);

      if (bk7258_jpegdec_decode(chunk, hdr[1], g_yuyv,
                                AVI_W, EYE_YRES) != 0)
        {
          printf("cal: decode failed\n");
          return 1;
        }

      printf("cal: expect bg~de96 (cream)\n");
      for (fmt = 0; fmt < 4; fmt++)
        {
          for (reve = 0; reve < 2; reve++)
            {
              uint16_t *o = (uint16_t *)g_rgbl;

              bk7258_dma2d_set_fgcfg(fmt, reve);
              memset(g_rgbl, 0, 64);
              if (bk7258_dma2d_split(g_yuyv, g_rgbl, g_rgbr) == 0)
                {
                  printf("cal: fmt%lu reve%d -> %04x %04x %04x %04x\n",
                         (unsigned long)fmt, reve,
                         o[10 * 160 + 8], o[10 * 160 + 9],
                         o[10 * 160 + 10], o[10 * 160 + 11]);
                }
              else
                {
                  printf("cal: fmt%lu reve%d -> ERR\n",
                         (unsigned long)fmt, reve);
                }
            }
        }

      bk7258_dma2d_set_fgcfg(0, 0);
      free(chunk);
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "KEYS") == 0)
    {
      /* Raw button probe: watch the three candidate pins directly and
       * print every level transition, bypassing the buttons driver.
       */

      extern void bk7258_gpio_config(int pin, bool output, bool pullup,
                                     bool pulldown);
      extern bool bk7258_gpio_read(int pin);
      static const int pins[3] = { 8, 12, 13 };
      bool last[3];
      int i;
      int t;

      for (i = 0; i < 3; i++)
        {
          bk7258_gpio_config(pins[i], false, true, false);
          last[i] = bk7258_gpio_read(pins[i]);
          printf("keys: gpio%d idle=%d\n", pins[i], last[i]);
        }

      printf("keys: press each button now (40 s window)\n");
      for (t = 0; t < 8000; t++)
        {
          for (i = 0; i < 3; i++)
            {
              bool v = bk7258_gpio_read(pins[i]);

              if (v != last[i])
                {
                  printf("keys: gpio%d -> %d\n", pins[i], v);
                  last[i] = v;
                }
            }

          usleep(5000);
        }

      printf("keys: done\n");
      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "WIRE") == 0)
    {
      /* Wire-order discriminator.  Same three-band pattern (top red,
       * middle green, bottom blue as raw little-endian RGB565) pushed
       * through both display paths at once: devno 0 via the putarea
       * reference route (known correct) and devno 1 via the raw blast
       * route the video player uses.  Whatever the blast panel shows
       * for known inputs uniquely identifies the wire transform.
       */

      extern int bk7258_gc9d01_putfull(int devno, const void *rgb565);
      extern void bk7258_qspi_blast_cpu(int devno, const uint16_t *px,
                                        size_t npx);
      uint16_t *bands = malloc(EYE_XRES * EYE_YRES * 2);
      int i;

      if (bands == NULL)
        {
          return 1;
        }

      for (i = 0; i < EYE_XRES * EYE_YRES; i++)
        {
          int row = i / EYE_XRES;
          bands[i] = row < 53 ? 0xf800 : row < 106 ? 0x07e0 : 0x001f;
        }

      bk7258_gc9d01_putfull(0, bands);
      bk7258_qspi_blast_cpu(1, bands, EYE_XRES * EYE_YRES);
      printf("face: WIRE up. ref panel = red/green/blue; report the "
             "other panel's three band colors top-to-bottom\n");
      free(bands);
      return 0;
    }

  if (argc > 3 && strcmp(argv[1], "RECV") == 0)
    {
      /* Raw file upload over the console: exactly <size> bytes follow,
       * acknowledged with 'K' every 4 KB block so the host self-paces
       * and the tiny UART RX buffer can never overflow.
       */

      const char *path = argv[2];
      long remaining = atol(argv[3]);
      uint8_t *buf = malloc(4096);
      int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

      if (fd < 0 || buf == NULL)
        {
          printf("face: recv setup failed\n");
          return 1;
        }

      printf("face: recv %ld bytes -> %s\nGO\n", remaining, path);
      fflush(stdout);

      while (remaining > 0)
        {
          size_t want = (remaining > 4096) ? 4096 : (size_t)remaining;
          size_t got = 0;

          while (got < want)
            {
              ssize_t n = read(0, buf + got, want - got);

              if (n > 0)
                {
                  got += (size_t)n;
                }
            }

          write(fd, buf, want);
          remaining -= want;
          printf("K");
          fflush(stdout);
        }

      close(fd);
      free(buf);
      printf("\nface: recv done\n");
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
      /* Idle = the vendor's genie eye (8.3 mangled name on the card). */

      const char *name = (cur < 0) ? "GENIE_~1" : g_exprs[cur];
      int r = play_avi(name, btnfd);

      if (r == PLAY_QUIT)
        {
          printf("face: quit (>> held)\n");
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
          if (cur < 0)
            {
              printf("face: cannot play idle, quitting\n");
              break;
            }

          printf("face: cannot play %s, back to idle\n", name);
          cur = -1;
        }
    }

  if (btnfd >= 0)
    {
      close(btnfd);
    }

  return 0;
}
