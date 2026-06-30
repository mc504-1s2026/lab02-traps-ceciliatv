#include <kernel/serial.h>
#include <kernel/panic.h>
#include <arch/io.h>
#include <arch/csr.h>
#include <arch/plic.h>
#include <arch/spinlock.h>

/* the kernel currently always runs on hart 0 (no SMP support yet) */
#define SERIAL_HART	0

/*
 * Internal state for the serial port driver.
 *
 * Received bytes are pushed into `buf` from serial_irq() (interrupt context)
 * and drained from serial_read() (the main application loop). The spinlock
 * protects both fields against the resulting race condition.
 */
struct serialdev {
	char buf[SERIAL_BUF_SIZE];
	size_t len;
	struct spinlock lock;
} dev;

static inline u8 uart_read(u64 reg)
{
	return ioread8((void *)((u64)SERIAL_BASE + reg));
}

static inline void uart_write(u64 reg, u8 val)
{
	iowrite8(val, (void *)((u64)SERIAL_BASE + reg));
}

void serial_init()
{
	dev.len = 0;
	spin_init(&dev.lock);

	/* 8 data bits, no parity, 1 stop bit (8N1); leave DLAB cleared so
	 * that register 0 maps to RBR/THR (and not the divisor latch) */
	uart_write(SERIAL_LCR, 0x03);

	/* enable the FIFOs and flush both the RX and TX FIFOs */
	uart_write(SERIAL_FCR, (u8)(SERIAL_FCR_FIFO_ENABLE |
				    SERIAL_FCR_RX_FIFO_CLEAR |
				    SERIAL_FCR_TX_FIFO_CLEAR));

	/* assert DTR, RTS and OUT2; OUT2 gates the interrupt output on the
	 * 16550, so it must be set for interrupts to be delivered */
	uart_write(SERIAL_MCR, 0x0b);

	/* enable the "received data available" interrupt (and only that one):
	 * we transmit synchronously by polling, so we don't want TX interrupts */
	uart_write(SERIAL_IER, (u8)SERIAL_IER_ERBFI);
}

void serial_irq_enable()
{
	/* unmask supervisor external interrupts (sie[SEIE]) */
	csr_set(CSR_SIE, CSR_SIE_SEIE);

	/* configure the PLIC to route the serial IRQ to this hart:
	 *  i)   give the IRQ a non-zero priority
	 *  ii)  lower this hart's threshold so the IRQ isn't masked
	 *  iii) enable the IRQ for this hart */
	plic_irq_set_priority(IRQ_SERIAL, 1);
	plic_hart_set_threshold(SERIAL_HART, 0);
	plic_hart_enable_irq(SERIAL_HART, IRQ_SERIAL);
}

void serial_irq_disable()
{
	csr_clear(CSR_SIE, CSR_SIE_SEIE);
	uart_write(SERIAL_IER, 0);
}

void serial_irq()
{
	/* already in interrupt context (interrupts disabled), so a plain lock
	 * is enough to guard against the main loop's serial_read() */
	spin_lock(&dev.lock);

	/* drain the RX FIFO while there is data available */
	while (uart_read(SERIAL_LSR) & SERIAL_LSR_DTR) {
		char c = (char)uart_read(SERIAL_RBR);
		if (dev.len < SERIAL_BUF_SIZE)
			dev.buf[dev.len++] = c;
		/* otherwise the buffer is full and we drop the byte */
	}

	spin_unlock(&dev.lock);
}

size_t serial_read(char *buf)
{
	size_t len;

	/* this runs in normal (interruptible) context, so we must disable
	 * interrupts while holding the lock to avoid deadlocking against
	 * serial_irq() */
	u64 flags = spin_lock_irqsave(&dev.lock);
	len = dev.len;
	for (size_t i = 0; i < len; i++)
		buf[i] = dev.buf[i];
	dev.len = 0;
	spin_unlock_irqrestore(&dev.lock, flags);

	return len;
}

void serial_putc(char c)
{
	/* wait until the transmitter holding register is empty, then write */
	while (!(uart_read(SERIAL_LSR) & SERIAL_LSR_THRE)) {}
	uart_write(SERIAL_THR, (u8)c);
}

void serial_puts(char *s)
{
	while (*s != '\0')
		serial_putc(*s++);
}
