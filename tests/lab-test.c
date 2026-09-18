#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "harness/unity.h"
#include "../src/lab.h"

void setUp(void) {}
void tearDown(void) {}

void test_parse_reply_code(void) {
    TEST_ASSERT_EQUAL_INT(220, smtp_parse_reply_code("220 hello", 9));
    TEST_ASSERT_EQUAL_INT(250, smtp_parse_reply_code("250-PIPELINING", 14));
    TEST_ASSERT_EQUAL_INT(354, smtp_parse_reply_code("354", 3));
    TEST_ASSERT_EQUAL_INT(-1, smtp_parse_reply_code("ab", 2));
    TEST_ASSERT_EQUAL_INT(-1, smtp_parse_reply_code("2a0 x", 5));
    TEST_ASSERT_EQUAL_INT(-1, smtp_parse_reply_code(NULL, 0));
    TEST_ASSERT_EQUAL_INT(-1, smtp_parse_reply_code("", 0));
}

void test_reply_is_final(void) {
    TEST_ASSERT_EQUAL_INT(0, smtp_reply_is_final("250-smtp.example.com", 21));
    TEST_ASSERT_EQUAL_INT(0, smtp_reply_is_final("250-PIPELINING", 14));
    TEST_ASSERT_EQUAL_INT(1, smtp_reply_is_final("250 SIZE 10240000", 18));
    TEST_ASSERT_EQUAL_INT(1, smtp_reply_is_final("221", 3));
    TEST_ASSERT_EQUAL_INT(1, smtp_reply_is_final("220 ready", 9));
    TEST_ASSERT_EQUAL_INT(-1, smtp_reply_is_final("xy", 2));
}

void test_build_command(void) {
    char *c1 = smtp_build_command("HELO", "onyx.boisestate.edu");
    TEST_ASSERT_EQUAL_STRING("HELO onyx.boisestate.edu\r\n", c1);
    free(c1);

    char *c2 = smtp_build_command("DATA", NULL);
    TEST_ASSERT_EQUAL_STRING("DATA\r\n", c2);
    free(c2);

    char *c3 = smtp_build_command("MAIL", "FROM:<me@boisestate.edu>");
    TEST_ASSERT_EQUAL_STRING("MAIL FROM:<me@boisestate.edu>\r\n", c3);
    free(c3);
}

void test_build_command_null_cmd(void) {
    TEST_ASSERT_NULL(smtp_build_command(NULL, "arg"));
    TEST_ASSERT_NULL(smtp_build_command(NULL, NULL));
}

void test_dot_stuff_basic(void) {
    char *s = smtp_dot_stuff("hello\nworld");
    TEST_ASSERT_EQUAL_STRING("hello\r\nworld\r\n", s);
    free(s);
}

void test_dot_stuff_leading_dot(void) {
    /* A body line that legitimately starts with a period must be
     * doubled, per RFC 5321 4.5.2. */
    char *s = smtp_dot_stuff("line one\n.line two\nline three");
    TEST_ASSERT_EQUAL_STRING("line one\r\n..line two\r\nline three\r\n", s);
    free(s);
}

void test_dot_stuff_solo_dot_line(void) {
    char *s = smtp_dot_stuff("above\n.\nbelow");
    TEST_ASSERT_EQUAL_STRING("above\r\n..\r\nbelow\r\n", s);
    free(s);
}

void test_dot_stuff_empty_and_null(void) {
    char *s1 = smtp_dot_stuff("");
    TEST_ASSERT_EQUAL_STRING("", s1);
    free(s1);

    char *s2 = smtp_dot_stuff(NULL);
    TEST_ASSERT_EQUAL_STRING("", s2);
    free(s2);
}

void test_dot_stuff_crlf_input(void) {
    /* Already-CRLF input should not be double-converted. */
    char *s = smtp_dot_stuff("a\r\nb\r\n");
    TEST_ASSERT_EQUAL_STRING("a\r\nb\r\n", s);
    free(s);
}

