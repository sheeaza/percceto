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

/* A name the caller built at run time takes the dynamic path. */
void probe_slices_dynamic(pf_trace *t, uint64_t trk, const char *built)
{
    char local[32];
    PF_SCOPED_SLICE(t, trk, built);
    PF_SCOPED_SLICE(t, trk, local);
    PF_SCOPED_SLICE_G(local);
}

/* Typed arguments, with integer, unsigned and floating values. */
void probe_slices_args(pf_trace *t, uint64_t trk)
{
    unsigned u = 3;
    PF_SCOPED_SLICE_ARGS(t, trk, "args", PF_KV("n", 1));
    PF_SCOPED_SLICE_ARGS(t, trk, "args_mixed", PF_KV("i", -2), PF_KV("u", u), PF_KV("d", 0.5));
    PF_SCOPED_SLICE_ARGS_G("args_g", PF_KV("n", 1), PF_KV("f", 2.0f));
}
