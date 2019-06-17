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

struct tokenizer {
	FILE *input;
	uint32_t line;
	uint32_t column;
	char buf[MAX_TOK_LEN];
	struct tokenizer_getc_buf getc_buf;
	const char* ml_comment_start;
	const char* ml_comment_end;
	const char* sl_comment_start;
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
void tokenizer_register_multiline_comment_marker(
	struct tokenizer *t, const char* startmarker, const char *endmarker);
void tokenizer_register_singleline_comment_marker(
	struct tokenizer *t, const char* marker);
int tokenizer_next(struct tokenizer *t, struct token* out);

#pragma RcB2 DEP "tokenizer.c"

#endif

