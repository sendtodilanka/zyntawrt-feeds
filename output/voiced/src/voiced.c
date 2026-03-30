// SPDX-License-Identifier: GPL-2.0-or-later
/* voiced — VoLTE call control daemon for ZyntaWRT AW1000
 * Bridges Quectel AT port (/dev/ttyUSB2) <-> Si3226x SLIC (/dev/proslic0)
 *
 * State machine (ASCII):
 *   IDLE --RING--> RINGING --offhook--> OFFHOOK_IN --hangup--> HANGUP --> IDLE
 *   IDLE --offhook--> DIALTONE --DTMF--> DIALING --timeout/#--> OFFHOOK_OUT
 *   OFFHOOK_OUT --hangup--> HANGUP --> IDLE
 *
 * Copyright (C) 2026 ZyntaWRT Project
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <syslog.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- SLIC ioctl definitions (must match slic-si3226x.c) ---- */
#define PROSLIC_IOC_MAGIC       'P'
#define PROSLIC_RING_ON         _IO(PROSLIC_IOC_MAGIC, 1)
#define PROSLIC_RING_OFF        _IO(PROSLIC_IOC_MAGIC, 2)
#define PROSLIC_GET_HOOK_STATUS _IOR(PROSLIC_IOC_MAGIC, 3, int)
#define PROSLIC_GET_DTMF        _IOR(PROSLIC_IOC_MAGIC, 4, int)
#define PROSLIC_PLAY_TONE       _IOW(PROSLIC_IOC_MAGIC, 5, int)
#define PROSLIC_SET_LINEFEED    _IOW(PROSLIC_IOC_MAGIC, 6, int)

#define TONE_NONE       0
#define TONE_DIAL       1
#define TONE_RINGBACK   2
#define TONE_BUSY       3
#define TONE_CONGESTION 4

/* ---- Configuration ---- */
#define AT_PORT_DEFAULT   "/dev/ttyUSB2"
#define SLIC_DEV_DEFAULT  "/dev/proslic0"
#define AT_BAUD           B115200
#define AT_BUF_SIZE       512
#define DTMF_BUF_SIZE     32
#define DTMF_INTER_DIGIT_MS  4000   /* 4 seconds inter-digit timeout */
#define DTMF_MIN_DIGITS      3
#define HOOK_POLL_MS         100    /* poll hook status every 100ms */
#define HANGUP_DELAY_MS      500

/* ---- Call states ---- */
enum call_state {
	ST_IDLE = 0,
	ST_RINGING,
	ST_OFFHOOK_IN,
	ST_DIALTONE,
	ST_DIALING,
	ST_OFFHOOK_OUT,
	ST_HANGUP,
};

static const char *state_names[] = {
	"IDLE", "RINGING", "OFFHOOK_IN", "DIALTONE",
	"DIALING", "OFFHOOK_OUT", "HANGUP",
};

/* ---- Global state ---- */
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_reopen_at = 0;

static int              g_at_fd = -1;
static int              g_slic_fd = -1;
static enum call_state  g_state = ST_IDLE;
static char             g_dtmf_buf[DTMF_BUF_SIZE];
static int              g_dtmf_len = 0;
static struct timespec  g_dtmf_last;
static int              g_verbose = 0;
static int              g_foreground = 0;
static const char      *g_at_path = AT_PORT_DEFAULT;
static const char      *g_slic_path = SLIC_DEV_DEFAULT;

#define LOG_TAG "voiced"
#define loginfo(fmt, ...) syslog(LOG_INFO, fmt, ##__VA_ARGS__)
#define logwarn(fmt, ...) syslog(LOG_WARNING, fmt, ##__VA_ARGS__)
#define logerr(fmt, ...)  syslog(LOG_ERR, fmt, ##__VA_ARGS__)
#define logdbg(fmt, ...) do { if (g_verbose) syslog(LOG_DEBUG, fmt, ##__VA_ARGS__); } while (0)

static void sig_handler(int sig) { if (sig == SIGHUP) g_reopen_at = 1; else g_running = 0; }

