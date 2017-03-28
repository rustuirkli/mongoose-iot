/*
 * Copyright (c) 2014-2016 Cesanta Software Limited
 * All rights reserved
 */

#include "fw/src/mgos_uart.h"

#include <stdlib.h>

/* Driverlib includes */
#include "hw_types.h"

#include "hw_ints.h"
#include "hw_memmap.h"
#include "hw_uart.h"
#include "interrupt.h"
#include "pin.h"
#include "prcm.h"
#include "rom.h"
#include "rom_map.h"
#include "uart.h"
#include "utils.h"

#include "oslib/osi.h"

#include "fw/src/mgos_utils.h"

#define UART_RX_INTS (UART_INT_RX | UART_INT_RT)
#define UART_TX_INTS (UART_INT_TX)
#define UART_INFO_INTS (UART_INT_OE)

#define CC3200_UART_ISR_RX_BUF_SIZE 64

static struct mgos_uart_state *s_us[2];

struct cc3200_uart_state {
  uint32_t base;
  /*
   * CC3200 has a very short hardware RX FIFO (16 bytes). To avoid loss we want
   * to be able to receive data from the ISR, but mOS UART API does not allow
   * sharing state with the int handler: RX buffer are guarded by mutex which
   * cannot be taken from the ISR. As a workaround, we have this small auxiliary
   * buffer: handler will receive as many bytes as possible right away and stash
   * them in this buffer. It must be accessed with UART interrupts disabled.
   */
  struct cs_rbuf isr_rx_buf;
};

uint32_t cc3200_uart_get_base(int uart_no) {
  return (uart_no == 0 ? UARTA0_BASE : UARTA1_BASE);
}

static int cc3200_uart_rx_bytes(uint32_t base, struct cs_rbuf *rxb) {
  int num_recd = 0;
  while (rxb->avail > 0 && MAP_UARTCharsAvail(base)) {
    uint32_t chf = HWREG(base + UART_O_DR);
    /* Note: There are error flags here, we may be interested in those. */
    cs_rbuf_append_one(rxb, (uint8_t) chf);
    num_recd++;
  }
  return num_recd;
}

static void cc3200_int_handler(struct mgos_uart_state *us) {
  if (us == NULL) return;
  struct cc3200_uart_state *ds = (struct cc3200_uart_state *) us->dev_data;
  uint32_t int_st = MAP_UARTIntStatus(ds->base, true /* masked */);
  us->stats.ints++;
  uint32_t int_mask = UART_TX_INTS;
  if (int_st & UART_INT_OE) us->stats.rx_overflows++;
  if (int_st & (UART_RX_INTS | UART_TX_INTS)) {
    if (int_st & UART_RX_INTS) {
      struct cs_rbuf *irxb = &ds->isr_rx_buf;
      us->stats.rx_ints++;
      cc3200_uart_rx_bytes(ds->base, irxb);
      /* Do not disable RX ints if we have space in the ISR buffer. */
      if (irxb->avail == 0) int_mask |= UART_RX_INTS;
    }
    if (int_st & UART_TX_INTS) us->stats.tx_ints++;
    mgos_uart_schedule_dispatcher(us->uart_no, true /* from_isr */);
  }
  MAP_UARTIntDisable(ds->base, int_mask);
  MAP_UARTIntClear(ds->base, int_st);
}

void mgos_uart_dev_dispatch_rx_top(struct mgos_uart_state *us) {
  bool recd = false;
  int num_recd = 0;
  struct cc3200_uart_state *ds = (struct cc3200_uart_state *) us->dev_data;
  cs_rbuf_t *rxb = &us->rx_buf;
  cs_rbuf_t *irxb = &ds->isr_rx_buf;
  /* First, check the ISR buffer. */
  if (irxb->used > 0) {
    MAP_UARTIntDisable(ds->base, UART_RX_INTS);
    do {
      uint8_t *data;
      num_recd = cs_rbuf_get(irxb, MIN(rxb->avail, irxb->used), &data);
      cs_rbuf_append(rxb, data, num_recd);
      cs_rbuf_consume(irxb, num_recd);
      us->stats.rx_bytes += num_recd;
      recd = recd || (num_recd > 0);
    } while (num_recd > 0);
  }
recv_more:
  num_recd = cc3200_uart_rx_bytes(ds->base, rxb);
  us->stats.rx_bytes += num_recd;
  recd = recd || (num_recd > 0);
  /* If we received something during this cycle and there is buffer space
   * available, "linger" for some more, maybe there's more to come. */
  if (recd && rxb->avail > 0 && us->cfg->rx_linger_micros > 0) {
    /* Magic constants below are tweaked so that the loop takes at most the
     * configured number of microseconds. */
    int ctr = us->cfg->rx_linger_micros * 31 / 12;
    // HWREG(GPIOA1_BASE + GPIO_O_GPIO_DATA + 8) = 0xFF; /* Pin 64 */
    while (ctr-- > 0) {
      if (MAP_UARTCharsAvail(ds->base)) {
        us->stats.rx_linger_conts++;
        goto recv_more;
      }
    }
    // HWREG(GPIOA1_BASE + GPIO_O_GPIO_DATA + 8) = 0; /* Pin 64 */
  }
  MAP_UARTIntClear(ds->base, UART_RX_INTS);
}

void mgos_uart_dev_dispatch_tx_top(struct mgos_uart_state *us) {
  struct cc3200_uart_state *ds = (struct cc3200_uart_state *) us->dev_data;
  cs_rbuf_t *txb = &us->tx_buf;
  while (txb->used > 0 && MAP_UARTSpaceAvail(ds->base)) {
    uint8_t *cp;
    if (cs_rbuf_get(txb, 1, &cp) == 1) {
      HWREG(ds->base + UART_O_DR) = *cp;
      cs_rbuf_consume(txb, 1);
      us->stats.tx_bytes++;
    }
  }
  MAP_UARTIntClear(ds->base, UART_TX_INTS);
}

