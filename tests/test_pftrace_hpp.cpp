/* Compile-only check that include/pftrace.hpp builds as C++11 and that every
 * macro form expands to valid code.
 *
 * Built by tests/CMakeLists.txt as an object file, never linked: nothing
 * here runs, and the library is not needed. A header that stops compiling
 * under C++11 fails this target's build, which is the whole point.
 */

#include "pftrace.hpp"

/* Each function uses one macro form. The names are unused on purpose --
 * the expansion is what is under test. */
void probe_slices(pf_trace *t, uint64_t trk)
{
    PF_SCOPED_SLICE(t, trk, "plain");
    PF_SCOPED_SLICE_G("plain_g");
}

void probe_args(pf_trace *t, uint64_t trk, int i, double d, unsigned u)
{
    PF_SCOPED_SLICE_ARGS(t, trk, "int=%d", i);
    PF_SCOPED_SLICE_ARGS(t, trk, "int=%d dbl=%f", i, d);
    PF_SCOPED_SLICE_ARGS(t, trk, "uint=%u", u);
    PF_SCOPED_SLICE_ARGS(t, trk, "float=%g", 1.5f);
}

void probe_args_g(int i, double d)
{
    PF_SCOPED_SLICE_ARGS_G("g_int=%d", i);
    PF_SCOPED_SLICE_ARGS_G("g_int=%d g_dbl=%e", i, d);
}