static void setup_signals(void)
{
	struct sigaction sa = { .sa_handler = sig_handler };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

static int at_open(const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { logerr("open(%s): %s", path, strerror(errno)); return -1; }
	struct termios tty = {0};
	tcgetattr(fd, &tty);
	cfsetispeed(&tty, AT_BAUD);
	cfsetospeed(&tty, AT_BAUD);
	tty.c_cflag = CS8 | CLOCAL | CREAD;
	tty.c_iflag = IGNPAR;
	tty.c_oflag = 0;
	tty.c_lflag = 0;
	tty.c_cc[VMIN] = 0;
	tty.c_cc[VTIME] = 1;
	tcflush(fd, TCIFLUSH);
	tcsetattr(fd, TCSANOW, &tty);
	return fd;
}

static int at_send(const char *cmd)
{
	char buf[256];
	int len = snprintf(buf, sizeof(buf), "%s\r", cmd);
	if (len < 0 || len >= (int)sizeof(buf)) return -1;
	logdbg("AT TX: %s", cmd);
	return (write(g_at_fd, buf, (size_t)len) < 0) ? -1 : 0;
}

static void at_init_modem(void)
{
	static const char *cmds[] = {
		"ATE0", "AT+CMEE=2", "AT+CRC=1", "AT+CLIP=1",
		"AT+QCFG=\"ims\",1", "AT+QCFG=\"ims/callwaiting\",1",
		"AT+CVHU=0", NULL
	};
	usleep(200000);
	for (const char **p = cmds; *p; p++) { at_send(*p); usleep(100000); }
	loginfo("AT port initialized");
}

static int slic_open(const char *p) { int fd = open(p, O_RDWR); if (fd < 0) logerr("open(%s): %s", p, strerror(errno)); return fd; }

static int slic_get_hook(void)
{
	int s = 0;
	return (ioctl(g_slic_fd, PROSLIC_GET_HOOK_STATUS, &s) < 0) ? -1 : s;
}

static void slic_ring(int on)
{
	ioctl(g_slic_fd, on ? PROSLIC_RING_ON : PROSLIC_RING_OFF);
	logdbg("SLIC: ring %s", on ? "ON" : "OFF");
}

static void slic_tone(int tone)
{
	ioctl(g_slic_fd, PROSLIC_PLAY_TONE, &tone);
	logdbg("SLIC: tone %d", tone);
}

static int slic_get_dtmf(void)
{
	int d = -1;
	return (ioctl(g_slic_fd, PROSLIC_GET_DTMF, &d) < 0) ? -1 : d;
}

static void time_now(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }
static long time_diff_ms(const struct timespec *a, const struct timespec *b)
{ return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L; }

static char dtmf_to_char(int d)
{ static const char m[] = "D1234567890*#ABC"; return (d >= 0 && d <= 15) ? m[d] : '?'; }

static void set_state(enum call_state s)
{ if (g_state != s) { loginfo("state: %s -> %s", state_names[g_state], state_names[s]); g_state = s; } }

static void do_dial(void)
{
	char cmd[64];
	if (g_dtmf_len < DTMF_MIN_DIGITS) { logwarn("too few digits (%d)", g_dtmf_len); slic_tone(TONE_CONGESTION); set_state(ST_HANGUP); return; }
	g_dtmf_buf[g_dtmf_len] = '\0';
	loginfo("dialing: %s", g_dtmf_buf);
	slic_tone(TONE_NONE);
	snprintf(cmd, sizeof(cmd), "ATD%s;", g_dtmf_buf);
	at_send(cmd);
	slic_tone(TONE_RINGBACK);
	set_state(ST_OFFHOOK_OUT);
}

static void handle_incoming_ring(void) { if (g_state == ST_IDLE) { slic_ring(1); set_state(ST_RINGING); } }

static void handle_no_carrier(void)
{
	switch (g_state) {
	case ST_RINGING:    slic_ring(0); set_state(ST_IDLE); break;
	case ST_OFFHOOK_IN: case ST_OFFHOOK_OUT: slic_tone(TONE_BUSY); set_state(ST_HANGUP); break;
	case ST_DIALING:    slic_tone(TONE_CONGESTION); set_state(ST_HANGUP); break;
	default:            set_state(ST_IDLE); break;
	}
}

static void handle_call_answered(void) { if (g_state == ST_OFFHOOK_OUT) { slic_tone(TONE_NONE); loginfo("outgoing call connected"); } }

static void handle_offhook(void)
{
	switch (g_state) {
	case ST_IDLE:    slic_tone(TONE_DIAL); g_dtmf_len = 0; set_state(ST_DIALTONE); break;
	case ST_RINGING: slic_ring(0); at_send("ATA"); slic_tone(TONE_NONE); set_state(ST_OFFHOOK_IN); break;
	default: break;
	}
}

static void handle_onhook(void)
{
	switch (g_state) {
	case ST_OFFHOOK_IN: case ST_OFFHOOK_OUT: at_send("ATH"); slic_tone(TONE_NONE); set_state(ST_HANGUP); break;
	case ST_DIALTONE: case ST_DIALING: at_send("ATH"); slic_tone(TONE_NONE); set_state(ST_IDLE); break;
	case ST_HANGUP: slic_tone(TONE_NONE); set_state(ST_IDLE); break;
	default: break;
	}
}

static void handle_dtmf_digit(int digit)
{
	char ch = dtmf_to_char(digit);
	switch (g_state) {
	case ST_DIALTONE:
		slic_tone(TONE_NONE); g_dtmf_len = 0; g_dtmf_buf[g_dtmf_len++] = ch;
		time_now(&g_dtmf_last); set_state(ST_DIALING); break;
	case ST_DIALING:
		if (ch == '#') { do_dial(); }
		else if (g_dtmf_len < DTMF_BUF_SIZE - 1) { g_dtmf_buf[g_dtmf_len++] = ch; time_now(&g_dtmf_last); }
		break;
	default: break;
	}
}

static char g_at_buf[AT_BUF_SIZE];
static int  g_at_buf_len = 0;

static void process_at_line(const char *line)
{
	logdbg("AT RX: %s", line);
	if (!strcmp(line, "RING") || !strncmp(line, "+CRING:", 7))        handle_incoming_ring();
	else if (!strcmp(line, "NO CARRIER"))                             handle_no_carrier();
	else if (!strncmp(line, "+CIEV: \"call\",0", 15) || !strncmp(line, "+CIEV: call,0", 13)) handle_no_carrier();
	else if (!strncmp(line, "+CIEV: \"call\",1", 15) || !strncmp(line, "+CIEV: call,1", 13)) handle_call_answered();
	else if (!strncmp(line, "+CLIP:", 6))                            loginfo("Caller ID: %s", line + 6);
}

static void at_read_and_parse(void)
{
	char tmp[256];
	int n = (int)read(g_at_fd, tmp, sizeof(tmp) - 1);
	if (n <= 0) return;
	for (int i = 0; i < n; i++) {
		if (tmp[i] == '\r') continue;
		if (tmp[i] == '\n') { if (g_at_buf_len > 0) { g_at_buf[g_at_buf_len] = '\0'; process_at_line(g_at_buf); g_at_buf_len = 0; } continue; }
		if (g_at_buf_len < AT_BUF_SIZE - 1) g_at_buf[g_at_buf_len++] = tmp[i];
	}
}

static void daemonize(void)
{
	pid_t pid = fork();
	if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
	if (pid > 0) exit(EXIT_SUCCESS);
	setsid();
	pid = fork();
	if (pid < 0) exit(EXIT_FAILURE);
	if (pid > 0) exit(EXIT_SUCCESS);
	umask(0027);
	(void)chdir("/");
	close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
	open("/dev/null", O_RDONLY); open("/dev/null", O_WRONLY); open("/dev/null", O_WRONLY);
}

static void cleanup(void)
{
	if (g_state == ST_RINGING) slic_ring(0);
	slic_tone(TONE_NONE);
	if (g_at_fd >= 0) { at_send("ATH"); close(g_at_fd); g_at_fd = -1; }
	if (g_slic_fd >= 0) { close(g_slic_fd); g_slic_fd = -1; }
}

static void reopen_at_port(void)
{
	loginfo("reopening AT port %s", g_at_path);
	if (g_at_fd >= 0) close(g_at_fd);
	g_at_fd = at_open(g_at_path);
	if (g_at_fd >= 0) at_init_modem();
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-d] [-a at_port] [-s slic_dev] [-v]\n", prog);
}

