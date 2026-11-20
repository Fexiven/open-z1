#include "cli_args.h"

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int cli_split(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *r = line, *w = line;
    for (;;) {
        while (is_space(*r)) {
            r++;
        }
        if (!*r) {
            return argc;
        }
        if (argc == max_args) {
            return -1;
        }
        argv[argc++] = w;
        int quoted = 0;
        while (*r && (quoted || !is_space(*r))) {
            if (*r == '"') {
                quoted = !quoted;
                r++;
            } else if (quoted && *r == '\\' && (r[1] == '"' || r[1] == '\\')) {
                *w++ = r[1];
                r += 2;
            } else {
                *w++ = *r++;
            }
        }
        if (quoted) {
            return -1;
        }
        // Writing the terminator may overwrite the separator we just stopped
        // on, so step past it first.
        if (*r) {
            r++;
        }
        *w++ = '\0';
    }
}
