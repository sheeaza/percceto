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

namespace detail
{

/*
 * Event arguments in the form pf_slice_begin_args_impl() takes them: up to
 * PF_MAX_ARGS, each as 64 bits plus a flag saying whether it is a double.
 */
struct ArgBits {
    unsigned nargs;
    uint64_t a0;
    bool a0_dbl;
    uint64_t a1;
    bool a1_dbl;
};

/* Stores a floating point argument as its IEEE-754 bits. */
template <typename T>
inline typename std::enable_if<std::is_floating_point<T>::value>::type
put_arg(T v, uint64_t &bits, bool &is_dbl) noexcept
{
    double d = static_cast<double>(v);
    std::memcpy(&bits, &d, sizeof bits);
    is_dbl = true;
}

/* Stores an integer argument sign-extended to 64 bits. */
template <typename T>
inline typename std::enable_if<std::is_integral<T>::value>::type put_arg(T v, uint64_t &bits,
                                                                         bool &is_dbl) noexcept
{
    bits = static_cast<uint64_t>(static_cast<int64_t>(v));
    is_dbl = false;
}

/* True when every type in As... is arithmetic. */
template <typename... As> struct all_arithmetic : std::true_type {};

template <typename T, typename... Rest>
struct all_arithmetic<T, Rest...>
    : std::integral_constant<bool, std::is_arithmetic<T>::value && all_arithmetic<Rest...>::value> {
};

/*
 * Packs 1 to PF_MAX_ARGS arguments. The C header's pf_slice_begin_args()
 * macros use _Generic, which is C-only, so C++ callers pack here instead.
 * Anything that is not arithmetic (a pointer, a string) is rejected.
 */
template <typename... As> inline ArgBits pack(As... as) noexcept
{
    static_assert(sizeof...(As) >= 1 && sizeof...(As) <= PF_MAX_ARGS,
                  "pftrace takes 1 to PF_MAX_ARGS event arguments");
    static_assert(all_arithmetic<As...>::value, "pftrace event arguments must be int or double");
    uint64_t bits[PF_MAX_ARGS] = { 0, 0 };
    bool is_dbl[PF_MAX_ARGS] = { false, false };
    unsigned i = 0;
    const int expand[] = { 0, (put_arg(as, bits[i], is_dbl[i]), ++i, 0)... };
    (void)expand;
    return ArgBits{ static_cast<unsigned>(sizeof...(As)), bits[0], is_dbl[0], bits[1], is_dbl[1] };
}

} // namespace detail

/**
 * class Slice - Begins a slice on a track; ends it when the scope exits.
 *
 * Use PF_SCOPED_SLICE() or PF_SCOPED_SLICE_ARGS() rather than constructing
 * this directly, so the name is checked to be a string literal at compile
 * time.
 */
class Slice {
public:
    Slice(pf_trace *t, uint64_t track_uuid, const char *name) noexcept : t_(t),
                                                                         track_uuid_(track_uuid)
    {
        pf_slice_begin_impl(t_, track_uuid_, name);
    }

    Slice(pf_trace *t, uint64_t track_uuid, const char *name, detail::ArgBits args) noexcept
        : t_(t),
          track_uuid_(track_uuid)
    {
        pf_slice_begin_args_impl(t_, track_uuid_, name, args.nargs, args.a0, args.a0_dbl, args.a1,
                                 args.a1_dbl);
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
 * Use PF_SCOPED_SLICE_G() or PF_SCOPED_SLICE_ARGS_G() rather than
 * constructing this directly.
 */
class ThreadSlice {
public:
    explicit ThreadSlice(const char *name) noexcept
    {
        pf_slice_begin_g_impl(name);
    }

    ThreadSlice(const char *name, detail::ArgBits args) noexcept
    {
        pf_slice_begin_args_g_impl(name, args.nargs, args.a0, args.a0_dbl, args.a1, args.a1_dbl);
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

/**
 * PF_SCOPED_SLICE_ARGS() - PF_SCOPED_SLICE() with 1 to PF_MAX_ARGS
 * arguments. See pf_slice_begin_args() in pftrace.h.
 * @t: Trace context (pf_trace *).
 * @track_uuid: Track to begin the slice on.
 * @name: Printf-style format, a literal, with one placeholder per argument.
 * @...: One int or double per placeholder.
 */
#define PF_SCOPED_SLICE_ARGS(t, track_uuid, name, ...)                                             \
    pf::Slice PF_CAT(pf_scoped_slice_args_, __COUNTER__)((t), (track_uuid), PF_NAME_LITERAL(name), \
                                                         pf::detail::pack(__VA_ARGS__))

/**
 * PF_SCOPED_SLICE_ARGS_G() - PF_SCOPED_SLICE_ARGS() on the calling thread's
 * track of the default trace.
 * @name: Printf-style format, a literal, with one placeholder per argument.
 * @...: One int or double per placeholder.
 */
#define PF_SCOPED_SLICE_ARGS_G(name, ...)                                               \
    pf::ThreadSlice PF_CAT(pf_scoped_slice_args_g_, __COUNTER__)(PF_NAME_LITERAL(name), \
                                                                 pf::detail::pack(__VA_ARGS__))

#endif /* PFTRACE_HPP */
