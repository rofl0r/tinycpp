#ifndef TOKENIZER_H
#define TOKENIZER_H

#define MAX_TOK_LEN 4096
#define MAX_UNGETC 8

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

struct tokenizer_getc_buf {
	char buf[MAX_UNGETC];
	size_t cnt, buffered;
};

enum markertype {
	MT_SINGLELINE_COMMENT_START = 0,
	MT_MULTILINE_COMMENT_START = 1,
	MT_MULTILINE_COMMENT_END = 2,
	MT_MAX = MT_MULTILINE_COMMENT_END
};

struct tokenizer {
	FILE *input;
	uint32_t line;
	uint32_t column;
	char buf[MAX_TOK_LEN];
	struct tokenizer_getc_buf getc_buf;
	const char* marker[MT_MAX+1];
};

enum tokentype {
	TT_IDENTIFIER,
	TT_SQSTRING_LIT,
	TT_DQSTRING_LIT,
	TT_ELLIPSIS,
	TT_HEX_INT_LIT,
	TT_OCT_INT_LIT,
	TT_DEC_INT_LIT,
	TT_SEP,
	/* errors and similar */
	TT_UNKNOWN,
	TT_OVERFLOW,
	TT_EOF,
};

const char* tokentype_to_str(enum tokentype tt);

struct token {
	enum tokentype type;
	uint32_t line;
	uint32_t column;
	int value;
};

void tokenizer_init(struct tokenizer *t, FILE* in);
void tokenizer_register_marker(struct tokenizer*, enum markertype, const char*);
int tokenizer_next(struct tokenizer *t, struct token* out);
void tokenizer_skip_until(struct tokenizer *t, const char *marker);

#pragma RcB2 DEP "tokenizer.c"

#endif

