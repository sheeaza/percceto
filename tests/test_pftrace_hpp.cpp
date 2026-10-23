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
