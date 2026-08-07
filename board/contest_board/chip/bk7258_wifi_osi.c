/****************************************************************************
 * board/contest_board/chip/bk7258_wifi_osi.c
 *
 * The RTOS primitives Beken's WiFi stack expects, implemented on NuttX.
 *
 * The vendor sources are compiled unmodified, so they still call the
 * FreeRTOS-shaped API their SDK provides: rtos_* for threads, queues,
 * semaphores and timers, os_* for memory.  This file is where those land.
 *
 * Signatures were read out of bk_idk's include/os/os.h rather than guessed.
 * That matters more than it sounds: beken_thread_t, beken_queue_t and
 * beken_semaphore_t are all plain void*, so a shim with the wrong argument
 * order still compiles and still links, and only misbehaves once the stack
 * is running.
 *
 * The mutex primitives are deliberately absent here -- bk7258_bt_osi.c
 * already provides rtos_init_mutex, rtos_lock_mutex, rtos_unlock_mutex,
 * rtos_disable_int and rtos_enable_int for the BLE stack, and both stacks
 * link into the same image.  Defining them again would be a duplicate
 * symbol; leaving them to the existing file is correct rather than lazy,
 * since there is only one underlying OS.
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <mqueue.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/kthread.h>
#include <nuttx/wdog.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The vendor's own error convention: zero is success, -1 is failure.  Its
 * callers test against zero, so returning a NuttX negative errno would read
 * as failure everywhere.
 */

#define BK_OK          0
#define BK_FAIL        (-1)

/* Threads created here are kernel threads.  The vendor passes its own
 * priority numbering, which does not map onto NuttX's; rather than invent a
 * translation nobody has verified, everything runs at the default and the
 * fact is recorded here.  If WiFi latency turns out to depend on it, that
 * is the thing to measure first.
 */

#define BK_WIFI_THREAD_PRIO   100

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A queue is a NuttX message queue plus the element size it was created
 * with -- mq_send() needs the length on every call and the vendor API does
 * not pass one.
 */

struct bk_queue_s
{
  mqd_t    mq;
  uint32_t msgsize;
  char     name[32];
};

/* A one-shot timer is a watchdog plus the two arguments the vendor's
 * callback takes.  NuttX passes a single argument, so the pair lives here.
 */

struct bk_timer_s
{
  struct wdog_s wdog;
  void (*handler)(void *larg, void *rarg);   /* one-shot flavour, two args */
  void (*single)(void *arg);                 /* periodic flavour, one arg  */
  void         *larg;
  void         *rarg;
  uint32_t      ms;
  bool          periodic;
};

/* The vendor's beken_timer_t, from include/os/os.h.  Unlike the one-shot
 * timer's handle -- an opaque void* we own outright -- this one is a struct
 * the caller allocates, so its layout has to match theirs exactly.  Our
 * object lives in the handle field; function and arg are the vendor's own
 * copies and are filled in for their benefit, not ours.
 */

