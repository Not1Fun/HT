#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef int32_t atomic_t;
typedef int32_t atomic_val_t;
static inline int32_t atomic_get(const atomic_t *p){return *p;}
static inline int32_t atomic_set(atomic_t *p,int32_t v){int32_t old=*p;*p=v;return old;}
static inline void atomic_clear(atomic_t *p){*p=0;}
static inline bool atomic_cas(atomic_t *p,int32_t old,int32_t v){if(*p!=old)return false;*p=v;return true;}
static inline void atomic_add(atomic_t *p,int32_t v){*p+=v;}
