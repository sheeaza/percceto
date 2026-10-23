#ifndef PFTRACE_HPP
#define PFTRACE_HPP

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <type_traits>

#include "pftrace.h"

/*
 * C++ wrapper over pftrace.h. Ownership and slice nesting are enforced by
 * scope: a pf::Slice ends its slice in its destructor, so begin/end pairs
 * stay balanced across early returns and exceptions.
 *
 * Slices on a track must nest in LIFO order. Guards are non-copyable and
 * non-movable, which keeps each guard tied to the scope it was declared in.
 */

namespace pf
{

/**
 * class Trace - Owns a pf_trace and closes it on destruction.
 *
 * Move-only. Construction throws std::system_error on failure, with the
 * errno pf_trace_open() set.
 */
class Trace {
public:
    explicit Trace(const pf_config &cfg)
        : t_(pf_trace_open(&cfg))
    {
        if (!t_) {
            throw std::system_error(errno, std::generic_category(), "pf_trace_open");
        }
    }

    ~Trace()
    {
        pf_trace_close(t_);
    }

    Trace(Trace &&other) noexcept : t_(other.t_)
    {
        other.t_ = nullptr;
    }

    Trace &operator=(Trace &&other) noexcept
    {
        if (this != &other) {
            pf_trace_close(t_);
            t_ = other.t_;
            other.t_ = nullptr;
        }
        return *this;
    }

    Trace(const Trace &) = delete;
    Trace &operator=(const Trace &) = delete;

    /** get() - The underlying context, for the C API. Owned by this object. */
    pf_trace *get() const noexcept
    {
        return t_;
    }

    int dump(const char *name, char *file, size_t size)
    {
        return pf_trace_dump(t_, name, file, size);
    }

    int flush(const char *name, char *file, size_t size)
    {
        return pf_trace_flush(t_, name, file, size);
    }

    void stats(pf_stats *out) const
    {
        pf_trace_stats(t_, out);
    }

private:
    pf_trace *t_;
};

/**
 * class Slice - Begins a slice on a track; ends it when the scope exits.
 *
 * Use PF_SCOPED_SLICE() rather than constructing this directly, so the name
 * is checked to be a string literal at compile time.
 */
class Slice {
public:
    Slice(pf_trace *t, uint64_t track_uuid, const char *name) noexcept : t_(t),
                                                                         track_uuid_(track_uuid)
    {
        pf_slice_begin_impl(t_, track_uuid_, name);
    }

    ~Slice()
    {
        pf_slice_end(t_, track_uuid_);
    }

    Slice(const Slice &) = delete;
    Slice &operator=(const Slice &) = delete;
    Slice(Slice &&) = delete;
    Slice &operator=(Slice &&) = delete;

private:
    pf_trace *t_;
    uint64_t track_uuid_;
};

/**
 * class ThreadSlice - Begins a slice on the calling thread's track of the
 * default trace; ends it when the scope exits.
 *
 * Use PF_SCOPED_SLICE_G() rather than
 * constructing this directly.
 */
class ThreadSlice {
public:
    explicit ThreadSlice(const char *name) noexcept
    {
        pf_slice_begin_g_impl(name);
    }

    ~ThreadSlice()
    {
        pf_slice_end_g();
    }

    ThreadSlice(const ThreadSlice &) = delete;
    ThreadSlice &operator=(const ThreadSlice &) = delete;
    ThreadSlice(ThreadSlice &&) = delete;
    ThreadSlice &operator=(ThreadSlice &&) = delete;
};

} // namespace pf

#define PF_CAT_IMPL(a, b) a##b
#define PF_CAT(a, b) PF_CAT_IMPL(a, b)

/**
 * PF_SCOPED_SLICE() - Begin a slice that ends at the end of the enclosing
 * scope.
 * @t: Trace context (pf_trace *).
 * @track_uuid: Track to begin the slice on.
 * @name: Slice name. Must be a string literal -- see PF_NAME_LITERAL().
 */
#define PF_SCOPED_SLICE(t, track_uuid, name) \
    pf::Slice PF_CAT(pf_scoped_slice_, __COUNTER__)((t), (track_uuid), PF_NAME_LITERAL(name))

/**
 * PF_SCOPED_SLICE_G() - PF_SCOPED_SLICE() on the calling thread's track of
 * the default trace.
 * @name: Slice name. Must be a string literal -- see PF_NAME_LITERAL().
 */
#define PF_SCOPED_SLICE_G(name) \
    pf::ThreadSlice PF_CAT(pf_scoped_slice_g_, __COUNTER__)(PF_NAME_LITERAL(name))

#endif /* PFTRACE_HPP */