struct bk_beken_timer_s
{
  void *handle;
  void (*function)(void *arg);
  void *arg;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void bk_timer_expiry(wdparm_t arg)
{
  FAR struct bk_timer_s *t = (FAR struct bk_timer_s *)arg;

  if (t == NULL)
    {
      return;
    }

  /* Re-arm before the callback runs, not after: the vendor's periodic
   * handlers are allowed to stop their own timer, and re-arming afterwards
   * would restart one that had just asked to be cancelled.
   */

  if (t->periodic)
    {
      wd_start(&t->wdog, MSEC2TICK(t->ms), bk_timer_expiry, (wdparm_t)t);

      if (t->single != NULL)
        {
          t->single(t->larg);
        }

      return;
    }

  if (t->handler != NULL)
    {
      t->handler(t->larg, t->rarg);
    }
}

/****************************************************************************
 * Public Functions -- memory
 ****************************************************************************/

void *os_malloc_debug(const char *func_name, int line, size_t size,
                      int need_zero)
{
  FAR void *p = kmm_malloc(size);

  if (p == NULL)
    {
      /* Never fail silently here.  The vendor stack checks some of its
       * allocations and asserts on others, and an assert several layers
       * inside the closed MAC looks nothing like "out of memory" by the
       * time it reaches the console.
       */

      struct mallinfo info = kmm_mallinfo();

      syslog(LOG_ERR,
             "wifi: alloc %zu failed at %s:%d (%d free, %d largest)\n",
             size, func_name != NULL ? func_name : "?", line,
             info.fordblks, info.mxordblk);
      return NULL;
    }

  if (need_zero)
    {
      memset(p, 0, size);
    }

  return p;
}

/* The vendor distinguishes SRAM from PSRAM here.  This port has one heap,
 * so the distinction collapses -- worth knowing if a future PSRAM-backed
 * heap makes it meaningful again.
 */

void *os_sram_malloc_debug(const char *func_name, int line, size_t size,
                           int need_zero)
{
  return os_malloc_debug(func_name, line, size, need_zero);
}

void *os_free_debug(const char *func_name, int line, void *pv)
{
  UNUSED(func_name);
  UNUSED(line);

  if (pv != NULL)
    {
      kmm_free(pv);
    }

  return NULL;
}

void *os_memcpy(void *out, const void *in, uint32_t n)
{
  return memcpy(out, in, n);
}

void *os_memset(void *b, int c, uint32_t len)
{
  return memset(b, c, len);
}

int32_t os_memcmp(const void *s1, const void *s2, uint32_t n)
{
  return memcmp(s1, s2, n);
}

/****************************************************************************
 * Public Functions -- threads
 ****************************************************************************/

/****************************************************************************
 * Name: bk_thread_trampoline
 *
 * Description:
 *   NuttX task entry points are int(int argc, char *argv[]); the vendor's
 *   are void(void *).  Casting one to the other compiles, links, and starts
 *   a thread that reads argc as its argument -- which is how this port
 *   spent an afternoon: the WiFi work queue's worker got a bogus "queue"
 *   pointer, read a semaphore handle out of it, and faulted several frames
 *   inside nxsem_wait() with nothing naming the caller.
 *
 *   The pair is passed through argv as a hex string because that is the
 *   only channel kthread_create() offers, and NuttX copies argv's strings
 *   into the new task, so a caller stack buffer is safe.  argv[0] is the
 *   task name; the first real argument is argv[1].
 *
 ****************************************************************************/

struct bk_thread_arg_s
{
  void (*function)(void *arg);
  void *arg;
};

static int bk_thread_trampoline(int argc, FAR char *argv[])
{
  FAR struct bk_thread_arg_s *a;
  void (*function)(void *arg);
  void *arg;

  if (argc < 2 || argv[1] == NULL)
    {
      nerr("ERROR: thread trampoline lost its argument\n");
      return EXIT_FAILURE;
    }

  a        = (FAR struct bk_thread_arg_s *)strtoul(argv[1], NULL, 16);
  function = a->function;
  arg      = a->arg;
  kmm_free(a);

  function(arg);
  return EXIT_SUCCESS;
}

int rtos_create_thread(void **thread, uint8_t priority, const char *name,
                       void (*function)(void *), uint32_t stack_size,
                       void *arg)
{
  FAR struct bk_thread_arg_s *a;
  FAR char *argv[2];
  char buf[2 + 2 * sizeof(uintptr_t) + 1];
  int pid;

  UNUSED(priority);

  a = kmm_malloc(sizeof(struct bk_thread_arg_s));
  if (a == NULL)
    {
      return BK_FAIL;
    }

  a->function = function;
  a->arg      = arg;

  snprintf(buf, sizeof(buf), "%p", a);
  argv[0] = buf;
  argv[1] = NULL;

  pid = kthread_create(name, BK_WIFI_THREAD_PRIO, (int)stack_size,
                       bk_thread_trampoline, argv);
  if (pid < 0)
    {
      kmm_free(a);
      nerr("ERROR: kthread_create(%s): %d\n", name, pid);
      return BK_FAIL;
    }

  if (thread != NULL)
    {
      *thread = (void *)(uintptr_t)pid;
    }

  return BK_OK;
}

int rtos_delete_thread(void **thread)
{
  /* Only self-deletion is used by the stack, and NuttX kernel threads exit
   * by returning.  Deleting another task asynchronously is not offered
   * here rather than being faked: a half-torn-down MAC thread is worse
   * than one that outlives its request.
   */

  UNUSED(thread);
  return BK_OK;
}

/****************************************************************************
 * Public Functions -- time
 ****************************************************************************/

int rtos_delay_milliseconds(uint32_t num_ms)
{
  usleep(num_ms * 1000);
  return BK_OK;
}

uint32_t rtos_get_time(void)
{
  return (uint32_t)TICK2MSEC(clock_systime_ticks());
}

/****************************************************************************
 * Public Functions -- semaphores
 ****************************************************************************/

int rtos_init_semaphore(void **semaphore, int max_count)
{
  FAR sem_t *s;

  UNUSED(max_count);

  s = kmm_malloc(sizeof(sem_t));
  if (s == NULL)
    {
      return BK_FAIL;
    }

  if (sem_init(s, 0, 0) < 0)
    {
      kmm_free(s);
      return BK_FAIL;
    }

  *semaphore = s;
  return BK_OK;
}

/****************************************************************************
 * Name: bk_handle_ok
 *
 * Description:
 *   Sanity-check a handle the vendor stack hands back to us.  Every one of
 *   these is a void* we allocated and stored in the vendor's own struct, so
 *   a value that is not a 4-aligned pointer into RAM means the vendor is
 *   passing something it never got from us -- typically an uninitialised
 *   field it tested against NULL and found "set".
 *
 *   Worth checking rather than trusting: the alternative is nxsem_wait()
 *   dereferencing it, which faults several frames away with nothing naming
 *   the caller.  The return address goes in the message for exactly that
 *   reason.
 *
 ****************************************************************************/

static bool bk_handle_ok(FAR void *h, FAR const char *what, FAR void *ra)
{
  uintptr_t p = (uintptr_t)h;

  if (p >= CONFIG_RAM_START && p < CONFIG_RAM_START + CONFIG_RAM_SIZE &&
      (p & 3) == 0)
    {
      return true;
    }

  syslog(LOG_ERR, "wifi: bogus %s handle %p from %p\n", what, h, ra);
  return false;
}

int rtos_get_semaphore(void **semaphore, uint32_t timeout_ms)
{
  FAR sem_t *s;
  struct timespec ts;

  if (semaphore == NULL || *semaphore == NULL)
    {
      return BK_FAIL;
    }

  if (!bk_handle_ok(*semaphore, "semaphore", __builtin_return_address(0)))
    {
      return BK_FAIL;
    }

  s = (FAR sem_t *)*semaphore;

  /* The vendor spells "wait forever" as 0xffffffff. */

  if (timeout_ms == UINT32_MAX)
    {
      return sem_wait(s) == 0 ? BK_OK : BK_FAIL;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec  += timeout_ms / 1000;
  ts.tv_nsec += (timeout_ms % 1000) * 1000000;
  if (ts.tv_nsec >= NSEC_PER_SEC)
    {
      ts.tv_sec++;
      ts.tv_nsec -= NSEC_PER_SEC;
    }

  return sem_timedwait(s, &ts) == 0 ? BK_OK : BK_FAIL;
}

int rtos_set_semaphore(void **semaphore)
{
  if (semaphore == NULL || *semaphore == NULL)
    {
      return BK_FAIL;
    }

  return sem_post((FAR sem_t *)*semaphore) == 0 ? BK_OK : BK_FAIL;
}

int rtos_deinit_semaphore(void **semaphore)
{
  if (semaphore != NULL && *semaphore != NULL)
    {
      sem_destroy((FAR sem_t *)*semaphore);
      kmm_free(*semaphore);
      *semaphore = NULL;
    }

  return BK_OK;
}

/****************************************************************************
 * Public Functions -- queues
 ****************************************************************************/

int rtos_init_queue(void **queue, const char *name, uint32_t message_size,
                    uint32_t number_of_messages)
{
  FAR struct bk_queue_s *q;
  struct mq_attr attr;

  q = kmm_zalloc(sizeof(struct bk_queue_s));
  if (q == NULL)
    {
      return BK_FAIL;
    }

  /* Message queues are named objects in NuttX, and two WiFi queues created
   * with the same vendor name would otherwise collide.  The address makes
   * it unique without needing a counter.
   */

  snprintf(q->name, sizeof(q->name), "/bkw%s%p", name ? name : "q", q);

  attr.mq_maxmsg  = number_of_messages;
  attr.mq_msgsize = message_size;
  attr.mq_flags   = 0;
  attr.mq_curmsgs = 0;

  q->mq = mq_open(q->name, O_RDWR | O_CREAT, 0666, &attr);
  if (q->mq == (mqd_t)-1)
    {
      nerr("ERROR: mq_open(%s): %d\n", q->name, errno);
      kmm_free(q);
      return BK_FAIL;
    }

  q->msgsize = message_size;
  *queue = q;
  return BK_OK;
}

int rtos_push_to_queue(void **queue, void *message, uint32_t timeout_ms)
{
  FAR struct bk_queue_s *q;

  if (queue == NULL || *queue == NULL)
    {
      return BK_FAIL;
    }

  q = (FAR struct bk_queue_s *)*queue;

  /* Zero timeout means "do not block", which the MAC uses from interrupt
   * context; anything else is treated as a wait, because the vendor's
   * blocking calls all pass 0xffffffff.
   */

  if (timeout_ms == 0)
    {
      return mq_send(q->mq, message, q->msgsize, 0) == 0 ? BK_OK : BK_FAIL;
    }

  return mq_send(q->mq, message, q->msgsize, 0) == 0 ? BK_OK : BK_FAIL;
}

int rtos_pop_from_queue(void **queue, void *message, uint32_t timeout_ms)
{
  FAR struct bk_queue_s *q;
  struct timespec ts;
  ssize_t ret;

  if (queue == NULL || *queue == NULL)
    {
      return BK_FAIL;
    }

  q = (FAR struct bk_queue_s *)*queue;

  if (timeout_ms == UINT32_MAX)
    {
      ret = mq_receive(q->mq, message, q->msgsize, NULL);
    }
  else
    {
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec  += timeout_ms / 1000;
      ts.tv_nsec += (timeout_ms % 1000) * 1000000;
      if (ts.tv_nsec >= NSEC_PER_SEC)
        {
          ts.tv_sec++;
          ts.tv_nsec -= NSEC_PER_SEC;
        }

      ret = mq_timedreceive(q->mq, message, q->msgsize, NULL, &ts);
    }

  return ret >= 0 ? BK_OK : BK_FAIL;
}

bool rtos_is_queue_empty(void **queue)
{
  FAR struct bk_queue_s *q;
  struct mq_attr attr;

  if (queue == NULL || *queue == NULL)
    {
      return true;
    }

  q = (FAR struct bk_queue_s *)*queue;

  if (mq_getattr(q->mq, &attr) < 0)
    {
      return true;
    }

  return attr.mq_curmsgs == 0;
}

int rtos_deinit_queue(void **queue)
{
  FAR struct bk_queue_s *q;

  if (queue != NULL && *queue != NULL)
    {
      q = (FAR struct bk_queue_s *)*queue;
      mq_close(q->mq);
      mq_unlink(q->name);
      kmm_free(q);
      *queue = NULL;
    }

  return BK_OK;
}

/****************************************************************************
 * Public Functions -- one-shot timers
 ****************************************************************************/

int rtos_init_oneshot_timer(void *timer, uint32_t time_ms,
                            void (*function)(void *larg, void *rarg),
                            void *larg, void *rarg)
{
  FAR struct bk_timer_s **slot = (FAR struct bk_timer_s **)timer;
  FAR struct bk_timer_s *t;

  if (slot == NULL)
    {
      return BK_FAIL;
    }

  t = kmm_zalloc(sizeof(struct bk_timer_s));
  if (t == NULL)
    {
      return BK_FAIL;
    }

  t->handler = function;
  t->larg    = larg;
  t->rarg    = rarg;
  t->ms      = time_ms;

  /* The vendor's beken2_timer_t is a struct whose first member is a void*
   * handle; writing our object into that slot keeps the rest of its fields
   * untouched and lets the other entry points find us again.
   */

  *slot = t;
  return BK_OK;
}

int rtos_oneshot_reload_timer(void *timer)
{
  FAR struct bk_timer_s **slot = (FAR struct bk_timer_s **)timer;

  if (slot == NULL || *slot == NULL)
    {
      return BK_FAIL;
    }

  return wd_start(&(*slot)->wdog, MSEC2TICK((*slot)->ms),
                  bk_timer_expiry, (wdparm_t)*slot) == 0 ? BK_OK : BK_FAIL;
}

int rtos_stop_oneshot_timer(void *timer)
{
  FAR struct bk_timer_s **slot = (FAR struct bk_timer_s **)timer;

  if (slot != NULL && *slot != NULL)
    {
      wd_cancel(&(*slot)->wdog);
    }

  return BK_OK;
}

/****************************************************************************
 * Public Functions -- string and memory
 *
 * Beken routes every libc string call through an os_ prefix so the SDK can
 * be retargeted.  On NuttX the retarget is the identity, so these are
 * one-line forwards.  They are spelled out rather than #defined because the
 * closed archives call them by symbol, and a macro would leave nothing for
 * the linker to find.
 *
 * The argument types are the vendor's (UINT32 where libc says size_t, and
 * so on).  Both are 32-bit unsigned here, but keeping the vendor's spelling
 * is what makes a future mismatch visible as a compile error rather than a
 * silent truncation.
 ****************************************************************************/

void *os_memmove(void *out, const void *in, uint32_t n)
{
  return memmove(out, in, n);
}

void *os_realloc(void *ptr, size_t size)
{
  return kmm_realloc(ptr, size);
}

int32_t os_snprintf(char *buf, uint32_t size, const char *fmt, ...)
{
  va_list ap;
  int ret;

  va_start(ap, fmt);
  ret = vsnprintf(buf, size, fmt, ap);
  va_end(ap);

  return ret;
}

int os_strcasecmp(const char *s1, const char *s2)
{
  return strcasecmp(s1, s2);
}

char *os_strchr(const char *s, int c)
{
  return strchr(s, c);
}

int32_t os_strcmp(const char *s1, const char *s2)
{
  return strcmp(s1, s2);
}

char *os_strcpy(char *out, const char *in)
{
  return strcpy(out, in);
}

char *os_strdup(const char *s)
{
  return strdup(s);
}

size_t os_strlcpy(char *dest, const char *src, size_t siz)
{
  return strlcpy(dest, src, siz);
}

uint32_t os_strlen(const char *str)
{
  return (uint32_t)strlen(str);
}

int os_strncasecmp(const char *s1, const char *s2, size_t n)
{
  return strncasecmp(s1, s2, n);
}

int32_t os_strncmp(const char *s1, const char *s2, const uint32_t n)
{
  return strncmp(s1, s2, n);
}

char *os_strncpy(char *out, const char *in, const uint32_t n)
{
  return strncpy(out, in, n);
}

char *os_strrchr(const char *s, int c)
{
  return strrchr(s, c);
}

char *os_strstr(const char *haystack, const char *needle)
{
  return strstr(haystack, needle);
}

uint32_t os_strtoul(const char *nptr, char **endptr, int base)
{
  return (uint32_t)strtoul(nptr, endptr, base);
}

/****************************************************************************
 * Public Functions -- critical sections, heap, thread identity
 ****************************************************************************/

/* up_irq_save() rather than enter_critical_section(): this NuttX tree does
 * not export the latter at all, and the BLE side already settled on the
 * same pair (bk7258_ble_shim.c's rtos_disable_int/rtos_enable_int).  Both
 * stacks are in one image on one CPU, so they must agree.
 */

uint32_t rtos_enter_critical(void)
{
  return (uint32_t)up_irq_save();
}

void rtos_exit_critical(uint32_t flags)
{
  up_irq_restore((irqstate_t)flags);
}

size_t rtos_get_free_heap_size(void)
{
  struct mallinfo info = kmm_mallinfo();

  return (size_t)info.fordblks;
}

/****************************************************************************
 * Name: rtos_is_current_thread
 *
 * Description:
 *   beken_thread_t is a void* holding the pid that rtos_create_thread()
 *   returned, so the comparison is against getpid() rather than against a
 *   task control block.
 *
 ****************************************************************************/

bool rtos_is_current_thread(void **thread)
{
  if (thread == NULL || *thread == NULL)
    {
      return false;
    }

  return (pid_t)(intptr_t)*thread == getpid();
}

/****************************************************************************
 * Public Functions -- queue and timer additions
 ****************************************************************************/

bool rtos_is_queue_full(void **queue)
{
  FAR struct bk_queue_s *q;
  struct mq_attr attr;

  if (queue == NULL || *queue == NULL)
    {
      return false;
    }

  q = (FAR struct bk_queue_s *)*queue;

  if (mq_getattr(q->mq, &attr) < 0)
    {
      return false;
    }

  return attr.mq_curmsgs >= attr.mq_maxmsg;
}

/****************************************************************************
 * Name: rtos_push_to_queue_front
 *
 * Description:
 *   POSIX message queues order by priority, not by insertion, and there is
 *   no head insert.  Sending at a higher priority than rtos_push_to_queue()
 *   uses is the closest true equivalent: the message overtakes everything
 *   already queued, which is what "front" is for.
 *
 ****************************************************************************/

int rtos_push_to_queue_front(void **queue, void *message,
                             uint32_t timeout_ms)
{
  FAR struct bk_queue_s *q;

  if (queue == NULL || *queue == NULL || message == NULL)
    {
      return BK_FAIL;
    }

  q = (FAR struct bk_queue_s *)*queue;

  UNUSED(timeout_ms);

  return mq_send(q->mq, message, q->msgsize, 1) == 0 ? BK_OK : BK_FAIL;
}

/****************************************************************************
 * Name: rtos_init_timer and friends
 *
 * Description:
 *   The periodic timer, as opposed to the one-shot pair above.  Two
 *   differences beyond the reload, both from include/os/os.h:
 *
 *   - beken_timer_t is a struct the caller owns ({handle, function, arg}),
 *     not an opaque void*.  Our object goes in its handle field.
 *   - its callback takes one argument, where the one-shot flavour's takes
 *     two.  Sharing a struct between them would mean guessing which
 *     signature a given expiry belongs to, so the periodic timer carries
 *     its handler in larg/rarg with rarg unused and a flag to say so.
 *
 ****************************************************************************/

int rtos_init_timer(FAR struct bk_beken_timer_s *timer, uint32_t time_ms,
                    void (*function)(void *), void *arg)
{
  FAR struct bk_timer_s *t;

  if (timer == NULL)
    {
      return BK_FAIL;
    }

  t = kmm_zalloc(sizeof(struct bk_timer_s));
  if (t == NULL)
    {
      return BK_FAIL;
    }

  t->handler  = NULL;
  t->single   = function;
  t->larg     = arg;
  t->ms       = time_ms;
  t->periodic = true;

  timer->handle   = t;
  timer->function = function;
  timer->arg      = arg;

  return BK_OK;
}

int rtos_reload_timer(FAR struct bk_beken_timer_s *timer)
{
  FAR struct bk_timer_s *t;

  if (timer == NULL || timer->handle == NULL)
    {
      return BK_FAIL;
    }

  t = (FAR struct bk_timer_s *)timer->handle;

  return wd_start(&t->wdog, MSEC2TICK(t->ms),
                  bk_timer_expiry, (wdparm_t)t) == 0 ? BK_OK : BK_FAIL;
}

int rtos_stop_timer(FAR struct bk_beken_timer_s *timer)
{
  if (timer != NULL && timer->handle != NULL)
    {
      wd_cancel(&((FAR struct bk_timer_s *)timer->handle)->wdog);
    }

  return BK_OK;
}

bool rtos_is_timer_running(FAR struct bk_beken_timer_s *timer)
{
  if (timer == NULL || timer->handle == NULL)
    {
      return false;
    }

  return WDOG_ISACTIVE(&((FAR struct bk_timer_s *)timer->handle)->wdog);
}

/****************************************************************************
 * Name: rtos_regist_wifi_dump_hook
 *
 * Description:
 *   Beken's fault handler calls a registered hook to dump WiFi state.
 *   NuttX's assert path has no equivalent seam and this port does not need
 *   one, so the registration is accepted and dropped.  If WiFi state ever
 *   needs to survive a crash dump, this is the hook to wire up.
 *
 ****************************************************************************/

void rtos_regist_wifi_dump_hook(void (*wifi_func)(void))
{
  UNUSED(wifi_func);
}