void test_dot_stuff_bare_cr(void) {
    /* A '\r' not followed by '\n' (mid-string or trailing) still needs
     * to become a proper CRLF line break on its own. */
    char *s1 = smtp_dot_stuff("a\rb");
    TEST_ASSERT_EQUAL_STRING("a\r\nb\r\n", s1);
    free(s1);

    char *s2 = smtp_dot_stuff("a\r");
    TEST_ASSERT_EQUAL_STRING("a\r\n", s2);
    free(s2);
}

void test_contains_crlf(void) {
    TEST_ASSERT_EQUAL_INT(0, smtp_contains_crlf("clean@example.com"));
    TEST_ASSERT_EQUAL_INT(
        1, smtp_contains_crlf("evil@example.com\r\nRCPT TO:<x>"));
    TEST_ASSERT_EQUAL_INT(1, smtp_contains_crlf("evil\nsubject"));
    TEST_ASSERT_EQUAL_INT(0, smtp_contains_crlf(NULL));
}

void test_build_data(void) {
    char *m = smtp_build_data("me@boisestate.edu", "you@example.com",
                               "hello", "This is the body.");
    TEST_ASSERT_EQUAL_STRING(
        "From: me@boisestate.edu\r\n"
        "To: you@example.com\r\n"
        "Subject: hello\r\n"
        "\r\n"
        "This is the body.\r\n"
        ".\r\n",
        m);
    free(m);
}

void test_build_data_null_from_or_to(void) {
    TEST_ASSERT_NULL(smtp_build_data(NULL, "you@example.com", "s", "b"));
    TEST_ASSERT_NULL(smtp_build_data("me@example.com", NULL, "s", "b"));
}

void test_build_data_null_subject(void) {
    char *m = smtp_build_data("a@b.com", "c@d.com", NULL, "body");
    TEST_ASSERT_EQUAL_STRING(
        "From: a@b.com\r\n"
        "To: c@d.com\r\n"
        "Subject: \r\n"
        "\r\n"
        "body\r\n"
        ".\r\n",
        m);
    free(m);
}

/* ------------------------------------------------------------------ */
/* Fake in-memory "server" used to drive the session layer with no    */
/* network at all.                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *script; /* bytes the "server" will hand back */
    size_t script_len;
    size_t script_pos;
    char sent[8192]; /* everything the client wrote, concatenated */
    size_t sent_len;
    int write_fail_at; /* 0 = never fail; N = fail on the Nth write call */
    int write_call_count;
} fake_server_t;

static ssize_t fake_read(void *ctx, char *buf, size_t len) {
    fake_server_t *fs = (fake_server_t *)ctx;
    size_t remaining = fs->script_len - fs->script_pos;
    if (remaining == 0) {
        return 0; /* EOF */
    }
    /* Dole out data a few bytes at a time to force the reader to cope
     * with a reply split across multiple reads. */
    size_t chunk = remaining < 3 ? remaining : 3;
    if (chunk > len) {
        chunk = len;
    }
    memcpy(buf, fs->script + fs->script_pos, chunk);
    fs->script_pos += chunk;
    return (ssize_t)chunk;
}

static ssize_t fake_write(void *ctx, const char *buf, size_t len) {
    fake_server_t *fs = (fake_server_t *)ctx;
    fs->write_call_count++;
    if (fs->write_fail_at != 0 && fs->write_call_count >= fs->write_fail_at) {
        return -1; /* simulate the peer having hung up on us */
    }
    if (fs->sent_len + len >= sizeof(fs->sent)) {
        return -1;
    }
    memcpy(fs->sent + fs->sent_len, buf, len);
    fs->sent_len += len;
    fs->sent[fs->sent_len] = '\0';
    return (ssize_t)len;
}

/* A read callback that never produces a newline, for the "reply the
 * buffer cannot hold" case. */
static ssize_t fake_read_endless_no_newline(void *ctx, char *buf, size_t len) {
    (void)ctx;
    memset(buf, 'x', len);
    return (ssize_t)len;
}

