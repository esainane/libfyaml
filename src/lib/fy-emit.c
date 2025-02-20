/*
 * fy-emit.c - Internal YAML emitter methods
 *
 * Copyright (c) 2019 Pantelis Antoniou <pantelis.antoniou@konsulko.com>
 *
 * SPDX-License-Identifier: MIT
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <ctype.h>

#include <libfyaml.h>

#include "fy-parse.h"
#include "fy-emit.h"

/* fwd decl */
void fy_emit_write(struct fy_emitter *emit, enum fy_emitter_write_type type, const char *str, int len);

int fy_emit_accum_grow(struct fy_emit_accum *ea)
{
	size_t asz;
	char *new_accum;

	asz = ea->alloc * 2;
	new_accum = realloc(ea->accum == ea->inplace ? NULL : ea->accum, asz);
	if (!new_accum)	/* out of memory */
		return -1;
	if (ea->accum == ea->inplace)
		memcpy(new_accum, ea->accum, ea->next);
	ea->alloc *= 2;
	ea->accum = new_accum;

	return 0;
}

static inline bool fy_emit_is_json_mode(const struct fy_emitter *emit)
{
	enum fy_emitter_cfg_flags flags = emit->cfg->flags & FYECF_MODE(FYECF_MODE_MASK);

	return flags == FYECF_MODE_JSON || flags == FYECF_MODE_JSON_TP || flags == FYECF_MODE_JSON_ONELINE;
}

static inline bool fy_emit_is_flow_mode(const struct fy_emitter *emit)
{
	enum fy_emitter_cfg_flags flags = emit->cfg->flags & FYECF_MODE(FYECF_MODE_MASK);

	return flags == FYECF_MODE_FLOW || flags == FYECF_MODE_FLOW_ONELINE;
}

static inline bool fy_emit_is_block_mode(const struct fy_emitter *emit)
{
	enum fy_emitter_cfg_flags flags = emit->cfg->flags & FYECF_MODE(FYECF_MODE_MASK);

	return flags == FYECF_MODE_BLOCK;
}

static inline bool fy_emit_is_oneline(const struct fy_emitter *emit)
{
	enum fy_emitter_cfg_flags flags = emit->cfg->flags & FYECF_MODE(FYECF_MODE_MASK);

	return flags == FYECF_MODE_FLOW_ONELINE || flags == FYECF_MODE_JSON_ONELINE;
}

static inline int fy_emit_indent(struct fy_emitter *emit)
{
	int indent;

	indent = (emit->cfg->flags & FYECF_INDENT(FYECF_INDENT_MASK)) >> FYECF_INDENT_SHIFT;
	return indent ? indent : 2;
}

static inline int fy_emit_width(struct fy_emitter *emit)
{
	int width;

	width = (emit->cfg->flags & FYECF_WIDTH(FYECF_WIDTH_MASK)) >> FYECF_WIDTH_SHIFT;
	if (width == 0)
		return 80;
	if (width == FYECF_WIDTH_MASK)
		return INT_MAX;
	return width;
}

static inline bool fy_emit_output_comments(struct fy_emitter *emit)
{
	return !!(emit->cfg->flags & FYECF_OUTPUT_COMMENTS);
}

void fy_emit_node_internal(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent);
void fy_emit_scalar(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent);
void fy_emit_sequence(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent);
void fy_emit_mapping(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent);

void fy_emit_write(struct fy_emitter *emit, enum fy_emitter_write_type type, const char *str, int len)
{
	int c, w;
	const char *m, *e;
	int outlen;

	if (!len)
		return;

	outlen = emit->cfg->output(emit, type, str, len, emit->cfg->userdata);
	if (outlen != len)
		emit->output_error = true;

	e = str + len;
	while ((c = fy_utf8_get(str, (e - str), &w)) != -1) {

		/* special handling for MSDOS */
		if (c == '\r' && (e - str) > 1 && str[1] == '\n') {
			str += 2;
			emit->column = 0;
			emit->line++;
			continue;
		}

		/* regular line break */
		if (fy_is_lb(c)) {
			emit->column = 0;
			emit->line++;
			str += w;
			continue;
		}

		/* completely ignore ANSI color escape sequences */
		if (c == '\x1b' && (e - str) > 2 && str[1] == '[' &&
		    (m = memchr(str, 'm', e - str)) != NULL) {
			str = m + 1;
			continue;
		}

		emit->column++;
		str += w;
	}
}

void fy_emit_puts(struct fy_emitter *emit, enum fy_emitter_write_type type, const char *str)
{
	fy_emit_write(emit, type, str, strlen(str));
}

void fy_emit_putc(struct fy_emitter *emit, enum fy_emitter_write_type type, int c)
{
	char buf[FY_UTF8_FORMAT_BUFMIN];

	fy_utf8_format(c, buf, fyue_none);
	fy_emit_puts(emit, type, buf);
}

void fy_emit_vprintf(struct fy_emitter *emit, enum fy_emitter_write_type type, const char *fmt, va_list ap)
{
	char *str;
	int size;
	va_list ap2;

	va_copy(ap2, ap);

	size = vsnprintf(NULL, 0, fmt, ap);
	if (size < 0)
		return;

	str = alloca(size + 1);
	size = vsnprintf(str, size + 1, fmt, ap2);
	if (size < 0)
		return;

	fy_emit_write(emit, type, str, size);
}

void fy_emit_printf(struct fy_emitter *emit, enum fy_emitter_write_type type, const char *fmt, ...)
		__attribute__((format(printf, 3, 4)));

void fy_emit_printf(struct fy_emitter *emit, enum fy_emitter_write_type type, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fy_emit_vprintf(emit, type, fmt, ap);
	va_end(ap);
}

void fy_emit_write_ws(struct fy_emitter *emit)
{
	fy_emit_putc(emit, fyewt_whitespace, ' ');
	emit->flags |= FYEF_WHITESPACE;
}

void fy_emit_write_indent(struct fy_emitter *emit, int indent)
{
	int len;
	char *ws;

	indent = indent > 0 ? indent : 0;

	if (!fy_emit_indentation(emit) || emit->column > indent ||
	    (emit->column == indent && !fy_emit_whitespace(emit)))
		fy_emit_putc(emit, fyewt_linebreak, '\n');

	if (emit->column < indent) {
		len = indent - emit->column;
		ws = alloca(len + 1);
		memset(ws, ' ', len);
		ws[len] = '\0';
		fy_emit_write(emit, fyewt_indent, ws, len);
	}

	emit->flags |= FYEF_WHITESPACE | FYEF_INDENTATION;
}

enum document_indicator {
	di_question_mark,
	di_colon,
	di_dash,
	di_left_bracket,
	di_right_bracket,
	di_left_brace,
	di_right_brace,
	di_comma,
	di_bar,
	di_greater,
	di_single_quote_start,
	di_single_quote_end,
	di_double_quote_start,
	di_double_quote_end,
	di_ambersand,
	di_star,
};

void fy_emit_write_indicator(struct fy_emitter *emit,
		enum document_indicator indicator,
		int flags, int indent,
		enum fy_emitter_write_type wtype)
{
	switch (indicator) {

	case di_question_mark:
		if (!fy_emit_whitespace(emit))
			fy_emit_write_ws(emit);
		fy_emit_putc(emit, wtype, '?');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_OPEN_ENDED);
		break;

	case di_colon:
		if (!(flags & DDNF_SIMPLE)) {
			if (!emit->flow_level && !fy_emit_is_oneline(emit))
				fy_emit_write_indent(emit, indent);
			if (!fy_emit_whitespace(emit))
				fy_emit_write_ws(emit);
		}
		fy_emit_putc(emit, wtype, ':');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_OPEN_ENDED);
		break;

	case di_dash:
		if (!fy_emit_whitespace(emit))
			fy_emit_write_ws(emit);
		fy_emit_putc(emit, wtype, '-');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_OPEN_ENDED);
		break;

	case di_left_bracket:
	case di_left_brace:
		emit->flow_level++;
		if (!fy_emit_whitespace(emit))
			fy_emit_write_ws(emit);
		fy_emit_putc(emit, wtype, indicator == di_left_bracket ? '[' : '{');
		emit->flags |= FYEF_WHITESPACE;
		emit->flags &= ~(FYEF_INDENTATION | FYEF_OPEN_ENDED);
		break;

	case di_right_bracket:
	case di_right_brace:
		emit->flow_level--;
		fy_emit_putc(emit, wtype, indicator == di_right_bracket ? ']' : '}');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_INDENTATION | FYEF_OPEN_ENDED);
		break;

	case di_comma:
		fy_emit_putc(emit, wtype, ',');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_INDENTATION | FYEF_OPEN_ENDED);
		break;

	case di_bar:
	case di_greater:
		if (!fy_emit_whitespace(emit))
			fy_emit_write_ws(emit);
		fy_emit_putc(emit, wtype, indicator == di_bar ? '|' : '>');
		emit->flags &= ~(FYEF_INDENTATION | FYEF_WHITESPACE | FYEF_OPEN_ENDED);
		break;

	case di_single_quote_start:
	case di_double_quote_start:
		if (!fy_emit_whitespace(emit))
			fy_emit_write_ws(emit);
		fy_emit_putc(emit, wtype, indicator == di_single_quote_start ? '\'' : '"');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_INDENTATION | FYEF_OPEN_ENDED);
		break;

	case di_single_quote_end:
	case di_double_quote_end:
		fy_emit_putc(emit, wtype, indicator == di_single_quote_end ? '\'' : '"');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_INDENTATION | FYEF_OPEN_ENDED);
		break;

	case di_ambersand:
		if (!fy_emit_whitespace(emit))
			fy_emit_write_ws(emit);
		fy_emit_putc(emit, wtype, '&');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_INDENTATION);
		break;

	case di_star:
		if (!fy_emit_whitespace(emit))
			fy_emit_write_ws(emit);
		fy_emit_putc(emit, wtype, '*');
		emit->flags &= ~(FYEF_WHITESPACE | FYEF_INDENTATION);
		break;
	}
}

int fy_emit_increase_indent(struct fy_emitter *emit, int flags, int indent)
{
	if (indent < 0)
		return (flags & DDNF_FLOW) ? fy_emit_indent(emit) : 0;

	if (!(flags & DDNF_INDENTLESS))
		return indent + fy_emit_indent(emit);

	return indent;
}

