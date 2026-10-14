Not copied: the two multi-producer ringbuf variants are no longer vendored
into src/. src/CMakeLists.txt links the `ringbuf_mpsc` and `ringbuf_zcopy`
targets from ringbuf/ (this repo's own tree, imported with its full history),
so ringbuf/ is the single source. Edit them there; there is no refresh step.
They build under the root's -Wall -Wextra like everything else.

Why the split between mpsc and zcopy matters here: both structs are defined
in their headers, not the .c files. The fields are exposed but still
internal -- reads are fine, every write belongs to the library functions,
which is where the memory ordering each field documents is established.

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