static void fake_server_init(fake_server_t *fs, const char *script) {
    memset(fs, 0, sizeof(*fs));
    fs->script = script;
    fs->script_len = strlen(script);
}

#define HAPPY_PATH_SCRIPT                  \
    "220 smtp.example.com ESMTP ready\r\n" \
    "250 smtp.example.com\r\n"             \
    "250 2.1.0 Ok\r\n"                     \
    "250 2.1.5 Ok\r\n"                     \
    "354 End data with .\r\n"              \
    "250 2.0.0 Ok: queued\r\n"             \
    "221 Bye\r\n"

/* ------------------------------------------------------------------ */
/* Layer 2 tests: smtp_read_line                                      */
/* ------------------------------------------------------------------ */

void test_read_line_split_across_reads(void) {
    /* fake_read only ever hands back 3 bytes at a time, so this also
     * exercises smtp_read_line's internal buffering directly. */
    fake_server_t fs;
    fake_server_init(&fs, "250-first\r\n250 second\r\n");

    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    char line[64];
    ssize_t n = smtp_read_line(&t, line, sizeof(line));
    TEST_ASSERT_EQUAL_INT(9, (int)n);
    TEST_ASSERT_EQUAL_STRING("250-first", line);

    n = smtp_read_line(&t, line, sizeof(line));
    TEST_ASSERT_EQUAL_INT(10, (int)n);
    TEST_ASSERT_EQUAL_STRING("250 second", line);
}

void test_read_line_rejects_bad_arguments(void) {
    fake_server_t fs;
    fake_server_init(&fs, "220 ready\r\n");
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    char line[64];
    TEST_ASSERT_EQUAL_INT(-1, (int)smtp_read_line(NULL, line, sizeof(line)));
    TEST_ASSERT_EQUAL_INT(-1, (int)smtp_read_line(&t, NULL, sizeof(line)));
    TEST_ASSERT_EQUAL_INT(-1, (int)smtp_read_line(&t, line, 0));
}

void test_read_line_too_long_for_caller_buffer(void) {
    fake_server_t fs;
    fake_server_init(&fs, "250 this line is longer than the tiny buffer\r\n");
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    char tiny[8];
    TEST_ASSERT_EQUAL_INT(-1, (int)smtp_read_line(&t, tiny, sizeof(tiny)));
}

void test_read_line_buffer_cannot_hold_reply(void) {
    /* A "line" that never produces a '\n' within SMTP_LINE_BUF_SIZE
     * bytes must fail rather than loop forever or overflow. */
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read_endless_no_newline, fake_write, NULL);

    char line[SMTP_LINE_BUF_SIZE];
    TEST_ASSERT_EQUAL_INT(-1, (int)smtp_read_line(&t, line, sizeof(line)));
}

/* ------------------------------------------------------------------ */
/* Layer 2 tests: smtp_read_reply                                     */
/* ------------------------------------------------------------------ */

void test_read_reply_malformed_line(void) {
    fake_server_t fs;
    fake_server_init(&fs, "not a status code\r\n");
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    int code = -1;
    TEST_ASSERT_EQUAL_INT(-1, smtp_read_reply(&t, &code, NULL, 0));
}

void test_read_reply_continuation_code_mismatch(void) {
    fake_server_t fs;
    fake_server_init(&fs, "250-first line\r\n251 different code\r\n");
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    int code = -1;
    TEST_ASSERT_EQUAL_INT(-1, smtp_read_reply(&t, &code, NULL, 0));
}

//smtp_write_all tests

void test_write_all_reports_failure(void) {
    fake_server_t fs;
    fake_server_init(&fs, "");
    fs.write_fail_at = 1; /* fail on the very first write */
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    TEST_ASSERT_EQUAL_INT(-1, smtp_write_all(&t, "hello", 5));
}

//smtp_command tests
void test_command_write_failure(void) {
    fake_server_t fs;
    fake_server_init(&fs, "250 ok\r\n");
    fs.write_fail_at = 1;
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    int got = -1;
    char reply[128];
    int rc = smtp_command(&t, "HELO", "host", 250, &got, reply,
                           sizeof(reply));
    TEST_ASSERT_EQUAL_INT(-2, rc);
    TEST_ASSERT_EQUAL_INT(-1, got);
}

