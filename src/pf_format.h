#ifndef PF_FORMAT_H
#define PF_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Expands @fmt with the @nargs argument values in @arg, whose types are in
 * @flags (the RF_ARG*_IS_DOUBLE bits), into @out, which is @cap bytes. Returns
 * false if the format does not match the args, or the result does not fit; the
 * caller then uses the bare format. A pure function of its inputs, so the
 * producer and any reader give the same name. */
bool pf_format_name(char *out, size_t cap, const char *fmt, uint32_t flags, const uint64_t *arg,
                    unsigned nargs);

#endif /* PF_FORMAT_H */
