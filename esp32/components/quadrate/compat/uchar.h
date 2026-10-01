/*
 * <uchar.h> for newlib, which has none. u8t takes char32_t from it and
 * nothing else; in C++ the type is built in, so there is nothing to declare.
 */
#ifndef QDOS_COMPAT_UCHAR_H
#define QDOS_COMPAT_UCHAR_H

#ifndef __cplusplus
#include <stdint.h>
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

#endif
