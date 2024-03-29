/*
 * LIRC SIR driver, (C) 2000 Milan Pikula <www@fornax.sk>
 *
 * sir_ir - Device driver for use with SIR (serial infra red)
 * mode of IrDA on many notebooks.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *
 * 2000/09/16 Frank Przybylski <mail@frankprzybylski.de> :
 *  added timeout and relaxed pulse detection, removed gap bug
 *
 * 2000/12/15 Christoph Bartelmus <lirc@bartelmus.de> :
 *   added support for Tekram Irmate 210 (sending does not work yet,
 *   kind of disappointing that nobody was able to implement that
 *   before),
 *   major clean-up
 *
 * 2001/02/27 Christoph Bartelmus <lirc@bartelmus.de> :
 *   added support for StrongARM SA1100 embedded microprocessor
 *   parts cut'n'pasted from sa1100_ir.c (C) 2000 Russell King
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/serial_reg.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/io.h>
#include <asm/irq.h>
#include <linux/fcntl.h>
#include <linux/platform_device.h>

#include <linux/timer.h>

#include <media/rc-core.h>

/* SECTION: Definitions */

/*** Tekram dongle ***/
#ifdef LIRC_SIR_TEKRAM
/* stolen from kernel source */
/* definitions for Tekram dongle */
#define TEKRAM_115200 0x00
#define TEKRAM_57600  0x01
#define TEKRAM_38400  0x02
#define TEKRAM_19200  0x03
#define TEKRAM_9600   0x04
#define TEKRAM_2400   0x08

#define TEKRAM_PW 0x10 /* Pulse select bit */

/* 10bit * 1s/115200bit in milliseconds = 87ms*/
#define TIME_CONST (10000000ul/115200ul)

#endif

#ifdef LIRC_SIR_ACTISYS_ACT200L
static void init_act200(void);
#elif defined(LIRC_SIR_ACTISYS_ACT220L)
static void init_act220(void);
#endif

#define PULSE '['

#ifndef LIRC_SIR_TEKRAM
/* 9bit * 1s/115200bit in milli seconds = 78.125ms*/
#define TIME_CONST (9000000ul/115200ul)
#endif


/* timeout for sequences in jiffies (=5/100s), must be longer than TIME_CONST */
#define SIR_TIMEOUT	(HZ*5/100)

#ifndef LIRC_ON_SA1100
#ifndef LIRC_IRQ
#define LIRC_IRQ 4
#endif
#ifndef LIRC_PORT
/* for external dongles, default to com1 */
#if defined(LIRC_SIR_ACTISYS_ACT200L)         || \
	    defined(LIRC_SIR_ACTISYS_ACT220L) || \
	    defined(LIRC_SIR_TEKRAM)
#define LIRC_PORT 0x3f8
#else
/* onboard sir ports are typically com3 */
#define LIRC_PORT 0x3e8
#endif
#endif

static int io = LIRC_PORT;
static int irq = LIRC_IRQ;
static int threshold = 3;
#endif

static DEFINE_SPINLOCK(timer_lock);
static struct timer_list timerlist;
/* time of last signal change detected */
static ktime_t last;
/* time of last UART data ready interrupt */
static ktime_t last_intr_time;
static int last_value;
static struct rc_dev *rcdev;

static struct platform_device *sir_ir_dev;

static DEFINE_SPINLOCK(hardware_lock);

static bool debug;

/* SECTION: Prototypes */

/* Communication with user-space */
static void add_read_queue(int flag, unsigned long val);
static int init_chrdev(void);
/* Hardware */
static irqreturn_t sir_interrupt(int irq, void *dev_id);
static void send_space(unsigned long len);
static void send_pulse(unsigned long len);
static int init_hardware(void);
static void drop_hardware(void);
/* Initialisation */
static int init_port(void);
static void drop_port(void);

static inline unsigned int sinp(int offset)
{
	return inb(io + offset);
}

static inline void soutp(int offset, int value)
{
	outb(value, io + offset);
}

#ifndef MAX_UDELAY_MS
#define MAX_UDELAY_US 5000
#else
#define MAX_UDELAY_US (MAX_UDELAY_MS*1000)
#endif

static void safe_udelay(unsigned long usecs)
{
	while (usecs > MAX_UDELAY_US) {
		udelay(MAX_UDELAY_US);
		usecs -= MAX_UDELAY_US;
	}
	udelay(usecs);
}

