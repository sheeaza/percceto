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

## Self-contained headers

Every header must compile on its own. A `.c` file that contains only
`#include "header.h"` must compile cleanly with the project's flags, including
`-Werror`. The header includes everything it uses, so no includer has to add
headers first.
