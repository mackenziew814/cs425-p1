#define _POSIX_C_SOURCE 200809L

#include "lab.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Layer 1: pure protocol helpers                                     */
/* ------------------------------------------------------------------ */

int smtp_parse_reply_code(const char *line, size_t len) {
    if (line == NULL || len < 3) {
        return -1;
    }
    for (size_t i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)line[i])) {
            return -1;
        }
    }
    return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

int smtp_reply_is_final(const char *line, size_t len) {
    if (smtp_parse_reply_code(line, len) < 0) {
        return -1;
    }
    if (len == 3) {
        return 1; /* just "250" with nothing after */
    }
    return (line[3] != '-') ? 1 : 0;
}

char *smtp_build_command(const char *cmd, const char *arg) {
    if (cmd == NULL) {
        return NULL;
    }
    size_t cmd_len = strlen(cmd);
    size_t arg_len = (arg != NULL) ? strlen(arg) : 0;
    /* cmd [' ' arg] "\r\n" '\0' */
    size_t total = cmd_len + (arg_len > 0 ? 1 + arg_len : 0) + 2 + 1;
    char *out = malloc(total);
    if (out == NULL)  // GCOVR_EXCL_START
    {
        return NULL; // malloc failed
    }  // GCOVR_EXCL_STOP
    if (arg_len > 0) {
        snprintf(out, total, "%s %s\r\n", cmd, arg);
    } else {
        snprintf(out, total, "%s\r\n", cmd);
    }
    return out;
}

char *smtp_dot_stuff(const char *body) {
    if (body == NULL) {
        body = "";
    }
    size_t len = strlen(body);

    /* Worst case: every character is a line-leading '.' that gets
     * doubled, plus we may need to add a CRLF at the very end, plus a
     * bare '\n' can grow by one byte becoming "\r\n". Be generous. */
    size_t cap = len * 2 + 8;
    char *out = malloc(cap);
    if (out == NULL)  // GCOVR_EXCL_START
    {
        return NULL; // malloc failed
    }  // GCOVR_EXCL_STOP

    size_t o = 0;
    size_t i = 0;
    int at_line_start = 1;

    while (i < len) {
        char c = body[i];

        if (at_line_start && c == '.') {
            out[o++] = '.';
            out[o++] = '.';
            i++;
            at_line_start = 0;
            continue;
        }
        if (c == '\r') {
            /* Swallow; we emit our own CRLF at the matching '\n' (or
             * right here if the '\r' has no following '\n'). */
            i++;
            if (i >= len || body[i] != '\n') {
                out[o++] = '\r';
                out[o++] = '\n';
                at_line_start = 1;
            }
            continue;
        }
        if (c == '\n') {
            out[o++] = '\r';
            out[o++] = '\n';
            i++;
            at_line_start = 1;
            continue;
        }
        out[o++] = c;
        i++;
        at_line_start = 0;
    }

    /* Ensure non-empty text ends with a CRLF so the terminating "."
     * line smtp_build_data() appends starts on its own line. A
     * genuinely empty body stays empty: smtp_build_data() already
     * supplies the CRLF after the header block's blank line, so
     * padding one in here would insert a spurious blank line. */
    if (o > 0 && !(o >= 2 && out[o - 2] == '\r' && out[o - 1] == '\n')) {
        out[o++] = '\r';
        out[o++] = '\n';
    }

    out[o] = '\0';
    return out;
}

int smtp_contains_crlf(const char *s) {
    if (s == NULL) {
        return 0;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\r' || *p == '\n') {
            return 1;
        }
    }
    return 0;
}

char *smtp_build_data(const char *from, const char *to, const char *subject, const char *body) {
    if (from == NULL || to == NULL) {
        return NULL;
    }
    if (subject == NULL) {
        subject = "";
    }

    char *stuffed = smtp_dot_stuff(body);
    if (stuffed == NULL)  // GCOVR_EXCL_START
    {
        return NULL; // smtp_dot_stuff's own malloc failed
    }  // GCOVR_EXCL_STOP

    size_t needed = strlen("From: ") + strlen(from) + 2 +
                     strlen("To: ") + strlen(to) + 2 +
                     strlen("Subject: ") + strlen(subject) + 2 +
                     2 /* blank line */ +
                     strlen(stuffed) +
                     3 /* ".\r\n" */ + 1 /* NUL */;

    char *out = malloc(needed);
    if (out == NULL)  // GCOVR_EXCL_START
    {
        free(stuffed);
        return NULL; // malloc failed
    }  // GCOVR_EXCL_STOP

    int n = snprintf(out, needed,
                      "From: %s\r\n"
                      "To: %s\r\n"
                      "Subject: %s\r\n"
                      "\r\n"
                      "%s"
                      ".\r\n",
                      from, to, subject, stuffed);
    free(stuffed);
    if (n < 0 || (size_t)n >= needed)  // GCOVR_EXCL_START
    {
        free(out);
        return NULL; // snprintf failed or unexpectedly truncated
    }  // GCOVR_EXCL_STOP
    return out;
}

