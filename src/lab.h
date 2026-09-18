#ifndef LAB_H
#define LAB_H
 
#include <stddef.h>
#include <sys/types.h>
 

/* ------------------------------------------------------------------ */
/* Layer 1: pure protocol helpers                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Parse the 3-digit status code at the start of a single reply line.
 *
 * `line` need not be NUL-terminated at the reply boundary; `len` is the
 * number of valid bytes in `line` (CR/LF already stripped by the caller,
 * but this function does not require that).
 *
 * @param line the reply line's bytes (not necessarily NUL-terminated)
 * @param len number of valid bytes in line
 * @return the numeric code (100-599) on success, or -1 if the line does
 *         not begin with exactly three digits
 */
int smtp_parse_reply_code(const char *line, size_t len);
 
/**
 * @brief Decide whether a single reply line is the *last* line of a
 * (possibly multi-line) reply.
 *
 * A non-final line has a '-' immediately after the
 * 3-digit code; a final line has a space there (or nothing at all, i.e.
 * the line is exactly 3 characters).
 *
 * @param line the reply line's bytes
 * @param len number of valid bytes in line
 * @return 1 if this is the final line of the reply, 0 if more lines
 *         follow, and -1 if the line is malformed (too short / no code)
 */
int smtp_reply_is_final(const char *line, size_t len);
 
/**
 * @brief Build a CRLF-terminated SMTP command line.
 *
 * Produces "CMD arg\r\n" if arg is non-NULL and non-empty, or "CMD\r\n"
 * if arg is NULL.
 *
 * @param cmd the command verb, e.g. "HELO"
 * @param arg the command argument, or NULL for none
 * @return a malloc'd, NUL-terminated string the caller must free(), or
 *         NULL on allocation failure
 */
char *smtp_build_command(const char *cmd, const char *arg);
 
/**
 * @brief Dot-stuff a message body
 *
 * Splits on '\n' (accepting bare LF or CRLF input), doubles any line
 * that begins with '.', and joins everything back together with CRLF
 * line endings. Non-empty input always ends with a CRLF in the result,
 * even if the input did not; empty input maps to an empty string. This
 * does NOT append the terminating "." line - see smtp_build_data().
 *
 * @param body the raw body text, or NULL (treated as empty)
 * @return a malloc'd, NUL-terminated string the caller must free(), or
 *         NULL on allocation failure
 */
char *smtp_dot_stuff(const char *body);
 
/**
 * @brief Build the complete payload to write after a 354 reply to DATA.
 *
 * Produces From/To/Subject headers, a blank line, the dot-stuffed body,
 * and the terminating line containing a single '.'.
 *
 * @param from envelope/header sender address
 * @param to envelope/header recipient address
 * @param subject subject line; NULL/empty sends an empty Subject: header
 * @param body message body; NULL is treated as empty
 * @return a malloc'd, NUL-terminated string the caller must free(), or
 *         NULL on allocation failure or if from/to is NULL
 */
char *smtp_build_data(const char *from, const char *to, const char *subject, const char *body);
 
/**
 * @brief Check whether a string contains a bare CR or LF.
 *
 * Used to reject header/injection attempts in -f/-t/-s/-H values before
 * they ever reach the wire.
 *
 * @param s the string to check, or NULL
 * @return 1 if s contains '\r' or '\n' anywhere, 0 otherwise (NULL is
 *         treated as clean, 0)
 */
int smtp_contains_crlf(const char *s);
 
/* ------------------------------------------------------------------ */
/* Layer 2: the session, over a pluggable transport                   */
/* ------------------------------------------------------------------ */ 

/**
 * @brief Read callback used by the transport.
 *
 * Semantics match read()/recv(): returns the number of bytes read (>0),
 * 0 on EOF, or -1 on error.
 */
typedef ssize_t (*smtp_read_fn)(void *ctx, char *buf, size_t len);
 
/**
 * @brief Write callback used by the transport.
 *
 * Unlike write()/send(), this callback must not do a short write: it
 * either writes all `len` bytes and returns `len`, or returns -1 on
 * error. (The socket transport in this file loops internally to
 * provide that guarantee.)
 */
typedef ssize_t (*smtp_write_fn)(void *ctx, const char *buf, size_t len);
 
#define SMTP_LINE_BUF_SIZE 4096
#define SMTP_REPLY_BUF_SIZE 4096
 
typedef struct {
    smtp_read_fn read_fn;
    smtp_write_fn write_fn;
    void *ctx;
 
    //internal read-ahead buffer
    char buf[SMTP_LINE_BUF_SIZE];
    size_t buf_len;
} smtp_transport_t;
 
/**
 * @brief Initialize a transport with the given callbacks.
 *
 * @param t transport to initialize
 * @param read_fn read callback
 * @param write_fn write callback
 * @param ctx opaque context pointer passed to both callbacks
 */
