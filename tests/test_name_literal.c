/* The compile-time half of the name contract.
 *
 * Event names are stored in the ring as a borrowed pointer and read later
 * on the drain thread, so a name that does not outlive the trace is a
 * dangling read -- and one that nothing at runtime can detect, because the
 * dereference happens on another thread at an unbounded delay. The macro
 * in pftrace.h rejects the cases where that is likely, and this file is
 * what proves it still does.
 *
 * Built four times by tests/CMakeLists.txt, once per MODE. MODE_OK must
 * compile; the rest must not, and CMake asserts the failure. A negative
 * test that silently started passing would be worse than no test, hence
 * the WILL_FAIL wiring rather than a comment saying "this should error".
 */

#include "pftrace.h"

#if defined(MODE_OK)
/* Literals, including the forms that must keep working: adjacent literal
 * concatenation, and a macro that expands to a literal. */
#define NAME_FROM_MACRO "from_macro"
void probe(struct pf_trace *t, uint64_t trk)
{
    pf_slice_begin(t, trk, "plain");
    pf_slice_begin(t, trk, "adja" "cent");
    pf_slice_begin(t, trk, NAME_FROM_MACRO);
    pf_instant_event(t, trk, "marker");
}

#elif defined(MODE_VARIABLE)
/* The mistake this exists to catch: a pointer whose target may die. */
void probe(struct pf_trace *t, uint64_t trk, const char *dynamic)
{
    pf_slice_begin(t, trk, dynamic);
}

#elif defined(MODE_STACK_BUFFER)
/* The worst version of it -- the buffer is gone before the drain thread
 * ever looks at the name. */
#include <stdio.h>
void probe(struct pf_trace *t, uint64_t trk, int i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "request-%d", i);
    pf_slice_begin(t, trk, buf);
}

#elif defined(MODE_EMPTY)
/* Compiles as a literal but names nothing, so the UI would show a blank
 * slice; the macro's length check rejects it. */
void probe(struct pf_trace *t, uint64_t trk)
{
    pf_slice_begin(t, trk, "");
}

#else
#error "define one of MODE_OK, MODE_VARIABLE, MODE_STACK_BUFFER, MODE_EMPTY"
#endif