/* ------------------------------------------------------------------ */
/* Layer 2: the session, over a pluggable transport                   */
/* ------------------------------------------------------------------ */

void smtp_transport_init(smtp_transport_t *t, smtp_read_fn read_fn, smtp_write_fn write_fn, void *ctx) {
    t->read_fn = read_fn;
    t->write_fn = write_fn;
    t->ctx = ctx;
    t->buf_len = 0;
}

ssize_t smtp_read_line(smtp_transport_t *t, char *out, size_t out_size) {
    if (t == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    for (;;) {
        char *nl = memchr(t->buf, '\n', t->buf_len);
        if (nl != NULL) {
            size_t idx = (size_t)(nl - t->buf); /* index of '\n' */
            size_t line_len = idx;
            if (line_len > 0 && t->buf[line_len - 1] == '\r') {
                line_len--;
            }

            if (line_len >= out_size) {
                return -1; /* line too long for caller's buffer */
            }

            memcpy(out, t->buf, line_len);
            out[line_len] = '\0';

            size_t consumed = idx + 1; /* including the '\n' */
            size_t remaining = t->buf_len - consumed;
            memmove(t->buf, t->buf + consumed, remaining);
            t->buf_len = remaining;

            return (ssize_t)line_len;
        }

        if (t->buf_len == sizeof(t->buf)) {
            return -1; /* buffer full, no newline: line too long */
        }

        ssize_t n = t->read_fn(t->ctx, t->buf + t->buf_len,
                                sizeof(t->buf) - t->buf_len);
        if (n <= 0) {
            return -1; /* EOF or error before a full line arrived */
        }
        t->buf_len += (size_t)n;
    }
}

int smtp_read_reply(smtp_transport_t *t, int *code, char *reply_out, size_t reply_size) {
    char line[SMTP_LINE_BUF_SIZE];
    int first_code = -1;
    size_t out_used = 0;

    if (reply_out != NULL && reply_size > 0) {
        reply_out[0] = '\0';
    }

    for (;;) {
        ssize_t n = smtp_read_line(t, line, sizeof(line));
        if (n < 0) {
            return -1;
        }
        size_t n_sz = (size_t)n;

        int c = smtp_parse_reply_code(line, n_sz);
        if (c < 0) {
            return -1; /* malformed reply line */
        }
        if (first_code == -1) {
            first_code = c;
        } else if (c != first_code) {
            return -1; /* continuation code doesn't match first line */
        }

        if (reply_out != NULL && reply_size > 0 && out_used < reply_size) {
            int written = snprintf(reply_out + out_used,
                                    reply_size - out_used, "%s%s",
                                    (out_used > 0) ? "\n" : "", line);
            if (written > 0) {
                size_t w = (size_t)written;
                size_t avail = reply_size - out_used;
                out_used += (w < avail) ? w : avail - 1;
            }
        }

        /* smtp_parse_reply_code() already validated this exact line, so
         * a 4th-character check is enough to tell final vs. continuation
         * here; there is no failure mode left to check for. */
        if (n_sz == 3 || line[3] != '-') {
            break;
        }
    }

    if (code != NULL) {
        *code = first_code;
    }
    return 0;
}

int smtp_write_all(smtp_transport_t *t, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = t->write_fn(t->ctx, buf + sent, len - sent);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int smtp_command(smtp_transport_t *t, const char *cmd, const char *arg, int expected_code, int *got_code, char *reply_out, size_t reply_size) {
    char *line = smtp_build_command(cmd, arg);
    if (line == NULL)  // GCOVR_EXCL_START
    {
        if (got_code != NULL) {
            *got_code = -1;
        }
        if (reply_out != NULL && reply_size > 0) {
            snprintf(reply_out, reply_size, "out of memory building command");
        }
        return -2; // smtp_build_command's own malloc failed
    }  // GCOVR_EXCL_STOP

    int rc = smtp_write_all(t, line, strlen(line));
    free(line);
    if (rc != 0) {
        if (got_code != NULL) {
            *got_code = -1;
        }
        if (reply_out != NULL && reply_size > 0) {
            snprintf(reply_out, reply_size,
                     "connection lost while sending %s", cmd);
        }
        return -2;
    }

    int code = -1;
    if (smtp_read_reply(t, &code, reply_out, reply_size) != 0) {
        if (got_code != NULL) {
            *got_code = -1;
        }
        if (reply_out != NULL && reply_size > 0 && reply_out[0] == '\0') {
            snprintf(reply_out, reply_size,
                     "connection lost or malformed reply while waiting for "
                     "response to %s",
                     cmd);
        }
        return -2;
    }

    if (got_code != NULL) {
        *got_code = code;
    }
    return (code == expected_code) ? 0 : -1;
}

static void set_fail(smtp_result_t *r, int io_error, const char *fmt, ...) {
    r->ok = 0;
    r->io_error = io_error;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->message, sizeof(r->message), fmt, ap);
    va_end(ap);
}

smtp_result_t smtp_send_mail(smtp_transport_t *t, const char *helo_host, const char *from, const char *to, const char *subject, const char *body) {
    smtp_result_t r;
    memset(&r, 0, sizeof(r));
    char reply[SMTP_REPLY_BUF_SIZE];
    int code = -1;

    /* Greeting */
    if (smtp_read_reply(t, &code, reply, sizeof(reply)) != 0) {
        set_fail(&r, 1, "connection closed before server sent a greeting");
        return r;
    }
    if (code != 220) {
        set_fail(&r, 0, "server did not greet us with 220: %s", reply);
        return r;
    }

    char mail_arg[512];
    char rcpt_arg[512];
    snprintf(mail_arg, sizeof(mail_arg), "FROM:<%s>", from);
    snprintf(rcpt_arg, sizeof(rcpt_arg), "TO:<%s>", to);

    struct {
        const char *cmd;
        const char *arg;
        int expect;
        const char *label;
    } steps[] = {
        {"HELO", helo_host, 250, "HELO"},
        {"MAIL", mail_arg, 250, "MAIL FROM"},
        {"RCPT", rcpt_arg, 250, "RCPT TO"},
        {"DATA", NULL, 354, "DATA"},
    };

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        int got = -1;
        int rc = smtp_command(t, steps[i].cmd, steps[i].arg, steps[i].expect,
                               &got, reply, sizeof(reply));
        if (rc == -2) {
            set_fail(&r, 1, "connection error during %s: %s", steps[i].label,
                      reply);
            return r;
        }
        if (rc == -1) {
            set_fail(&r, 0, "server rejected %s (expected %d, got %s)",
                      steps[i].label, steps[i].expect, reply);
            return r;
        }
    }

    /* DATA payload: headers + dot-stuffed body + terminating "." */
    char *payload = smtp_build_data(from, to, subject, body);
    if (payload == NULL) {
        set_fail(&r, 1, "out of memory building message");
        return r;
    }
    if (smtp_write_all(t, payload, strlen(payload)) != 0) {
        free(payload);
        set_fail(&r, 1, "connection lost while sending message data");
        return r;
    }
    free(payload);

    if (smtp_read_reply(t, &code, reply, sizeof(reply)) != 0) {
        set_fail(&r, 1, "connection lost waiting for reply to message data");
        return r;
    }
    if (code != 250) {
        set_fail(&r, 0,
                  "server did not accept the message (expected 250, got %s)",
                  reply);
        return r;
    }

    int got = -1;
    int rc = smtp_command(t, "QUIT", NULL, 221, &got, reply, sizeof(reply));
    if (rc == -2) {
        set_fail(&r, 1, "connection error during QUIT: %s", reply);
        return r;
    }
    if (rc == -1) {
        set_fail(&r, 0, "server did not reply 221 to QUIT (got %s)", reply);
        return r;
    }

    r.ok = 1;
    r.io_error = 0;
    r.message[0] = '\0';
    return r;
}