/* SECTION: Communication with user-space */
static int sir_tx_ir(struct rc_dev *dev, unsigned int *tx_buf,
		     unsigned int count)
{
	unsigned long flags;
	int i;

	local_irq_save(flags);
	for (i = 0; i < count;) {
		if (tx_buf[i])
			send_pulse(tx_buf[i]);
		i++;
		if (i >= count)
			break;
		if (tx_buf[i])
			send_space(tx_buf[i]);
		i++;
	}
	local_irq_restore(flags);

	return count;
}

static void add_read_queue(int flag, unsigned long val)
{
	DEFINE_IR_RAW_EVENT(ev);

	pr_debug("add flag %d with val %lu\n", flag, val);

	/*
	 * statistically, pulses are ~TIME_CONST/2 too long. we could
	 * maybe make this more exact, but this is good enough
	 */
	if (flag) {
		/* pulse */
		if (val > TIME_CONST / 2)
			val -= TIME_CONST / 2;
		else /* should not ever happen */
			val = 1;
		ev.pulse = true;
	} else {
		val += TIME_CONST / 2;
	}
	ev.duration = US_TO_NS(val);

	ir_raw_event_store_with_filter(rcdev, &ev);
}

static int init_chrdev(void)
{
	rcdev = devm_rc_allocate_device(&sir_ir_dev->dev, RC_DRIVER_IR_RAW);
	if (!rcdev)
		return -ENOMEM;

	rcdev->input_phys = KBUILD_MODNAME "/input0";
	rcdev->input_id.bustype = BUS_HOST;
	rcdev->input_id.vendor = 0x0001;
	rcdev->input_id.product = 0x0001;
	rcdev->input_id.version = 0x0100;
	rcdev->tx_ir = sir_tx_ir;
	rcdev->allowed_protocols = RC_BIT_ALL_IR_DECODER;
	rcdev->map_name = RC_MAP_RC6_MCE;
	rcdev->timeout = IR_DEFAULT_TIMEOUT;
	rcdev->dev.parent = &sir_ir_dev->dev;

	return devm_rc_register_device(&sir_ir_dev->dev, rcdev);
}

/* SECTION: Hardware */
static void sir_timeout(unsigned long data)
{
	/*
	 * if last received signal was a pulse, but receiving stopped
	 * within the 9 bit frame, we need to finish this pulse and
	 * simulate a signal change to from pulse to space. Otherwise
	 * upper layers will receive two sequences next time.
	 */

	unsigned long flags;
	unsigned long pulse_end;

	/* avoid interference with interrupt */
	spin_lock_irqsave(&timer_lock, flags);
	if (last_value) {
		/* clear unread bits in UART and restart */
		outb(UART_FCR_CLEAR_RCVR, io + UART_FCR);
		/* determine 'virtual' pulse end: */
		pulse_end = min_t(unsigned long,
				  ktime_us_delta(last, last_intr_time),
				  IR_MAX_DURATION);
		dev_dbg(&sir_ir_dev->dev, "timeout add %d for %lu usec\n",
			last_value, pulse_end);
		add_read_queue(last_value, pulse_end);
		last_value = 0;
		last = last_intr_time;
	}
	spin_unlock_irqrestore(&timer_lock, flags);
	ir_raw_event_handle(rcdev);
}