void test_command_read_reply_failure(void) {
    /* Write succeeds, but the "server" hangs up before replying. */
    fake_server_t fs;
    fake_server_init(&fs, "");
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    int got = -1;
    char reply[128];
    int rc = smtp_command(&t, "HELO", "host", 250, &got, reply,
                           sizeof(reply));
    TEST_ASSERT_EQUAL_INT(-2, rc);
    TEST_ASSERT_EQUAL_INT(-1, got);
    TEST_ASSERT_NOT_NULL(strstr(reply, "HELO"));
}

//smtp_send_mail full session tests
void test_session_happy_path(void) {
    fake_server_t fs;
    fake_server_init(&fs, HAPPY_PATH_SCRIPT);

    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "onyx.boisestate.edu", "me@boisestate.edu",
                        "you@example.com", "hello",
                        "This is the message body.");
    TEST_ASSERT_EQUAL_INT(1, r.ok);
    TEST_ASSERT_NOT_NULL(strstr(fs.sent, "HELO onyx.boisestate.edu\r\n"));
    TEST_ASSERT_NOT_NULL(
        strstr(fs.sent, "MAIL FROM:<me@boisestate.edu>\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(fs.sent, "RCPT TO:<you@example.com>\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(fs.sent, "DATA\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(fs.sent, "Subject: hello\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(fs.sent, "\r\n.\r\n"));
    TEST_ASSERT_NOT_NULL(strstr(fs.sent, "QUIT\r\n"));
}

void test_session_multiline_greeting(void) {
    /* Continuation lines before the final line of a reply. */
    const char *script =
        "220-smtp.example.com ESMTP\r\n"
        "220-say hello\r\n"
        "220 ready\r\n"
        "250-smtp.example.com\r\n"
        "250-PIPELINING\r\n"
        "250 SIZE 10240000\r\n"
        "250 2.1.0 Ok\r\n"
        "250 2.1.5 Ok\r\n"
        "354 End data with .\r\n"
        "250 2.0.0 Ok: queued\r\n"
        "221 Bye\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);

    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r = smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com",
                                      "s", "body");
    TEST_ASSERT_EQUAL_INT(1, r.ok);
}

/* -- wrong status code at every step of the sequence -- */

void test_session_bad_greeting(void) {
    fake_server_t fs;
    fake_server_init(&fs, "421 too busy\r\n");

    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_EQUAL_INT(0, r.io_error);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "421"));
    /* Must not have sent HELO after a bad greeting. */
    TEST_ASSERT_EQUAL_INT(0, (int)fs.sent_len);
}

void test_session_helo_rejected(void) {
    const char *script =
        "220 ready\r\n"
        "500 command not recognized\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "HELO"));
    TEST_ASSERT_NULL(strstr(fs.sent, "MAIL"));
}

void test_session_mail_rejected(void) {
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "451 temporary failure\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "MAIL FROM"));
    TEST_ASSERT_NULL(strstr(fs.sent, "RCPT"));
}

void test_session_rcpt_rejected(void) {
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "250 sender ok\r\n"
        "550 no such user\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);

    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "RCPT"));
    /* Must not have sent DATA after RCPT was rejected. */
    TEST_ASSERT_NULL(strstr(fs.sent, "DATA\r\n"));
}

void test_session_data_command_rejected(void) {
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "250 sender ok\r\n"
        "250 recipient ok\r\n"
        "550 no DATA allowed\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "DATA"));
    TEST_ASSERT_NULL(strstr(fs.sent, "Subject:"));
}

void test_session_data_rejected(void) {
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "250 sender ok\r\n"
        "250 recipient ok\r\n"
        "354 go ahead\r\n"
        "554 transaction failed\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);

    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_TRUE(strstr(r.message, "554") != NULL ||
                      strstr(r.message, "message") != NULL);
}

