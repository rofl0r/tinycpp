#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>

#include "tokenizer.h"

static int tokenizer_ungetc(struct tokenizer *t, int c)
{
	++t->getc_buf.buffered;
	assert(t->getc_buf.buffered<sizeof(t->getc_buf.buf));
	assert(t->getc_buf.cnt > 0);
	--t->getc_buf.cnt;
	assert(t->getc_buf.buf[t->getc_buf.cnt % sizeof(t->getc_buf.buf)] == c);
	return c;
}
static int tokenizer_getc(struct tokenizer *t)
{
	int c;
	if(t->getc_buf.buffered) {
		t->getc_buf.buffered--;
		c = t->getc_buf.buf[(t->getc_buf.cnt) % sizeof(t->getc_buf.buf)];
	} else {
		c = getc(t->input);
		t->getc_buf.buf[t->getc_buf.cnt % sizeof(t->getc_buf.buf)] = c;
	}
	++t->getc_buf.cnt;
	return c;
}

const char* tokentype_to_str(enum tokentype tt) {
	switch(tt) {
		case TT_IDENTIFIER: return "iden";
		case TT_SQSTRING_LIT: return "single-quoted string";
		case TT_DQSTRING_LIT: return "double-quoted string";
		case TT_ELLIPSIS: return "ellipsis";
		case TT_HEX_INT_LIT: return "hexint";
		case TT_OCT_INT_LIT: return "octint";
		case TT_DEC_INT_LIT: return "decint";
		case TT_SEP: return "separator";
		case TT_UNKNOWN: return "unknown";
		case TT_OVERFLOW: return "overflow";
		case TT_EOF: return "eof";
	}
	return "????";
}

static int has_ul_tail(const char *p) {
	char tail[4];
	int tc = 0, c;
	while(tc < 4 ) {
		if(!*p) break;
		c = tolower(*p);
		if(c == 'u' || c == 'l') {
			tail[tc++] = c;
		} else {
			return 0;
		}
		p++;
	}
	if(tc == 1) return 1;
	if(tc == 2) {
		if(!memcmp(tail, "lu", 2)) return 1;
		if(!memcmp(tail, "ul", 2)) return 1;
		if(!memcmp(tail, "ll", 2)) return 1;
	}
	if(tc == 3) {
		if(!memcmp(tail, "llu", 3)) return 1;
		if(!memcmp(tail, "ull", 3)) return 1;
	}
	return 0;
}

static int is_hex_int_literal(const char *s) {
	if(s[0] == '-') s++;
	if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		const char* p = s+2;
		while(*p) {
			if(!strchr("0123456789abcdef", tolower(*p))) {
				if(p == s+2) return 0;
				return has_ul_tail(p);
			}
			p++;
		}
		return 1;
	}
	return 0;
}

static int is_dec_int_literal(const char *s) {
	if(s[0] == '-') s++;
	if(s[0] == '0') return 0;
	while(*s) {
		if(!isdigit(*(s++))) {
			return has_ul_tail(s);
			return 0;
		}
	}
	return 1;
}
static int is_oct_int_literal(const char *s) {
	if(s[0] == '-') s++;
	if(s[0] != '0') return 0;
	while(*s) {
		if(!strchr("01234567", *s)) return 0;
		s++;
	}
	return 1;
}

static int is_ellipsis(const char *s) {
	return !strcmp(s, "...");
}

static int is_identifier(const char *s) {
	#define ALPHA_UP "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	#define ALPHA_LO "abcdefghijklmnopqrstuvwxyz"
	#define DIGIT    "0123456789"
	static const char iden_head[] = "_" ALPHA_UP ALPHA_LO;
	static const char iden_tail[] = "_" ALPHA_UP ALPHA_LO DIGIT;

	if(!strchr(iden_head, *(s++))) return 0;
	while(*s) {
		if(!strchr(iden_tail, *s))
			return 0;
		s++;
	}
	return 1;
}

static enum tokentype categorize(const char *s) {
	if(is_ellipsis(s)) return TT_ELLIPSIS;
	if(is_hex_int_literal(s)) return TT_HEX_INT_LIT;
	if(is_dec_int_literal(s)) return TT_DEC_INT_LIT;
	if(is_oct_int_literal(s)) return TT_OCT_INT_LIT;
	if(is_identifier(s)) return TT_IDENTIFIER;
	return TT_UNKNOWN;
}


static int is_sep(int c) {
	return !!strchr(" \t\n()[]<>{}\?:;.,!=+-*&|/%#'\"", c);
}

static int apply_coords(struct tokenizer *t, struct token* out, char *end, int retval) {
	out->line = t->line;
	uintptr_t len = end - t->buf;
	out->column = t->column - len;
	return retval;
}