static irqreturn_t sir_interrupt(int irq, void *dev_id)
{
	unsigned char data;
	ktime_t curr_time;
	static unsigned long delt;
	unsigned long deltintr;
	unsigned long flags;
	int iir, lsr;

	while ((iir = inb(io + UART_IIR) & UART_IIR_ID)) {
		switch (iir&UART_IIR_ID) { /* FIXME toto treba preriedit */
		case UART_IIR_MSI:
			(void) inb(io + UART_MSR);
			break;
		case UART_IIR_RLSI:
			(void) inb(io + UART_LSR);
			break;
		case UART_IIR_THRI:
#if 0
			if (lsr & UART_LSR_THRE) /* FIFO is empty */
				outb(data, io + UART_TX)
#endif
			break;
		case UART_IIR_RDI:
			/* avoid interference with timer */
			spin_lock_irqsave(&timer_lock, flags);
			do {
				del_timer(&timerlist);
				data = inb(io + UART_RX);
				curr_time = ktime_get();
				delt = min_t(unsigned long,
					     ktime_us_delta(last, curr_time),
					     IR_MAX_DURATION);
				deltintr = min_t(unsigned long,
						 ktime_us_delta(last_intr_time,
								curr_time),
						 IR_MAX_DURATION);
				dev_dbg(&sir_ir_dev->dev, "t %lu, d %d\n",
					deltintr, (int)data);
				/*
				 * if nothing came in last X cycles,
				 * it was gap
				 */
				if (deltintr > TIME_CONST * threshold) {
					if (last_value) {
						dev_dbg(&sir_ir_dev->dev, "GAP\n");
						/* simulate signal change */
						add_read_queue(last_value,
							       delt -
							       deltintr);
						last_value = 0;
						last = last_intr_time;
						delt = deltintr;
					}
				}
				data = 1;
				if (data ^ last_value) {
					/*
					 * deltintr > 2*TIME_CONST, remember?
					 * the other case is timeout
					 */
					add_read_queue(last_value,
						       delt-TIME_CONST);
					last_value = data;
					last = curr_time;
					last = ktime_sub_us(last,
							    TIME_CONST);
				}
				last_intr_time = curr_time;
				if (data) {
					/*
					 * start timer for end of
					 * sequence detection
					 */
					timerlist.expires = jiffies +
								SIR_TIMEOUT;
					add_timer(&timerlist);
				}

				lsr = inb(io + UART_LSR);
			} while (lsr & UART_LSR_DR); /* data ready */
			spin_unlock_irqrestore(&timer_lock, flags);
			break;
		default:
			break;
		}
	}
	ir_raw_event_handle(rcdev);
	return IRQ_RETVAL(IRQ_HANDLED);
}

static void send_space(unsigned long len)
{
	safe_udelay(len);
}

static void send_pulse(unsigned long len)
{
	long bytes_out = len / TIME_CONST;

	if (bytes_out == 0)
		bytes_out++;

	while (bytes_out--) {
		outb(PULSE, io + UART_TX);
		/* FIXME treba seriozne cakanie z char/serial.c */
		while (!(inb(io + UART_LSR) & UART_LSR_THRE))
			;
	}
}

static int init_hardware(void)
{
	unsigned long flags;

	spin_lock_irqsave(&hardware_lock, flags);
	/* reset UART */
#if defined(LIRC_SIR_TEKRAM)
	/* disable FIFO */
	soutp(UART_FCR,
	      UART_FCR_CLEAR_RCVR|
	      UART_FCR_CLEAR_XMIT|
	      UART_FCR_TRIGGER_1);

	/* Set DLAB 0. */
	soutp(UART_LCR, sinp(UART_LCR) & (~UART_LCR_DLAB));

	/* First of all, disable all interrupts */
	soutp(UART_IER, sinp(UART_IER) &
	      (~(UART_IER_MSI|UART_IER_RLSI|UART_IER_THRI|UART_IER_RDI)));

	/* Set DLAB 1. */
	soutp(UART_LCR, sinp(UART_LCR) | UART_LCR_DLAB);

	/* Set divisor to 12 => 9600 Baud */
	soutp(UART_DLM, 0);
	soutp(UART_DLL, 12);

	/* Set DLAB 0. */
	soutp(UART_LCR, sinp(UART_LCR) & (~UART_LCR_DLAB));

	/* power supply */
	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2);
	safe_udelay(50*1000);

	/* -DTR low -> reset PIC */
	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_OUT2);
	udelay(1*1000);

	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2);
	udelay(100);


	/* -RTS low -> send control byte */
	soutp(UART_MCR, UART_MCR_DTR|UART_MCR_OUT2);
	udelay(7);
	soutp(UART_TX, TEKRAM_115200|TEKRAM_PW);

	/* one byte takes ~1042 usec to transmit at 9600,8N1 */
	udelay(1500);

	/* back to normal operation */
	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2);
	udelay(50);

	udelay(1500);

	/* read previous control byte */
	pr_info("0x%02x\n", sinp(UART_RX));

	/* Set DLAB 1. */
	soutp(UART_LCR, sinp(UART_LCR) | UART_LCR_DLAB);

	/* Set divisor to 1 => 115200 Baud */
	soutp(UART_DLM, 0);
	soutp(UART_DLL, 1);

	/* Set DLAB 0, 8 Bit */
	soutp(UART_LCR, UART_LCR_WLEN8);
	/* enable interrupts */
	soutp(UART_IER, sinp(UART_IER)|UART_IER_RDI);
