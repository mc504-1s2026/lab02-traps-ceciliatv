#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/trap.h>
#include <kernel/serial.h>
#include <arch/timer.h>

/* maximum length of a single command line */
#define LINE_MAX	256

/*
 * shell_exec(): parse and run a single command line.
 *
 * Supported commands:
 *   uptime        -> print seconds since boot as "<n>s"
 *   echo [str]    -> print the rest of the line back
 *   alarm [time]  -> print "alarm" `time` seconds from now (via the timer IRQ)
 */
static void shell_exec(char *line)
{
	char out[64];
	char *cmd, *arg;

	/* skip leading whitespace */
	cmd = line;
	while (*cmd == ' ')
		cmd++;

	/* empty line: nothing to do */
	if (*cmd == '\0')
		return;

	/* split the command word from its arguments */
	arg = cmd;
	while (*arg != ' ' && *arg != '\0')
		arg++;
	if (*arg == ' ') {
		*arg++ = '\0';
		while (*arg == ' ')
			arg++;
	}
	/* `arg` now points at the (possibly empty) argument string */

	if (strcmp(cmd, "uptime") == 0) {
		u64 secs = timer_read() / TIMER_FREQ;
		snprintf(out, sizeof(out), "%lus\r\n", secs);
		serial_puts(out);
	} else if (strcmp(cmd, "echo") == 0) {
		serial_puts(arg);
		serial_puts("\r\n");
	} else if (strcmp(cmd, "alarm") == 0) {
		u64 secs = strtou64(arg, 10);
		timer_set_alarm(secs);
		/* nothing is printed now; the timer IRQ will make us print
		 * "alarm" once the deadline is reached */
	} else {
		serial_puts(cmd);
		serial_puts(": command not found\r\n");
	}
}

static void shell()
{
	char line[LINE_MAX];
	char rx[SERIAL_BUF_SIZE];
	size_t len = 0;

	serial_puts("> ");

	while (1) {
		/* drain whatever the serial IRQ has buffered for us */
		size_t n = serial_read(rx);
		for (size_t i = 0; i < n; i++) {
			char c = rx[i];

			if (c == '\r' || c == '\n') {
				/* execute the command and reprint the prompt */
				serial_puts("\r\n");
				line[len] = '\0';
				shell_exec(line);
				len = 0;
				serial_puts("> ");
			} else if (c == 0x7f || c == 0x08) {
				/* backspace / delete: erase last character */
				if (len > 0) {
					len--;
					serial_puts("\b \b");
				}
			} else if (len < LINE_MAX - 1) {
				/* echo the character back so the user sees it */
				line[len++] = c;
				serial_putc(c);
			}
		}

		/* print "alarm" for every alarm that fired while we weren't
		 * looking (the timer IRQ counts them for us) */
		u32 fired = timer_poll_alarms();
		while (fired-- > 0)
			serial_puts("alarm\r\n");
	}
}

extern int _hartid[];
void kmain()
{
	printk_set_level(LOG_INFO);
	info("entered S-mode\n");
	info("booting on hart %d\n", _hartid[0]);
	info("setting up virtual memory...\n");
	vm_init();

	info("enabling traps...\n");
	trap_setup();
	info("enabling timer...\n");
	timer_irq_enable();
	info("enabling serial...\n");
	serial_init();
	serial_irq_enable();

	shell();
}
