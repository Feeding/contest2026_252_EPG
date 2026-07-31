/****************************************************************************
 * board/contest_board/chip/bk7258_serial.c
 *
 * BK7258 UART character driver.
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

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/serial/serial.h>

#include "arm_internal.h"
#include "bk7258_lowputc.h"
#include "bk7258_uart.h"
#include "chip.h"

#ifdef USE_SERIALDRIVER

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Decide which UART becomes /dev/console and how the others are numbered.
 * The console always takes ttyS0 so that it keeps a stable name.
 */

#if defined(CONFIG_UART0_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART0)
#  define CONSOLE_DEV     g_uart0port
#  define TTYS0_DEV       g_uart0port
#  define UART0_ASSIGNED  1
#elif defined(CONFIG_UART1_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART1)
#  define CONSOLE_DEV     g_uart1port
#  define TTYS0_DEV       g_uart1port
#  define UART1_ASSIGNED  1
#elif defined(CONFIG_UART2_SERIAL_CONSOLE) && defined(CONFIG_BK7258_UART2)
#  define CONSOLE_DEV     g_uart2port
#  define TTYS0_DEV       g_uart2port
#  define UART2_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART0)
#  define TTYS0_DEV       g_uart0port
#  define UART0_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART1)
#  define TTYS0_DEV       g_uart1port
#  define UART1_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART2)
#  define TTYS0_DEV       g_uart2port
#  define UART2_ASSIGNED  1
#endif

#if defined(CONFIG_BK7258_UART0) && !defined(UART0_ASSIGNED)
#  define TTYS1_DEV       g_uart0port
#  define UART0_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART1) && !defined(UART1_ASSIGNED)
#  define TTYS1_DEV       g_uart1port
#  define UART1_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART2) && !defined(UART2_ASSIGNED)
#  define TTYS1_DEV       g_uart2port
#  define UART2_ASSIGNED  1
#endif

