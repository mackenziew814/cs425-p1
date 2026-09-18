
#include "lab.h"
 
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
 
#ifdef TEST
#define main main_exclude
#endif
 
static void print_usage(FILE *out) {
    fprintf(out,
        "Usage: myapp -f <from> -t <to> [-s subject] [-b body] [-p port]\n"
        "          [-H helo-host] <server>\n"
        "\n"
        "  -f <from>       envelope sender, for example you@example.com\n"
        "  -t <to>         envelope recipient\n"
        "  -s <subject>    subject line (default: empty)\n"
        "  -b <body>       message body (default: read from stdin)\n"
        "  -p <port>       port or service name (default: 25)\n"
        "  -H <helo-host>  host name sent with HELO (default: localhost)\n"
        "  <server>        host name or address of the mail server\n");
}
 
/* Read all of stdin into a malloc'd, NUL-terminated buffer. */
static char *read_all_stdin(void) {
    //Will increase cap if needed
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
 
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
    }
    buf[len] = '\0';
    return buf;
}
 
int main(int argc, char **argv) {
    if (argc == 1) {
        print_usage(stdout);
        return 0;
    }
 
    const char *from = NULL;
    const char *to = NULL;
    const char *subject = "";
    const char *body_arg = NULL;
    const char *port = "25";
    const char *helo_host = "localhost";
 
    int opt;
    while ((opt = getopt(argc, argv, "f:t:s:b:p:H:h")) != -1) {
        switch (opt) {
            case 'f': from = optarg; break;
            case 't': to = optarg; break;
            case 's': subject = optarg; break;
            case 'b': body_arg = optarg; break;
            case 'p': port = optarg; break;
            case 'H': helo_host = optarg; break;
            case 'h':
                print_usage(stdout);
                return 0;
            default:
                print_usage(stderr);
                return 1;
        }
    }
 
    if (optind >= argc) {
        fprintf(stderr, "myapp: missing <server>\n");
        print_usage(stderr);
        return 1;
    }
    const char *server = argv[optind];
 
    if (from == NULL || to == NULL) {
        fprintf(stderr, "myapp: -f and -t are required\n");
        print_usage(stderr);
        return 1;
    }
 
    if (smtp_contains_crlf(from) || smtp_contains_crlf(to) ||
        smtp_contains_crlf(subject) || smtp_contains_crlf(helo_host)) {
        fprintf(stderr,
                "myapp: -f, -t, -s and -H may not contain a CR or LF "
                "(refusing to risk SMTP command/header injection)\n");
        return 1;
    }
 
    char *owned_body = NULL;
    const char *body;
    if (body_arg != NULL) {
        body = body_arg;
    } else {
        owned_body = read_all_stdin();
        if (owned_body == NULL) {
            fprintf(stderr, "myapp: out of memory reading body from stdin\n");
            return 2;
        }
        body = owned_body;
    }
 
    int fd = -1;
    char errbuf[256];
    if (smtp_socket_connect(server, port, &fd, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "myapp: %s\n", errbuf);
        free(owned_body);
        return 2;
    }
 
    smtp_transport_t t;
    smtp_transport_init(&t, smtp_socket_read, smtp_socket_write, &fd);
 
    smtp_result_t result =
        smtp_send_mail(&t, helo_host, from, to, subject, body);
 
    close(fd);
    free(owned_body);
 
    if (!result.ok) {
        fprintf(stderr, "myapp: %s\n", result.message);
        return 2;
    }
 
    return 0;
}