void mgos_uart_dev_dispatch_bottom(struct mgos_uart_state *us) {
  struct cc3200_uart_state *ds = (struct cc3200_uart_state *) us->dev_data;
  cs_rbuf_t *txb = &us->tx_buf;
  uint32_t int_ena = UART_INFO_INTS;
  if (us->rx_enabled && ds->isr_rx_buf.avail > 0) int_ena |= UART_RX_INTS;
  if (txb->used > 0) int_ena |= UART_TX_INTS;
  MAP_UARTIntEnable(ds->base, int_ena);
}

void mgos_uart_dev_set_rx_enabled(struct mgos_uart_state *us, bool enabled) {
  struct cc3200_uart_state *ds = (struct cc3200_uart_state *) us->dev_data;
  uint32_t ctl = HWREG(ds->base + UART_O_CTL);
  if (enabled) {
    if (us->cfg->rx_fc_ena) {
      ctl |= UART_CTL_RTSEN;
    }
  } else {
    /* Put /RTS under software control and set to 1. */
    ctl &= ~UART_CTL_RTSEN;
    ctl |= UART_CTL_RTS;
  }
  HWREG(ds->base + UART_O_CTL) = ctl;
}

void mgos_uart_dev_flush_fifo(struct mgos_uart_state *us) {
  struct cc3200_uart_state *ds = (struct cc3200_uart_state *) us->dev_data;
  while (MAP_UARTBusy(ds->base)) {
  }
}

static void u0_int(void) {
  cc3200_int_handler(s_us[0]);
}

static void u1_int(void) {
  cc3200_int_handler(s_us[1]);
}

void mgos_uart_dev_set_defaults(struct mgos_uart_config *cfg) {
  (void) cfg;
}

bool mgos_uart_dev_init(struct mgos_uart_state *us) {
  uint32_t base = cc3200_uart_get_base(us->uart_no);
  uint32_t periph, int_no;
  void (*int_handler)();

  /* TODO(rojer): Configurable pin mappings? */
  if (us->uart_no == 0) {
    periph = PRCM_UARTA0;
    int_no = INT_UARTA0;
    int_handler = u0_int;
    MAP_PinTypeUART(PIN_55, PIN_MODE_3); /* UART0_TX */
    MAP_PinTypeUART(PIN_57, PIN_MODE_3); /* UART0_RX */
    if (us->cfg->tx_fc_ena || us->cfg->rx_fc_ena) {
      /* No FC on UART0, according to the TRM. */
      return false;
    }
  } else if (us->uart_no == 1) {
    periph = PRCM_UARTA1;
    int_no = INT_UARTA1;
    int_handler = u1_int;
    MAP_PinTypeUART(PIN_07, PIN_MODE_5); /* UART1_TX */
    MAP_PinTypeUART(PIN_08, PIN_MODE_5); /* UART1_RX */
  } else {
    return false;
  }
  struct cc3200_uart_state *ds =
      (struct cc3200_uart_state *) calloc(1, sizeof(*ds));
  ds->base = base;
  cs_rbuf_init(&ds->isr_rx_buf, CC3200_UART_ISR_RX_BUF_SIZE);
  MAP_PRCMPeripheralClkEnable(periph, PRCM_RUN_MODE_CLK);
  MAP_UARTConfigSetExpClk(
      base, MAP_PRCMPeripheralClockGet(periph), us->cfg->baud_rate,
      (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE));
  if (us->cfg->tx_fc_ena || us->cfg->rx_fc_ena) {
    /* Note: only UART1 */
    uint32_t ctl = HWREG(base + UART_O_CTL);
    if (us->cfg->tx_fc_ena) {
      ctl |= UART_CTL_CTSEN;
      MAP_PinTypeUART(PIN_61, PIN_MODE_3); /* UART1_CTS */
    }
    if (us->cfg->rx_fc_ena) {
      ctl |= UART_CTL_RTSEN;
      MAP_PinTypeUART(PIN_62, PIN_MODE_3); /* UART1_RTS */
    }
    HWREG(base + UART_O_CTL) = ctl;
  }
  MAP_UARTFIFOLevelSet(base, UART_FIFO_TX1_8, UART_FIFO_RX7_8);
  MAP_UARTFIFOEnable(base);
  MAP_UARTIntDisable(base, ~0); /* Start with ints disabled. */
  osi_InterruptRegister(int_no, int_handler, INT_PRIORITY_LVL_1);
  us->dev_data = ds;
  s_us[us->uart_no] = us;
  return true;
}

void mgos_uart_dev_deinit(struct mgos_uart_state *us) {
  struct cc3200_uart_state *ds = (struct cc3200_uart_state *) us->dev_data;
  MAP_UARTDisable(ds->base);
  MAP_UARTIntDisable(ds->base, ~0);
  s_us[us->uart_no] = NULL;
  free(ds);
}

int cc3200_uart_cts(int uart_no) {
  uint32_t base = cc3200_uart_get_base(uart_no);
  return (UARTModemStatusGet(base) != 0);
}

uint32_t cc3200_uart_raw_ints(int uart_no) {
  uint32_t base = cc3200_uart_get_base(uart_no);
  return MAP_UARTIntStatus(base, false /* masked */);
}

uint32_t cc3200_uart_int_mask(int uart_no) {
  uint32_t base = cc3200_uart_get_base(uart_no);
  return HWREG(base + UART_O_IM);
}