void test_session_quit_wrong_code(void) {
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "250 sender ok\r\n"
        "250 recipient ok\r\n"
        "354 go ahead\r\n"
        "250 queued\r\n"
        "500 huh?\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_EQUAL_INT(0, r.io_error);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "QUIT"));
}

/* -- from/to validation surfaced through the session -- */

void test_session_null_from_fails_to_build_message(void) {
    /* HELO/MAIL/RCPT/DATA all succeed (their replies don't depend on
     * from/to content), but building the DATA payload itself requires
     * a non-NULL from/to and must fail cleanly. */
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "250 sender ok\r\n"
        "250 recipient ok\r\n"
        "354 go ahead\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", NULL, "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_EQUAL_INT(1, r.io_error);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "message"));
}

/* -- a server that hangs up mid-session -- */

void test_session_hangup_before_greeting_completes(void) {
    fake_server_t fs;
    fake_server_init(&fs, "220 rea"); /* no CRLF, ever */

    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_EQUAL_INT(1, r.io_error);
}

void test_session_hangup_mid_command(void) {
    /* Greeting and HELO succeed, then the connection dies while we're
     * waiting on the reply to MAIL FROM. */
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_EQUAL_INT(1, r.io_error);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "MAIL FROM"));
}

void test_session_hangup_writing_payload(void) {
    /* Every command up through DATA is accepted; the write of the
     * message payload itself is what fails (e.g. the peer reset the
     * connection right as we started sending message data). Writes,
     * in order: HELO, MAIL, RCPT, DATA, <payload>, QUIT - fail on the
     * 5th. */
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "250 sender ok\r\n"
        "250 recipient ok\r\n"
        "354 go ahead\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    fs.write_fail_at = 5;
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_EQUAL_INT(1, r.io_error);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "message data"));
}

void test_session_hangup_waiting_for_queued_reply(void) {
    /* The payload is fully written, but the server disappears before
     * confirming with a 250. */
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "250 sender ok\r\n"
        "250 recipient ok\r\n"
        "354 go ahead\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_EQUAL_INT(1, r.io_error);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "message data"));
}

void test_session_hangup_waiting_for_quit_reply(void) {
    /* Everything through "250 queued" succeeds, but the server
     * disappears before replying to QUIT. */
    const char *script =
        "220 ready\r\n"
        "250 hello\r\n"
        "250 sender ok\r\n"
        "250 recipient ok\r\n"
        "354 go ahead\r\n"
        "250 queued\r\n";
    fake_server_t fs;
    fake_server_init(&fs, script);
    smtp_transport_t t;
    smtp_transport_init(&t, fake_read, fake_write, &fs);

    smtp_result_t r =
        smtp_send_mail(&t, "localhost", "a@b.com", "c@d.com", "s", "body");
    TEST_ASSERT_EQUAL_INT(0, r.ok);
    TEST_ASSERT_EQUAL_INT(1, r.io_error);
    TEST_ASSERT_NOT_NULL(strstr(r.message, "QUIT"));
}

/* ------------------------------------------------------------------ */
/* Layer 3 tests: the real socket transport                           */
/* ------------------------------------------------------------------ */

