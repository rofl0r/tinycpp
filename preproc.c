#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "tokenizer.h"
#include "../cdev/cdev/agsutils/List.h"
#include "khash.h"

struct token_str_tup {
	struct token tok;
	const char *strbuf;
};

struct macro {
	unsigned num_args;
	FILE* str_contents;
	char *str_contents_buf;
	List /*const char* */ argnames;
};

static int token_needs_string(struct token *tok) {
	switch(tok->type) {
		case TT_IDENTIFIER:
		case TT_SQSTRING_LIT:
		case TT_DQSTRING_LIT:
                case TT_ELLIPSIS:
                case TT_HEX_INT_LIT:
                case TT_OCT_INT_LIT:
                case TT_DEC_INT_LIT:
			return 1;
		default:
			return 0;
	}
}

static void tokenizer_from_file(struct tokenizer *t, FILE* f) {
	tokenizer_init(t, f, TF_PARSE_STRINGS);
	tokenizer_set_filename(t, "<macro>");
}

/* iobuf needs to point to a char[2], which will be used for a character token.
   after success, it'll point to either the original buffer, or the tokenizer's
 */
static size_t token_as_string(struct tokenizer *t, struct token *tok, char** iobuf) {
	if(token_needs_string(tok)) {
		*iobuf = t->buf;
		return strlen(t->buf);
	} else {
		iobuf[0][0] = tok->value;
		iobuf[0][1] = 0;
		return 1;
	}
}

KHASH_MAP_INIT_STR(macros, struct macro)

static khash_t(macros) *macros;

static struct macro* get_macro(const char *name) {
	khint_t k = kh_get(macros, macros, name);
	if(k == kh_end(macros)) return 0;
	return &kh_value(macros, k);
}

static int add_macro(const char *name, struct macro*m) {
	int absent;
	khint_t k = kh_put(macros, macros, name, &absent);
	if (!absent) {
		// FIXME free contents of macro struct
		// kh_del(macros, macros, k);
	}
	kh_value(macros, k) = *m;
	return !absent;
}

static int undef_macro(const char *name) {
	khint_t k = kh_get(macros, macros, name);
	if(k == kh_end(macros)) return 0;
	struct macro *m = &kh_value(macros, k);
	fclose(m->str_contents);
	free(m->str_contents_buf);
	size_t i;
	for(i = 0; i < List_size(&m->argnames); i++) {
		char *item;
		List_get(&m->argnames, i, &item);
		free(item);
	}
	List_free(&m->argnames);
	kh_del(macros, macros, k);
	return 1;
}

static void error_or_warning(const char *err, const char* type, struct tokenizer *t, struct token *curr) {
	unsigned column = curr ? curr->column : t->column;
	unsigned line  = curr ? curr->line : t->line;
	dprintf(2, "<%s> %u:%u %s: '%s'\n", t->filename, line, column, type, err);
	dprintf(2, "%s\n", t->buf);
	for(int i = 0; i < strlen(t->buf); i++)
		dprintf(2, "^");
	dprintf(2, "\n");
}
static void error(const char *err, struct tokenizer *t, struct token *curr) {
	error_or_warning(err, "error", t, curr);
}
static void warning(const char *err, struct tokenizer *t, struct token *curr) {
	error_or_warning(err, "warning", t, curr);
}

static void emit(FILE *out, const char *s) {
	fprintf(out, "%s", s);
}

static int x_tokenizer_next(struct tokenizer *t, struct token *tok) {
	int ret = tokenizer_next(t, tok);
	if(ret == 0) {
		error("unexpected tokenizer error", t, tok);
		abort();
	}
	return ret;
}

/* return index of matching item in values array, or -1 on error */
static int expect(struct tokenizer *t, enum tokentype tt, const char* values[], struct token *token)
{
	int ret;
	do {
		ret = tokenizer_next(t, token);
		if(ret == 0 || token->type == TT_EOF) goto err;
	} while(token->type == TT_SEP && isspace(token->value));

	if(token->type != tt) {
err:
		error("unexpected token", t, token);
		return -1;
	}
	int i = 0;
	while(values[i]) {
		if(!strcmp(values[i], t->buf))
			return i;
		++i;
	}
	return -1;
}