#if defined(CONFIG_BK7258_UART1) && !defined(UART1_ASSIGNED)
#  define TTYS2_DEV       g_uart1port
#  define UART1_ASSIGNED  1
#elif defined(CONFIG_BK7258_UART2) && !defined(UART2_ASSIGNED)
#  define TTYS2_DEV       g_uart2port
#  define UART2_ASSIGNED  1
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct bk7258_dev_s
{
  uintptr_t    base;      /* Base address of the UART registers */
  uint32_t     baud;      /* Configured baud rate */
  uint8_t      irq;       /* IRQ number of this UART */
  uint8_t      uart;      /* UART index, 0-2 */
  uint8_t      databits;  /* Number of data bits */
  uint8_t      parity;    /* 0=none, 1=odd, 2=even */
  bool         stop2;     /* true: two stop bits */
  uint32_t     im;        /* Shadow copy of the interrupt enable register */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  bk7258_setup(struct uart_dev_s *dev);
static void bk7258_shutdown(struct uart_dev_s *dev);
static int  bk7258_attach(struct uart_dev_s *dev);
static void bk7258_detach(struct uart_dev_s *dev);
static int  bk7258_interrupt(int irq, void *context, void *arg);
static int  bk7258_ioctl(struct file *filep, int cmd, unsigned long arg);
static int  bk7258_receive(struct uart_dev_s *dev, unsigned int *status);
static void bk7258_rxint(struct uart_dev_s *dev, bool enable);
static bool bk7258_rxavailable(struct uart_dev_s *dev);
static void bk7258_send(struct uart_dev_s *dev, int ch);
static void bk7258_txint(struct uart_dev_s *dev, bool enable);
static bool bk7258_txready(struct uart_dev_s *dev);
static bool bk7258_txempty(struct uart_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct uart_ops_s g_uart_ops =
{
  .setup       = bk7258_setup,
  .shutdown    = bk7258_shutdown,
  .attach      = bk7258_attach,
  .detach      = bk7258_detach,
  .ioctl       = bk7258_ioctl,
  .receive     = bk7258_receive,
  .rxint       = bk7258_rxint,
  .rxavailable = bk7258_rxavailable,
  .send        = bk7258_send,
  .txint       = bk7258_txint,
  .txready     = bk7258_txready,
  .txempty     = bk7258_txempty,
};

#ifdef CONFIG_BK7258_UART0
static char g_uart0rxbuffer[CONFIG_UART0_RXBUFSIZE];
static char g_uart0txbuffer[CONFIG_UART0_TXBUFSIZE];

static struct bk7258_dev_s g_uart0priv =
{
  .base     = BK7258_UART0_BASE,
  .baud     = CONFIG_UART0_BAUD,
  .irq      = BK7258_IRQ_UART0,
  .uart     = 0,
  .databits = CONFIG_UART0_BITS,
  .parity   = CONFIG_UART0_PARITY,
  .stop2    = CONFIG_UART0_2STOP != 0,
};

static struct uart_dev_s g_uart0port =
{
  .recv     =
  {
    .size   = CONFIG_UART0_RXBUFSIZE,
    .buffer = g_uart0rxbuffer,
  },
  .xmit     =
  {
    .size   = CONFIG_UART0_TXBUFSIZE,
    .buffer = g_uart0txbuffer,
  },
  .ops      = &g_uart_ops,
  .priv     = &g_uart0priv,
};
#endif

#ifdef CONFIG_BK7258_UART1
static char g_uart1rxbuffer[CONFIG_UART1_RXBUFSIZE];
static char g_uart1txbuffer[CONFIG_UART1_TXBUFSIZE];

static struct bk7258_dev_s g_uart1priv =
{
  .base     = BK7258_UART1_BASE,
  .baud     = CONFIG_UART1_BAUD,
  .irq      = BK7258_IRQ_UART1,
  .uart     = 1,
  .databits = CONFIG_UART1_BITS,
  .parity   = CONFIG_UART1_PARITY,
  .stop2    = CONFIG_UART1_2STOP != 0,
};

static struct uart_dev_s g_uart1port =
{
  .recv     =
  {
    .size   = CONFIG_UART1_RXBUFSIZE,
    .buffer = g_uart1rxbuffer,
  },
  .xmit     =
  {
    .size   = CONFIG_UART1_TXBUFSIZE,
    .buffer = g_uart1txbuffer,
  },
  .ops      = &g_uart_ops,
  .priv     = &g_uart1priv,
};
#endif

#ifdef CONFIG_BK7258_UART2
static char g_uart2rxbuffer[CONFIG_UART2_RXBUFSIZE];
static char g_uart2txbuffer[CONFIG_UART2_TXBUFSIZE];

static struct bk7258_dev_s g_uart2priv =
{
  .base     = BK7258_UART2_BASE,
  .baud     = CONFIG_UART2_BAUD,
  .irq      = BK7258_IRQ_UART2,
  .uart     = 2,
  .databits = CONFIG_UART2_BITS,
  .parity   = CONFIG_UART2_PARITY,
  .stop2    = CONFIG_UART2_2STOP != 0,
};

static struct uart_dev_s g_uart2port =
{
  .recv     =
  {
    .size   = CONFIG_UART2_RXBUFSIZE,
    .buffer = g_uart2rxbuffer,
  },
  .xmit     =
  {
    .size   = CONFIG_UART2_TXBUFSIZE,
    .buffer = g_uart2txbuffer,
  },
  .ops      = &g_uart_ops,
  .priv     = &g_uart2priv,
};
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t bk7258_serialin(struct bk7258_dev_s *priv,
                                       unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void bk7258_serialout(struct bk7258_dev_s *priv,
                                    unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/****************************************************************************
 * Name: bk7258_setup
 *
 * Description:
 *   Configure the UART baud, bits, parity and stop bits.  Interrupts are
 *   not enabled here; that happens in bk7258_attach().
 *
 ****************************************************************************/

static int bk7258_setup(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

#ifndef CONFIG_SUPPRESS_UART_CONFIG
  bk7258_uart_configure(priv->base, priv->uart, priv->baud, priv->databits,
                        priv->parity, priv->stop2);
#endif

  priv->im = 0;
  bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, 0);

  return OK;
}

/****************************************************************************
 * Name: bk7258_shutdown
 ****************************************************************************/

static void bk7258_shutdown(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  /* Mask every source and stop the transceiver. */

  priv->im = 0;
  bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, 0);

  modifyreg32(priv->base + BK7258_UART_CONFIG_OFFSET,
              UART_CONFIG_TX_ENABLE | UART_CONFIG_RX_ENABLE, 0);
}

/****************************************************************************
 * Name: bk7258_attach
 ****************************************************************************/

static int bk7258_attach(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;
  int ret;

  ret = irq_attach(priv->irq, bk7258_interrupt, dev);
  if (ret == OK)
    {
      up_enable_irq(priv->irq);
    }

  return ret;
}

/****************************************************************************
 * Name: bk7258_detach
 ****************************************************************************/

static void bk7258_detach(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);
}

/****************************************************************************
 * Name: bk7258_interrupt
 ****************************************************************************/

static int bk7258_interrupt(int irq, void *context, void *arg)
{
  struct uart_dev_s *dev = (struct uart_dev_s *)arg;
  struct bk7258_dev_s *priv;
  uint32_t status;

  DEBUGASSERT(dev != NULL && dev->priv != NULL);
  priv = dev->priv;

  /* Read and acknowledge whatever is pending.  The status bits are
   * write-1-to-clear.
   */

  status = bk7258_serialin(priv, BK7258_UART_INT_STATUS_OFFSET);
  bk7258_serialout(priv, BK7258_UART_INT_STATUS_OFFSET, status);

  /* Only act on sources we asked for. */

  status &= priv->im;

  if ((status & (UART_INT_RX_NEED_READ | UART_INT_RX_FINISH)) != 0)
    {
      uart_recvchars(dev);
    }

  if ((status & UART_INT_TX_NEED_WRITE) != 0)
    {
      uart_xmitchars(dev);
    }

  return OK;
}

/****************************************************************************
 * Name: bk7258_ioctl
 ****************************************************************************/

static int bk7258_ioctl(struct file *filep, int cmd, unsigned long arg)
{
#ifdef CONFIG_SERIAL_TERMIOS
  struct inode *inode = filep->f_inode;
  struct uart_dev_s *dev = inode->i_private;
  struct bk7258_dev_s *priv = dev->priv;
#endif
  int ret = OK;

  switch (cmd)
    {
#ifdef CONFIG_SERIAL_TERMIOS
      case TCGETS:
        {
          struct termios *termiosp = (struct termios *)arg;

          if (termiosp == NULL)
            {
              return -EINVAL;
            }

          termiosp->c_cflag = 0;

          if (priv->parity == 1)
            {
              termiosp->c_cflag |= PARENB | PARODD;
            }
          else if (priv->parity == 2)
            {
              termiosp->c_cflag |= PARENB;
            }

          if (priv->stop2)
            {
              termiosp->c_cflag |= CSTOPB;
            }

          termiosp->c_cflag |= CS5 + (priv->databits - 5);

          cfsetispeed(termiosp, priv->baud);
          cfsetospeed(termiosp, priv->baud);
        }
        break;

      case TCSETS:
        {
          struct termios *termiosp = (struct termios *)arg;

          if (termiosp == NULL)
            {
              return -EINVAL;
            }

          switch (termiosp->c_cflag & CSIZE)
            {
              case CS5:
                priv->databits = 5;
                break;

              case CS6:
                priv->databits = 6;
                break;

              case CS7:
                priv->databits = 7;
                break;

              default:
                priv->databits = 8;
                break;
            }

          if ((termiosp->c_cflag & PARENB) == 0)
            {
              priv->parity = 0;
            }
          else
            {
              priv->parity = (termiosp->c_cflag & PARODD) != 0 ? 1 : 2;
            }

          priv->stop2 = (termiosp->c_cflag & CSTOPB) != 0;
          priv->baud  = cfgetispeed(termiosp);

          /* Re-apply the line settings.  Interrupt enables survive because
           * bk7258_uart_configure() masks them and we restore our shadow.
           */

          bk7258_uart_configure(priv->base, priv->uart, priv->baud,
                                priv->databits, priv->parity, priv->stop2);
          bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, priv->im);
        }
        break;
#endif /* CONFIG_SERIAL_TERMIOS */

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: bk7258_receive
 ****************************************************************************/

static int bk7258_receive(struct uart_dev_s *dev, unsigned int *status)
{
  struct bk7258_dev_s *priv = dev->priv;
  uint32_t regval;

  if (status != NULL)
    {
      *status = bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET);
    }

  regval = bk7258_serialin(priv, BK7258_UART_FIFO_PORT_OFFSET);
  return (int)((regval & UART_FIFO_PORT_RXDATA_MASK) >>
               UART_FIFO_PORT_RXDATA_SHIFT);
}

/****************************************************************************
 * Name: bk7258_rxint
 ****************************************************************************/

static void bk7258_rxint(struct uart_dev_s *dev, bool enable)
{
  struct bk7258_dev_s *priv = dev->priv;
  irqstate_t flags;

  flags = enter_critical_section();

  if (enable)
    {
      priv->im |= UART_INT_RX_ALL;
    }
  else
    {
      priv->im &= ~UART_INT_RX_ALL;
    }

  bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, priv->im);
  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_rxavailable
 ****************************************************************************/

static bool bk7258_rxavailable(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  return (bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET) &
          UART_FIFO_STATUS_RX_EMPTY) == 0;
}