int main(int argc, char *argv[])
{
	int opt;
	int prev_hook = 0;
	struct timespec now;
	struct timespec hangup_enter;
	int hangup_timer_active = 0;

	while ((opt = getopt(argc, argv, "da:s:vh")) != -1) {
		switch (opt) {
		case 'd':
			g_foreground = 1;
			break;
		case 'a':
			g_at_path = optarg;
			break;
		case 's':
			g_slic_path = optarg;
			break;
		case 'v':
			g_verbose = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	openlog(LOG_TAG, LOG_PID | (g_foreground ? LOG_PERROR : 0), LOG_DAEMON);
	setlogmask(LOG_UPTO(g_verbose ? LOG_DEBUG : LOG_INFO));

	loginfo("voiced starting (AT=%s SLIC=%s)", g_at_path, g_slic_path);

	if (!g_foreground)
		daemonize();

	setup_signals();

	/* Open devices */
	g_at_fd = at_open(g_at_path);
	if (g_at_fd < 0) {
		logerr("cannot open AT port, exiting");
		return EXIT_FAILURE;
	}

	g_slic_fd = slic_open(g_slic_path);
	if (g_slic_fd < 0) {
		logerr("cannot open SLIC device, exiting");
		close(g_at_fd);
		return EXIT_FAILURE;
	}

	at_init_modem();

	/* Get initial hook state */
	prev_hook = slic_get_hook();
	if (prev_hook < 0)
		prev_hook = 0;

	loginfo("voiced running, initial hook=%s",
		prev_hook ? "offhook" : "onhook");

	/* ---- Main event loop ---- */
	while (g_running) {
		struct pollfd fds[2];
		int timeout_ms = HOOK_POLL_MS;
		int nfds, ret;

		/* Handle SIGHUP */
		if (g_reopen_at) {
			g_reopen_at = 0;
			reopen_at_port();
		}

		/* Set up poll descriptors */
		nfds = 0;
		if (g_at_fd >= 0) {
			fds[nfds].fd = g_at_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}
		if (g_slic_fd >= 0) {
			fds[nfds].fd = g_slic_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		/* Adjust timeout for DTMF inter-digit timer */
		if (g_state == ST_DIALING) {
			time_now(&now);
			long elapsed = time_diff_ms(&g_dtmf_last, &now);
			long remain = DTMF_INTER_DIGIT_MS - elapsed;

			if (remain <= 0) {
				do_dial();
				continue;
			}
			if (remain < timeout_ms)
				timeout_ms = (int)remain;
		}

		/* Hangup state auto-returns to idle */
		if (g_state == ST_HANGUP) {
			if (!hangup_timer_active) {
				time_now(&hangup_enter);
				hangup_timer_active = 1;
			} else {
				time_now(&now);
				if (time_diff_ms(&hangup_enter, &now) >= HANGUP_DELAY_MS) {
					slic_tone(TONE_NONE);
					set_state(ST_IDLE);
					hangup_timer_active = 0;
				}
			}
			if (timeout_ms > 100)
				timeout_ms = 100;
		} else {
			hangup_timer_active = 0;
		}

		ret = poll(fds, (nfds_t)nfds, timeout_ms);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			logerr("poll: %s", strerror(errno));
			break;
		}

		/* Process AT port data */
		for (int i = 0; i < nfds; i++) {
			if (fds[i].fd == g_at_fd && (fds[i].revents & POLLIN))
				at_read_and_parse();
		}

		/* Poll hook status */
		int hook = slic_get_hook();
		if (hook >= 0 && hook != prev_hook) {
			logdbg("hook: %s -> %s",
			       prev_hook ? "offhook" : "onhook",
			       hook ? "offhook" : "onhook");
			if (hook)
				handle_offhook();
			else
				handle_onhook();
			prev_hook = hook;
		}

		/* Poll DTMF (when in dialtone/dialing state) */
		if (g_state == ST_DIALTONE || g_state == ST_DIALING) {
			int digit = slic_get_dtmf();

			if (digit >= 0)
				handle_dtmf_digit(digit);
		}
	}

	loginfo("voiced shutting down");
	cleanup();
	closelog();

	return EXIT_SUCCESS;
}