void fy_emit_write_comment(struct fy_emitter *emit, int flags, int indent, const char *str, size_t len)
{
	const char *s, *e, *sr;
	int c, w;
	bool breaks;

	if (!str || !len)
		return;

	if (len == (size_t)-1)
		len = strlen(str);

	if (!fy_emit_whitespace(emit))
		fy_emit_write_ws(emit);
	indent = emit->column;

	s = str;
	e = str + len;

	sr = s;	/* start of normal output run */
	breaks = false;
	while (s < e && (c = fy_utf8_get(s, e - s, &w)) > 0) {

		if (fy_is_break(c)) {

			/* output run */
			fy_emit_write(emit, fyewt_comment, sr, s - sr);
			sr = s + w;
			fy_emit_write_indent(emit, indent);
			emit->flags |= FYEF_INDENTATION;
			breaks = true;
		} else {

			if (breaks) {
				fy_emit_write(emit, fyewt_comment, sr, s - sr);
				sr = s;
				fy_emit_write_indent(emit, indent);
			}
			emit->flags &= ~FYEF_INDENTATION;
			breaks = false;
		}

		s += w;
	}

	/* dump what's remaining */
	fy_emit_write(emit, fyewt_comment, sr, s - sr);

	emit->flags |= (FYEF_WHITESPACE | FYEF_INDENTATION);
}

struct fy_atom *fy_emit_token_comment_handle(struct fy_emitter *emit, struct fy_token *fyt, enum fy_comment_placement placement)
{
	struct fy_atom *handle;

	if (!fyt)
		return NULL;

	handle = &fyt->comment[placement];
	return fy_atom_is_set(handle) ? handle : NULL;
}

struct fy_token *fy_node_value_token(struct fy_node *fyn)
{
	struct fy_token *fyt;

	if (!fyn)
		return NULL;

	switch (fyn->type) {
	case FYNT_SCALAR:
		fyt = fyn->scalar;
		break;
	case FYNT_SEQUENCE:
		fyt = fyn->sequence_start;
		break;
	case FYNT_MAPPING:
		fyt = fyn->mapping_start;
		break;
	default:
		fyt = NULL;
		break;
	}

	return fyt;
}

bool fy_emit_token_has_comment(struct fy_emitter *emit, struct fy_token *fyt, enum fy_comment_placement placement)
{
	return fy_emit_token_comment_handle(emit, fyt, placement) ? true : false;
}

bool fy_emit_node_has_comment(struct fy_emitter *emit, struct fy_node *fyn, enum fy_comment_placement placement)
{
	return fy_emit_token_has_comment(emit, fy_node_value_token(fyn), placement);
}

void fy_emit_token_comment(struct fy_emitter *emit, struct fy_token *fyt, int flags, int indent,
			  enum fy_comment_placement placement)
{
	struct fy_atom *handle;

	handle = fy_emit_token_comment_handle(emit, fyt, placement);
	if (!handle)
		return;

	if (placement == fycp_top || placement == fycp_bottom) {
		fy_emit_write_indent(emit, indent);
		emit->flags |= FYEF_WHITESPACE;
	}

	fy_emit_write_comment(emit, flags, indent, fy_atom_get_text_a(handle), -1);

	emit->flags &= ~FYEF_INDENTATION;

	if (placement == fycp_top || placement == fycp_bottom) {
		fy_emit_write_indent(emit, indent);
		emit->flags |= FYEF_WHITESPACE;
	}
}

void fy_emit_node_comment(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent,
			  enum fy_comment_placement placement)
{
	struct fy_token *fyt;

	if (!fy_emit_output_comments(emit) || (unsigned int)placement >= fycp_max)
		return;

	fyt = fy_node_value_token(fyn);
	if (!fyt)
		return;

	fy_emit_token_comment(emit, fyt, flags, indent, placement);
}

void fy_emit_common_node_preamble(struct fy_emitter *emit,
		struct fy_token *fyt_anchor,
		struct fy_token *fyt_tag,
		int flags, int indent)
{
	const char *anchor = NULL;
	const char *tag = NULL;
	const char *td_prefix __FY_DEBUG_UNUSED__;
	const char *td_handle;
	size_t td_prefix_size, td_handle_size;
	size_t tag_len = 0, anchor_len = 0;
	bool json_mode = false;

	json_mode = fy_emit_is_json_mode(emit);

	if (!json_mode) {
		if (!(emit->cfg->flags & FYECF_STRIP_LABELS)) {
			if (fyt_anchor)
				anchor = fy_token_get_text(fyt_anchor, &anchor_len);
		}

		if (!(emit->cfg->flags & FYECF_STRIP_TAGS)) {
			if (fyt_tag)
				tag = fy_token_get_text(fyt_tag, &tag_len);
		}

		if (anchor) {
			fy_emit_write_indicator(emit, di_ambersand, flags, indent, fyewt_anchor);
			fy_emit_write(emit, fyewt_anchor, anchor, anchor_len);
		}

		if (tag) {
			if (!fy_emit_whitespace(emit))
				fy_emit_write_ws(emit);

			td_handle = fy_tag_token_get_directive_handle(fyt_tag, &td_handle_size);
			assert(td_handle);
			td_prefix = fy_tag_token_get_directive_prefix(fyt_tag, &td_prefix_size);
			assert(td_prefix);

			if (!td_handle_size)
				fy_emit_printf(emit, fyewt_tag, "!<%.*s>", (int)tag_len, tag);
			else
				fy_emit_printf(emit, fyewt_tag, "%.*s%.*s",
						(int)td_handle_size, td_handle,
						(int)(tag_len - td_prefix_size), tag + td_prefix_size);

			emit->flags &= ~(FYEF_WHITESPACE | FYEF_INDENTATION);
		}
	}

	/* content for root always starts on a new line */
	if ((flags & DDNF_ROOT) && emit->column != 0 &&
            !(emit->flags & FYEF_HAD_DOCUMENT_START)) {
		fy_emit_putc(emit, fyewt_linebreak, '\n');
		emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
	}
}

void fy_emit_node_internal(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent)
{
	enum fy_node_type type;
	struct fy_anchor *fya;
	struct fy_token *fyt_anchor = NULL;

	if (!(emit->cfg->flags & FYECF_STRIP_LABELS)) {
		fya = fy_document_lookup_anchor_by_node(emit->fyd, fyn);
		if (fya)
			fyt_anchor = fya->anchor;
	}

	fy_emit_common_node_preamble(emit, fyt_anchor, fyn->tag, flags, indent);

	type = fyn ? fyn->type : FYNT_SCALAR;

	if (type != FYNT_SCALAR && (flags & DDNF_ROOT) && emit->column != 0) {
		fy_emit_putc(emit, fyewt_linebreak, '\n');
		emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
	}
	switch (type) {
	case FYNT_SCALAR:
		fy_emit_scalar(emit, fyn, flags, indent);
		break;
	case FYNT_SEQUENCE:
		fy_emit_sequence(emit, fyn, flags, indent);
		break;
	case FYNT_MAPPING:
		fy_emit_mapping(emit, fyn, flags, indent);
		break;
	}
}

void fy_emit_token_write_plain(struct fy_emitter *emit, struct fy_token *fyt, int flags, int indent)
{
	bool allow_breaks, should_indent, spaces, breaks;
	int c;
	enum fy_emitter_write_type wtype;
	const char *str = NULL;
	size_t len = 0;
	struct fy_atom *atom;
	struct fy_atom_iter iter;

	if (!fyt)
		goto out;

	wtype = (flags & DDNF_SIMPLE_SCALAR_KEY) ? fyewt_plain_scalar_key : fyewt_plain_scalar;

	/* simple case first (90% of cases) */
	str = fy_token_get_direct_output(fyt, &len);
	if (str) {
		fy_emit_write(emit, wtype, str, len);
		goto out;
	}

	atom = fy_token_atom(fyt);
	if (!atom)
		goto out;

	allow_breaks = !(flags & DDNF_SIMPLE) && !fy_emit_is_json_mode(emit) && !fy_emit_is_oneline(emit);

	spaces = false;
	breaks = false;

	fy_atom_iter_start(atom, &iter);
	fy_emit_accum_start(&emit->ea, wtype);
	while ((c = fy_atom_iter_utf8_get(&iter)) > 0) {

		if (fy_is_ws(c)) {

			should_indent = allow_breaks && !spaces &&
					fy_emit_accum_column(&emit->ea) > fy_emit_width(emit);

			if (should_indent && !fy_is_ws(fy_atom_iter_utf8_peek(&iter))) {
				fy_emit_accum_output(&emit->ea);
				emit->flags &= ~FYEF_INDENTATION;
				fy_emit_write_indent(emit, indent);
			} else
				fy_emit_accum_utf8_put(&emit->ea, c);

			spaces = true;

		} else if (fy_is_lb(c)) {

			/* blergh */
			if (!allow_breaks)
				break;

			/* output run */
			if (!breaks) {
				fy_emit_accum_output(&emit->ea);
				fy_emit_write_indent(emit, indent);
			}

			emit->flags &= ~FYEF_INDENTATION;
			fy_emit_write_indent(emit, indent);

			breaks = true;

		} else {

			if (breaks)
				fy_emit_write_indent(emit, indent);

			fy_emit_accum_utf8_put(&emit->ea, c);

			emit->flags &= ~FYEF_INDENTATION;

			spaces = false;
			breaks = false;
		}
	}
	fy_emit_accum_output(&emit->ea);
	fy_emit_accum_finish(&emit->ea);
	fy_atom_iter_finish(&iter);

out:
	emit->flags &= ~(FYEF_WHITESPACE | FYEF_INDENTATION);
}

void fy_emit_token_write_alias(struct fy_emitter *emit, struct fy_token *fyt, int flags, int indent)
{
	const char *str = NULL;
	size_t len = 0;
	struct fy_atom_iter iter;
	int c;

	if (!fyt)
		return;

	fy_emit_write_indicator(emit, di_star, flags, indent, fyewt_alias);

	/* try direct output first (99% of cases) */
	str = fy_token_get_direct_output(fyt, &len);
	if (str) {
		fy_emit_write(emit, fyewt_alias, str, len);
		return;
	}

	/* corner case, use iterator */
	fy_atom_iter_start(fy_token_atom(fyt), &iter);
	fy_emit_accum_start(&emit->ea, fyewt_alias);
	while ((c = fy_atom_iter_utf8_get(&iter)) > 0)
		fy_emit_accum_utf8_put(&emit->ea, c);
	fy_emit_accum_output(&emit->ea);
	fy_emit_accum_finish(&emit->ea);
	fy_atom_iter_finish(&iter);
}

