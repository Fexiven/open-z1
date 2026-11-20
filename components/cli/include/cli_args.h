#pragma once
// Pure command-line splitting for the USB console.

#include <stddef.h>

// Splits line in place into at most max_args arguments. Whitespace separates
// arguments; "double quotes" group, and inside them \" and \\ are escapes.
// Returns the argument count, or -1 on an unterminated quote or too many
// arguments.
int cli_split(char *line, char **argv, int max_args);