#else
	outb(0, io + UART_MCR);
	outb(0, io + UART_IER);
	/* init UART */
	/* set DLAB, speed = 115200 */
	outb(UART_LCR_DLAB | UART_LCR_WLEN7, io + UART_LCR);
	outb(1, io + UART_DLL); outb(0, io + UART_DLM);
	/* 7N1+start = 9 bits at 115200 ~ 3 bits at 44000 */
	outb(UART_LCR_WLEN7, io + UART_LCR);
	/* FIFO operation */
	outb(UART_FCR_ENABLE_FIFO, io + UART_FCR);
	/* interrupts */
	/* outb(UART_IER_RLSI|UART_IER_RDI|UART_IER_THRI, io + UART_IER); */
	outb(UART_IER_RDI, io + UART_IER);
	/* turn on UART */
	outb(UART_MCR_DTR|UART_MCR_RTS|UART_MCR_OUT2, io + UART_MCR);
#ifdef LIRC_SIR_ACTISYS_ACT200L
	init_act200();
#elif defined(LIRC_SIR_ACTISYS_ACT220L)
	init_act220();
#endif
#endif
	spin_unlock_irqrestore(&hardware_lock, flags);
	return 0;
}

static void drop_hardware(void)
{
	unsigned long flags;

	spin_lock_irqsave(&hardware_lock, flags);

	/* turn off interrupts */
	outb(0, io + UART_IER);

	spin_unlock_irqrestore(&hardware_lock, flags);
}

/* SECTION: Initialisation */

static int init_port(void)
{
	int retval;

	/* get I/O port access and IRQ line */
	if (!request_region(io, 8, KBUILD_MODNAME)) {
		pr_err("i/o port 0x%.4x already in use.\n", io);
		return -EBUSY;
	}
	retval = request_irq(irq, sir_interrupt, 0,
			     KBUILD_MODNAME, NULL);
	if (retval < 0) {
		release_region(io, 8);
		pr_err("IRQ %d already in use.\n", irq);
		return retval;
	}
	pr_info("I/O port 0x%.4x, IRQ %d.\n", io, irq);

	setup_timer(&timerlist, sir_timeout, 0);

	return 0;
}

static void drop_port(void)
{
	free_irq(irq, NULL);
	del_timer_sync(&timerlist);
	release_region(io, 8);
}

#ifdef LIRC_SIR_ACTISYS_ACT200L
/* Crystal/Cirrus CS8130 IR transceiver, used in Actisys Act200L dongle */
/* some code borrowed from Linux IRDA driver */

/* Register 0: Control register #1 */
#define ACT200L_REG0    0x00
#define ACT200L_TXEN    0x01 /* Enable transmitter */
#define ACT200L_RXEN    0x02 /* Enable receiver */
#define ACT200L_ECHO    0x08 /* Echo control chars */

/* Register 1: Control register #2 */
#define ACT200L_REG1    0x10
#define ACT200L_LODB    0x01 /* Load new baud rate count value */
#define ACT200L_WIDE    0x04 /* Expand the maximum allowable pulse */

/* Register 3: Transmit mode register #2 */
#define ACT200L_REG3    0x30
#define ACT200L_B0      0x01 /* DataBits, 0=6, 1=7, 2=8, 3=9(8P)  */
#define ACT200L_B1      0x02 /* DataBits, 0=6, 1=7, 2=8, 3=9(8P)  */
#define ACT200L_CHSY    0x04 /* StartBit Synced 0=bittime, 1=startbit */

/* Register 4: Output Power register */
#define ACT200L_REG4    0x40
#define ACT200L_OP0     0x01 /* Enable LED1C output */
#define ACT200L_OP1     0x02 /* Enable LED2C output */
#define ACT200L_BLKR    0x04

/* Register 5: Receive Mode register */
#define ACT200L_REG5    0x50
#define ACT200L_RWIDL   0x01 /* fixed 1.6us pulse mode */
    /*.. other various IRDA bit modes, and TV remote modes..*/

/* Register 6: Receive Sensitivity register #1 */
#define ACT200L_REG6    0x60
#define ACT200L_RS0     0x01 /* receive threshold bit 0 */
#define ACT200L_RS1     0x02 /* receive threshold bit 1 */

/* Register 7: Receive Sensitivity register #2 */
#define ACT200L_REG7    0x70
#define ACT200L_ENPOS   0x04 /* Ignore the falling edge */

/* Register 8,9: Baud Rate Divider register #1,#2 */
#define ACT200L_REG8    0x80
#define ACT200L_REG9    0x90