void fy_emit_token_write_quoted(struct fy_emitter *emit, struct fy_token *fyt, int flags, int indent, char qc)
{
	bool allow_breaks, spaces, breaks;
	int c, i, w, digit;
	enum fy_emitter_write_type wtype;
	const char *str = NULL;
	size_t len = 0;
	bool should_indent;
	struct fy_atom *atom;
	struct fy_atom_iter iter;

	wtype = qc == '\'' ?
		((flags & DDNF_SIMPLE_SCALAR_KEY) ?
		 	fyewt_single_quoted_scalar_key : fyewt_single_quoted_scalar) :
		((flags & DDNF_SIMPLE_SCALAR_KEY) ?
		 	fyewt_double_quoted_scalar_key : fyewt_double_quoted_scalar);

	fy_emit_write_indicator(emit,
			qc == '\'' ? di_single_quote_start : di_double_quote_start,
			flags, indent, wtype);

	/* XXX check whether this is ever the case */
	if (!fyt)
		goto out;

	/* simple case of direct output (large amount of cases) */
	str = fy_token_get_direct_output(fyt, &len);
	if (str) {
		fy_emit_write(emit, wtype, str, len);
		goto out;
	}

	/* no atom? i.e. empty */
	atom = fy_token_atom(fyt);
	if (!atom)
		goto out;

	allow_breaks = !(flags & DDNF_SIMPLE) && !fy_emit_is_json_mode(emit) && !fy_emit_is_oneline(emit);

	spaces = false;
	breaks = false;

	fy_atom_iter_start(atom, &iter);
	fy_emit_accum_start(&emit->ea, wtype);
	while ((c = fy_atom_iter_utf8_get(&iter)) >= 0) {
		if (fy_is_ws(c)) {
			should_indent = allow_breaks && !spaces &&
					fy_emit_accum_column(&emit->ea) > fy_emit_width(emit);

			if (should_indent &&
				((qc == '\'' && fy_is_ws(fy_atom_iter_utf8_peek(&iter))) ||
				  qc == '"')) {
				fy_emit_accum_output(&emit->ea);

				if (qc == '"' && fy_is_ws(fy_atom_iter_utf8_peek(&iter)))
					fy_emit_putc(emit, wtype, '\\');

				emit->flags &= ~FYEF_INDENTATION;
				fy_emit_write_indent(emit, indent);
			} else
				fy_emit_accum_utf8_put(&emit->ea, c);

			spaces = true;
			breaks = false;

		} else if (qc == '\'' && fy_is_lb(c)) {

			/* blergh */
			if (!allow_breaks)
				break;

			/* output run */
			if (!breaks) {
				fy_emit_accum_output(&emit->ea);
				fy_emit_write_indent(emit, indent);
			}

			emit->flags &= ~FYEF_INDENTATION;
			fy_emit_write_indent(emit, indent);

			breaks = true;
		} else {
			/* output run */
			if (breaks) {
				fy_emit_accum_output(&emit->ea);
				fy_emit_write_indent(emit, indent);
			}

			/* escape */
			if (qc == '\'' && c == '\'') {
				fy_emit_accum_utf8_put(&emit->ea, '\'');
				fy_emit_accum_utf8_put(&emit->ea, '\'');
			} else if (qc == '"' &&
				   (!fy_is_print(c) || c == FY_UTF8_BOM ||
				    fy_is_break(c) || c == '"' || c == '\\')) {

				fy_emit_accum_utf8_put(&emit->ea, '\\');
				switch (c) {
				case '\0':
					fy_emit_accum_utf8_put(&emit->ea, '0');
					break;
				case '\a':
					fy_emit_accum_utf8_put(&emit->ea, 'a');
					break;
				case '\b':
					fy_emit_accum_utf8_put(&emit->ea, 'b');
					break;
				case '\t':
					fy_emit_accum_utf8_put(&emit->ea, 't');
					break;
				case '\n':
					fy_emit_accum_utf8_put(&emit->ea, 'n');
					break;
				case '\v':
					fy_emit_accum_utf8_put(&emit->ea, 'v');
					break;
				case '\f':
					fy_emit_accum_utf8_put(&emit->ea, 'f');
					break;
				case '\r':
					fy_emit_accum_utf8_put(&emit->ea, 'r');
					break;
				case '\e':
					fy_emit_accum_utf8_put(&emit->ea, 'e');
					break;
				case '"':
					fy_emit_accum_utf8_put(&emit->ea, '"');
					break;
				case '\\':
					fy_emit_accum_utf8_put(&emit->ea, '\\');
					break;
				case 0x85:
					fy_emit_accum_utf8_put(&emit->ea, 'N');
					break;
				case 0xa0:
					fy_emit_accum_utf8_put(&emit->ea, '_');
					break;
				case 0x2028:
					fy_emit_accum_utf8_put(&emit->ea, 'L');
					break;
				case 0x2029:
					fy_emit_accum_utf8_put(&emit->ea, 'P');
					break;
				default:
					if ((unsigned int)c <= 0xff) {
						fy_emit_accum_utf8_put(&emit->ea, 'x');
						w = 2;
					} else if ((unsigned int)c <= 0xffff) {
						fy_emit_accum_utf8_put(&emit->ea, 'u');
						w = 4;
					} else if ((unsigned int)c <= 0xffffffff) {
						fy_emit_accum_utf8_put(&emit->ea, 'U');
						w = 8;
					}

					for (i = w - 1; i >= 0; i--) {
						digit = ((unsigned int)c >> (i * 4)) & 15;
						fy_emit_accum_utf8_put(&emit->ea,
								digit <= 9 ? ('0' + digit) : ('A' + digit - 10));
					}
					break;
				}
			} else
				fy_emit_accum_utf8_put(&emit->ea, c);

			emit->flags &= ~FYEF_INDENTATION;
			spaces = false;
			breaks = false;
		}
	}
	fy_emit_accum_output(&emit->ea);
	fy_emit_accum_finish(&emit->ea);
	fy_atom_iter_finish(&iter);

out:
	fy_emit_write_indicator(emit,
			qc == '\'' ? di_single_quote_end : di_double_quote_end,
			flags, indent, wtype);
}

bool fy_emit_token_write_block_hints(struct fy_emitter *emit, struct fy_token *fyt, int flags, int indent, char *chompp)
{
	char chomp = '\0';
	bool explicit_chomp = false;
	struct fy_atom *atom;

	atom = fy_token_atom(fyt);
	if (!atom) {
		emit->flags &= ~FYEF_OPEN_ENDED;
		chomp = '-';
		goto out;
	}

	if (atom->starts_with_ws || atom->starts_with_lb) {
		fy_emit_putc(emit, fyewt_indicator, '0' + fy_emit_indent(emit));
		explicit_chomp = true;
	}

	if (!atom->ends_with_lb) {
		emit->flags &= ~FYEF_OPEN_ENDED;
		chomp = '-';
		goto out;
	}

	if (atom->trailing_lb) {
		emit->flags |= FYEF_OPEN_ENDED;
		chomp = '+';
		goto out;
	}
	emit->flags &= ~FYEF_OPEN_ENDED;

out:
	if (chomp)
		fy_emit_putc(emit, fyewt_indicator, chomp);
	*chompp = chomp;
	return explicit_chomp;
}

void fy_emit_token_write_literal(struct fy_emitter *emit, struct fy_token *fyt, int flags, int indent)
{
	bool breaks;
	int c;
	char chomp;
	struct fy_atom *atom;
	struct fy_atom_iter iter;

	fy_emit_write_indicator(emit, di_bar, flags, indent, fyewt_indicator);

	fy_emit_token_write_block_hints(emit, fyt, flags, indent, &chomp);
	if (flags & DDNF_ROOT)
		indent += fy_emit_indent(emit);

	fy_emit_putc(emit, fyewt_linebreak, '\n');
	emit->flags |= FYEF_WHITESPACE | FYEF_INDENTATION;

	atom = fy_token_atom(fyt);
	if (!atom)
		goto out;

	breaks = true;

	fy_atom_iter_start(atom, &iter);
	fy_emit_accum_start(&emit->ea, fyewt_literal_scalar);
	while ((c = fy_atom_iter_utf8_get(&iter)) > 0) {

		if (breaks) {
			fy_emit_write_indent(emit, indent);
			breaks = false;
		}

		if (fy_is_break(c)) {
			fy_emit_accum_output(&emit->ea);
			emit->flags &= ~FYEF_INDENTATION;
			breaks = true;
		} else
			fy_emit_accum_utf8_put(&emit->ea, c);
	}
	fy_emit_accum_output(&emit->ea);
	fy_emit_accum_finish(&emit->ea);
	fy_atom_iter_finish(&iter);

out:
	emit->flags &= ~FYEF_INDENTATION;
}

void fy_emit_token_write_folded(struct fy_emitter *emit, struct fy_token *fyt, int flags, int indent)
{
	bool leading_spaces, breaks;
	int c, nrbreaks, nrbreakslim;
	char chomp;
	struct fy_atom *atom;
	struct fy_atom_iter iter;

	fy_emit_write_indicator(emit, di_greater, flags, indent, fyewt_indicator);

	fy_emit_token_write_block_hints(emit, fyt, flags, indent, &chomp);
	if (flags & DDNF_ROOT)
		indent += fy_emit_indent(emit);

	fy_emit_putc(emit, fyewt_linebreak, '\n');
	emit->flags |= FYEF_WHITESPACE | FYEF_INDENTATION;

	atom = fy_token_atom(fyt);
	if (!atom)
		return;

	breaks = true;
	leading_spaces = true;

	fy_atom_iter_start(atom, &iter);
	fy_emit_accum_start(&emit->ea, fyewt_folded_scalar);
	while ((c = fy_atom_iter_utf8_get(&iter)) > 0) {

		if (fy_is_break(c)) {

			/* output run */
			if (fy_emit_accum_utf8_size(&emit->ea)) {
				fy_emit_accum_output(&emit->ea);
				/* do not output a newline (indent) if at the end or
				 * this is a leading spaces line */
				if (!fy_is_z(fy_atom_iter_utf8_peek(&iter)) && !leading_spaces)
					fy_emit_write_indent(emit, indent);
			}

			/* count the number of consecutive breaks */
			nrbreaks = 1;
			while (fy_is_break(c = fy_atom_iter_utf8_peek(&iter))) {
				nrbreaks++;
				(void)fy_atom_iter_utf8_get(&iter);
			}

			/* NOTE: Because the number of indents is tricky
			 * if it's a non blank, non end, it's the number of breaks
			 * if it's a blank, it's the number of breaks minus 1
			 * if it's the end, it's the number of breaks minus 2
			 */
			nrbreakslim = fy_is_z(c) ? 2 : fy_is_blank(c) ? 1 : 0;
			while (nrbreaks-- > nrbreakslim) {
				emit->flags &= ~FYEF_INDENTATION;
				fy_emit_write_indent(emit, indent);
			}

			breaks = true;

		} else {

			/* if we had a break, output an indent */
			if (breaks) {
				fy_emit_write_indent(emit, indent);

				/* if this line starts with whitespace we need to know */
				leading_spaces = fy_is_ws(c);
			}

			if (!breaks && fy_is_space(c) &&
			    !fy_is_space(fy_atom_iter_utf8_peek(&iter)) &&
			    fy_emit_accum_column(&emit->ea) > fy_emit_width(emit)) {
				fy_emit_accum_output(&emit->ea);
				emit->flags &= ~FYEF_INDENTATION;
				fy_emit_write_indent(emit, indent);
			} else
				fy_emit_accum_utf8_put(&emit->ea, c);

			breaks = false;
		}
	}
	fy_emit_accum_output(&emit->ea);
	fy_emit_accum_finish(&emit->ea);
	fy_atom_iter_finish(&iter);
}