static int is_char(struct token *tok, int ch) {
	return tok->type == TT_SEP && tok->value == ch;
}

static int is_whitespace_token(struct token *token)
{
	return token->type == TT_SEP &&
		(token->value == ' ' || token->value == '\t');
}

static void flush_whitespace(FILE *out, int *ws_count) {
	while(*ws_count > 0) {
		emit(out, " ");
		--(*ws_count);
	}
}

/* skips until the next non-whitespace token (if the current one is one too)*/
static int eat_whitespace(struct tokenizer *t, struct token *token, int *count) {
	*count = 0;
	int ret = 1;
	while (is_whitespace_token(token)) {
		++(*count);
		ret = x_tokenizer_next(t, token);
		if(!ret) break;
	}
	return ret;
}
/* fetches the next token until it is non-whitespace */
static int skip_next_and_ws(struct tokenizer *t, struct token *tok) {
	int ret = tokenizer_next(t, tok);
	if(!ret) return ret;
	unsigned ws_count;
	ret = eat_whitespace(t, tok, &ws_count);
	return ret;
}

static void emit_token(FILE* out, struct token *tok, const char* strbuf) {
	if(tok->type == TT_SEP) {
		fprintf(out, "%c", tok->value);
	} else if(strbuf && token_needs_string(tok)) {
		fprintf(out, "%s", strbuf);
	} else {
		dprintf(2, "oops, dunno how to handle\n");
	}
}

int parse_file(FILE *f, const char*, FILE *out);
static int include_file(struct tokenizer *t, FILE* out) {
	static const char* inc_chars[] = { "\"", "<", 0};
	static const char* inc_chars_end[] = { "\"", ">", 0};
	struct token tok;
	tokenizer_set_flags(t, 0); // disable string tokenization

	int inc1sep = expect(t, TT_SEP, inc_chars, &tok);
	if(inc1sep == -1) {
		error("expected one of [\"<]", t, &tok);
		return 0;
	}
	int ret = tokenizer_read_until(t, inc_chars_end[inc1sep], 1);
	if(!ret) {
		error("error parsing filename", t, &tok);
		return 0;
	}
	// TODO: different path lookup depending on whether " or <
	FILE *f = fopen(t->buf, "r");
	if(!f) {
		dprintf(2, "%s: ", t->buf);
		perror("fopen");
		return 0;
	}
	const char *fn = strdup(t->buf);
	assert(tokenizer_next(t, &tok) && is_char(&tok, inc_chars_end[inc1sep][0]));

	tokenizer_set_flags(t, TF_PARSE_STRINGS);
	return parse_file(f, fn, out);
}

static int emit_error_or_warning(struct tokenizer *t, int is_error) {
	int ws_count;
	int ret = tokenizer_skip_chars(t, " \t", &ws_count);
	if(!ret) return ret;
	struct token tmp = {.column = t->column, .line = t->line};
	ret = tokenizer_read_until(t, "\n", 1);
	if(is_error) {
		error(t->buf, t, &tmp);
		return 0;
	}
	warning(t->buf, t, &tmp);
	return 1;
}

static FILE *freopen_r(FILE *f, char **buf, size_t *size) {
	fflush(f);
	fclose(f);
	return fmemopen(*buf, *size, "r");
}

static int expand_macro(struct tokenizer *t, FILE* out, const char* name, unsigned rec_level);