#define ACT200L_2400    0x5f
#define ACT200L_9600    0x17
#define ACT200L_19200   0x0b
#define ACT200L_38400   0x05
#define ACT200L_57600   0x03
#define ACT200L_115200  0x01

/* Register 13: Control register #3 */
#define ACT200L_REG13   0xd0
#define ACT200L_SHDW    0x01 /* Enable access to shadow registers */

/* Register 15: Status register */
#define ACT200L_REG15   0xf0

/* Register 21: Control register #4 */
#define ACT200L_REG21   0x50
#define ACT200L_EXCK    0x02 /* Disable clock output driver */
#define ACT200L_OSCL    0x04 /* oscillator in low power, medium accuracy mode */

static void init_act200(void)
{
	int i;
	__u8 control[] = {
		ACT200L_REG15,
		ACT200L_REG13 | ACT200L_SHDW,
		ACT200L_REG21 | ACT200L_EXCK | ACT200L_OSCL,
		ACT200L_REG13,
		ACT200L_REG7  | ACT200L_ENPOS,
		ACT200L_REG6  | ACT200L_RS0  | ACT200L_RS1,
		ACT200L_REG5  | ACT200L_RWIDL,
		ACT200L_REG4  | ACT200L_OP0  | ACT200L_OP1 | ACT200L_BLKR,
		ACT200L_REG3  | ACT200L_B0,
		ACT200L_REG0  | ACT200L_TXEN | ACT200L_RXEN,
		ACT200L_REG8 |  (ACT200L_115200       & 0x0f),
		ACT200L_REG9 | ((ACT200L_115200 >> 4) & 0x0f),
		ACT200L_REG1 | ACT200L_LODB | ACT200L_WIDE
	};

	/* Set DLAB 1. */
	soutp(UART_LCR, UART_LCR_DLAB | UART_LCR_WLEN8);

	/* Set divisor to 12 => 9600 Baud */
	soutp(UART_DLM, 0);
	soutp(UART_DLL, 12);

	/* Set DLAB 0. */
	soutp(UART_LCR, UART_LCR_WLEN8);
	/* Set divisor to 12 => 9600 Baud */

	/* power supply */
	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2);
	for (i = 0; i < 50; i++)
		safe_udelay(1000);

		/* Reset the dongle : set RTS low for 25 ms */
	soutp(UART_MCR, UART_MCR_DTR|UART_MCR_OUT2);
	for (i = 0; i < 25; i++)
		udelay(1000);

	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2);
	udelay(100);

	/* Clear DTR and set RTS to enter command mode */
	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_OUT2);
	udelay(7);

	/* send out the control register settings for 115K 7N1 SIR operation */
	for (i = 0; i < sizeof(control); i++) {
		soutp(UART_TX, control[i]);
		/* one byte takes ~1042 usec to transmit at 9600,8N1 */
		udelay(1500);
	}

	/* back to normal operation */
	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2);
	udelay(50);

	udelay(1500);
	soutp(UART_LCR, sinp(UART_LCR) | UART_LCR_DLAB);

	/* Set DLAB 1. */
	soutp(UART_LCR, UART_LCR_DLAB | UART_LCR_WLEN7);

	/* Set divisor to 1 => 115200 Baud */
	soutp(UART_DLM, 0);
	soutp(UART_DLL, 1);

	/* Set DLAB 0. */
	soutp(UART_LCR, sinp(UART_LCR) & (~UART_LCR_DLAB));

	/* Set DLAB 0, 7 Bit */
	soutp(UART_LCR, UART_LCR_WLEN7);

	/* enable interrupts */
	soutp(UART_IER, sinp(UART_IER)|UART_IER_RDI);
}
#endif

#ifdef LIRC_SIR_ACTISYS_ACT220L
/*
 * Derived from linux IrDA driver (net/irda/actisys.c)
 * Drop me a mail for any kind of comment: maxx@spaceboyz.net
 */

