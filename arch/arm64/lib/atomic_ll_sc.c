#include <asm/atomic.h>
#define __ARM64_IN_ATOMIC_IMPL

/*
 * Disarm the refcount checks in the out-of-line LL/SC routines. These are
 * redundant, given that the LSE callers already perform the same checks.
 * We do have to make sure that we exit with a zero value if the pre-check
 * detected a zero value.
 */
#undef REFCOUNT_POST_CHECK_NEG
#undef REFCOUNT_POST_CHECK_NEG_OR_ZERO
#define REFCOUNT_POST_CHECK_NEG
#define REFCOUNT_POST_CHECK_NEG_OR_ZERO   "csel %w[val], wzr, %w[val], eq\n"

#include <asm/atomic_ll_sc.h>