void smtp_transport_init(smtp_transport_t *t, smtp_read_fn read_fn, smtp_write_fn write_fn, void *ctx);
 
/**
 * @brief Read a single CRLF- (or LF-) terminated line, terminator stripped.
 *
 * @param t transport to read from
 * @param out buffer to receive the NUL-terminated line
 * @param out_size capacity of out, in bytes
 * @return the line length on success, -1 on I/O error, EOF before a
 *         full line arrived, or a line too long for out/the internal
 *         buffer
 */
ssize_t smtp_read_line(smtp_transport_t *t, char *out, size_t out_size);
 
/**
 * @brief Read a complete (possibly multi-line) SMTP reply.
 *
 * The numeric code is stored in *code. If reply_out is non-NULL, the
 * full reply text (all lines, joined with '\n') is copied into it for
 * error reporting; reply_size is its capacity.
 *
 * @param t transport to read from
 * @param code out-parameter for the parsed status code
 * @param reply_out optional buffer to receive the full reply text
 * @param reply_size capacity of reply_out, in bytes
 * @return 0 on success, -1 on I/O error or a malformed reply
 */
int smtp_read_reply(smtp_transport_t *t, int *code, char *reply_out, size_t reply_size);
 
/**
 * @brief Write `len` bytes, looping on the write callback as needed.
 *
 * @param t transport to write to
 * @param buf bytes to write
 * @param len number of bytes to write
 * @return 0 on success, -1 on I/O error
 */
int smtp_write_all(smtp_transport_t *t, const char *buf, size_t len);
 
/**
 * @brief Send "CMD arg\r\n" (arg may be NULL) and read the reply.
 *
 * Either way, the reply text (or an I/O error description) is copied
 * into reply_out/reply_size for the caller to report, and, if non-NULL,
 * *got_code is set to the parsed code (or -1 on I/O error).
 *
 * @param t transport to use
 * @param cmd command verb, e.g. "MAIL"
 * @param arg command argument, or NULL
 * @param expected_code the status code that counts as success
 * @param got_code optional out-parameter for the code actually received
 * @param reply_out buffer to receive the reply text or error description
 * @param reply_size capacity of reply_out, in bytes
 * @return 0 if the reply's code == expected_code, -1 if the reply
 *         parsed fine but had a different code, or -2 on I/O error
 */
int smtp_command(smtp_transport_t *t, const char *cmd, const char *arg, int expected_code, int *got_code, char *reply_out, size_t reply_size);
 
/** Outcome of a full send-mail session; see smtp_send_mail(). */
typedef struct {
    int ok;         /* 1 if the message was queued successfully */
    int io_error;   /* 1 if the failure was a connection/I/O problem rather than an unexpected reply code */
    char message[256]; /* human-readable description on failure */
} smtp_result_t;
 
/**
 * @brief Run an entire SMTP session over `t`.
 *
 * Reads the greeting, then sends HELO, MAIL FROM, RCPT TO, DATA + the
 * message, and QUIT, checking the reply code at every step and
 * stopping at the first one that doesn't match what the protocol
 * requires.
 *
 * @param t transport to run the session over
 * @param helo_host host name to send with HELO
 * @param from envelope sender address
 * @param to envelope recipient address
 * @param subject subject line (may be NULL/empty)
 * @param body message body (may be NULL/empty)
 * @return an smtp_result_t describing success or the first failure
 */
smtp_result_t smtp_send_mail(smtp_transport_t *t, const char *helo_host, const char *from, const char *to, const char *subject, const char *body);
 
/* ------------------------------------------------------------------ */
/* Layer 3: the socket transport                                      */
/* ------------------------------------------------------------------ */
 
/**
 * @brief Resolve host/port and connect a TCP socket to it.
 *
 * Uses getaddrinfo() and tries each returned address with connect()
 * until one succeeds.
 *
 * @param host host name or address to connect to
 * @param port port number or service name, as a string
 * @param out_fd out-parameter; set to the connected socket on success
 * @param errbuf buffer to receive an error description on failure
 * @param errbuf_size capacity of errbuf, in bytes
 * @return 0 on success, -1 on failure
 */
int smtp_socket_connect(const char *host, const char *port, int *out_fd, char *errbuf, size_t errbuf_size);
 
/**
 * @brief smtp_read_fn implementation over a socket file descriptor.
 * @param ctx must point at an int holding the connected file descriptor
 */
ssize_t smtp_socket_read(void *ctx, char *buf, size_t len);
 
/**
 * @brief smtp_write_fn implementation over a socket file descriptor.
 * @param ctx must point at an int holding the connected file descriptor
 */
ssize_t smtp_socket_write(void *ctx, const char *buf, size_t len);
 
 
#endif /* LAB_H */