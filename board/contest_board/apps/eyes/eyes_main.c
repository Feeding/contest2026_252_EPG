/****************************************************************************
 * apps/eyes/eyes_main.c
 *
 * Robot-face eye animation for the contest board's two GC9D01 panels.
 * Everything is drawn procedurally: a glowing iris ring, a pupil that
 * leads the gaze, a fixed specular highlight, and an eyelid for blinks.
 * Only the union of the previous and current iris boxes is redrawn and
 * flushed, so the QSPI path moves a few kilobytes per frame instead of
 * the whole screen.
 *
 * Usage: eyes [seconds]   (0 = run until reset; default 30)
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/video/fb.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
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
#define EYE_CX     80
#define EYE_CY     80

#define R_RING     34         /* Outer edge of the glowing ring */
#define R_IRIS     29         /* Inner iris disc */
#define R_PUP      13         /* Pupil */
#define R_HIL      4          /* Specular highlight */
#define G_MAX      18         /* Gaze offset limit for the iris centre */

#define C_BG       0x0000     /* Black face */
#define C_RING     0x07ff     /* Cyan ring */
#define C_IRIS     0x0350     /* Deep teal fill */
#define C_PUP      0x0841     /* Near-black pupil */
#define C_HIL      0xffff     /* White highlight */

#define FRAME_US   33000      /* ~30 fps pacing */

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

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Percent closed per blink phase: shut fast, open a touch slower. */

static const uint8_t g_lidfrac[] =
{
  35, 75, 100, 100, 80, 55, 30, 10, 0
};

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
 * Name: eye_render
 *
 * Description:
 *   Repaint one rectangle of one eye.  Every pixel in the rectangle is
 *   classified from scratch (lid, highlight, pupil, iris, ring, bg), so
 *   the caller only has to get the dirty box right.
 *
 ****************************************************************************/

static void eye_render(struct eye_fb_s *e, int cx, int cy, int lid,
                       int x0, int y0, int x1, int y1)
{
  /* The pupil leads the gaze slightly and the highlight rides the iris. */

  int px = cx + ((cx - EYE_CX) >> 3);
  int py = cy + ((cy - EYE_CY) >> 3);
  int hx = cx - 11;
  int hy = cy - 11;
  int x;
  int y;

  for (y = y0; y <= y1; y++)
    {
      uint16_t *row = e->mem + y * e->stride_px;

      for (x = x0; x <= x1; x++)
        {
          uint16_t c = C_BG;

          if (y >= lid)
            {
              int dx = x - cx;
              int dy = y - cy;
              int d2 = dx * dx + dy * dy;

              if (d2 <= R_RING * R_RING)
                {
                  int ex = x - hx;
                  int ey = y - hy;
                  int qx = x - px;
                  int qy = y - py;

                  if (ex * ex + ey * ey <= R_HIL * R_HIL)
                    {
                      c = C_HIL;
                    }
                  else if (qx * qx + qy * qy <= R_PUP * R_PUP)
                    {
                      c = C_PUP;
                    }
                  else if (d2 <= R_IRIS * R_IRIS)
                    {
                      c = C_IRIS;
                    }
                  else
                    {
                      c = C_RING;
                    }
                }
            }

          row[x] = c;
        }
    }
}

static int eye_open(struct eye_fb_s *e, const char *path)
{
  struct fb_planeinfo_s pinfo;

  e->fd = open(path, O_RDWR);
  if (e->fd < 0)
    {
      printf("eyes: cannot open %s\n", path);
      return -1;
    }

  if (ioctl(e->fd, FBIOGET_PLANEINFO, (unsigned long)(uintptr_t)&pinfo) < 0)
    {
      printf("eyes: FBIOGET_PLANEINFO failed on %s\n", path);
      close(e->fd);
      return -1;
    }

  e->len = pinfo.fblen;
  e->stride_px = pinfo.stride / 2;
  e->mem = mmap(NULL, pinfo.fblen, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_FILE, e->fd, 0);
  if (e->mem == MAP_FAILED)
    {
      printf("eyes: mmap failed on %s\n", path);
      close(e->fd);
      return -1;
    }

  return 0;
}

