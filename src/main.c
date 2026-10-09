#include "lab.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TEST
#define main main_exclude
#endif

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: myapp send -s <session> [-w window] [-T timeout-ms] "
            "[-l loss]\n"
            "              [-c corrupt] [-d dup] [-p port] <relay> <file>\n"
            "       myapp recv -s <session> [-p port] <relay> <file>\n");
}

static bool unsigned_value(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL || *text == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed > UINT_MAX) {
        return false;
    }
    *value = (unsigned int) parsed;
    return true;
}

static bool probability_value(const char *text, double *value)
{
    char *end = NULL;
    double parsed;

    if (text == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' ||
        !isfinite(parsed) || parsed < 0.0 || parsed > 0.5) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool valid_session(const char *session)
{
    size_t length;

    if (session == NULL) {
        return false;
    }
    length = strlen(session);
    if (length == 0U || length > LAB_MAX_SESSION_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = session[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '-')) {
            return false;
        }
    }
    return true;
}

static bool parse_options(int argc, char **argv, LabTransferOptions *options)
{
    const char *format;
    int option;

    if (argc < 2 || (strcmp(argv[1], "send") != 0 &&
                     strcmp(argv[1], "recv") != 0)) {
        return false;
    }
    options->mode = strcmp(argv[1], "send") == 0 ? LAB_SEND_MODE :
                                                   LAB_RECEIVE_MODE;
    options->session = NULL;
    options->window = LAB_DEFAULT_WINDOW;
    options->timeout_ms = LAB_DEFAULT_TIMEOUT_MS;
    options->port = LAB_DEFAULT_PORT;
    options->loss = 0.0;
    options->corrupt = 0.0;
    options->duplicate = 0.0;
    format = options->mode == LAB_SEND_MODE ? "s:w:T:l:c:d:p:" : "s:p:";
    optind = 2;
    opterr = 0;
    while ((option = getopt(argc, argv, format)) != -1) {
        switch (option) {
        case 's':
            options->session = optarg;
            break;
        case 'w':
            if (!unsigned_value(optarg, &options->window) ||
                options->window < 1U || options->window > LAB_MAX_WINDOW) {
                return false;
            }
            break;
        case 'T':
            if (!unsigned_value(optarg, &options->timeout_ms) ||
                options->timeout_ms == 0U) {
                return false;
            }
            break;
        case 'l':
            if (!probability_value(optarg, &options->loss)) return false;
            break;
        case 'c':
            if (!probability_value(optarg, &options->corrupt)) return false;
            break;
        case 'd':
            if (!probability_value(optarg, &options->duplicate)) return false;
            break;
        case 'p':
            {
                unsigned int port;
                if (!unsigned_value(optarg, &port) || port == 0U ||
                    port > UINT16_MAX) {
                    return false;
                }
                options->port = (uint16_t) port;
            }
            if (options->port == 0U) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    if (!valid_session(options->session) || argc - optind != 2) {
        return false;
    }
    options->relay = argv[optind];
    options->file = argv[optind + 1];
    return options->relay[0] != '\0' && options->file[0] != '\0';
}

int main(int argc, char **argv)
{
    LabTransferOptions options;

    if (argc == 1) {
        usage(stdout);
        return 0;
    }
    if (!parse_options(argc, argv, &options)) {
        usage(stderr);
        return 1;
    }
    return lab_run_transfer(&options);
}