static int parse_macro(struct tokenizer *t) {
	int ws_count;
	int ret = tokenizer_skip_chars(t, " \t", &ws_count);
	if(!ret) return ret;
	struct token curr; //tmp = {.column = t->column, .line = t->line};
	ret = tokenizer_next(t, &curr) && curr.type != TT_EOF;
	if(!ret) {
		error("parsing macro name", t, &curr);
		return ret;
	}
	if(curr.type != TT_IDENTIFIER) {
		error("expected identifier", t, &curr);
		return 0;
	}
	const char* macroname = strdup(t->buf);
	if(get_macro(macroname)) {
		char buf[128];
		sprintf(buf, "redefinition of macro %s", macroname);
		warning(buf, t, 0);
	}

	struct macro new = { 0 };
	List_init(&new.argnames, sizeof(char*));

	ret = x_tokenizer_next(t, &curr) && curr.type != TT_EOF;
	if(!ret) return ret;

	if (is_char(&curr, '(')) {
		ret = tokenizer_skip_chars(t, " \t", &ws_count);
		if(!ret) return ret;
		while(1) {
			ret = x_tokenizer_next(t, &curr) && curr.type != TT_EOF;
			if(!ret) return ret;
			if(curr.type != TT_IDENTIFIER) {
				error("expected identifier for macro arg", t, &curr);
				return 0;
			}
			{
				const char *tmps = strdup(t->buf);
				List_add(&new.argnames, &tmps);
			}
			++new.num_args;
			ret = x_tokenizer_next(t, &curr) && curr.type != TT_EOF;
			if(!ret) return ret;
			if(curr.type != TT_SEP) {
				error("expected ) or ,", t, &curr);
				return 0;
			}
			switch(curr.value) {
				case ')':
				case ',':
					ret = tokenizer_skip_chars(t, " \t", &ws_count);
					if(!ret) return ret;
					if(curr.value == ')')
						goto break_loop1;
					break;
				default:
					error("unexpected character", t, &curr);
					return 0;
			}
		}
		break_loop1:;
	} else if(is_whitespace_token(&curr)) {
		ret = tokenizer_skip_chars(t, " \t", &ws_count);
		if(!ret) return ret;
	} else if(is_char(&curr, '\n')) {
		/* content-less macro */
		goto done;
	}

	struct FILE_container {
		FILE *f;
		char *buf;
		size_t len;
        } contents;
	contents.f = open_memstream(&contents.buf, &contents.len);

	int backslash_seen = 0;
	while(1) {
		ret = x_tokenizer_next(t, &curr) && curr.type != TT_EOF;
		if(!ret) return ret;

		if (curr.type == TT_SEP) {
			if(curr.value == '\\')
				backslash_seen = 1;
			else {
				if(curr.value == '\n' && !backslash_seen) break;
				emit_token(contents.f, &curr, t->buf);
				backslash_seen = 0;
			}
		} else {
			emit_token(contents.f, &curr, t->buf);
		}
	}
	new.str_contents = freopen_r(contents.f, &contents.buf, &contents.len);
	new.str_contents_buf = contents.buf;
done:
	add_macro(macroname, &new);
	return 1;
}

static size_t macro_arglist_pos(struct macro *m, const char* iden) {
	size_t i;
	for(i = 0; i < List_size(&m->argnames); i++) {
		char *item;
		List_get(&m->argnames, i, &item);
		if(!strcmp(item, iden)) return i;
	}
	return (size_t) -1;
}

#define MAX_RECURSION 32

