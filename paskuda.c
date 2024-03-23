/* Copyright © 2022-2023 Jakub Wilk <jwilk@jwilk.net>
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#define PROGRAM_NAME "paskuda"

static void show_usage(FILE *fp)
{
    fprintf(fp, "Usage: %s [PROMPT]\n", PROGRAM_NAME);
    if (fp != stdout)
        return;
    fprintf(fp,
        "\n"
        "Options:\n"
        "  -h, --help  show this help message and exit\n"
    );
}

static void xerror(const char *context)
{
    int orig_errno = errno;
    fprintf(stderr, "%s: ", PROGRAM_NAME);
    errno = orig_errno;
    perror(context);
    exit(EXIT_FAILURE);
}

typedef void *(*memset_fn) (void *s, int c, size_t n);
static const volatile memset_fn xmemset = memset;

static int tty_fd = -1;
static struct termios orig_tio;

static void restore_tty()
{
    if (tty_fd < 0)
        return;
    int rc = tcsetattr(tty_fd, TCSAFLUSH, &orig_tio);
    if (rc < 0)
        xerror("tcsetattr()");
    tty_fd = -1;
}

static void init_tty(int fd)
{
    struct termios tio;
    int rc = tcgetattr(fd, &tio);
    if (rc < 0)
        xerror("tcgetattr()");
    orig_tio = tio;
    tio.c_lflag &= ~(ECHO | ICANON);
    rc = tcsetattr(fd, TCSAFLUSH, &tio);
    if (rc < 0)
        xerror("tcsetattr()");
    tty_fd = fd;
    atexit(restore_tty);
}

static void xprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = vdprintf(fd, fmt, ap);
    if (rc < 0)
        xerror("dprintf()");
    va_end(ap);
}

static void clear_n(int fd, size_t n)
{
    while (n-- > 0)
        xprintf(fd, "\b \b");
}

static void clear_s(int fd, const char *s)
{
    clear_n(fd, strlen(s));
}

static const char *prompt = "Password:";
static const char *msg_press_tab = "(press TAB for no echo) ";
static const char *msg_no_echo = "(no echo) ";

static enum {
    STATE_INIT,
    STATE_ECHO,
    STATE_NO_ECHO,
} state = STATE_INIT;

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "h-:")) != -1)
        switch (opt) {
        case 'h':
            show_usage(stdout);
            exit(EXIT_SUCCESS);
        case '-':
            if (strcmp(optarg, "help") == 0) {
                show_usage(stdout);
                exit(EXIT_SUCCESS);
            }
            /* fall through */
        default:
            show_usage(stderr);
            exit(EXIT_FAILURE);
        }
    argc -= optind;
    argv += optind;
    if (argc > 1) {
        show_usage(stderr);
        exit(EXIT_FAILURE);
    }
    if (argc)
        prompt = argv[0];
    init_tty(STDIN_FILENO);
    const int fd = STDERR_FILENO;
    xprintf(fd, "%s ", prompt);
    xprintf(fd, "%s", msg_press_tab);
    errno = EOVERFLOW;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 0)
        xerror("sysconf(_SC_PAGESIZE)");
    const size_t buf_size = page_size;
    char *passwd = NULL;
    int rc = posix_memalign((void **)&passwd, page_size, buf_size);
    if (rc != 0) {
        errno = rc;
        xerror("posix_memalign()");
    }
    rc = mlock(passwd, buf_size);
    if (rc < 0)
        xerror("mlock()");
#ifdef __linux__
    rc = prctl(PR_SET_DUMPABLE, 0);
    if (rc < 0)
        xerror("prctl(PR_SET_DUMPABLE, 0)");
#endif
    size_t len = 0;
    while (1) {
        assert(len < buf_size);
        int rc = read(STDIN_FILENO, passwd + len, 1);
        register char c = passwd[len];
        if (rc < 0)
            xerror("read()");
        if (rc == 0 || c == '\n')
            break;
        if (state == STATE_INIT) {
            clear_s(fd, msg_press_tab);
            msg_press_tab = NULL;
            if (c == '\b' || c == 0x7F /* DEL */) {
                xprintf(fd, "%s", msg_no_echo);
                state = STATE_NO_ECHO;
                continue;
            }
            else
                state = STATE_ECHO;
        }
        switch (c)
        {
        case 0x08: // ^H
        case 0x7F: // DEL
            if (len) {
                if (state == STATE_ECHO)
                    clear_n(fd, 1);
                len--;
            } else
                xprintf(fd, "\a");
            break;
        case 0x15: // ^U
            clear_n(fd, len);
            len = 0;
            break;
        case '\t':
            if (state == STATE_ECHO) {
                clear_n(fd, len);
                xprintf(fd, "%s", msg_no_echo);
            }
            state = STATE_NO_ECHO;
            break;
        default:
            if (len < buf_size - 1) {
                len++;
                if (state == STATE_ECHO)
                    xprintf(fd, "*");
            } else
                xprintf(fd, "\a");
        }
    }
    passwd[len] = '\0';
    if (state == STATE_INIT)
        clear_s(fd, msg_press_tab);
    if (state == STATE_ECHO)
        clear_n(fd, len);
    xprintf(fd, "\n");
    restore_tty();
    xprintf(STDOUT_FILENO, "%s", passwd);
    xmemset(passwd, '\0', buf_size);
    free(passwd);
}

/* vim:set ts=4 sts=4 sw=4 et:*/
