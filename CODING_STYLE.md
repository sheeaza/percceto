# C coding style

Rules that apply to C across the whole repository. Project-specific guidance
lives next to the code it covers (for example `ringbuf/CLAUDE.md`).

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