static enum fy_node_style
fy_emit_token_scalar_style(struct fy_emitter *emit, struct fy_token *fyt,
			   int flags, enum fy_node_style style)
{
	const char *value = NULL;
	size_t len = 0;
	bool json, flow;
	struct fy_atom *atom;

	atom = fy_token_atom(fyt);

	/* check if style is allowed (i.e. no block styles in flow context) */
	if ((flags & DDNF_FLOW) && (style == FYNS_LITERAL || style == FYNS_FOLDED))
		style = FYNS_ANY;

	json = fy_emit_is_json_mode(emit);

	/* literal in JSON mode is output as quoted */
	if (json && (style == FYNS_LITERAL || style == FYNS_FOLDED)) {
		style = FYNS_DOUBLE_QUOTED;
		goto out;
	}

	if (json && style == FYNS_PLAIN &&
		(!atom ||
		 atom->size0 ||
		 !fy_atom_strcmp(atom, "false") ||
		 !fy_atom_strcmp(atom, "true") ||
		 !fy_atom_strcmp(atom, "null") ||
		 fy_atom_is_number(atom))) {

		style = FYNS_PLAIN;
		goto out;
	}

	if (json) {
		style = FYNS_DOUBLE_QUOTED;
		goto out;
	}

	flow = fy_emit_is_flow_mode(emit);

	/* in flow mode, we can't let a bare plain */
	if (flow && (!fyt || fy_token_get_text_length(fyt) == 0))
		style = FYNS_DOUBLE_QUOTED;

	if (flow && (style == FYNS_ANY || style == FYNS_LITERAL || style == FYNS_FOLDED)) {

		if (fyt && !value)
			value = fy_token_get_text(fyt, &len);

		/* if there's a linebreak, use double quoted style */
		if (fy_find_lb(value, len)) {
			style = FYNS_DOUBLE_QUOTED;
			goto out;
		}

		/* check if there's a non printable */
		if (!fy_find_non_print(value, len)) {
			style = FYNS_SINGLE_QUOTED;
			goto out;
		}

		style = FYNS_DOUBLE_QUOTED;
	}

out:
	if (style == FYNS_ANY) {
		if (fyt)
			value = fy_token_get_text(fyt, &len);

		style = (fy_token_text_analyze(fyt) & FYTTAF_DIRECT_OUTPUT) ?
				FYNS_PLAIN : FYNS_DOUBLE_QUOTED;
	}

	return style;
}

void fy_emit_token_scalar(struct fy_emitter *emit, struct fy_token *fyt, int flags, int indent, enum fy_node_style style)
{
	assert(style != FYNS_FLOW && style != FYNS_BLOCK);

	indent = fy_emit_increase_indent(emit, flags, indent);

	if (!fy_emit_whitespace(emit))
		fy_emit_write_ws(emit);

	style = fy_emit_token_scalar_style(emit, fyt, flags, style);

	switch (style) {
	case FYNS_ALIAS:
		fy_emit_token_write_alias(emit, fyt, flags, indent);
		break;
	case FYNS_PLAIN:
		fy_emit_token_write_plain(emit, fyt, flags, indent);
		break;
	case FYNS_DOUBLE_QUOTED:
		fy_emit_token_write_quoted(emit, fyt, flags, indent, '"');
		break;
	case FYNS_SINGLE_QUOTED:
		fy_emit_token_write_quoted(emit, fyt, flags, indent, '\'');
		break;
	case FYNS_LITERAL:
		fy_emit_token_write_literal(emit, fyt, flags, indent);
		break;
	case FYNS_FOLDED:
		fy_emit_token_write_folded(emit, fyt, flags, indent);
		break;
	default:
		break;
	}
}

void fy_emit_scalar(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent)
{
	fy_emit_token_scalar(emit,
			fyn ? fyn->scalar : NULL,
			flags, indent,
			fyn ? fyn->style : FYNS_ANY);
}

static void fy_emit_sequence_prolog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc)
{
	bool json = fy_emit_is_json_mode(emit);
	bool oneline = fy_emit_is_oneline(emit);

	sc->old_indent = sc->indent;
	if (!json) {
		if (fy_emit_is_flow_mode(emit))
			sc->flow = true;
		else if (fy_emit_is_block_mode(emit))
			sc->flow = false;
		else
			sc->flow = emit->flow_level || sc->flow_token || sc->empty;

		if (sc->flow) {
			if (!emit->flow_level) {
				sc->indent = fy_emit_increase_indent(emit, sc->flags, sc->indent);
				sc->old_indent = sc->indent;
			}

			sc->flags = (sc->flags | DDNF_FLOW) | (sc->flags & ~DDNF_INDENTLESS);
			fy_emit_write_indicator(emit, di_left_bracket, sc->flags, sc->indent, fyewt_indicator);
		} else {
			sc->flags = (sc->flags & ~DDNF_FLOW) | ((sc->flags & DDNF_MAP) ? DDNF_INDENTLESS : 0);
		}
	} else {
		sc->flags = (sc->flags | DDNF_FLOW) | (sc->flags & ~DDNF_INDENTLESS);
		fy_emit_write_indicator(emit, di_left_bracket, sc->flags, sc->indent, fyewt_indicator);
	}

	if (!oneline)
		sc->indent = fy_emit_increase_indent(emit, sc->flags, sc->indent);

	sc->flags &= ~DDNF_ROOT;
}

static void fy_emit_sequence_epilog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc)
{
	if (sc->flow || fy_emit_is_json_mode(emit)) {
		if (!fy_emit_is_oneline(emit) && !sc->empty)
			fy_emit_write_indent(emit, sc->old_indent);
		fy_emit_write_indicator(emit, di_right_bracket, sc->flags, sc->old_indent, fyewt_indicator);
	}
}

static void fy_emit_sequence_item_prolog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc,
					 struct fy_token *fyt_value)
{
	int tmp_indent;

	sc->flags |= DDNF_SEQ;

	if (!fy_emit_is_oneline(emit))
		fy_emit_write_indent(emit, sc->indent);

	if (!sc->flow && !fy_emit_is_json_mode(emit))
		fy_emit_write_indicator(emit, di_dash, sc->flags, sc->indent, fyewt_indicator);

	tmp_indent = sc->indent;
	if (fy_emit_token_has_comment(emit, fyt_value, fycp_top)) {
		if (!sc->flow && !fy_emit_is_json_mode(emit))
			tmp_indent = fy_emit_increase_indent(emit, sc->flags, sc->indent);
		fy_emit_token_comment(emit, fyt_value, sc->flags, tmp_indent, fycp_top);
	}
}

static void fy_emit_sequence_item_epilog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc,
					 bool last, struct fy_token *fyt_value)
{
	if ((sc->flow || fy_emit_is_json_mode(emit)) && !last)
		fy_emit_write_indicator(emit, di_comma, sc->flags, sc->indent, fyewt_indicator);

	fy_emit_token_comment(emit, fyt_value, sc->flags, sc->indent, fycp_right);

	if (last && (sc->flow || fy_emit_is_json_mode(emit)) && !fy_emit_is_oneline(emit) && !sc->empty)
		fy_emit_write_indent(emit, sc->old_indent);

	sc->flags &= ~DDNF_SEQ;
}

void fy_emit_sequence(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent)
{
	struct fy_node *fyni, *fynin;
	struct fy_token *fyt_value;
	bool last;
	struct fy_emit_save_ctx sct, *sc = &sct;

	memset(sc, 0, sizeof(*sc));

	sc->flags = flags;
	sc->indent = indent;
	sc->empty = fy_node_list_empty(&fyn->sequence);
	sc->flow_token = fyn->style == FYNS_FLOW;
	sc->flow = false;
	sc->old_indent = sc->indent;

	fy_emit_sequence_prolog(emit, sc);

	for (fyni = fy_node_list_head(&fyn->sequence); fyni; fyni = fynin) {

		fynin = fy_node_next(&fyn->sequence, fyni);
		last = !fynin;
		fyt_value = fy_node_value_token(fyni);

		fy_emit_sequence_item_prolog(emit, sc, fyt_value);
		fy_emit_node_internal(emit, fyni, sc->flags, sc->indent);
		fy_emit_sequence_item_epilog(emit, sc, last, fyt_value);
	}

	fy_emit_sequence_epilog(emit, sc);
}

static void fy_emit_mapping_prolog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc)
{
	bool json = fy_emit_is_json_mode(emit);
	bool oneline = fy_emit_is_oneline(emit);

	sc->old_indent = sc->indent;
	if (!json) {
		if (fy_emit_is_flow_mode(emit))
			sc->flow = true;
		else if (fy_emit_is_block_mode(emit))
			sc->flow = false;
		else
			sc->flow = emit->flow_level || sc->flow_token || sc->empty;

		if (sc->flow) {
			if (!emit->flow_level) {
				sc->indent = fy_emit_increase_indent(emit, sc->flags, sc->indent);
				sc->old_indent = sc->indent;
			}

			sc->flags = (sc->flags | DDNF_FLOW) | (sc->flags & ~DDNF_INDENTLESS);
			fy_emit_write_indicator(emit, di_left_brace, sc->flags, sc->indent, fyewt_indicator);
		} else {
			sc->flags &= ~(DDNF_FLOW | DDNF_INDENTLESS);
		}
	} else {
		sc->flags = (sc->flags | DDNF_FLOW) | (sc->flags & ~DDNF_INDENTLESS);
		fy_emit_write_indicator(emit, di_left_brace, sc->flags, sc->indent, fyewt_indicator);
	}

	if (!oneline && !sc->empty)
		sc->indent = fy_emit_increase_indent(emit, sc->flags, sc->indent);

	sc->flags &= ~DDNF_ROOT;
}

static void fy_emit_mapping_epilog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc)
{
	if (sc->flow || fy_emit_is_json_mode(emit)) {
		if (!fy_emit_is_oneline(emit) && !sc->empty)
			fy_emit_write_indent(emit, sc->old_indent);
		fy_emit_write_indicator(emit, di_right_brace, sc->flags, sc->old_indent, fyewt_indicator);
	}
}

static void fy_emit_mapping_key_prolog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc,
				       struct fy_token *fyt_key, bool simple_key)
{
	sc->flags = DDNF_MAP;

	if (simple_key) {
		sc->flags |= DDNF_SIMPLE;
		if (fyt_key && fyt_key->type == FYTT_SCALAR)
			sc->flags |= DDNF_SIMPLE_SCALAR_KEY;
	}

	if (!fy_emit_is_oneline(emit))
		fy_emit_write_indent(emit, sc->indent);

	/* complex? */
	if (!(sc->flags & DDNF_SIMPLE))
		fy_emit_write_indicator(emit, di_question_mark, sc->flags, sc->indent, fyewt_indicator);
}

static void fy_emit_mapping_key_epilog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc,
				       struct fy_token *fyt_key)
{
	int tmp_indent;