/****************************************************************************
 * Name: bk7258_send
 ****************************************************************************/

static void bk7258_send(struct uart_dev_s *dev, int ch)
{
  struct bk7258_dev_s *priv = dev->priv;

  bk7258_serialout(priv, BK7258_UART_FIFO_PORT_OFFSET,
                   (uint32_t)(ch & 0xff));
}

/****************************************************************************
 * Name: bk7258_txint
 ****************************************************************************/

static void bk7258_txint(struct uart_dev_s *dev, bool enable)
{
  struct bk7258_dev_s *priv = dev->priv;
  irqstate_t flags;

  flags = enter_critical_section();

  if (enable)
    {
      priv->im |= UART_INT_TX_NEED_WRITE;
      bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, priv->im);

      /* Prime the pump: the FIFO may already be below the threshold, in
       * which case no fresh edge would ever arrive.
       */

      uart_xmitchars(dev);
    }
  else
    {
      priv->im &= ~UART_INT_TX_NEED_WRITE;
      bk7258_serialout(priv, BK7258_UART_INT_ENABLE_OFFSET, priv->im);
    }

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: bk7258_txready
 ****************************************************************************/

static bool bk7258_txready(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  return (bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET) &
          UART_FIFO_STATUS_WR_READY) != 0;
}

