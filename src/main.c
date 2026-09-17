#include "lab.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef TEST
#define main main_exclude
#endif

//Prints usage to specified stream (stderr, stdout)
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

//Used to read the body from stdin, no cap size
static char *read_all_stdin(void) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += n;
        //Increase allocated space to allow messages of any size
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


int main(int argc, char **argv)
{
    if(argc == 1){
        print_usage(stdout);
        return 0;
    }

    const char *from = NULL;
    const char *to = NULL;
    const char *subject = "";
    const char *port = "25";
    const char *heloHost = "localhost";

    int opt = 0;
    while((opt = getopt(argc, argv, "f:t:s:b:p:H:h")) != -1){
        switch(opt){
            case'f': 
                from = optarg;
                break;
        }
    }
    return 0;
}