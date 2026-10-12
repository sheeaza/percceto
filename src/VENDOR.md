Vendored from /home/max/project/ringbuf at commit
2f465b223c7b7a55517668a15cfd61c478764e1c ("Move ringbuf_mpsc and
ringbuf_zcopy's structs into their headers").

    ringbuf_mpsc.c   ringbuf_mpsc.h
    ringbuf_zcopy.c  ringbuf_zcopy.h

Copied verbatim -- no local edits, so refreshing them is a copy rather than
a merge. They compile warning-free under this project's -Wall -Wextra with
Clang 21, which gate 1 of tools/run_checks.sh requires; if a future version
does not, scope a pragma or build those two objects with -w rather than
editing the source.

Why that commit matters here: it moved both structs out of the .c files and
into the headers. The fields are exposed but still internal -- reads are
fine, every write belongs to the library functions, which is where the
memory ordering each field documents is established.

Both structs end in `_Alignas(64) char tail[]`. C11 6.7.2.1p3 forbids an
FAM-terminated struct as a member of another, so src/pf_ring_mpsc.h and
src/pf_ring_zcopy.h hold a POINTER from *_create() rather than embedding
one by value. Clang does accept the embedding as an extension, but sizeof
is then just the 192-byte header with no slot storage behind it, and making
it real would mean over-allocating struct pf_trace and hand-rolling the
stamp initialisation -- which hangs rather than failing a check when it is
wrong. See the plan's "Decisions already made" for the full reasoning.

These are the two multi-producer variants of five in the upstream tree. The
other three (ringbuf, ringbuf_mutex, ringbuf_vslot) are SPSC and keep their
structs private; they are not vendored because pftrace has concurrent
producers.

Upstream documents the contracts in the headers themselves -- read
ringbuf_zcopy.h's "SEVERAL PRODUCERS" section before touching
pf_ring_zcopy.c, since its three rules are load-bearing and one of them is
a deadlock.
