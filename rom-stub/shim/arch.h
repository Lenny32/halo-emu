/*
 * arch.h — freestanding replacement for the firmware-side plf/arch.h.
 *
 * The vendored RivieraWaves/Alif headers include "arch.h" for a handful of
 * compiler/architecture macros.  The firmware's copy pulls in Zephyr
 * (<zephyr/toolchain.h>); the ROM stub is bare-metal, so provide the same
 * macros directly with GCC attributes.
 */

#ifndef __ALIF_ARCH_H__
#define __ALIF_ARCH_H__

#include <stdbool.h>
#include <stdint.h>

/* ARM is little endian */
#define CPU_LE 1

#ifndef __PACKED
#define __PACKED __attribute__((packed))
#endif

#define __ALIGN(n) __attribute__((aligned(n)))

#ifndef __ARRAY_EMPTY
#define __ARRAY_EMPTY
#endif

#ifndef __STATIC
#define __STATIC static
#endif

#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static inline __attribute__((always_inline))
#endif

#ifndef __INLINE
#define __INLINE static inline
#endif

#ifndef ASSERT_ERR
#define ASSERT_ERR(cond) ((void)(cond))
#endif
#ifndef ASSERT_INFO
#define ASSERT_INFO(cond, p0, p1) ((void)(cond))
#endif
#ifndef ASSERT_WARN
#define ASSERT_WARN(cond, p0, p1) ((void)(cond))
#endif

#define PLF_EM_FETCH_TIME_US 40
#define PLF_EM_UPDATE_TIME_US 14

#endif /* __ALIF_ARCH_H__ */
