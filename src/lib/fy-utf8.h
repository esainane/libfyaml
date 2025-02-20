/*
 * fy-utf8.h - UTF-8 methods
 *
 * Copyright (c) 2019 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FY_UTF8_H
#define FY_UTF8_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <alloca.h>
#include <assert.h>

#include <libfyaml.h>

static inline int
fy_utf8_width_by_first_octet(uint8_t c)
{
	return (c & 0x80) == 0x00 ? 1 :
	       (c & 0xe0) == 0xc0 ? 2 :
	       (c & 0xf0) == 0xe0 ? 3 :
	       (c & 0xf8) == 0xf0 ? 4 : 0;
}

/* assumes valid utf8 character */
static inline size_t
fy_utf8_width(int c)
{
	return c <      0x80 ? 1 :
	       c <     0x800 ? 2 :
	       c <   0x10000 ? 3 : 4;
}

static inline bool
fy_utf8_is_valid(int c)
{
	return c >= 0 && !((c >= 0xd800 && c <= 0xdfff) || c >= 0x110000);
}

/* generic utf8 decoder (not inlined) */
int fy_utf8_get_generic(const void *ptr, int left, int *widthp);

static inline int fy_utf8_get(const void *ptr, int left, int *widthp)
{
	const uint8_t *p = ptr;

	/* single byte (hot path) */
	if (left <= 0) {
		*widthp = 0;
		return -1;
	}

	if (!(p[0] & 0x80)) {
		*widthp = 1;
		return p[0] & 0x7f;
	}
	return fy_utf8_get_generic(ptr, left, widthp);
}

int fy_utf8_get_right_generic(const void *ptr, int left, int *widthp);

static inline int fy_utf8_get_right(const void *ptr, int left, int *widthp)
{
	const uint8_t *p = ptr + left;

	/* single byte (hot path) */
	if (left > 0 && !(p[-1] & 0x80)) {
		if (widthp)
			*widthp = 1;
		return p[-1] & 0x7f;
	}
	return fy_utf8_get_right_generic(ptr, left, widthp);
}


/* for when you _know_ that there's enough room and c is valid */
static inline void *fy_utf8_put_unchecked(void *ptr, int c)
{
	uint8_t *s = ptr;

	assert(c >= 0);
	if (c < 0x80)
		*s++ = c;
	else if (c < 0x800) {
		*s++ = (c >> 6) | 0xc0;
		*s++ = (c & 0x3f) | 0x80;
	} else if (c < 0x10000) {
		*s++ = (c >> 12) | 0xe0;
		*s++ = ((c >> 6) & 0x3f) | 0x80;
		*s++ = (c & 0x3f) | 0x80;
	} else {
		*s++ = (c >> 18) | 0xf0;
		*s++ = ((c >> 12) & 0x3f) | 0x80;
		*s++ = ((c >> 6) & 0x3f) | 0x80;
		*s++ = (c & 0x3f) | 0x80;
	}
	return s;
}

static inline void *fy_utf8_put(void *ptr, size_t left, int c)
{
	if (!fy_utf8_is_valid(c) || fy_utf8_width(c) > left)
		return NULL;

	return fy_utf8_put_unchecked(ptr, c);
}

/* buffer must contain at least 5 characters */
#define FY_UTF8_FORMAT_BUFMIN	5
enum fy_utf8_escape {
	fyue_none,
	fyue_doublequote,
	fyue_singlequote,
};

char *fy_utf8_format(int c, char *buf, enum fy_utf8_escape esc);

#define fy_utf8_format_a(_c, _esc) \
	({ \
	 	char *_buf = alloca(FY_UTF8_FORMAT_BUFMIN); \
	 	fy_utf8_format((_c), _buf, _esc); \
	})

int fy_utf8_format_text_length(const char *buf, size_t len,
			       enum fy_utf8_escape esc);
char *fy_utf8_format_text(const char *buf, size_t len,
			  char *out, size_t maxsz,
			  enum fy_utf8_escape esc);

#define fy_utf8_format_text_a(_buf, _len, _esc) \
	({ \
		const char *__buf = (_buf); \
		size_t __len = (_len); \
		enum fy_utf8_escape __esc = (_esc); \
		size_t _outsz = fy_utf8_format_text_length(__buf, __len, __esc); \
		char *_out = alloca(_outsz + 1); \
		fy_utf8_format_text(__buf, __len, _out, _outsz, __esc); \
	})

const void *fy_utf8_memchr_generic(const void *s, int c, size_t n);

static inline const void *fy_utf8_memchr(const void *s, int c, size_t n)
{
	if (c < 0 || !n)
		return NULL;
	if (c < 0x80)
		return memchr(s, c, n);
	return fy_utf8_memchr_generic(s, c, n);
}

static inline const void *fy_utf8_strchr(const void *s, int c)
{
	if (c < 0)
		return NULL;
	if (c < 0x80)
		return strchr(s, c);
	return fy_utf8_memchr_generic(s, c, strlen(s));
}

static inline int fy_utf8_count(const void *ptr, size_t len)
{
	const uint8_t *s = ptr, *e = ptr + len;
	int w, count;

	count = 0;
	while (s < e) {
		w = fy_utf8_width_by_first_octet(*s);

		/* malformed? */
		if (!w || s + w > e)
			break;
		s += w;

		count++;
	}

	return count;
}

int fy_utf8_parse_escape(const char **strp, size_t len);

#endif