/****************************************************************************
 * Name: bk7258_txempty
 ****************************************************************************/

static bool bk7258_txempty(struct uart_dev_s *dev)
{
  struct bk7258_dev_s *priv = dev->priv;

  return (bk7258_serialin(priv, BK7258_UART_FIFO_STATUS_OFFSET) &
          UART_FIFO_STATUS_TX_EMPTY) != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: arm_earlyserialinit
 *
 * Description:
 *   Bring up the console UART early so that debug output works before the
 *   full serial driver is registered.
 *
 ****************************************************************************/

void arm_earlyserialinit(void)
{
#ifdef CONSOLE_DEV
  CONSOLE_DEV.isconsole = true;
  bk7258_setup(&CONSOLE_DEV);
#endif
}

/****************************************************************************
 * Name: arm_serialinit
 *
 * Description:
 *   Register /dev/console and /dev/ttyS[n].
 *
 ****************************************************************************/

void arm_serialinit(void)
{
#ifdef CONSOLE_DEV
  uart_register("/dev/console", &CONSOLE_DEV);
#endif
#ifdef TTYS0_DEV
  uart_register("/dev/ttyS0", &TTYS0_DEV);
#endif
#ifdef TTYS1_DEV
  uart_register("/dev/ttyS1", &TTYS1_DEV);
#endif
#ifdef TTYS2_DEV
  uart_register("/dev/ttyS2", &TTYS2_DEV);
#endif
}

/****************************************************************************
 * Name: up_putc
 *
 * Description:
 *   Write one character to the console, bypassing the driver.  Used by the
 *   low-level debug and syslog paths.
 *
 ****************************************************************************/

void up_putc(int ch)
{
#ifdef HAVE_CONSOLE
  arm_lowputc((char)ch);
#endif
}

#endif /* USE_SERIALDRIVER */
