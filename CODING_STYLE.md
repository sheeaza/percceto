# C coding style

Rules that apply to C across the whole repository. Project-specific guidance
lives next to the code it covers.

## Failure paths

Use a goto ladder for cleanup. Each failure site jumps to the label that frees
exactly the resources acquired before that site. Labels are ordered in reverse
acquisition order and fall through to one another.

- Do not guard cleanup with `if (resource)` checks in a shared `fail:` label.
  A shared label cannot tell which resources a given path actually acquired,
  so it either frees something not yet acquired or misses something that was.
- Every `goto` target must be used. An unused label is a compile error under
  `-Werror`, and it usually means the cleanup chain is wrong. Steps in the
  middle of a chain do not need labels: they are reached by fall-through from
  the label above them.
- Teardown mirrors init. Release resources in the reverse order they were
  initialized, so the last thing set up is the first thing torn down.
- Cleanup for a step belongs on the failure path that follows it, not inside
  the branch that fails. A failing `pthread_create` jumps into the chain; it
  does not run its own destroy calls first.
- Every resource acquired must be freed on every later failure path. Check
  each init and warm call, not only the ones with a visible free.

```c
    a = acquire_a();
    if (!a)
        goto out;

    b = acquire_b();
    if (!b)
        goto release_a;

    c = acquire_c();
    if (!c)
        goto release_b;

    return obj;

release_b:
    release_b(b);
release_a:
    release_a(a);
out:
    return NULL;
```

Each `goto` lands on the label that frees exactly what was acquired before it.
A failure in `acquire_c` jumps to `release_b`, which frees `b` and then falls
through to `release_a`. A failure in `acquire_a` jumps to `out` and frees
nothing.

## No unnecessary checks

Test a value only when it can actually take the case the test handles. A check
is a statement about the code: `if (p)` says that `p` may be `NULL` there.
When it never is, the check is false documentation. A reader has to work out
which paths would make it `NULL`, and finds none.

- Before adding a check, decide whether the case is possible at that point,
  from the callers and the invariants. If it is not, leave the check out.
- When a function is only reached once some state holds, rely on it. A destroy
  function, for example, runs only after its init succeeded, so the resources
  init acquired are all there, and destroy releases them without testing.
- Remove a check that has become dead after a change, rather than keeping it
  in case.
- When a value can genuinely be absent, say so where it is declared, and keep
  the check at each use.

```c
/* Called only after thing_init succeeded: a and b are both set. */
void thing_destroy(struct thing *t)
{
    free(t->b);
    munmap(t->a, t->a_bytes);
}
```

## Acquisition checks

Check each acquisition right after it is made, and return early on failure.
An acquisition is any call that obtains a resource: memory, a file, a lock,
a handle. Do not acquire several resources and test them together afterwards.

- Test the result on the line after the call, before any other acquisition.
- A failure with nothing to free returns at once.
- A failure that must release an earlier resource jumps to a cleanup label
  with `goto`, following the goto ladder rules above. Do not release inline
  before returning.
- Set `errno` before returning when the caller depends on it.

```c
    a = acquire_a();
    if (!a) {
        errno = ENOMEM;
        return -1;
    }

    b = acquire_b();
    if (!b)
        goto release_a;

    return 0;

release_a:
    release_a(a);
    errno = ENOMEM;
    return -1;
```

## File-local symbols

Declare a function or type in a header only when a file other than its own
`.c` uses it. Everything else stays file-local.

- Mark helpers that only the `.c` calls as `static`, and define them before
  their first use so no forward declaration is needed.
- Move a struct used only inside the `.c` out of the header and into the `.c`.
- Remove the matching prototypes from the header when you make a symbol static.
- Macros and inline functions that other files use stay in the header.
- Rebuild and run the tests after moving a symbol. A removed prototype that
  another file still needs fails at compile or link time.

## Hot-path structs and memset

A struct on a hot path is not cleared with `memset` as a whole when it can
grow. A fixed-size clear of a struct that later gains optional fields turns
into a real libc call once it is large enough, and that call can cost more
than the work it guards.

- Set each field the code reads, in the order the code needs them. Leave out
  fields a path never reads when it does not take them.
- Keep optional state in its own struct, and point at it only when it is
  present. A path with no optional state then clears none of it.
- After a change to a hot struct, check the compiled code for a `memset` call
  on the path, for example in the disassembly or with `callgrind`. A call that
  was inline before is a sign the clear grew.
- Measure the path before and after. Record the numbers in the change notes.

## Self-contained headers

Every header must compile on its own. A `.c` file that contains only
`#include "header.h"` must compile cleanly with the project's flags, including
`-Werror`. The header includes everything it uses, so no includer has to add
headers first.