void init_act220(void)
{
	int i;

	/* DLAB 1 */
	soutp(UART_LCR, UART_LCR_DLAB|UART_LCR_WLEN7);

	/* 9600 baud */
	soutp(UART_DLM, 0);
	soutp(UART_DLL, 12);

	/* DLAB 0 */
	soutp(UART_LCR, UART_LCR_WLEN7);

	/* reset the dongle, set DTR low for 10us */
	soutp(UART_MCR, UART_MCR_RTS|UART_MCR_OUT2);
	udelay(10);

	/* back to normal (still 9600) */
	soutp(UART_MCR, UART_MCR_DTR|UART_MCR_RTS|UART_MCR_OUT2);

	/*
	 * send RTS pulses until we reach 115200
	 * i hope this is really the same for act220l/act220l+
	 */
	for (i = 0; i < 3; i++) {
		udelay(10);
		/* set RTS low for 10 us */
		soutp(UART_MCR, UART_MCR_DTR|UART_MCR_OUT2);
		udelay(10);
		/* set RTS high for 10 us */
		soutp(UART_MCR, UART_MCR_RTS|UART_MCR_DTR|UART_MCR_OUT2);
	}

	/* back to normal operation */
	udelay(1500); /* better safe than sorry ;) */

	/* Set DLAB 1. */
	soutp(UART_LCR, UART_LCR_DLAB | UART_LCR_WLEN7);

	/* Set divisor to 1 => 115200 Baud */
	soutp(UART_DLM, 0);
	soutp(UART_DLL, 1);

	/* Set DLAB 0, 7 Bit */
	/* The dongle doesn't seem to have any problems with operation at 7N1 */
	soutp(UART_LCR, UART_LCR_WLEN7);

	/* enable interrupts */
	soutp(UART_IER, UART_IER_RDI);
}
#endif

static int init_sir_ir(void)
{
	int retval;

	retval = init_port();
	if (retval < 0)
		return retval;
	init_hardware();
	pr_info("Installed.\n");
	return 0;
}

static int sir_ir_probe(struct platform_device *dev)
{
	return 0;
}

static int sir_ir_remove(struct platform_device *dev)
{
	return 0;
}

static struct platform_driver sir_ir_driver = {
	.probe		= sir_ir_probe,
	.remove		= sir_ir_remove,
	.driver		= {
		.name	= "sir_ir",
	},
};

static int __init sir_ir_init(void)
{
	int retval;

	retval = platform_driver_register(&sir_ir_driver);
	if (retval) {
		pr_err("Platform driver register failed!\n");
		return -ENODEV;
	}

	sir_ir_dev = platform_device_alloc("sir_ir", 0);
	if (!sir_ir_dev) {
		pr_err("Platform device alloc failed!\n");
		retval = -ENOMEM;
		goto pdev_alloc_fail;
	}

	retval = platform_device_add(sir_ir_dev);
	if (retval) {
		pr_err("Platform device add failed!\n");
		retval = -ENODEV;
		goto pdev_add_fail;
	}

	retval = init_chrdev();
	if (retval < 0)
		goto fail;

	retval = init_sir_ir();
	if (retval)
		goto fail;

	return 0;

fail:
	platform_device_del(sir_ir_dev);
pdev_add_fail:
	platform_device_put(sir_ir_dev);
pdev_alloc_fail:
	platform_driver_unregister(&sir_ir_driver);
	return retval;
}

static void __exit sir_ir_exit(void)
{
	drop_hardware();
	drop_port();
	platform_device_unregister(sir_ir_dev);
	platform_driver_unregister(&sir_ir_driver);
	pr_info("Uninstalled.\n");
}

module_init(sir_ir_init);
module_exit(sir_ir_exit);

#ifdef LIRC_SIR_TEKRAM
MODULE_DESCRIPTION("Infrared receiver driver for Tekram Irmate 210");
MODULE_AUTHOR("Christoph Bartelmus");
#elif defined(LIRC_SIR_ACTISYS_ACT200L)
MODULE_DESCRIPTION("LIRC driver for Actisys Act200L");
MODULE_AUTHOR("Karl Bongers");
#elif defined(LIRC_SIR_ACTISYS_ACT220L)
MODULE_DESCRIPTION("LIRC driver for Actisys Act220L(+)");
MODULE_AUTHOR("Jan Roemisch");
#else
MODULE_DESCRIPTION("Infrared receiver driver for SIR type serial ports");
MODULE_AUTHOR("Milan Pikula");
#endif
MODULE_LICENSE("GPL");

module_param(io, int, 0444);
MODULE_PARM_DESC(io, "I/O address base (0x3f8 or 0x2f8)");

module_param(irq, int, 0444);
MODULE_PARM_DESC(irq, "Interrupt (4 or 3)");

module_param(threshold, int, 0444);
MODULE_PARM_DESC(threshold, "space detection threshold (3)");

module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debugging messages");
