#ifndef PF_UTIL_H
#define PF_UTIL_H

/* Small helpers shared by the ring backends. Private to src/, like pf_ring.h. */

/* Pause between spins: tells the core this is a spin-wait loop, which costs
 * less power and clears the pipeline of speculated loads. It is only a hint,
 * so the fallback is a compiler barrier and no correctness depends on it. */
static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

#endif /* PF_UTIL_H */
