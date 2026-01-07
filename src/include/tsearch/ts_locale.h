/*-------------------------------------------------------------------------
 *
 * ts_locale.h
 *		locale compatibility layer for tsearch
 *
 * Copyright (c) 1998-2014, PostgreSQL Global Development Group
 *
 * src/include/tsearch/ts_locale.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef __TSLOCALE_H__
#define __TSLOCALE_H__

#include <ctype.h>
#include <limits.h>

#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"

/*
 * towlower() and friends should be in <wctype.h>, but some pre-C99 systems
 * declare them in <wchar.h>.
 */
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif
#ifdef HAVE_WCTYPE_H
#include <wctype.h>
#endif

/* working state for tsearch_readline (should be a local var in caller) */
typedef struct
{
	FILE	   *fp;
	const char *filename;
	int			lineno;
	char	   *curline;
	ErrorContextCallback cb;
} tsearch_readline_state;

#define TOUCHAR(x)	(*((const unsigned char *) (x)))

/* The second argument of t_iseq() must be a plain ASCII character */
#define t_iseq(x,c)		(TOUCHAR(x) == (unsigned char) (c))

/* Copy multibyte character of known byte length, return byte length. */
static inline int
ts_copychar_with_len(void *dest, const void *src, int length)
{
	memcpy(dest, src, length);
	return length;
}

#define GENERATE_T_ISCLASS_DECL(character_class) \
extern int	t_is##character_class##_with_len(const char *ptr, int len); \
extern int	t_is##character_class##_cstr(const char *ptr); \
extern int	t_is##character_class##_unbounded(const char *ptr); \
\
/* deprecated */ \
extern int	t_is##character_class(const char *ptr);

#ifdef USE_WIDE_UPPER_LOWER

/* Copy multibyte character from null-terminated string,  return byte length. */
static inline int
ts_copychar_cstr(void *dest, const void *src)
{
	return ts_copychar_with_len(dest, src, pg_mblen_cstr((const char *) src));
}

GENERATE_T_ISCLASS_DECL(alpha);
GENERATE_T_ISCLASS_DECL(digit);
GENERATE_T_ISCLASS_DECL(print);
GENERATE_T_ISCLASS_DECL(space);

#else							/* not USE_WIDE_UPPER_LOWER */

/*
 * Without the wide-character functions we only ever look at the first byte,
 * so every flavor collapses to the single-byte test and no multibyte length
 * is ever computed.  The bounds arguments are accepted and ignored so that
 * callers need not be conditional.
 */
static inline int
ts_copychar_cstr(void *dest, const void *src)
{
	return ts_copychar_with_len(dest, src, 1);
}

#define GENERATE_T_ISCLASS_FALLBACK(character_class) \
static inline int \
t_is##character_class##_with_len(const char *ptr, int len) \
{ \
	return is##character_class(TOUCHAR(ptr)); \
} \
static inline int \
t_is##character_class##_cstr(const char *ptr) \
{ \
	return is##character_class(TOUCHAR(ptr)); \
} \
static inline int \
t_is##character_class##_unbounded(const char *ptr) \
{ \
	return is##character_class(TOUCHAR(ptr)); \
} \
static inline int \
t_is##character_class(const char *ptr) \
{ \
	return is##character_class(TOUCHAR(ptr)); \
}

GENERATE_T_ISCLASS_FALLBACK(alpha)
GENERATE_T_ISCLASS_FALLBACK(digit)
GENERATE_T_ISCLASS_FALLBACK(print)
GENERATE_T_ISCLASS_FALLBACK(space)

#endif   /* USE_WIDE_UPPER_LOWER */

/* Historical macro for ts_copychar_cstr(). */
#define COPYCHAR ts_copychar_cstr

extern char *lowerstr(const char *str);
extern char *lowerstr_with_len(const char *str, int len);

extern bool tsearch_readline_begin(tsearch_readline_state *stp,
					   const char *filename);
extern char *tsearch_readline(tsearch_readline_state *stp);
extern void tsearch_readline_end(tsearch_readline_state *stp);

extern char *t_readline(FILE *fp);

#endif   /* __TSLOCALE_H__ */