	/* if the key is an alias, always output an extra whitespace */
	if (fyt_key && fyt_key->type == FYTT_ALIAS)
		fy_emit_write_ws(emit);

	sc->flags &= ~DDNF_MAP;

	fy_emit_write_indicator(emit, di_colon, sc->flags, sc->indent, fyewt_indicator);

	tmp_indent = sc->indent;
	if (fy_emit_token_has_comment(emit, fyt_key, fycp_right)) {

		if (!sc->flow && !fy_emit_is_json_mode(emit))
			tmp_indent = fy_emit_increase_indent(emit, sc->flags, sc->indent);

		fy_emit_token_comment(emit, fyt_key, sc->flags, tmp_indent, fycp_right);
		fy_emit_write_indent(emit, tmp_indent);
	}

	sc->flags = DDNF_MAP;
}

static void fy_emit_mapping_value_prolog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc,
					 struct fy_token *fyt_value)
{
	/* nothing */
}

static void fy_emit_mapping_value_epilog(struct fy_emitter *emit, struct fy_emit_save_ctx *sc,
					 bool last, struct fy_token *fyt_value)
{
	if ((sc->flow || fy_emit_is_json_mode(emit)) && !last)
		fy_emit_write_indicator(emit, di_comma, sc->flags, sc->indent, fyewt_indicator);

	fy_emit_token_comment(emit, fyt_value, sc->flags, sc->indent, fycp_right);

	if (last && (sc->flow || fy_emit_is_json_mode(emit)) && !fy_emit_is_oneline(emit) && !sc->empty)
		fy_emit_write_indent(emit, sc->old_indent);

	sc->flags &= ~DDNF_MAP;
}

void fy_emit_mapping(struct fy_emitter *emit, struct fy_node *fyn, int flags, int indent)
{
	struct fy_node_pair *fynp, *fynpn, **fynpp = NULL;
	struct fy_token *fyt_key, *fyt_value;
	bool last, simple_key;
	int aflags, i;
	struct fy_emit_save_ctx sct, *sc = &sct;

	memset(sc, 0, sizeof(*sc));

	sc->flags = flags;
	sc->indent = indent;
	sc->empty = fy_node_pair_list_empty(&fyn->mapping);
	sc->flow_token = fyn->style == FYNS_FLOW;
	sc->flow = false;
	sc->old_indent = sc->indent;

	fy_emit_mapping_prolog(emit, sc);

	if (!(emit->cfg->flags & FYECF_SORT_KEYS)) {
		fynp = fy_node_pair_list_head(&fyn->mapping);
		fynpp = NULL;
	} else {
		fynpp = fy_node_mapping_sort_array(fyn, NULL, NULL, NULL);
		i = 0;
		fynp = fynpp[i];
	}

	for (; fynp; fynp = fynpn) {

		if (!fynpp)
			fynpn = fy_node_pair_next(&fyn->mapping, fynp);
		else
			fynpn = fynpp[++i];

		last = !fynpn;
		fyt_key = fy_node_value_token(fynp->key);
		fyt_value = fy_node_value_token(fynp->value);

		simple_key = false;
		if (fynp->key) {
			switch (fynp->key->type) {
			case FYNT_SCALAR:
				aflags = fy_token_text_analyze(fynp->key->scalar);
				simple_key = !!(aflags & FYTTAF_CAN_BE_SIMPLE_KEY);
				break;
			case FYNT_SEQUENCE:
				simple_key = fy_node_list_empty(&fynp->key->sequence);
				break;
			case FYNT_MAPPING:
				simple_key = fy_node_pair_list_empty(&fynp->key->mapping);
				break;
			}
		}

		fy_emit_mapping_key_prolog(emit, sc, fyt_key, simple_key);
		if (fynp->key)
			fy_emit_node_internal(emit, fynp->key, sc->flags, sc->indent);
		fy_emit_mapping_key_epilog(emit, sc, fyt_key);

		fy_emit_mapping_value_prolog(emit, sc, fyt_value);
		if (fynp->value)
			fy_emit_node_internal(emit, fynp->value, sc->flags, sc->indent);
		fy_emit_mapping_value_epilog(emit, sc, last, fyt_value);
	}

	if (fynpp)
		fy_node_mapping_sort_release_array(fyn, fynpp);

	fy_emit_mapping_epilog(emit, sc);
}

int fy_emit_common_document_start(struct fy_emitter *emit,
				  struct fy_document_state *fyds,
				  bool root_tag_or_anchor)
{
	struct fy_token *fyt_chk;
	const char *td_handle, *td_prefix;
	size_t td_handle_size, td_prefix_size;
	enum fy_emitter_cfg_flags flags = emit->cfg->flags;
	enum fy_emitter_cfg_flags vd_flags = flags & FYECF_VERSION_DIR(FYECF_VERSION_DIR_MASK);
	enum fy_emitter_cfg_flags td_flags = flags & FYECF_TAG_DIR(FYECF_TAG_DIR_MASK);
	enum fy_emitter_cfg_flags dsm_flags = flags & FYECF_DOC_START_MARK(FYECF_DOC_START_MARK_MASK);
	bool vd, td, dsm;
	bool had_non_default_tag = false;

	if (!emit || !fyds || emit->fyds)
		return -1;

	emit->fyds = fyds;

	vd = ((vd_flags == FYECF_VERSION_DIR_AUTO && fyds->version_explicit) ||
	       vd_flags == FYECF_VERSION_DIR_ON) &&
	      !(emit->cfg->flags & FYECF_STRIP_DOC);
	td = ((td_flags == FYECF_TAG_DIR_AUTO && fyds->tags_explicit) ||
	       td_flags == FYECF_TAG_DIR_ON) &&
	      !(emit->cfg->flags & FYECF_STRIP_DOC);

	/* if either a version or directive tags exist, and no previous
	 * explicit document end existed, output one now
	 */
	if (!fy_emit_is_json_mode(emit) && (vd || td) && !(emit->flags & FYEF_HAD_DOCUMENT_END)) {
		if (emit->column)
			fy_emit_putc(emit, fyewt_linebreak, '\n');
		if (!(emit->cfg->flags & FYECF_STRIP_DOC)) {
			fy_emit_puts(emit, fyewt_document_indicator, "...");
			emit->flags &= ~FYEF_WHITESPACE;
			emit->flags |= FYEF_HAD_DOCUMENT_END;
		}
	}

	if (!fy_emit_is_json_mode(emit) && vd) {
		if (emit->column)
			fy_emit_putc(emit, fyewt_linebreak, '\n');
		fy_emit_printf(emit, fyewt_version_directive, "%%YAML %d.%d",
					fyds->version.major, fyds->version.minor);
		fy_emit_putc(emit, fyewt_linebreak, '\n');
		emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
	}

	if (!fy_emit_is_json_mode(emit) && td) {

		for (fyt_chk = fy_token_list_first(&fyds->fyt_td); fyt_chk; fyt_chk = fy_token_next(&fyds->fyt_td, fyt_chk)) {

			td_handle = fy_tag_directive_token_handle(fyt_chk, &td_handle_size);
			td_prefix = fy_tag_directive_token_prefix(fyt_chk, &td_prefix_size);
			assert(td_handle && td_prefix);

			if (fy_tag_is_default(td_handle, td_handle_size, td_prefix, td_prefix_size))
				continue;

			had_non_default_tag = true;

			if (emit->column)
				fy_emit_putc(emit, fyewt_linebreak, '\n');
			fy_emit_printf(emit, fyewt_tag_directive, "%%TAG %.*s %.*s",
					(int)td_handle_size, td_handle,
					(int)td_prefix_size, td_prefix);
			fy_emit_putc(emit, fyewt_linebreak, '\n');
			emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
		}
	}

	/* always output document start indicator:
	 * - was explicit
	 * - document has tags
	 * - document has an explicit version
	 * - root exists & has a tag or an anchor
	 */
	dsm = (dsm_flags == FYECF_DOC_START_MARK_AUTO &&
			(!fyds->start_implicit ||
			  fyds->tags_explicit || fyds->version_explicit ||
			  had_non_default_tag)) ||
	       dsm_flags == FYECF_DOC_START_MARK_ON;

	/* if there was previous output without document end */
	if (!dsm && (emit->flags & FYEF_HAD_DOCUMENT_OUTPUT) &&
	           !(emit->flags & FYEF_HAD_DOCUMENT_END))
		dsm = true;

	if (!fy_emit_is_json_mode(emit) && dsm) {
		if (emit->column)
			fy_emit_putc(emit, fyewt_linebreak, '\n');
		if (!(emit->cfg->flags & FYECF_STRIP_DOC)) {
			fy_emit_puts(emit, fyewt_document_indicator, "---");
			emit->flags &= ~FYEF_WHITESPACE;
			emit->flags |= FYEF_HAD_DOCUMENT_START;
		}
	} else
		emit->flags &= ~FYEF_HAD_DOCUMENT_START;

	/* clear that in any case */
	emit->flags &= ~FYEF_HAD_DOCUMENT_END;

	return 0;
}

int fy_emit_document_start(struct fy_emitter *emit, struct fy_document *fyd,
			   struct fy_node *fyn_root)
{
	struct fy_node *root;
	bool root_tag_or_anchor;
	int ret;

	if (!emit || !fyd || !fyd->fyds)
		return -1;

	root = fyn_root ? : fy_document_root(fyd);

	root_tag_or_anchor = root && (root->tag || fy_document_lookup_anchor_by_node(fyd, root));

	ret = fy_emit_common_document_start(emit, fyd->fyds, root_tag_or_anchor);
	if (ret)
		return ret;

	emit->fyd = fyd;

	return 0;
}

int fy_emit_common_document_end(struct fy_emitter *emit)
{
	const struct fy_document_state *fyds;
	enum fy_emitter_cfg_flags flags = emit->cfg->flags;
	enum fy_emitter_cfg_flags dem_flags = flags & FYECF_DOC_END_MARK(FYECF_DOC_END_MARK_MASK);
	bool dem;

	if (!emit || !emit->fyds)
		return -1;

	fyds = emit->fyds;

	if (emit->column != 0) {
		fy_emit_putc(emit, fyewt_linebreak, '\n');
		emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
	}

	dem = ((dem_flags == FYECF_DOC_END_MARK_AUTO && !fyds->end_implicit) ||
	        dem_flags == FYECF_DOC_END_MARK_ON) &&
	       !(emit->cfg->flags & FYECF_STRIP_DOC);
	if (!fy_emit_is_json_mode(emit) && dem) {
		fy_emit_puts(emit, fyewt_document_indicator, "...");
		fy_emit_putc(emit, fyewt_linebreak, '\n');
		emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
		emit->flags |= FYEF_HAD_DOCUMENT_END;
	} else
		emit->flags &= ~FYEF_HAD_DOCUMENT_END;

	/* mark that we did output a document earlier */
	emit->flags |= FYEF_HAD_DOCUMENT_OUTPUT;

	/* stop our association with the document */
	emit->fyds = NULL;

	return 0;
}