static int expand_macro(struct tokenizer *t, FILE* out, const char* name, unsigned rec_level) {
	struct macro *m = get_macro(name);
	if(!m) {
		emit(out, name);
		return 1;
	}
	if(rec_level > MAX_RECURSION) {
		error("max recursion level reached", t, 0);
		return 0;
	}
#ifdef DEBUG
	dprintf(2, "expanding macro %s (%s)\n", name, m->str_contents_buf);
#endif

	size_t i;
	struct token tok;
	struct FILE_container {
		FILE *f;
		char *buf;
		size_t len;
		struct tokenizer t;
	} *argvalues = calloc(m->num_args, sizeof(struct FILE_container));

	for(i=0; i < m->num_args; i++)
		argvalues[i].f = open_memstream(&argvalues[i].buf, &argvalues[i].len);

	/* replace named arguments in the contents of the macro call */
	if(m->num_args) {
		if(expect(t, TT_SEP, (const char*[]){"(", 0}, &tok) != 0) {
			error("expected (", t, &tok);
			return 0;
		}
		unsigned curr_arg = 0, need_arg = 1, parens = 0, ws_count;
		if(!tokenizer_skip_chars(t, " \t", &ws_count)) return 0;

		while(1) {
			int ret = x_tokenizer_next(t, &tok);
			if(!ret) return 0;
			if( tok.type == TT_EOF) {
				dprintf(2, "warning EOF\n");
				break;
			}
			if(!parens && is_char(&tok, ',')) {
				if(need_arg && !ws_count) {
					error("unexpected: ','", t, &tok);
					return 0;
				}
				need_arg = 1;
				curr_arg++;
				if(curr_arg >= m->num_args) {
					error("too many arguments for function macro", t, &tok);
					return 0;
				}
				ret = tokenizer_skip_chars(t, " \t", &ws_count);
				if(!ret) return ret;
				continue;
			} else if(is_char(&tok, '(')) {
				++parens;
			} else if(is_char(&tok, ')')) {
				if(!parens) {
					if(curr_arg != m->num_args-1) {
						error("too few args for function macro", t, &tok);
						return 0;
					}
					break;
				}
				--parens;
			}
			need_arg = 0;
			emit_token(argvalues[curr_arg].f, &tok, t->buf);
		}
	}

	for(i=0; i < m->num_args; i++) {
		argvalues[i].f = freopen_r(argvalues[i].f, &argvalues[i].buf, &argvalues[i].len);
		tokenizer_from_file(&argvalues[i].t, argvalues[i].f);
#ifdef DEBUG
		dprintf(2, "macro argument %i: %s\n", (int) i, argvalues[i].buf);
#endif
	}

	if(!m->str_contents) goto cleanup;

	struct FILE_container cwae = {0}; /* contents_with_args_expanded */
	FILE* output = out;
	int expand_now = 1;
	if(m->num_args) {
		cwae.f = open_memstream(&cwae.buf, &cwae.len);
		output = cwae.f;
		expand_now = 0;
	}

	struct tokenizer t2;
	tokenizer_from_file(&t2, m->str_contents);
	fseek(m->str_contents, 0, SEEK_SET);
	int hash_count = 0;
	int ws_count = 0;
	while(1) {
		int ret = x_tokenizer_next(&t2, &tok);
		if(!ret) return ret;
		if(tok.type == TT_EOF) break;
		if(tok.type == TT_IDENTIFIER) {
			flush_whitespace(output, &ws_count);
			size_t arg_nr = macro_arglist_pos(m, t2.buf), j;
			if(arg_nr != (size_t) -1) {
				if(hash_count == 1) {
					struct token fake = {
						.type = TT_SEP,
						.value = '"'
					};
					emit_token(output, &fake, argvalues[arg_nr].t.buf);
				}
				fseek(argvalues[arg_nr].f, 0, SEEK_SET);
				while(1) {
					ret = x_tokenizer_next(&argvalues[arg_nr].t, &tok);
					if(!ret) return ret;
					if(tok.type == TT_EOF) break;
					if(expand_now && tok.type == TT_IDENTIFIER) {
						if(!expand_macro(&argvalues[arg_nr].t, output, argvalues[arg_nr].t.buf, rec_level+1))
							return 0;
					} else
						emit_token(output, &tok, argvalues[arg_nr].t.buf);
				}
				if(hash_count == 1) {
					struct token fake = {
						.type = TT_SEP,
						.value = '"'
					};
					emit_token(output, &fake, argvalues[arg_nr].t.buf);
					hash_count = 0;
				}
			} else {
				if(hash_count == 1) {
		hash_err:
					error("'#' is not followed by macro parameter", &t2, &tok);
					return 0;
				}
				if(expand_now) {
					if(!expand_macro(&t2, output, t2.buf, rec_level+1))
						return 0;
				} else
					emit_token(output, &tok, t2.buf);
			}
		} else if(is_char(&tok, '#')) {
			++hash_count;
		} else if(is_whitespace_token(&tok)) {
			ws_count++;
		} else {
			if(hash_count == 1) goto hash_err;
			flush_whitespace(output, &ws_count);
			emit_token(output, &tok, t2.buf);
		}
		if(hash_count > 2) {
			error("only two '#' characters allowed for macro expansion", &t2, &tok);
			return 0;
		}
		/* handle token concatention operator ## by emitting whitespace before/after*/
		if(!is_whitespace_token(&tok) && !ws_count) {
			flush_whitespace(output, &ws_count);
		} else if(ws_count && hash_count == 2) {
			ret = tokenizer_skip_chars(&t2, " \t", &ws_count);
			if(!ret) return ret;
			ws_count = 0;
		}
	}
	flush_whitespace(output, &ws_count);

	/* we need to expand macros after the macro arguments have been inserted */
	if(m->num_args) {
		cwae.f = freopen_r(cwae.f, &cwae.buf, &cwae.len);
#ifdef DEBUG
		dprintf(2, "contents with args expanded: %s\n", cwae.buf);
#endif
		tokenizer_from_file(&cwae.t, cwae.f);
		while(1) {
			int ret = x_tokenizer_next(&cwae.t, &tok);
			if(!ret) return ret;
			if(tok.type == TT_EOF) break;
			if(tok.type == TT_IDENTIFIER) {
				if(!expand_macro(&cwae.t, out, cwae.t.buf, rec_level+1))
					return 0;
			} else emit_token(out, &tok, cwae.t.buf);
		}
		fclose(cwae.f);
		free(cwae.buf);
	}

cleanup:
	for(i=0; i < m->num_args; i++) {
		fclose(argvalues[i].f);
		free(argvalues[i].buf);
	}
	free(argvalues);
	return 1;
}