void test_socket_read_write_via_socketpair(void) {
    int fds[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    ssize_t n = smtp_socket_write(&fds[0], "hello", 5);
    TEST_ASSERT_EQUAL_INT(5, (int)n);

    char buf[16] = {0};
    n = smtp_socket_read(&fds[1], buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(5, (int)n);
    TEST_ASSERT_EQUAL_STRING("hello", buf);

    close(fds[0]);
    close(fds[1]);
}

void test_socket_connect_success(void) {
    /* Stand up a real local listener and connect to it. */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(listen_fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* let the OS pick a free port */

    TEST_ASSERT_EQUAL_INT(
        0, bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, listen(listen_fd, 1));

    socklen_t addr_len = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(
        0, getsockname(listen_fd, (struct sockaddr *)&addr, &addr_len));
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", (int)ntohs(addr.sin_port));

    int fd = -1;
    char errbuf[128];
    int rc = smtp_socket_connect("127.0.0.1", port_str, &fd, errbuf,
                                  sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(fd >= 0);

    close(fd);
    close(listen_fd);
}

void test_socket_connect_refused(void) {
    /* Bind an ephemeral port, then close it immediately so nothing is
     * listening there, and try to connect - should fail fast with a
     * connection-refused style error, entirely on loopback. */
    int probe_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(probe_fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    TEST_ASSERT_EQUAL_INT(
        0, bind(probe_fd, (struct sockaddr *)&addr, sizeof(addr)));
    socklen_t addr_len = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(
        0, getsockname(probe_fd, (struct sockaddr *)&addr, &addr_len));
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", (int)ntohs(addr.sin_port));
    close(probe_fd); /* nobody is listening on this port now */

    int fd = -1;
    char errbuf[128];
    int rc = smtp_socket_connect("127.0.0.1", port_str, &fd, errbuf,
                                  sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_NOT_NULL(strstr(errbuf, "could not connect"));
}

void test_socket_connect_resolution_failure(void) {
    /* A numeric host needs no DNS, so this fails purely on service
     * name lookup (checked against /etc/services locally) - no
     * network access required, and deterministic. */
    int fd = -1;
    char errbuf[128];
    int rc = smtp_socket_connect(
        "127.0.0.1", "not-a-real-service-name-xyz", &fd, errbuf,
        sizeof(errbuf));
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_NOT_NULL(strstr(errbuf, "could not resolve"));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_parse_reply_code);
    RUN_TEST(test_reply_is_final);
    RUN_TEST(test_build_command);
    RUN_TEST(test_build_command_null_cmd);
    RUN_TEST(test_dot_stuff_basic);
    RUN_TEST(test_dot_stuff_leading_dot);
    RUN_TEST(test_dot_stuff_solo_dot_line);
    RUN_TEST(test_dot_stuff_empty_and_null);
    RUN_TEST(test_dot_stuff_crlf_input);
    RUN_TEST(test_dot_stuff_bare_cr);
    RUN_TEST(test_contains_crlf);
    RUN_TEST(test_build_data);
    RUN_TEST(test_build_data_null_from_or_to);
    RUN_TEST(test_build_data_null_subject);

    RUN_TEST(test_read_line_split_across_reads);
    RUN_TEST(test_read_line_rejects_bad_arguments);
    RUN_TEST(test_read_line_too_long_for_caller_buffer);
    RUN_TEST(test_read_line_buffer_cannot_hold_reply);

    RUN_TEST(test_read_reply_malformed_line);
    RUN_TEST(test_read_reply_continuation_code_mismatch);

    RUN_TEST(test_write_all_reports_failure);

    RUN_TEST(test_command_write_failure);
    RUN_TEST(test_command_read_reply_failure);

    RUN_TEST(test_session_happy_path);
    RUN_TEST(test_session_multiline_greeting);

    RUN_TEST(test_session_bad_greeting);
    RUN_TEST(test_session_helo_rejected);
    RUN_TEST(test_session_mail_rejected);
    RUN_TEST(test_session_rcpt_rejected);
    RUN_TEST(test_session_data_command_rejected);
    RUN_TEST(test_session_data_rejected);
    RUN_TEST(test_session_quit_wrong_code);

    RUN_TEST(test_session_null_from_fails_to_build_message);

    RUN_TEST(test_session_hangup_before_greeting_completes);
    RUN_TEST(test_session_hangup_mid_command);
    RUN_TEST(test_session_hangup_writing_payload);
    RUN_TEST(test_session_hangup_waiting_for_queued_reply);
    RUN_TEST(test_session_hangup_waiting_for_quit_reply);

    RUN_TEST(test_socket_read_write_via_socketpair);
    RUN_TEST(test_socket_connect_success);
    RUN_TEST(test_socket_connect_refused);
    RUN_TEST(test_socket_connect_resolution_failure);

    return UNITY_END();
}