int fy_emit_document_end(struct fy_emitter *emit)
{
	int ret;

	ret = fy_emit_common_document_end(emit);
	if (ret)
		return ret;

	emit->fyd = NULL;
	return 0;
}

int fy_emit_common_explicit_document_end(struct fy_emitter *emit)
{
	if (!emit)
		return -1;

	if (emit->column != 0) {
		fy_emit_putc(emit, fyewt_linebreak, '\n');
		emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
	}

	if (!fy_emit_is_json_mode(emit)) {
		fy_emit_puts(emit, fyewt_document_indicator, "...");
		fy_emit_putc(emit, fyewt_linebreak, '\n');
		emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
		emit->flags |= FYEF_HAD_DOCUMENT_END;
	} else
		emit->flags &= ~FYEF_HAD_DOCUMENT_END;

	/* mark that we did output a document earlier */
	emit->flags |= FYEF_HAD_DOCUMENT_OUTPUT;

	/* stop our association with the document */
	emit->fyds = NULL;

	return 0;
}

int fy_emit_explicit_document_end(struct fy_emitter *emit)
{
	int ret;

	ret = fy_emit_common_explicit_document_end(emit);
	if (ret)
		return ret;

	emit->fyd = NULL;
	return 0;
}

void fy_emit_reset(struct fy_emitter *emit, bool reset_events)
{
	struct fy_eventp *fyep;

	emit->line = 0;
	emit->column = 0;
	emit->flow_level = 0;
	emit->output_error = 0;
	/* start as if there was a previous document with an explicit end */
	/* this allows implicit documents start without an indicator */
	emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION | FYEF_HAD_DOCUMENT_END;

	emit->state = FYES_NONE;

	/* reset the accumulator */
	fy_emit_accum_reset(&emit->ea);

	/* streaming mode indent */
	emit->s_indent = -1;
	/* streaming mode flags */
	emit->s_flags = DDNF_ROOT;

	emit->state_stack_top = 0;
	emit->sc_stack_top = 0;

	/* and release any queued events */
	if (reset_events) {
		while ((fyep = fy_eventp_list_pop(&emit->queued_events)) != NULL)
			fy_eventp_release(fyep);
	}
}

void fy_emit_setup(struct fy_emitter *emit, const struct fy_emitter_cfg *cfg)
{
	memset(emit, 0, sizeof(*emit));

	emit->cfg = cfg;
	fy_emit_accum_init(&emit->ea, emit);
	fy_eventp_list_init(&emit->queued_events);

	emit->state_stack = emit->state_stack_inplace;
	emit->state_stack_alloc = sizeof(emit->state_stack_inplace)/sizeof(emit->state_stack_inplace[0]);

	emit->sc_stack = emit->sc_stack_inplace;
	emit->sc_stack_alloc = sizeof(emit->sc_stack_inplace)/sizeof(emit->sc_stack_inplace[0]);

	fy_emit_reset(emit, false);
}

void fy_emit_cleanup(struct fy_emitter *emit)
{
	struct fy_eventp *fyep;

	if (!emit->fyd && emit->fyds)
		fy_document_state_unref(emit->fyds);

	fy_emit_accum_cleanup(&emit->ea);

	while ((fyep = fy_eventp_list_pop(&emit->queued_events)) != NULL)
		fy_eventp_release(fyep);

	if (emit->state_stack && emit->state_stack != emit->state_stack_inplace)
		free(emit->state_stack);

	if (emit->sc_stack && emit->sc_stack != emit->sc_stack_inplace)
		free(emit->sc_stack);
}

int fy_emit_node(struct fy_emitter *emit, struct fy_node *fyn)
{
	if (fyn)
		fy_emit_node_internal(emit, fyn, DDNF_ROOT, -1);
	return 0;
}

int fy_emit_root_node(struct fy_emitter *emit, struct fy_node *fyn)
{
	if (!emit || !fyn)
		return -1;

	/* top comment first */
	fy_emit_node_comment(emit, fyn, DDNF_ROOT, -1, fycp_top);

	fy_emit_node_internal(emit, fyn, DDNF_ROOT, -1);

	/* right comment next */
	fy_emit_node_comment(emit, fyn, DDNF_ROOT, -1, fycp_right);

	/* bottom comment last */
	fy_emit_node_comment(emit, fyn, DDNF_ROOT, -1, fycp_bottom);

	return 0;
}

int fy_emit_document(struct fy_emitter *emit, struct fy_document *fyd)
{
	int rc;

	rc = fy_emit_document_start(emit, fyd, NULL);
	if (rc)
		return rc;

	rc = fy_emit_root_node(emit, fyd->root);
	if (rc)
		return rc;

	rc = fy_emit_document_end(emit);

	return rc;
}

const struct fy_emitter_cfg *fy_emitter_get_cfg(struct fy_emitter *emit)
{
	if (!emit)
		return NULL;

	return emit->cfg;
}

struct fy_emitter *fy_emitter_create(struct fy_emitter_cfg *cfg)
{
	struct fy_emitter *emit;

	if (!cfg)
		return NULL;

	emit = malloc(sizeof(*emit));
	if (!emit)
		return NULL;

	fy_emit_setup(emit, cfg);

	return emit;
}

void fy_emitter_destroy(struct fy_emitter *emit)
{
	if (!emit)
		return;

	fy_emit_cleanup(emit);

	free(emit);
}

struct fy_emit_buffer_state {
	char *buf;
	int size;
	int pos;
	int need;
	bool grow;
};

static int do_buffer_output(struct fy_emitter *emit, enum fy_emitter_write_type type, const char *str, int len, void *userdata)
{
	struct fy_emit_buffer_state *state = emit->cfg->userdata;
	int left;
	int pagesize = 0;
	int size;
	char *bufnew;

	state->need += len;
	left = state->size - state->pos;
	if (left < len) {
		if (!state->grow)
			return 0;

		pagesize = sysconf(_SC_PAGESIZE);
		size = state->need + pagesize - 1;
		size = size - size % pagesize;

		bufnew = realloc(state->buf, size);
		if (!bufnew)
			return -1;
		state->buf = bufnew;
		state->size = size;
		left = state->size - state->pos;

	}

	if (len > left)
		len = left;
	if (state->buf)
		memcpy(state->buf + state->pos, str, len);
	state->pos += len;

	return len;
}

static int fy_emit_str_internal(struct fy_document *fyd,
				enum fy_emitter_cfg_flags flags,
				struct fy_node *fyn, char **bufp, int *sizep,
				bool grow)
{
	struct fy_emitter emit_state, *emit = &emit_state;
	struct fy_emitter_cfg emit_cfg;
	struct fy_emit_buffer_state state;
	int rc = -1;

	memset(&emit_cfg, 0, sizeof(emit_cfg));
	memset(&state, 0, sizeof(state));

	emit_cfg.output = do_buffer_output;
	emit_cfg.userdata = &state;
	emit_cfg.flags = flags;
	state.buf = *bufp;
	state.size = *sizep;
	state.grow = grow;

	fy_emit_setup(emit, &emit_cfg);
	rc = fyd ? fy_emit_document(emit, fyd) : fy_emit_node(emit, fyn);
	fy_emit_cleanup(emit);

	if (rc)
		goto out_err;

	/* terminating zero */
	rc = do_buffer_output(emit, fyewt_terminating_zero, "\0", 1, emit->cfg->userdata);

	if (rc != 1) {
		rc = -1;
		goto out_err;
	}

	*sizep = state.need;

	if (!grow)
		return 0;

	*bufp = realloc(state.buf, *sizep);
	if (!*bufp) {
		rc = -1;
		goto out_err;
	}

	return 0;

out_err:
	if (grow && state.buf)
		free(state.buf);
	*bufp = NULL;
	*sizep = 0;

	return rc;
}

int fy_emit_document_to_buffer(struct fy_document *fyd, enum fy_emitter_cfg_flags flags, char *buf, int size)
{
	int rc;

	rc = fy_emit_str_internal(fyd, flags, NULL, &buf, &size, false);
	if (rc != 0)
		return -1;
	return size;
}

char *fy_emit_document_to_string(struct fy_document *fyd, enum fy_emitter_cfg_flags flags)
{
	char *buf = NULL;
	int rc, size = 0;

	rc = fy_emit_str_internal(fyd, flags, NULL, &buf, &size, true);
	if (rc != 0)
		return NULL;
	return buf;
}

static int do_file_output(struct fy_emitter *emit, enum fy_emitter_write_type type, const char *str, int len, void *userdata)
{
	FILE *fp = userdata;

	return fwrite(str, 1, len, fp);
}

int fy_emit_document_to_fp(struct fy_document *fyd, enum fy_emitter_cfg_flags flags,
			   FILE *fp)
{
	struct fy_emitter emit_state, *emit = &emit_state;
	struct fy_emitter_cfg emit_cfg;
	int rc;

	if (!fp)
		return -1;

	memset(&emit_cfg, 0, sizeof(emit_cfg));
	emit_cfg.output = do_file_output;
	emit_cfg.userdata = fp;
	emit_cfg.flags = flags;
	fy_emit_setup(emit, &emit_cfg);
	rc = fy_emit_document(emit, fyd);
	fy_emit_cleanup(emit);

	return rc ? rc : 0;
}

int fy_emit_document_to_file(struct fy_document *fyd,
			     enum fy_emitter_cfg_flags flags,
			     const char *filename)
{
	FILE *fp;
	int rc;

	fp = filename ? fopen(filename, "wa") : stdout;
	if (!fp)
		return -1;

	rc = fy_emit_document_to_fp(fyd, flags, fp);

	if (fp != stdout)
		fclose(fp);

	return rc ? rc : 0;
}

int fy_emit_node_to_buffer(struct fy_node *fyn, enum fy_emitter_cfg_flags flags, char *buf, int size)
{
	int rc;

	rc = fy_emit_str_internal(NULL, flags, fyn, &buf, &size, false);
	if (rc != 0)
		return -1;
	return size;
}

char *fy_emit_node_to_string(struct fy_node *fyn, enum fy_emitter_cfg_flags flags)
{
	char *buf = NULL;
	int rc, size = 0;

	rc = fy_emit_str_internal(NULL, flags, fyn, &buf, &size, true);
	if (rc != 0)
		return NULL;
	return buf;
}