/* FIXME: at the moment we only evaluate the first decimal number */
static int do_eval(struct tokenizer *t, int *result) {
	*result = 0;
	struct token curr;
	while(1) {
		int ret = tokenizer_next(t, &curr);
		if(!ret) return ret;
		if(curr.type == TT_EOF) break;
		if(curr.type == TT_DEC_INT_LIT) {
			*result = atoi(t->buf);
			break;
		}
	}
	return 1;
}

static int evaluate_condition(struct tokenizer *t, int *result) {
	int ret, backslash_seen = 0;
	struct token curr;
	char *bufp;
	size_t size;
	ret = tokenizer_next(t, &curr);
	if(!ret) return ret;
	if(!is_whitespace_token(&curr)) {
		error("expected whitespace after if/elif", t, &curr);
		return 0;
	}
	FILE *f = open_memstream(&bufp, &size);
	while(1) {
		ret = tokenizer_next(t, &curr);
		if(!ret) return ret;
		if(curr.type == TT_IDENTIFIER) {
			if(!expand_macro(t, f, t->buf, 0)) return 0;
		} else if(curr.type == TT_SEP) {
			if(curr.value == '\\')
				backslash_seen = 1;
			else {
				if(curr.value == '\n') {
					if(!backslash_seen) break;
				} else {
					emit_token(f, &curr, t->buf);
				}
				backslash_seen = 0;
			}
		} else {
			emit_token(f, &curr, t->buf);
		}
	}
	f = freopen_r(f, &bufp, &size);
	if(!f || size == 0) {
		error("#(el)if with no expression", t, &curr);
		return 0;
	}
	struct tokenizer t2;
	tokenizer_from_file(&t2, f);
	ret = do_eval(&t2, result);
	fclose(f);
	free(bufp);
	return ret;
}

int parse_file(FILE *f, const char *fn, FILE *out) {
	struct tokenizer t;
	struct token curr;
	tokenizer_init(&t, f, TF_PARSE_STRINGS);
	tokenizer_set_filename(&t, fn);
	tokenizer_register_marker(&t, MT_MULTILINE_COMMENT_START, "/*"); /**/
	tokenizer_register_marker(&t, MT_MULTILINE_COMMENT_END, "*/");
	tokenizer_register_marker(&t, MT_SINGLELINE_COMMENT_START, "//");
	int ret, newline=1, ws_count = 0;

	int if_level = 0, if_level_active = 0, if_level_satisfied = 0;

#define all_levels_active() (if_level_active == if_level)
#define prev_level_active() (if_level_active == if_level-1)
#define set_level(X, V) do { \
		if(if_level_active > X) if_level_active = X; \
		if(if_level_satisfied > X) if_level_satisfied = X; \
		if(V != -1) { \
			if(V) if_level_active = X; \
			else if(if_level_active == X) if_level_active = X-1; \
			if(V && if_level_active == X) if_level_satisfied = X; \
		} \
		if_level = X; \
	} while(0)
