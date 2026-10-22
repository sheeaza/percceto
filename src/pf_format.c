/* Event-argument formatting: expands a format string's conversions with an
 * event's argument values. Shared by the producer, which renders the name when
 * it encodes the record, and the protobuf drain. See pf_format.h. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pf_format.h"
#include "pftrace_internal.h"

/* One parsed conversion, "%[flags][width][.prec][len]conv". Accepts only
 * what the Python converter accepts too, so both writers agree on every
 * name: flags - + space 0, width and precision of up to three digits, a
 * length of l or ll, and the conversions in parse_spec(). */
struct pf_spec {
    const char *start; /* the '%' */
    size_t len; /* bytes from the '%' through the conversion char */
    size_t body; /* bytes before the length modifier */
    char conv;
    bool minus, plus, space, zero;
    int width; /* -1 if none */
    int prec; /* -1 if none */
    int lenmod; /* 0 none, 1 "l", 2 "ll" */
};

/* Parses the spec whose '%' is at @start. Returns false for anything it
 * does not accept, including '#'. C and Python render %#o and %+u
 * differently, so neither accepts them. */
static bool parse_spec(const char *start, struct pf_spec *s)
{
    const char *p = start + 1;
    memset(s, 0, sizeof *s);
    s->start = start;
    s->width = -1;
    s->prec = -1;

    for (;; p++) {
        if (*p == '-')
            s->minus = true;
        else if (*p == '+')
            s->plus = true;
        else if (*p == ' ')
            s->space = true;
        else if (*p == '0')
            s->zero = true;
        else
            break;
    }

    int digits = 0;
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 3)
            return false;
        n = n * 10 + (*p++ - '0');
    }
    if (digits)
        s->width = n;

    if (*p == '.') {
        p++;
        digits = 0;
        n = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3)
                return false;
            n = n * 10 + (*p++ - '0');
        }
        if (digits == 0)
            return false;
        s->prec = n;
    }

    s->body = (size_t)(p - start);
    if (*p == 'l') {
        s->lenmod = 1;
        p++;
        if (*p == 'l') {
            s->lenmod = 2;
            p++;
        }
    }
    s->conv = *p;
    if (!s->conv)
        return false;
    s->len = (size_t)(p + 1 - start);

    switch (s->conv) {
    case 'd':
    case 'i':
        return true;
    case 'u':
    case 'x':
    case 'X':
    case 'o':
        return !s->plus && !s->space;
    case 'c':
    case 'p':
        return !s->plus && !s->space && !s->zero && s->prec < 0 && s->lenmod == 0;
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
        return s->lenmod == 0;
    default:
        return false;
    }
}

/* Renders one argument for @s into @dst, which is @room bytes. Returns the
 * number of characters the result needs, or -1 if the argument's type does
 * not suit the conversion. A result of @room or more is too long; the
 * caller treats it as a failure. */
static int render_arg(char *dst, size_t room, const struct pf_spec *s, uint64_t bits,
                      bool is_double)
{
    char fs[40];
    double d;

    switch (s->conv) {
    case 'd':
    case 'i':
    case 'u':
    case 'x':
    case 'X':
    case 'o':
        if (is_double)
            return -1;
        /* Ints always go through long long, so the spec is rewritten with
         * "ll" whatever length it had. */
        memcpy(fs, s->start, s->body);
        fs[s->body] = 'l';
        fs[s->body + 1] = 'l';
        fs[s->body + 2] = s->conv;
        fs[s->body + 3] = '\0';
        if (s->conv == 'd' || s->conv == 'i')
            return snprintf(dst, room, fs, (long long)bits);
        return snprintf(dst, room, fs, (unsigned long long)bits);

    case 'c':
        /* One byte, and only 7-bit ASCII, so the output is valid UTF-8 and
         * matches the Python side. */
        if (is_double || bits < 1 || bits > 127)
            return -1;
        memcpy(fs, s->start, s->body);
        fs[s->body] = 'c';
        fs[s->body + 1] = '\0';
        return snprintf(dst, room, fs, (int)bits);

    case 'p': {
        /* Hand-formatted rather than %p, whose text is libc's choice. */
        if (is_double)
            return -1;
        char hex[32];
        int n = snprintf(hex, sizeof hex, "0x%llx", (unsigned long long)bits);
        int total = s->width > n ? s->width : n;
        if ((size_t)total >= room)
            return total;
        int lead = s->minus ? 0 : total - n;
        int o = 0;
        for (int i = 0; i < lead; i++)
            dst[o++] = ' ';
        memcpy(dst + o, hex, (size_t)n);
        o += n;
        for (int i = 0; i < total - n - lead; i++)
            dst[o++] = ' ';
        dst[o] = '\0';
        return total;
    }

    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
        if (!is_double)
            return -1;
        memcpy(&d, &bits, sizeof d);
        memcpy(fs, s->start, s->len);
        fs[s->len] = '\0';
        return snprintf(dst, room, fs, d);

    default:
        return -1;
    }
}

/* Expands @fmt with the event's args into @out, which is @cap bytes. Returns
 * false if the format does not match the args, or the result does not fit.
 * The caller then falls back to the bare format string. */
bool pf_format_name(char *out, size_t cap, const char *fmt, uint32_t flags, const uint64_t *arg,
                    unsigned nargs)
{
    size_t o = 0;
    unsigned k = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (o + 1 >= cap)
                return false;
            out[o++] = *p;
            continue;
        }
        if (p[1] == '%') {
            if (o + 1 >= cap)
                return false;
            out[o++] = '%';
            p++;
            continue;
        }
        struct pf_spec s;
        if (!parse_spec(p, &s) || k >= nargs)
            return false;
        uint32_t is_dbl_bit = k == 0 ? RF_ARG0_IS_DOUBLE : RF_ARG1_IS_DOUBLE;
        bool is_double = (flags & is_dbl_bit) != 0;
        int n = render_arg(out + o, cap - o, &s, arg[k], is_double);
        if (n < 0 || (size_t)n >= cap - o)
            return false;
        o += (size_t)n;
        k++;
        p += s.len - 1;
    }
    if (k != nargs)
        return false;
    out[o] = '\0';
    return true;
}