static bool fy_emit_ready(struct fy_emitter *emit)
{
	struct fy_eventp *fyep;
	int need, count, level;

	/* no events in the list, not ready */
	fyep = fy_eventp_list_head(&emit->queued_events);
	if (!fyep)
		return false;

	/* some events need more than one */
	switch (fyep->e.type) {
	case FYET_DOCUMENT_START:
		need = 1;
		break;
	case FYET_SEQUENCE_START:
		need = 2;
		break;
	case FYET_MAPPING_START:
		need = 3;
		break;
	default:
		need = 0;
		break;
	}

	/* if we don't need any more, that's enough */
	if (!need)
		return true;

	level = 0;
	count = 0;
	for (; fyep; fyep = fy_eventp_next(&emit->queued_events, fyep)) {
		count++;

		if (count > need)
			return true;

		switch (fyep->e.type) {
		case FYET_STREAM_START:
		case FYET_DOCUMENT_START:
		case FYET_SEQUENCE_START:
		case FYET_MAPPING_START:
			level++;
			break;
		case FYET_STREAM_END:
		case FYET_DOCUMENT_END:
		case FYET_SEQUENCE_END:
		case FYET_MAPPING_END:
			level--;
			break;
		default:
			break;
		}

		if (!level)
			return true;
	}

	return false;
}

extern const char *fy_event_type_txt[];

const char *fy_emitter_state_txt[] = {
	[FYES_NONE]			= "NONE",
	[FYES_STREAM_START]		= "STREAM_START",
	[FYES_FIRST_DOCUMENT_START]	= "FIRST_DOCUMENT_START",
	[FYES_DOCUMENT_START]		= "DOCUMENT_START",
	[FYES_DOCUMENT_CONTENT]		= "DOCUMENT_CONTENT",
	[FYES_DOCUMENT_END]		= "DOCUMENT_END",
	[FYES_SEQUENCE_FIRST_ITEM]	= "SEQUENCE_FIRST_ITEM",
	[FYES_SEQUENCE_ITEM]		= "SEQUENCE_ITEM",
	[FYES_MAPPING_FIRST_KEY]	= "MAPPING_FIRST_KEY",
	[FYES_MAPPING_KEY]		= "MAPPING_KEY",
	[FYES_MAPPING_SIMPLE_VALUE]	= "MAPPING_SIMPLE_VALUE",
	[FYES_MAPPING_VALUE]		= "MAPPING_VALUE",
	[FYES_END]			= "END",
};

struct fy_eventp *
fy_emit_next_event(struct fy_emitter *emit)
{
	if (!fy_emit_ready(emit))
		return NULL;

	return fy_eventp_list_pop(&emit->queued_events);
}

struct fy_eventp *
fy_emit_peek_next_event(struct fy_emitter *emit)
{
	if (!fy_emit_ready(emit))
		return NULL;

	return fy_eventp_list_head(&emit->queued_events);
}

bool fy_emit_streaming_sequence_empty(struct fy_emitter *emit)
{
	struct fy_eventp *fyepn;
	struct fy_event *fyen;

	fyepn = fy_emit_peek_next_event(emit);
	fyen = fyepn ? &fyepn->e : NULL;

	return !fyen || fyen->type == FYET_SEQUENCE_END;
}

bool fy_emit_streaming_mapping_empty(struct fy_emitter *emit)
{
	struct fy_eventp *fyepn;
	struct fy_event *fyen;

	fyepn = fy_emit_peek_next_event(emit);
	fyen = fyepn ? &fyepn->e : NULL;

	return !fyen || fyen->type == FYET_MAPPING_END;
}

static void fy_emit_goto_state(struct fy_emitter *emit, enum fy_emitter_state state)
{
	if (emit->state == state)
		return;

	/* fy_notice(NULL, "emit: %s -> %s",
			fy_emitter_state_txt[emit->state],
			fy_emitter_state_txt[state]); */

	emit->state = state;
}

static int fy_emit_push_state(struct fy_emitter *emit, enum fy_emitter_state state)
{
	enum fy_emitter_state *states;

	if (emit->state_stack_top >= emit->state_stack_alloc) {
		states = realloc(emit->state_stack == emit->state_stack_inplace ? NULL : emit->state_stack,
				sizeof(emit->state_stack[0]) * emit->state_stack_alloc * 2);
		if (!state)
			return -1;

		if (emit->state_stack == emit->state_stack_inplace)
			memcpy(states, emit->state_stack, sizeof(emit->state_stack[0]) * emit->state_stack_top);
		emit->state_stack = states;
		emit->state_stack_alloc *= 2;
	}
	emit->state_stack[emit->state_stack_top++] = state;

	return 0;
}

enum fy_emitter_state fy_emit_pop_state(struct fy_emitter *emit)
{
	if (!emit->state_stack_top)
		return FYES_NONE;

	return emit->state_stack[--emit->state_stack_top];
}

int fy_emit_push_sc(struct fy_emitter *emit, struct fy_emit_save_ctx *sc)
{
	struct fy_emit_save_ctx *scs;

	if (emit->sc_stack_top >= emit->sc_stack_alloc) {
		scs = realloc(emit->sc_stack == emit->sc_stack_inplace ? NULL : emit->sc_stack,
				sizeof(emit->sc_stack[0]) * emit->sc_stack_alloc * 2);
		if (!scs)
			return -1;

		if (emit->sc_stack == emit->sc_stack_inplace)
			memcpy(scs, emit->sc_stack, sizeof(emit->sc_stack[0]) * emit->sc_stack_top);
		emit->sc_stack = scs;
		emit->sc_stack_alloc *= 2;
	}
	emit->sc_stack[emit->sc_stack_top++] = *sc;

	return 0;
}

int fy_emit_pop_sc(struct fy_emitter *emit, struct fy_emit_save_ctx *sc)
{
	if (!emit->sc_stack_top)
		return -1;

	*sc = emit->sc_stack[--emit->sc_stack_top];

	return 0;
}

static int fy_emit_streaming_node(struct fy_emitter *emit, struct fy_eventp *fyep, int flags)
{
	struct fy_parser *fyp = fyep->fyp;
	struct fy_event *fye = &fyep->e;
	struct fy_emit_save_ctx *sc = &emit->s_sc;
	enum fy_node_style style;
	int ret, s_flags, s_indent;

	if (fye->type != FYET_ALIAS && fye->type != FYET_SCALAR &&
			(emit->s_flags & DDNF_ROOT) && emit->column != 0) {
		fy_emit_putc(emit, fyewt_linebreak, '\n');
		emit->flags = FYEF_WHITESPACE | FYEF_INDENTATION;
	}

	emit->s_flags = flags;

	switch (fye->type) {
	case FYET_ALIAS:
		fy_emit_token_write_alias(emit, fye->alias.anchor, emit->s_flags, emit->s_indent);
		fy_emit_goto_state(emit, fy_emit_pop_state(emit));
		break;

	case FYET_SCALAR:
		fy_emit_common_node_preamble(emit, fye->scalar.anchor, fye->scalar.tag, emit->s_flags, emit->s_indent);
		style = fye->scalar.value ?
				fy_node_style_from_scalar_style(fye->scalar.value->scalar.style) :
				FYNS_PLAIN;
		fy_emit_token_scalar(emit, fye->scalar.value, emit->s_flags, emit->s_indent, style);
		fy_emit_goto_state(emit, fy_emit_pop_state(emit));
		break;

	case FYET_SEQUENCE_START:

		/* save this context */
		ret = fy_emit_push_sc(emit, sc);
		if (ret)
			return ret;

		s_flags = emit->s_flags;
		s_indent = emit->s_indent;

		fy_emit_common_node_preamble(emit, fye->sequence_start.anchor, fye->sequence_start.tag, emit->s_flags, emit->s_indent);

		/* create new context */
		memset(sc, 0, sizeof(*sc));
		sc->flags = DDNF_SEQ | (emit->s_flags & DDNF_ROOT);
		sc->indent = emit->s_indent;
		sc->empty = fy_emit_streaming_sequence_empty(emit);
		sc->flow_token = fye->sequence_start.sequence_start &&
				 fye->sequence_start.sequence_start->type == FYTT_FLOW_SEQUENCE_START;
		sc->flow = false;
		sc->old_indent = sc->indent;
		sc->s_flags = s_flags;
		sc->s_indent = s_indent;
		sc->s_flags = s_flags;
		sc->s_indent = s_indent;

		fy_emit_sequence_prolog(emit, sc);

		emit->s_flags = sc->flags;
		emit->s_indent = sc->indent;

		fy_emit_goto_state(emit, FYES_SEQUENCE_FIRST_ITEM);
		break;

	case FYET_MAPPING_START:
		/* save this context */
		ret = fy_emit_push_sc(emit, sc);
		if (ret)
			return ret;

		s_flags = emit->s_flags;
		s_indent = emit->s_indent;

		fy_emit_common_node_preamble(emit, fye->mapping_start.anchor, fye->mapping_start.tag, emit->s_flags, emit->s_indent);

		/* create new context */
		memset(sc, 0, sizeof(*sc));
		sc->flags = DDNF_MAP | (emit->s_flags & DDNF_ROOT);
		sc->indent = emit->s_indent;
		sc->empty = fy_emit_streaming_mapping_empty(emit);
		sc->flow_token = fye->mapping_start.mapping_start &&
				 fye->mapping_start.mapping_start->type == FYTT_FLOW_MAPPING_START;
		sc->flow = false;
		sc->old_indent = sc->indent;
		sc->s_flags = s_flags;
		sc->s_indent = s_indent;
		sc->s_flags = s_flags;
		sc->s_indent = s_indent;

		fy_emit_mapping_prolog(emit, sc);

		emit->s_flags = sc->flags;
		emit->s_indent = sc->indent;

		fy_emit_goto_state(emit, FYES_MAPPING_FIRST_KEY);
		break;

	default:
		fy_error(fyp, "%s: expected ALIAS|SCALAR|SEQUENCE_START|MAPPING_START", __func__);
		return -1;
	}

	return 0;
}

static int fy_emit_handle_stream_start(struct fy_emitter *emit, struct fy_eventp *fyep)
{
	struct fy_parser *fyp = fyep->fyp;
	struct fy_event *fye = &fyep->e;

	if (fye->type != FYET_STREAM_START) {
		fy_error(fyp, "%s: expected FYET_STREAM_START", __func__);
		return -1;
	}

	fy_emit_reset(emit, false);

	fy_emit_goto_state(emit, FYES_FIRST_DOCUMENT_START);

	return 0;
}

static int fy_emit_handle_document_start(struct fy_emitter *emit, struct fy_eventp *fyep, bool first)
{
	struct fy_parser *fyp = fyep->fyp;
	struct fy_event *fye = &fyep->e;
	struct fy_document_state *fyds;

	if (fye->type != FYET_DOCUMENT_START &&
	    fye->type != FYET_STREAM_END) {
		fy_error(fyp, "%s: expected FYET_DOCUMENT_START|FYET_STREAM_END", __func__);
		return -1;
	}

	if (fye->type == FYET_STREAM_END) {
		fy_emit_goto_state(emit, FYES_END);
		return 0;
	}

	/* transfer ownership to the emitter */
	fyds = fye->document_start.document_state;
	fye->document_start.document_state = NULL;

	fy_emit_common_document_start(emit, fyds, false);

	fy_emit_goto_state(emit, FYES_DOCUMENT_CONTENT);

	return 0;
}

