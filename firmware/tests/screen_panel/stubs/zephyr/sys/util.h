#pragma once
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ARG_UNUSED(x) (void)(x)
#define BUILD_ASSERT(x, msg) _Static_assert((x), msg)
#define DT_PATH(x) 0
#define DT_GPIO_CTLR(n,p) 0
#define DT_SAME_NODE(a,b) 1
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
