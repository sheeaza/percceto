#ifndef PFTRACE_HPP
#define PFTRACE_HPP

#include <array>
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
 * kv_arg() - Build one typed argument from a key and a numeric value.
 *
 * The C PF_KV() selects the helper with _Generic, which C++ does not have, so
 * the overloads below pick it by type. Pointers do not match any overload,
 * as they do not compile in PF_KV().
 */
template <typename T, typename std::enable_if<std::is_floating_point<T>::value, int>::type = 0>
inline pf_arg kv_arg(const char *key, T value)
{
    return pf_arg_double(key, static_cast<double>(value));
}

template <typename T, typename std::enable_if<
                          std::is_integral<T>::value && std::is_unsigned<T>::value, int>::type = 0>
inline pf_arg kv_arg(const char *key, T value)
{
    return pf_arg_uint(key, static_cast<uint64_t>(value));
}

template <typename T, typename std::enable_if<
                          std::is_integral<T>::value && std::is_signed<T>::value, int>::type = 0>
inline pf_arg kv_arg(const char *key, T value)
{
    return pf_arg_int(key, static_cast<int64_t>(value));
}

/**
 * make_args() - Collect PF_KV() arguments into a fixed-size array.
 *
 * The count is the number of arguments, so the size check happens at compile
 * time in the constructors that take the array.
 */
template <typename... Args> inline std::array<pf_arg, sizeof...(Args)> make_args(Args... args)
{
    return std::array<pf_arg, sizeof...(Args)>{ { args... } };
}

/**
 * class Slice - Ends a slice on a track when the scope exits.
 *
 * Use PF_SCOPED_SLICE() rather than constructing this directly. The macro
 * begins the slice with pf_slice_begin(), so it takes the same static,
 * dynamic, or format path, then hands the guard the track to end.
 */
class Slice {
public:
    /* Tag for the constructor below: the slice has already begun. */
    struct Begun {};

    Slice(Begun, pf_trace *t, uint64_t track_uuid) noexcept : t_(t), track_uuid_(track_uuid)
    {
    }

    /* Typed arguments, as pf_slice_begin_args(). The name must be a literal. */
    template <std::size_t N>
    Slice(pf_trace *t, uint64_t track_uuid, const char *name,
          const std::array<pf_arg, N> &args) noexcept : t_(t),
                                                        track_uuid_(track_uuid)
    {
        static_assert(N >= 1 && N <= PF_MAX_ARGS, "PF_SCOPED_SLICE_ARGS() takes 1 to PF_MAX_ARGS");
        pf_slice_begin_args_impl(t_, track_uuid_, name, static_cast<unsigned>(N), args.data());
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
 * Use PF_SCOPED_SLICE_G() rather than constructing this directly. The macro
 * begins the slice with pf_slice_begin_g(), as it does for Slice.
 */
class ThreadSlice {
public:
    /* Tag for the constructor below: the slice has already begun. */
    struct Begun {};

    explicit ThreadSlice(Begun) noexcept
    {
    }

    /* Typed arguments, as pf_slice_begin_args_g(). The name must be a literal. */
    template <std::size_t N>
    ThreadSlice(const char *name, const std::array<pf_arg, N> &args) noexcept
    {
        static_assert(N >= 1 && N <= PF_MAX_ARGS,
                      "PF_SCOPED_SLICE_ARGS_G() takes 1 to PF_MAX_ARGS");
        pf_slice_begin_args_g_impl(name, static_cast<unsigned>(N), args.data());
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

/* Instant event with typed arguments, as pf_instant_event_args(). The array
 * comes in by reference, so the macro below needs no compound literal: GCC 13
 * rejects one in C++ ("taking address of temporary array"). */
template <std::size_t N>
inline void instant_args(pf_trace *t, uint64_t track_uuid, const char *name,
                         const std::array<pf_arg, N> &args) noexcept
{
    static_assert(N >= 1 && N <= PF_MAX_ARGS, "pf_instant_event_args() takes 1 to PF_MAX_ARGS");
    pf_instant_event_args_impl(t, track_uuid, name, static_cast<unsigned>(N), args.data());
}

} // namespace pf

#define PF_CAT_IMPL(a, b) a##b
#define PF_CAT(a, b) PF_CAT_IMPL(a, b)

/**
 * PF_SCOPED_SLICE() - Begin a slice that ends at the end of the enclosing
 * scope.
 * @t: Trace context (pf_trace *).
 * @track_uuid: Track to begin the slice on.
 * @name: Slice name: a string literal, a name the caller built, or a printf
 *      format when @... is given. Dispatched as pf_slice_begin() is, including
 *      the constraint on static buffers.
 * @...: Arguments for @name as a printf format.
 */
#define PF_SCOPED_SLICE(t, track_uuid, name, ...)                                 \
    pf::Slice PF_CAT(pf_scoped_slice_, __COUNTER__)(                              \
        (pf_slice_begin((t), (track_uuid), name __VA_OPT__(, ) __VA_ARGS__),      \
         pf::Slice::Begun{}),                                                     \
        (t), (track_uuid))

/**
 * PF_SCOPED_SLICE_G() - PF_SCOPED_SLICE() on the calling thread's track of
 * the default trace.
 * @name: Slice name, dispatched as PF_SCOPED_SLICE() dispatches it.
 * @...: Arguments for @name as a printf format.
 */
#define PF_SCOPED_SLICE_G(name, ...)                                                    \
    pf::ThreadSlice PF_CAT(pf_scoped_slice_g_, __COUNTER__)(                            \
        (pf_slice_begin_g(name __VA_OPT__(, ) __VA_ARGS__), pf::ThreadSlice::Begun{}))

/*
 * PF_KV() in C selects its helper with _Generic, which C++ lacks. Redefine it
 * for C++ on top of kv_arg(), with the same key rule: a string literal.
 */
#undef PF_KV
#define PF_KV(key, value) pf::kv_arg(PF_NAME_LITERAL(key), (value))

/**
 * PF_SCOPED_SLICE_ARGS() - PF_SCOPED_SLICE() with typed arguments, as
 * pf_slice_begin_args().
 * @t: Trace context (pf_trace *).
 * @track_uuid: Track to begin the slice on.
 * @name: Slice name: a string literal.
 * @...: One to PF_MAX_ARGS PF_KV() arguments.
 */
#define PF_SCOPED_SLICE_ARGS(t, track_uuid, name, ...)                                             \
    pf::Slice PF_CAT(pf_scoped_slice_args_, __COUNTER__)((t), (track_uuid), PF_NAME_LITERAL(name), \
                                                         pf::make_args(__VA_ARGS__))

/**
 * PF_SCOPED_SLICE_ARGS_G() - PF_SCOPED_SLICE_ARGS() on the calling thread's
 * track of the default trace.
 * @name: Slice name: a string literal.
 * @...: One to PF_MAX_ARGS PF_KV() arguments.
 */
#define PF_SCOPED_SLICE_ARGS_G(name, ...)                                               \
    pf::ThreadSlice PF_CAT(pf_scoped_slice_args_g_, __COUNTER__)(PF_NAME_LITERAL(name), \
                                                                 pf::make_args(__VA_ARGS__))

/*
 * pf_instant_event_args() in C takes its arguments as a compound literal,
 * which GCC rejects in C++ here. Redefine it on top of pf::instant_args().
 */
#undef pf_instant_event_args
#define pf_instant_event_args(t, track_uuid, name, ...) \
    pf::instant_args((t), (track_uuid), PF_NAME_LITERAL(name), pf::make_args(__VA_ARGS__))

#endif /* PFTRACE_HPP */