static int fy_emit_handle_document_content(struct fy_emitter *emit, struct fy_eventp *fyep)
{
	int ret;

	ret = fy_emit_push_state(emit, FYES_DOCUMENT_END);
	if (ret)
		return ret;

	return fy_emit_streaming_node(emit, fyep, DDNF_ROOT);
}

static int fy_emit_handle_document_end(struct fy_emitter *emit, struct fy_eventp *fyep)
{
	struct fy_document_state *fyds;
	struct fy_parser *fyp = fyep->fyp;
	struct fy_event *fye = &fyep->e;
	int ret;

	if (fye->type != FYET_DOCUMENT_END) {
		fy_error(fyp, "%s: expected FYET_DOCUMENT_END", __func__);
		return -1;
	}

	fyds = emit->fyds;

	ret = fy_emit_common_document_end(emit);
	if (ret)
		return ret;

	fy_document_state_unref(fyds);

	fy_emit_reset(emit, false);
	fy_emit_goto_state(emit, FYES_DOCUMENT_START);
	return 0;
}

static int fy_emit_handle_sequence_item(struct fy_emitter *emit, struct fy_eventp *fyep, bool first)
{
	struct fy_parser *fyp = fyep->fyp;
	struct fy_event *fye = &fyep->e;
	struct fy_emit_save_ctx *sc = &emit->s_sc;
	struct fy_token *fyt_item = NULL;
	int ret;

	fy_token_unref(sc->fyt_last_value);
	sc->fyt_last_value = NULL;

	switch (fye->type) {
	case FYET_SEQUENCE_END:
		fy_emit_sequence_item_epilog(emit, sc, true, sc->fyt_last_value);

		/* emit epilog */
		fy_emit_sequence_epilog(emit, sc);
		/* pop state */
		ret = fy_emit_pop_sc(emit, sc);
		/* pop state */
		fy_emit_goto_state(emit, fy_emit_pop_state(emit));

		/* restore indent and flags */
		emit->s_indent = sc->s_indent;
		emit->s_flags = sc->s_flags;
		return ret;

	case FYET_ALIAS:
		fyt_item = fye->alias.anchor;
		break;
	case FYET_SCALAR:
		fyt_item = fye->scalar.value;
		break;
	case FYET_SEQUENCE_START:
		fyt_item = fye->sequence_start.sequence_start;
		break;
	case FYET_MAPPING_START:
		fyt_item = fye->mapping_start.mapping_start;
		break;
	default:
		fy_error(fyp, "%s: expected SEQUENCE_END|ALIAS|SCALAR|SEQUENCE_START|MAPPING_START", __func__);
		return -1;
	}

	ret = fy_emit_push_state(emit, FYES_SEQUENCE_ITEM);
	if (ret)
		return ret;

	/* reset indent and flags for each item */
	emit->s_indent = sc->indent;
	emit->s_flags = sc->flags;

	if (!first)
		fy_emit_sequence_item_epilog(emit, sc, false, sc->fyt_last_value);

	sc->fyt_last_value = fyt_item;

	fy_emit_sequence_item_prolog(emit, sc, fyt_item);

	ret = fy_emit_streaming_node(emit, fyep, sc->flags);

	switch (fye->type) {
	case FYET_ALIAS:
		fye->alias.anchor = NULL;	/* take ownership */
		break;
	case FYET_SCALAR:
		fye->scalar.value = NULL;	/* take ownership */
		break;
	case FYET_SEQUENCE_START:
		fye->sequence_start.sequence_start = NULL;	/* take ownership */
		break;
	case FYET_MAPPING_START:
		fye->mapping_start.mapping_start = NULL;	/* take ownership */
		break;
	default:
		break;
	}

	return ret;
}

static int fy_emit_handle_mapping_key(struct fy_emitter *emit, struct fy_eventp *fyep, bool first)
{
	struct fy_parser *fyp = fyep->fyp;
	struct fy_event *fye = &fyep->e;
	struct fy_emit_save_ctx *sc = &emit->s_sc;
	struct fy_token *fyt_key = NULL;
	int ret, aflags;
	bool simple_key;

	fy_token_unref(sc->fyt_last_key);
	sc->fyt_last_key = NULL;
	fy_token_unref(sc->fyt_last_value);
	sc->fyt_last_value = NULL;

	simple_key = false;

	switch (fye->type) {
	case FYET_MAPPING_END:
		fy_emit_mapping_value_epilog(emit, sc, true, sc->fyt_last_value);

		/* emit epilog */
		fy_emit_mapping_epilog(emit, sc);
		/* pop state */
		ret = fy_emit_pop_sc(emit, sc);
		/* pop state */
		fy_emit_goto_state(emit, fy_emit_pop_state(emit));

		/* restore indent and flags */
		emit->s_indent = sc->s_indent;
		emit->s_flags = sc->s_flags;
		return ret;

	case FYET_ALIAS:
		fyt_key = fye->alias.anchor;
		simple_key = true;
		break;
	case FYET_SCALAR:
		fyt_key = fye->scalar.value;
		aflags = fy_token_text_analyze(fyt_key);
		simple_key = !!(aflags & FYTTAF_CAN_BE_SIMPLE_KEY);
		break;
	case FYET_SEQUENCE_START:
		fyt_key = fye->sequence_start.sequence_start;
		simple_key = fy_emit_streaming_sequence_empty(emit);
		break;
	case FYET_MAPPING_START:
		fyt_key = fye->mapping_start.mapping_start;
		simple_key = fy_emit_streaming_mapping_empty(emit);
		break;
	default:
		fy_error(fyp, "%s: expected MAPPING_END|ALIAS|SCALAR|SEQUENCE_START|MAPPING_START", __func__);
		return -1;
	}

	ret = fy_emit_push_state(emit, FYES_MAPPING_VALUE);
	if (ret)
		return ret;

	/* reset indent and flags for each key/value pair */
	emit->s_indent = sc->indent;
	emit->s_flags = sc->flags;

	if (!first)
		fy_emit_mapping_value_epilog(emit, sc, false, sc->fyt_last_value);

	sc->fyt_last_key = fyt_key;

	fy_emit_mapping_key_prolog(emit, sc, fyt_key, simple_key);

	ret = fy_emit_streaming_node(emit, fyep, sc->flags);

	switch (fye->type) {
	case FYET_ALIAS:
		fye->alias.anchor = NULL;	/* take ownership */
		break;
	case FYET_SCALAR:
		fye->scalar.value = NULL;	/* take ownership */
		break;
	case FYET_SEQUENCE_START:
		fye->sequence_start.sequence_start = NULL;	/* take ownership */
		break;
	case FYET_MAPPING_START:
		fye->mapping_start.mapping_start = NULL;	/* take ownership */
		break;
	default:
		break;
	}

	return ret;
}

static int fy_emit_handle_mapping_value(struct fy_emitter *emit, struct fy_eventp *fyep, bool simple)
{
	struct fy_parser *fyp = fyep->fyp;
	struct fy_event *fye = &fyep->e;
	struct fy_emit_save_ctx *sc = &emit->s_sc;
	struct fy_token *fyt_value = NULL;
	int ret;

	switch (fye->type) {
	case FYET_ALIAS:
		fyt_value = fye->alias.anchor;
		break;
	case FYET_SCALAR:
		fyt_value = fye->scalar.value;	/* take ownership */
		break;
	case FYET_SEQUENCE_START:
		fyt_value = fye->sequence_start.sequence_start;
		break;
	case FYET_MAPPING_START:
		fyt_value = fye->mapping_start.mapping_start;
		break;
	default:
		fy_error(fyp, "%s: expected ALIAS|SCALAR|SEQUENCE_START|MAPPING_START", __func__);
		return -1;
	}

	ret = fy_emit_push_state(emit, FYES_MAPPING_KEY);
	if (ret)
		return ret;

	fy_emit_mapping_key_epilog(emit, sc, sc->fyt_last_key);

	sc->fyt_last_value = fyt_value;

	fy_emit_mapping_value_prolog(emit, sc, fyt_value);

	ret = fy_emit_streaming_node(emit, fyep, sc->flags);

	switch (fye->type) {
	case FYET_ALIAS:
		fye->alias.anchor = NULL;	/* take ownership */
		break;
	case FYET_SCALAR:
		fye->scalar.value = NULL;	/* take ownership */
		break;
	case FYET_SEQUENCE_START:
		fye->sequence_start.sequence_start = NULL;	/* take ownership */
		break;
	case FYET_MAPPING_START:
		fye->mapping_start.mapping_start = NULL;	/* take ownership */
		break;
	default:
		break;
	}

	return ret;
}

int fy_emit_event(struct fy_emitter *emit, struct fy_event *fye)
{
	struct fy_eventp *fyep;
	int ret;

	if (!emit || !fye)
		return -1;

	/* we're using the raw emitter interface, now mark first state */
	if (emit->state == FYES_NONE)
		emit->state = FYES_STREAM_START;

	fyep = container_of(fye, struct fy_eventp, e);

	/* fy_notice(NULL, "> %s", fy_event_type_txt[fyep->e.type]); */

	fy_eventp_list_add_tail(&emit->queued_events, fyep);

	ret = 0;
	while ((fyep = fy_emit_next_event(emit)) != NULL) {

		/* fy_notice(NULL, "! %s", fy_event_type_txt[fyep->e.type]); */

		switch (emit->state) {
		case FYES_STREAM_START:
			ret = fy_emit_handle_stream_start(emit, fyep);
			break;

		case FYES_FIRST_DOCUMENT_START:
		case FYES_DOCUMENT_START:
			ret = fy_emit_handle_document_start(emit, fyep,
					emit->state == FYES_FIRST_DOCUMENT_START);
			break;

		case FYES_DOCUMENT_CONTENT:
			ret = fy_emit_handle_document_content(emit, fyep);
			break;

		case FYES_DOCUMENT_END:
			ret = fy_emit_handle_document_end(emit, fyep);
			break;

		case FYES_SEQUENCE_FIRST_ITEM:
		case FYES_SEQUENCE_ITEM:
			ret = fy_emit_handle_sequence_item(emit, fyep,
					emit->state == FYES_SEQUENCE_FIRST_ITEM);
			break;

		case FYES_MAPPING_FIRST_KEY:
		case FYES_MAPPING_KEY:
			ret = fy_emit_handle_mapping_key(emit, fyep,
					emit->state == FYES_MAPPING_FIRST_KEY);
			break;

		case FYES_MAPPING_SIMPLE_VALUE:
		case FYES_MAPPING_VALUE:
			ret = fy_emit_handle_mapping_value(emit, fyep,
					emit->state == FYES_MAPPING_SIMPLE_VALUE);
			break;

		case FYES_END:
			ret = -1;
			break;

		default:
			assert(1);      /* Invalid state. */
		}

		/* always release the event */
		fy_eventp_release(fyep);

		if (ret)
			break;
	}

	return ret;
}
