/* Compile check for the name macros: each takes a string literal and a name
 * the caller built, and both must compile. Built by tests/CMakeLists.txt as an
 * object file, never linked: nothing here runs.
 */

#include <stdio.h>

#include "pftrace.h"

void probe_literal(struct pf_trace *t, uint64_t trk)
{
    pf_slice_begin(t, trk, "plain");
    pf_instant_event(t, trk, "marker");
    pf_slice_begin_g("plain_g");
    pf_instant_event_g("marker_g");
}

void probe_built(struct pf_trace *t, uint64_t trk, const char *dynamic)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "req-%s", dynamic);
    pf_slice_begin(t, trk, buf);
    pf_slice_begin(t, trk, "req-%d", 7);
    pf_slice_begin(t, trk, "req-%s", dynamic);
    pf_instant_event(t, trk, dynamic);
    pf_slice_begin_g(buf);
    pf_instant_event_g(dynamic);
    pf_instant_event(t, trk, "req-%d", 3);
    pf_slice_begin_g("req-%s", dynamic);
    pf_instant_event_g("req-%d", 4);
}
