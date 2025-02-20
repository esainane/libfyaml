/*
 * fy-atom.h - internal YAML atom methods
 *
 * Copyright (c) 2019 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FY_ATOM_H
#define FY_ATOM_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#include <fy-list.h>

#include <libfyaml.h>

struct fy_parser;
struct fy_input;

enum fy_atom_style {
	FYAS_PLAIN,
	FYAS_SINGLE_QUOTED,
	FYAS_DOUBLE_QUOTED,
	FYAS_LITERAL,
	FYAS_FOLDED,
	FYAS_URI,	/* special style for URIs */
	FYAS_DOUBLE_QUOTED_MANUAL,
	FYAS_COMMENT	/* (possibly multi line) comment */
};

static inline bool fy_atom_style_is_quoted(enum fy_atom_style style)
{
	return style == FYAS_SINGLE_QUOTED || style == FYAS_DOUBLE_QUOTED;
}

static inline bool fy_atom_style_is_block(enum fy_atom_style style)
{
	return style == FYAS_LITERAL || style == FYAS_FOLDED;
}

enum fy_atom_chomp {
	FYAC_STRIP,
	FYAC_CLIP,
	FYAC_KEEP,
};

struct fy_atom {
	struct fy_mark start_mark;
	struct fy_mark end_mark;
	size_t storage_hint;	/* guaranteed to fit in this amount of bytes */
	struct fy_input *fyi;	/* input on which atom is on */
	unsigned int increment;
	/* save a little bit of space with bitfields */
	enum fy_atom_style style : 6;
	enum fy_atom_chomp chomp : 4;
	bool direct_output : 1;		/* can directly output */
	bool storage_hint_valid : 1;
	bool empty : 1;			/* atom contains whitespace and linebreaks only if length > 0 */
	bool has_lb : 1;		/* atom contains at least one linebreak */
	bool has_ws : 1;		/* atom contains at least one whitespace */
	bool starts_with_ws : 1;	/* atom starts with whitespace */
	bool starts_with_lb : 1;	/* atom starts with linebreak */
	bool ends_with_ws : 1;		/* atom ends with whitespace */
	bool ends_with_lb : 1;		/* atom ends with linebreak */
	bool trailing_lb : 1;		/* atom ends with trailing linebreaks > 1 */ 
	bool size0 : 1;			/* atom contains absolutely nothing */
};

static inline bool fy_atom_is_set(const struct fy_atom *atom)
{
	return atom && atom->fyi;
}

int fy_atom_format_text_length(struct fy_atom *atom);
const char *fy_atom_format_text(struct fy_atom *atom, char *buf, size_t maxsz);

#define fy_atom_get_text_a(_atom) \
	({ \
		struct fy_atom *_a = (_atom); \
		int _len; \
		char *_buf; \
		const char *_txt = ""; \
		\
		if (!_a->direct_output) { \
			_len = fy_atom_format_text_length(_a); \
			if (_len > 0) { \
				_buf = alloca(_len + 1); \
				memset(_buf, 0, _len + 1); \
				fy_atom_format_text(_a, _buf, _len + 1); \
				_buf[_len] = '\0'; \
				_txt = _buf; \
			} \
		} else { \
			_len = fy_atom_size(_a); \
			_buf = alloca(_len + 1); \
			memset(_buf, 0, _len + 1); \
			memcpy(_buf, fy_atom_data(_a), _len); \
			_buf[_len] = '\0'; \
			_txt = _buf; \
		} \
		_txt; \
	})

void fy_fill_atom_start(struct fy_parser *fyp, struct fy_atom *handle);
void fy_fill_atom_end_at(struct fy_parser *fyp, struct fy_atom *handle, struct fy_mark *end_mark);
void fy_fill_atom_end(struct fy_parser *fyp, struct fy_atom *handle);
struct fy_atom *fy_fill_atom(struct fy_parser *fyp, int advance, struct fy_atom *handle);

#define fy_fill_atom_a(_fyp, _advance)  fy_fill_atom((_fyp), (_advance), alloca(sizeof(struct fy_atom)))

struct fy_atom_iter_line_info {
	const char *start;
	const char *end;
	const char *nws_start;
	const char *nws_end;
	const char *chomp_start;
	bool trailing_ws : 1;
	bool empty : 1;
	bool trailing_breaks : 1;
	bool trailing_breaks_ws : 1;
	bool first : 1;		/* first */
	bool last : 1;		/* last (only ws/lb afterwards */
	bool final : 1;		/* the final iterator */
	bool indented : 1;
	bool lb_end : 1;
	bool need_nl : 1;
	bool need_sep : 1;
	size_t start_ws, end_ws;
	const char *s;
	const char *e;
};

struct fy_atom_iter_chunk {
	struct fy_iter_chunk ic;
	/* note that it is guaranteed for copied chunks to be
	 * less or equal to 10 characters (the maximum digitbuf
	 * for double quoted escapes */
	char inplace_buf[10];	/* small copies in place */
};

#define NR_STARTUP_CHUNKS	8
#define SZ_STARTUP_COPY_BUFFER	32

struct fy_atom_iter {
	const struct fy_atom *atom;
	const char *s, *e;
	unsigned int chomp;
	int tabsize;
	bool single_line : 1;
	bool dangling_end_quote : 1;
	bool empty : 1;
	bool current : 1;
	bool done : 1;	/* last iteration (for block styles) */
	struct fy_atom_iter_line_info li[2];
	unsigned int alloc;
	unsigned int top;
	unsigned int read;
	struct fy_atom_iter_chunk *chunks;
	struct fy_atom_iter_chunk startup_chunks[NR_STARTUP_CHUNKS];
	int unget_c;
};

void fy_atom_iter_start(const struct fy_atom *atom, struct fy_atom_iter *iter);
void fy_atom_iter_finish(struct fy_atom_iter *iter);
const struct fy_iter_chunk *fy_atom_iter_peek_chunk(struct fy_atom_iter *iter);
const struct fy_iter_chunk *fy_atom_iter_chunk_next(struct fy_atom_iter *iter, const struct fy_iter_chunk *curr, int *errp);
void fy_atom_iter_advance(struct fy_atom_iter *iter, size_t len);

struct fy_atom_iter *fy_atom_iter_create(const struct fy_atom *atom);
void fy_atom_iter_destroy(struct fy_atom_iter *iter);
ssize_t fy_atom_iter_read(struct fy_atom_iter *iter, void *buf, size_t count);
int fy_atom_iter_getc(struct fy_atom_iter *iter);
int fy_atom_iter_ungetc(struct fy_atom_iter *iter, int c);
int fy_atom_iter_peekc(struct fy_atom_iter *iter);
int fy_atom_iter_utf8_get(struct fy_atom_iter *iter);
int fy_atom_iter_utf8_unget(struct fy_atom_iter *iter, int c);
int fy_atom_iter_utf8_peek(struct fy_atom_iter *iter);

int fy_atom_memcmp(struct fy_atom *atom, const void *ptr, size_t len);
int fy_atom_strcmp(struct fy_atom *atom, const char *str);
bool fy_atom_is_number(struct fy_atom *atom);
int fy_atom_cmp(struct fy_atom *atom1, struct fy_atom *atom2);

#endif