static void eye_flush(struct eye_fb_s *e, int x0, int y0, int x1, int y1)
{
  struct fb_area_s area;

  area.x = x0;
  area.y = y0;
  area.w = x1 - x0 + 1;
  area.h = y1 - y0 + 1;
  ioctl(e->fd, FBIO_UPDATE, (unsigned long)(uintptr_t)&area);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct eye_fb_s eye[2];
  int duration = (argc > 1) ? atoi(argv[1]) : 30;
  int gx = 0;
  int gy = 0;
  int tx = 0;
  int ty = 0;
  int blink = -1;
  int px0 = 0;
  int py0 = 0;
  int px1 = EYE_XRES - 1;
  int py1 = EYE_YRES - 1;
  uint32_t start;
  uint32_t next_gaze;
  uint32_t next_blink;
  uint32_t frames = 0;
  uint32_t t_render = 0;
  uint32_t t_flush = 0;
  int i;

  if (eye_open(&eye[0], "/dev/fb0") < 0)
    {
      return 1;
    }

  if (eye_open(&eye[1], "/dev/fb1") < 0)
    {
      munmap(eye[0].mem, eye[0].len);
      close(eye[0].fd);
      return 1;
    }

  srand(now_ms());
  start      = now_ms();
  next_gaze  = start + 1500;
  next_blink = start + 2200;

  for (; ; )
    {
      uint32_t t0 = now_ms();
      int cx;
      int cy;
      int lid;
      int x0;
      int y0;
      int x1;
      int y1;

      if (duration > 0 && (t0 - start) >= (uint32_t)duration * 1000)
        {
          break;
        }

      /* Pick a fresh gaze target now and then (never mid-blink). */

      if (blink < 0 && t0 >= next_gaze)
        {
          do
            {
              tx = rand() % (2 * G_MAX + 1) - G_MAX;
              ty = rand() % (2 * G_MAX + 1) - G_MAX;
            }
          while (tx * tx + ty * ty > G_MAX * G_MAX);

          next_gaze = t0 + 1200 + rand() % 1800;
        }

      /* Ease a quarter of the remaining distance each frame. */

      if (gx != tx)
        {
          int step = (tx - gx) / 4;
          gx += (step != 0) ? step : ((tx > gx) ? 1 : -1);
        }

      if (gy != ty)
        {
          int step = (ty - gy) / 4;
          gy += (step != 0) ? step : ((ty > gy) ? 1 : -1);
        }

      /* Blink state machine: the lid is just a row below which we draw. */

      if (blink < 0 && t0 >= next_blink)
        {
          blink = 0;
        }

      if (blink >= 0)
        {
          lid = (EYE_CY - R_RING) +
                (2 * R_RING * g_lidfrac[blink]) / 100;
          blink++;
          if (blink >= (int)sizeof(g_lidfrac))
            {
              blink = -1;
              next_blink = t0 + 2000 + rand() % 2500;
            }
        }
      else
        {
          lid = 0;
        }

      cx = EYE_CX + gx;
      cy = EYE_CY + gy;

      /* Dirty box: current iris box united with the previous one. */

      x0 = cx - R_RING - 1;
      y0 = cy - R_RING - 1;
      x1 = cx + R_RING + 1;
      y1 = cy + R_RING + 1;

      if (px0 < x0) x0 = px0;
      if (py0 < y0) y0 = py0;
      if (px1 > x1) x1 = px1;
      if (py1 > y1) y1 = py1;

      if (x0 < 0) x0 = 0;
      if (y0 < 0) y0 = 0;
      if (x1 > EYE_XRES - 1) x1 = EYE_XRES - 1;
      if (y1 > EYE_YRES - 1) y1 = EYE_YRES - 1;

        {
          uint32_t ta = now_ms();
          uint32_t tb;

          for (i = 0; i < 2; i++)
            {
              eye_render(&eye[i], cx, cy, lid, x0, y0, x1, y1);
            }

          tb = now_ms();

          for (i = 0; i < 2; i++)
            {
              eye_flush(&eye[i], x0, y0, x1, y1);
            }

          t_render += tb - ta;
          t_flush  += now_ms() - tb;
        }

      px0 = cx - R_RING - 1;
      py0 = cy - R_RING - 1;
      px1 = cx + R_RING + 1;
      py1 = cy + R_RING + 1;

      frames++;

      /* Pace to the frame grid; a slow frame just runs back-to-back. */

      {
        uint32_t spent = now_ms() - t0;

        if (spent * 1000 < FRAME_US)
          {
            usleep(FRAME_US - spent * 1000);
          }
      }
    }

  {
    uint32_t total = now_ms() - start;

    printf("eyes: %lu frames in %lu ms (%lu fps) render=%lums flush=%lums\n",
           (unsigned long)frames, (unsigned long)total,
           (unsigned long)(total ? frames * 1000 / total : 0),
           (unsigned long)(frames ? t_render / frames : 0),
           (unsigned long)(frames ? t_flush / frames : 0));
  }

  for (i = 0; i < 2; i++)
    {
      munmap(eye[i].mem, eye[i].len);
      close(eye[i].fd);
    }

  return 0;
}
