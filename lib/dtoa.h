#ifndef DTOA_H
#define DTOA_H

#include <stdint.h>

int dtoa_format(double value, char fmt_char, int precision, char *buf, size_t bufsize);

#endif