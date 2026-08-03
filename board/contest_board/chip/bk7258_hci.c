/****************************************************************************
 * boards/arm/bk7258/contest_board/chip/bk7258_hci.c
 *
 * H4 HCI transport as a character device, so the openvela Bluetooth
 * service can drive this controller the way it drives any other.  Its
 * Zephyr stack opens CONFIG_BLUETOOTH_SERVICE_HCI_UART_NAME
 * (/dev/ttyHCI0 by default) and speaks H4 over it, and that is the only
 * contract this file implements.
 *
 * The transport is a byte pipe, not a protocol.  Disassembling the
 * closed controller settles the framing in both directions:
 * bk_ble_hci_raw_to_controller() reads buf[0] as the packet type and
 * tail-calls the typed entry with buf+1/len-1, and ble_hci_send_to_uart()
 * pushes host-bound traffic out with the same leading type byte.  So
 * write() forwards whole frames untouched and the receive callback
 * queues them untouched.
 *
 * Framing still has to be parsed on the way down, because a writer may
 * split a frame across calls: the driver accumulates until the H4
 * header says the frame is complete, then submits it in one piece.
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <assert.h>

#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "arm_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HCI_PKT_CMD           0x01
#define HCI_PKT_ACL           0x02
#define HCI_PKT_SCO           0x03
#define HCI_PKT_EVT           0x04
#define HCI_PKT_ISO           0x05

/* Bytes of header that follow the type byte, and where the payload
 * length lives inside that header, per packet type.
 */

#define HCI_CMD_HDR           3        /* opcode 2 + plen 1, plen at [2] */
#define HCI_ACL_HDR           4        /* handle 2 + len 2, len at [2..3] */
#define HCI_SCO_HDR           3        /* handle 2 + len 1, len at [2] */
#define HCI_EVT_HDR           2        /* code 1 + plen 1, plen at [1] */
#define HCI_ISO_HDR           4        /* handle 2 + len 2 (12 bits) */

#define HCI_FRAME_MAX         (1 + 4 + 255 + 4)

/* Host-bound ring.  Sized for a burst of advertising reports arriving
 * while the service thread is busy: each report runs well under 64
 * bytes, and losing them silently is worse than briefly blocking a
 * controller callback, so overflow is counted and reported.
 */