static inline char *assign_bufchar(struct tokenizer *t, char *s, int c) {
	t->column++;
	*s = c;
	return s + 1;
}

static int get_string(struct tokenizer *t, char quote_char, struct token* out) {
	char *s = t->buf+1;
	int escaped = 0;
	while((uintptr_t)s < (uintptr_t)t->buf + MAX_TOK_LEN + 2) {
		int c = tokenizer_getc(t);
		if(c == EOF) {
			out->type = TT_EOF;
			*s = 0;
			return apply_coords(t, out, s, 0);
		}
		if(c == '\n') {
			out->type = TT_UNKNOWN;
			s = assign_bufchar(t, s, 0);
			return apply_coords(t, out, s, 0);
		}
		if(!escaped) {
			if(c == quote_char) {
				s = assign_bufchar(t, s, c);
				*s = 0;
				//s = assign_bufchar(t, s, 0);
				out->type = (quote_char == '"'? TT_DQSTRING_LIT : TT_SQSTRING_LIT);
				return apply_coords(t, out, s, 1);
			}
			if(c == '\\') escaped = 1;
		} else {
			escaped = 0;
		}
		s = assign_bufchar(t, s, c);
	}
	t->buf[MAX_TOK_LEN-1] = 0;
	out->type = TT_OVERFLOW;
	return apply_coords(t, out, s, 0);
}

static int sequence_follows(struct tokenizer *t, int c, const char *which)
{
	if(!which || !which[0]) return 0;
	size_t i = 0;
	while(c == which[i]) {
		if(!which[++i]) break;
		c = tokenizer_getc(t);
	}
	if(!which[i]) return 1;
	while(i > 0) {
		tokenizer_ungetc(t, c);
		c = which[--i];
	}
	return 0;
}

static void ignore_until(struct tokenizer *t, const char* marker, int col_advance)
{
	t->column += col_advance;
	int c;
	do {
		c = tokenizer_getc(t);
		if(c == '\n') {
			t->line++;
			t->column = 0;
		} else t->column++;
	} while(!sequence_follows(t, c, marker));
	t->column += strlen(marker)-1;
}

void tokenizer_skip_until(struct tokenizer *t, const char *marker)
{
	ignore_until(t, marker, 0);
}

int tokenizer_next(struct tokenizer *t, struct token* out) {
	char *s = t->buf;
	out->value = 0;
	while(1) {
		int c = tokenizer_getc(t);
		if(c == EOF) {
			out->type = TT_EOF;
			return apply_coords(t, out, s, 1);
		}
		/* components of multi-line comment marker might be terminals themselves */
		if(sequence_follows(t, c, t->marker[MT_MULTILINE_COMMENT_START])) {
			ignore_until(t, t->marker[MT_MULTILINE_COMMENT_END], strlen(t->marker[MT_MULTILINE_COMMENT_START]));
			continue;
		}
		if(sequence_follows(t, c, t->marker[MT_SINGLELINE_COMMENT_START])) {
			ignore_until(t, "\n", strlen(t->marker[MT_SINGLELINE_COMMENT_START]));
			continue;
		}
		if(is_sep(c)) {
			tokenizer_ungetc(t, c);
			break;
		}

		s = assign_bufchar(t, s, c);
		if(t->column + 1 >= MAX_TOK_LEN) {
			out->type = TT_OVERFLOW;
			return apply_coords(t, out, s, 0);
		}
	}
	if(s == t->buf) {
		int c = tokenizer_getc(t);
		s = assign_bufchar(t, s, c);
		*s = 0;
		//s = assign_bufchar(t, s, 0);
		if(c == '"' || c == '\'')
			if(t->flags & TF_PARSE_STRINGS) return get_string(t, c, out);
		out->type = TT_SEP;
		out->value = c;
		if(c == '\n') {
			apply_coords(t, out, s, 1);
			t->line++;
			t->column=0;
			return 1;
		}
		return apply_coords(t, out, s, 1);
	}
	//s = assign_bufchar(t, s, 0);
	*s = 0;
	out->type = categorize(t->buf);
	return apply_coords(t, out, s, out->type != TT_UNKNOWN);
}

void tokenizer_set_flags(struct tokenizer *t, int flags) {
	t->flags = flags;
}

void tokenizer_init(struct tokenizer *t, FILE* in, int flags) {
	*t = (struct tokenizer){ .input = in, .line = 1, .flags = flags };
}

void tokenizer_register_marker(struct tokenizer *t, enum markertype mt, const char* marker)
{
	t->marker[mt] = marker;
}