#define skip_conditional_block (if_level > if_level_active)

	const char *macro_name = 0;
	static const char* directives[] = {"include", "error", "warning", "define", "undef", "if", "elif", "else", "ifdef", "endif", 0};
	while((ret = tokenizer_next(&t, &curr)) && curr.type != TT_EOF) {
		newline = curr.column == 0;
		if(newline) {
			ret = eat_whitespace(&t, &curr, &ws_count);
		}
		if(!ret || curr.type == TT_EOF) break;
		if(skip_conditional_block && !is_char(&curr, '#')) continue;
		if(is_char(&curr, '#')) {
			if(!newline) {
				error("stray #", &t, &curr);
				return 0;
			}
			int index = expect(&t, TT_IDENTIFIER, directives, &curr);
			if(index == -1) return 1;
			if(skip_conditional_block) switch(index) {
				case 0: case 1: case 2: case 3: case 4:
					continue;
				default: break;
			}
			switch(index) {
			case 0:
				ret = include_file(&t, out);
				if(!ret) return ret;
				break;
			case 1:
				ret = emit_error_or_warning(&t, 1);
				if(!ret) return ret;
				break;
			case 2:
				ret = emit_error_or_warning(&t, 0);
				if(!ret) return ret;
				break;
			case 3:
				ret = parse_macro(&t);
				if(!ret) return ret;
				break;
			case 4:
				if(!skip_next_and_ws(&t, &curr)) return 0;
				if(curr.type != TT_IDENTIFIER) {
					error("expected identifier", &t, &curr);
					return 0;
				}
				undef_macro(t.buf);
				break;
			case 5: // if
				if(all_levels_active()) {
					if(!evaluate_condition(&t, &ret)) return 0;
					set_level(if_level + 1, ret);
				} else {
					set_level(if_level + 1, 0);
				}
				break;
			case 6: // elif
				if(prev_level_active() && if_level_satisfied < if_level) {
					if(!evaluate_condition(&t, &ret)) return 0;
					if(ret) {
						if_level_active = if_level;
						if_level_satisfied = if_level;
					}
				} else if(if_level_active == if_level) {
					--if_level_active;
				}
				break;
			case 7: // else
				if(prev_level_active() && if_level_satisfied < if_level) {
					if(1) {
						if_level_active = if_level;
						if_level_satisfied = if_level;
					}
				} else if(if_level_active == if_level) {
					--if_level_active;
				}
				break;
			case 8: // ifdef
				if(!skip_next_and_ws(&t, &curr) || curr.type == TT_EOF) return 0;
				ret = !!get_macro(t.buf);

				if(all_levels_active()) {
					set_level(if_level + 1, ret);
				} else {
					set_level(if_level + 1, 0);
				}
				break;
			case 9: // endif
				set_level(if_level-1, -1);
			default:
				break;
			}
			continue;
		} else {
			while(ws_count) {
				emit(out, " ");
				--ws_count;
			}
		}
#if DEBUG
		dprintf(1, "(stdin:%u,%u) ", curr.line, curr.column);
		if(curr.type == TT_SEP)
			dprintf(1, "separator: %c\n", curr.value == '\n'? ' ' : curr.value);
		else
			dprintf(1, "%s: %s\n", tokentype_to_str(curr.type), t.buf);
#endif
		if(curr.type == TT_IDENTIFIER) {
			if(!expand_macro(&t, out, t.buf, 0))
				return 0;
		} else {
			emit_token(out, &curr, t.buf);
		}
	}
	if(!ret) {
		error("unknown", &t, &curr);
	}
	if(if_level) {
		error("unterminated #if", &t, &curr);
		return 0;
	}
	return ret;
}

int main(int argc, char** argv) {
	macros = kh_init(macros);
	return !parse_file(stdin, "stdin", stdout);
}