#define HCI_RX_RING           4096

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_hci_s
{
  mutex_t   lock;                      /* One writer at a time */
  sem_t     rxsem;                     /* Posted when bytes arrive */
  uint8_t   rxbuf[HCI_RX_RING];
  uint16_t  rxhead;                    /* Producer: controller callback */
  uint16_t  rxtail;                    /* Consumer: read() */
  uint32_t  rxdrop;                    /* Frames lost to a full ring */
  uint8_t   txfrm[HCI_FRAME_MAX];      /* Partial frame from write() */
  uint16_t  txlen;
  bool      opened;
  FAR struct pollfd *fds;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int     bk7258_hci_open(FAR struct file *filep);
static int     bk7258_hci_close(FAR struct file *filep);
static ssize_t bk7258_hci_read(FAR struct file *filep, FAR char *buffer,
                               size_t buflen);
static ssize_t bk7258_hci_write(FAR struct file *filep,
                                FAR const char *buffer, size_t buflen);
static int     bk7258_hci_poll(FAR struct file *filep,
                               FAR struct pollfd *fds, bool setup);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_hci_fops =
{
  bk7258_hci_open,      /* open */
  bk7258_hci_close,     /* close */
  bk7258_hci_read,      /* read */
  bk7258_hci_write,     /* write */
  NULL,                 /* seek */
  NULL,                 /* ioctl */
  NULL,                 /* mmap */
  NULL,                 /* truncate */
  bk7258_hci_poll       /* poll */
};

static struct bk7258_hci_s g_hci;

/* Closed controller, controller-only archive.  Both take and give whole
 * H4 frames -- see the file banner for how that was established.
 */

extern int bk_ble_hci_raw_to_controller(uint8_t *buf, uint16_t len);
extern int bk_ble_reg_hci_raw_recv_callback(int (*cb)(uint8_t *buf,
                                                      uint16_t len));

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hci_frame_len
 *
 * Description:
 *   Total length of the H4 frame that starts at buf, or 0 when more
 *   bytes are needed to tell, or -1 when the type byte is not one this
 *   transport carries.
 *
 ****************************************************************************/

static int hci_frame_len(FAR const uint8_t *buf, uint16_t have)
{
  uint16_t hdr;
  uint16_t plen;

  if (have < 1)
    {
      return 0;
    }

  switch (buf[0])
    {
      case HCI_PKT_CMD:
        hdr = HCI_CMD_HDR;
        break;

      case HCI_PKT_ACL:
      case HCI_PKT_ISO:
        hdr = HCI_ACL_HDR;
        break;

      case HCI_PKT_SCO:
        hdr = HCI_SCO_HDR;
        break;

      case HCI_PKT_EVT:
        hdr = HCI_EVT_HDR;
        break;

      default:
        return -1;
    }

  if (have < 1 + hdr)
    {
      return 0;
    }

  switch (buf[0])
    {
      case HCI_PKT_CMD:
        plen = buf[3];
        break;

      case HCI_PKT_ACL:
        plen = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
        break;

      case HCI_PKT_ISO:
        plen = ((uint16_t)buf[3] | ((uint16_t)buf[4] << 8)) & 0x3fff;
        break;

      case HCI_PKT_SCO:
        plen = buf[3];
        break;

      default:
        plen = buf[2];
        break;
    }

  return 1 + hdr + plen;
}

/****************************************************************************
 * Name: hci_rx_used / hci_rx_space
 ****************************************************************************/

static uint16_t hci_rx_used(void)
{
  return (uint16_t)((g_hci.rxhead - g_hci.rxtail) & (HCI_RX_RING - 1));
}

static uint16_t hci_rx_space(void)
{
  return (uint16_t)(HCI_RX_RING - 1 - hci_rx_used());
}

/****************************************************************************
 * Name: bk7258_hci_recv
 *
 * Description:
 *   Controller callback, one whole H4 frame per call.  Runs in the
 *   controller's context, so it only copies and signals.  A frame that
 *   does not fit is dropped whole rather than in part: half a frame in
 *   the ring would desynchronise the reader for good.
 *
 ****************************************************************************/

static int bk7258_hci_recv(FAR uint8_t *buf, uint16_t len)
{
  irqstate_t flags;
  uint16_t i;
  int semcount;

  if (buf == NULL || len == 0)
    {
      return 0;
    }

  flags = enter_critical_section();

  if (len > hci_rx_space())
    {
      g_hci.rxdrop++;
      leave_critical_section(flags);
      return 0;
    }

  for (i = 0; i < len; i++)
    {
      g_hci.rxbuf[g_hci.rxhead] = buf[i];
      g_hci.rxhead = (uint16_t)((g_hci.rxhead + 1) & (HCI_RX_RING - 1));
    }

  leave_critical_section(flags);

  if (nxsem_get_value(&g_hci.rxsem, &semcount) == OK && semcount <= 0)
    {
      nxsem_post(&g_hci.rxsem);
    }

  poll_notify(&g_hci.fds, 1, POLLIN);
  return 0;
}

/****************************************************************************
 * Name: bk7258_hci_open / bk7258_hci_close
 ****************************************************************************/

static int bk7258_hci_open(FAR struct file *filep)
{
  int ret;

  ret = nxmutex_lock(&g_hci.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_hci.opened)
    {
      /* Start from an empty pipe: anything the controller emitted
       * before a host was listening belongs to nobody.
       */

      g_hci.rxhead = 0;
      g_hci.rxtail = 0;
      g_hci.txlen  = 0;

      ret = bk_ble_reg_hci_raw_recv_callback(bk7258_hci_recv);
      if (ret != 0)
        {
          nxmutex_unlock(&g_hci.lock);
          return -EIO;
        }

      g_hci.opened = true;
    }

  nxmutex_unlock(&g_hci.lock);
  return OK;
}

static int bk7258_hci_close(FAR struct file *filep)
{
  return OK;
}

/****************************************************************************
 * Name: bk7258_hci_read
 *
 * Description:
 *   Drain host-bound bytes.  Blocks for at least one byte unless the
 *   file was opened non-blocking; never splits across the ring wrap in
 *   a way the caller can see.
 *
 ****************************************************************************/

static ssize_t bk7258_hci_read(FAR struct file *filep, FAR char *buffer,
                               size_t buflen)
{
  irqstate_t flags;
  size_t n = 0;
  int ret;

  if (buflen == 0)
    {
      return 0;
    }

  while (hci_rx_used() == 0)
    {
      if ((filep->f_oflags & O_NONBLOCK) != 0)
        {
          return -EAGAIN;
        }

      ret = nxsem_wait(&g_hci.rxsem);
      if (ret < 0)
        {
          return ret;
        }
    }

  flags = enter_critical_section();

  while (n < buflen && g_hci.rxtail != g_hci.rxhead)
    {
      buffer[n++] = (char)g_hci.rxbuf[g_hci.rxtail];
      g_hci.rxtail = (uint16_t)((g_hci.rxtail + 1) & (HCI_RX_RING - 1));
    }

  leave_critical_section(flags);
  return (ssize_t)n;
}

/****************************************************************************
 * Name: bk7258_hci_write
 *
 * Description:
 *   Accumulate until a whole H4 frame is in hand, then hand it to the
 *   controller unmodified.  Returns the byte count consumed so a writer
 *   splitting a frame across calls behaves.
 *
 ****************************************************************************/

static ssize_t bk7258_hci_write(FAR struct file *filep,
                                FAR const char *buffer, size_t buflen)
{
  size_t consumed = 0;
  int ret;

  if (buflen == 0)
    {
      return 0;
    }

  ret = nxmutex_lock(&g_hci.lock);
  if (ret < 0)
    {
      return ret;
    }

  while (consumed < buflen)
    {
      int total;

      if (g_hci.txlen >= HCI_FRAME_MAX)
        {
          g_hci.txlen = 0;
          nxmutex_unlock(&g_hci.lock);
          return -EMSGSIZE;
        }

      g_hci.txfrm[g_hci.txlen++] = (uint8_t)buffer[consumed++];

      total = hci_frame_len(g_hci.txfrm, g_hci.txlen);
      if (total < 0)
        {
          /* An unknown type byte means the stream is not H4 or we lost
           * sync; report it rather than feed the controller garbage.
           */

          g_hci.txlen = 0;
          nxmutex_unlock(&g_hci.lock);
          return -EPROTO;
        }

      if (total > 0 && g_hci.txlen >= total)
        {
          ret = bk_ble_hci_raw_to_controller(g_hci.txfrm, g_hci.txlen);
          g_hci.txlen = 0;

          if (ret != 0)
            {
              nxmutex_unlock(&g_hci.lock);
              return -EIO;
            }
        }
    }

  nxmutex_unlock(&g_hci.lock);
  return (ssize_t)consumed;
}

/****************************************************************************
 * Name: bk7258_hci_poll
 ****************************************************************************/

static int bk7258_hci_poll(FAR struct file *filep, FAR struct pollfd *fds,
                           bool setup)
{
  int ret;

  ret = nxmutex_lock(&g_hci.lock);
  if (ret < 0)
    {
      return ret;
    }

  if (setup)
    {
      if (g_hci.fds != NULL)
        {
          ret = -EBUSY;
          goto out;
        }

      g_hci.fds = fds;
      fds->priv = &g_hci.fds;

      /* Writes never block here, and bytes may already be waiting. */

      poll_notify(&g_hci.fds, 1, POLLOUT |
                  (hci_rx_used() > 0 ? POLLIN : 0));
    }
  else
    {
      g_hci.fds = NULL;
      fds->priv = NULL;
    }

out:
  nxmutex_unlock(&g_hci.lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: bk7258_hci_register
 *
 * Description:
 *   Publish the transport.  Pass NULL for the openvela default,
 *   /dev/ttyHCI0, which is what CONFIG_BLUETOOTH_SERVICE_HCI_UART_NAME
 *   carries unless a board overrides it.
 *
 *   The controller must already be up: this only carries traffic, it
 *   does not bring the radio online.
 *
 ****************************************************************************/

int bk7258_hci_register(FAR const char *path)
{
  static bool registered;

  if (registered)
    {
      return OK;
    }

  memset(&g_hci, 0, sizeof(g_hci));
  nxmutex_init(&g_hci.lock);
  nxsem_init(&g_hci.rxsem, 0, 0);

  registered = true;
  return register_driver(path != NULL ? path : "/dev/ttyHCI0",
                         &g_hci_fops, 0666, NULL);
}

/****************************************************************************
 * Name: bk7258_hci_dropped
 *
 * Description:
 *   Frames the ring had no room for.  Non-zero means the host is not
 *   draining fast enough, which shows up as missing advertising reports
 *   rather than as an error anywhere.
 *
 ****************************************************************************/

uint32_t bk7258_hci_dropped(void)
{
  return g_hci.rxdrop;
}