/* ------------------------------------------------------------------ */
/* Layer 3: the socket transport                                      */
/* ------------------------------------------------------------------ */

int smtp_socket_connect(const char *host, const char *port, int *out_fd, char *errbuf, size_t errbuf_size) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai_rc = getaddrinfo(host, port, &hints, &res);
    if (gai_rc != 0) {
        if (errbuf != NULL && errbuf_size > 0) {
            snprintf(errbuf, errbuf_size, "could not resolve %s:%s (%s)",
                      host, port, gai_strerror(gai_rc));
        }
        return -1;
    }

    int fd = -1;
    int last_errno = 0;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1)  // GCOVR_EXCL_START
        {
            last_errno = errno;
            continue; // this address family/type unavailable; try the next
        }  // GCOVR_EXCL_STOP
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; /* success */
        }
        last_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd == -1) {
        if (errbuf != NULL && errbuf_size > 0) {
            snprintf(errbuf, errbuf_size, "could not connect to %s:%s (%s)",
                      host, port, strerror(last_errno));
        }
        return -1;
    }

    *out_fd = fd;
    return 0;
}

ssize_t smtp_socket_read(void *ctx, char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n;
    do {
        n = recv(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    return n;
}

ssize_t smtp_socket_write(void *ctx, const char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n;
    do {
        n = send(fd, buf, len, 0);
    } while (n < 0 && errno == EINTR);
    return n;
}