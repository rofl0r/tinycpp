#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "tokenizer.h"
#include "../cdev/cdev/agsutils/List.h"
#include "khash.h"

#define TT_MACRO_ARGUMENT (TT_CUSTOM + 1)
struct token_str_tup {
	struct token tok;
	const char *strbuf;
};
struct macro_content {
	enum tokentype type;
	union {
		struct token_str_tup tokstr;
		unsigned arg_nr;
	};
};

struct macro {
	unsigned num_args;
	/* XXX */ char marker[4];
	MG str_contents;
	List /*<struct macro_content>*/ macro_contents;
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
	}
	return 0;
}

static void tokenizer_from_file(struct tokenizer *t, FILE* f) {
	tokenizer_init(t, f, TF_PARSE_STRINGS);
	tokenizer_set_filename(t, "<macro>");
}
static void tokenizer_from_mem(struct tokenizer *t, MG* mem, FILE** fout) {
	void* memptr = mem_getptr(mem, 0, 1);
	*fout = fmemopen(memptr ? memptr : "", mem->used, "r");
	tokenizer_from_file(t, *fout);
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

static void tokstr_fill(struct token_str_tup *dst, struct tokenizer *t, struct token *src) {
	dst->tok = *src;
	dst->strbuf = 0;
	if(token_needs_string(src))
		dst->strbuf = strdup(t->buf);
}

KHASH_MAP_INIT_STR(macro_exp_level, unsigned)

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
		kh_del(macros, macros, k);
	}
	kh_value(macros, k) = *m;
	return !absent;
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

static int eat_whitespace(struct tokenizer *t, struct token *token, int *count) {
	*count = 0;
	int ret = 1;
	while (is_whitespace_token(token)) {
		*count++;
		ret = x_tokenizer_next(t, token);
		if(!ret) break;
	}
	return ret;
}

static void emit(FILE *out, const char *s) {
	fprintf(out, "%s", s);
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
	struct macro new = { .marker = "ABCD", 0 };
	List argnames;
	List_init(&argnames, sizeof(char*));

	ret = x_tokenizer_next(t, &curr) && curr.type != TT_EOF;
	if(!ret) return ret;

	List_init(&new.macro_contents, sizeof(struct macro_content));
	mem_init(&new.str_contents);

	if (is_char(&curr, '(')) {
		while(1) {
			ret = x_tokenizer_next(t, &curr) && curr.type != TT_EOF;
			if(!ret) return ret;
			if(curr.type != TT_IDENTIFIER) {
				error("expected identifier for macro arg", t, &curr);
				return 0;
			}
			{
				const char *tmps = strdup(t->buf);
				List_add(&argnames, &tmps);
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
	} else if (is_whitespace_token(&curr)) {
		/* do nothing */
	} else {
		error("unexpected!", t, &curr);
	}
	int backslash_seen = 0;
	while(1) {
		ret = x_tokenizer_next(t, &curr) && curr.type != TT_EOF;
		if(!ret) return ret;
		{
			char tbuf[2], *foo = tbuf, **str = &foo;
			size_t len = token_as_string(t, &curr, str);
			mem_append(&new.str_contents, *str, len);
		}
		struct macro_content cont = {0};
		if(curr.type == TT_IDENTIFIER) {
			backslash_seen = 0;
			size_t i;
			for(i=0; i<new.num_args; i++) {
				const char *item;
				List_get(&argnames, i, &item);
				if(!strcmp(item, t->buf)) {
					cont.type = TT_MACRO_ARGUMENT;
					cont.arg_nr = i;
					break;
				}
			}
			/* no argument, expand if needed*/
			if(cont.type != TT_MACRO_ARGUMENT && get_macro(t->buf)) {
				char *bufp;
				size_t size;
				FILE *tokf = open_memstream(&bufp, &size);
				if(!expand_macro(t, tokf, t->buf, 0)) return 0;
				fflush(tokf);
				fclose(tokf);
				tokf = fmemopen(bufp, size, "r");
				struct tokenizer t2;
				tokenizer_from_file(&t2, tokf);
				while(1) {
					ret = x_tokenizer_next(&t2, &curr);
					if(curr.type == TT_EOF) break;
					tokstr_fill(&cont.tokstr, &t2, &curr);
					List_add(&new.macro_contents, &cont);
				}
				fclose(tokf);
				free(bufp);
				continue;
			}
		} else if (curr.type == TT_SEP) {
			if(curr.value == '\\')
				backslash_seen = 1;
			else {
				if(curr.value == '\n' && !backslash_seen) break;
				backslash_seen = 0;
			}
		}
		if(cont.type != TT_MACRO_ARGUMENT && !(is_char(&curr,  '\\'))) {
			cont.type = curr.type;
			tokstr_fill(&cont.tokstr, t, &curr);
		}
		if(!backslash_seen) List_add(&new.macro_contents, &cont);
	}
	add_macro(macroname, &new);
	//ret = tokenizer_skip_chars(t, " \t", &ws_count);
	return 1;
}

#define MAX_RECURSION 32

//int expand_macro(struct tokenizer *t, khash_t(macro_exp_level) *m_exp, unsigned rec_level) {
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

	//if( get_macro_exp_level(m_exp, t->buf, &exp_lvl) && );
	size_t i;
	struct token tok;

	List *argvalues = malloc(sizeof(List) * m->num_args);
	for(i=0; i < m->num_args; i++)
		List_init(&argvalues[i], sizeof (struct token_str_tup));

	if(m->num_args) {
		if(expect(t, TT_SEP, (const char*[]){"(", 0}, &tok) != 0) {
			error("expected (", t, &tok);
			return 0;
		}
		unsigned curr_arg = 0, need_arg = 1, parens = 0;
		while(1) {
			int ret = x_tokenizer_next(t, &tok) && tok.type != TT_EOF;
			if(!ret) return 0;
			if(need_arg && !parens && is_char(&tok, ',')) {
				error("unexpected: ','", t, &tok);
				return 0;
			} else if(!parens && is_char(&tok, ',')) {
				need_arg = 1;
				curr_arg++;
				if(curr_arg >= m->num_args) {
					error("too many arguments for function macro", t, &tok);
					return 0;
				}
				unsigned ws_count;
				ret = tokenizer_skip_chars(t, " \t", &ws_count);
				if(!ret) return ret;
			} else if(is_char(&tok, '(')) {
				++parens;
				goto append;
			} else if(is_char(&tok, ')')) {
				if(!parens) {
					if(curr_arg != m->num_args-1) {
						error("too few args for function macro", t, &tok);
						return 0;
					}
					break;
				}
				--parens;
				goto append;
#if 0
			} else if (tok.type == TT_IDENTIFIER) {
				ret = expand_macro(t, t->buf, rec_level+1);
				if(!ret) return ret;
				need_arg = 0;
#endif
			} else {
	append:;
				struct token_str_tup tmp;
				tokstr_fill(&tmp, t, &tok);
				List_add(&argvalues[curr_arg], &tmp);
				need_arg = 0;
			}
		}
	}
	size_t j;
	for(i=0; i<List_size(&m->macro_contents); i++) {
		struct macro_content *mc = List_getptr(&m->macro_contents, i);
		if(mc->type == TT_MACRO_ARGUMENT) {
			for(j=0; j < List_size(&argvalues[mc->arg_nr]); j++) {
				struct token_str_tup tmp;
				assert(List_get(&argvalues[mc->arg_nr], j, &tmp));
				if(tmp.tok.type == TT_IDENTIFIER) {
					struct tokenizer t2;
					FILE *t2f;
					tokenizer_from_mem(&t2, &m->str_contents, &t2f);
					if(!expand_macro(&t2, out, tmp.strbuf, rec_level+1))
						return 0;
					fclose(t2f);
				} else
					emit_token(out, &tmp.tok, tmp.strbuf);
			}
		} else {
			emit_token(out, &mc->tokstr.tok, mc->tokstr.strbuf);
		}
	}
	return 1;
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
	const char *macro_name = 0;
	static const char* directives[] = {"include", "error", "warning", "define", "undef", "if", "elif", "ifdef", "endif", 0};
	while((ret = tokenizer_next(&t, &curr)) && curr.type != TT_EOF) {
		newline = curr.column == 0;
		if(newline) {
			ret = eat_whitespace(&t, &curr, &ws_count);
			if(ws_count) emit(out, " ");
		}
		if(!ret || curr.type == TT_EOF) break;
		if(curr.type == TT_SEP && curr.value == '#') {
			if(!newline) {
				error("stray #", &t, &curr);
				return 0;
			}
			int index = expect(&t, TT_IDENTIFIER, directives, &curr);
			if(index == -1) return 1;
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
				//remove_macro(&t);
				break;
			case 5:
				// tokenizer_skip_until
				//evaluate_condition(&t, );
			default:
				break;
			}
			continue;
		}
#if DEBUG
		dprintf(1, "(stdin:%u,%u) ", curr.line, curr.column);
		if(curr.type == TT_SEP)
			dprintf(1, "separator: %c\n", curr.value == '\n'? ' ' : curr.value);
		else
			dprintf(1, "%s: %s\n", tokentype_to_str(curr.type), t.buf);
#endif
		if(curr.type == TT_IDENTIFIER) {
			khash_t(macro_exp_level) *macro_exp_level = kh_init(macro_exp_level);
			if(!expand_macro(&t, out, t.buf, 0))
				return 0;
		} else {
			emit_token(out, &curr, t.buf);
		}
	}
	if(!ret) {
		error("unknown", &t, &curr);
	}
	return ret;
}

int main(int argc, char** argv) {
	macros = kh_init(macros);
	return !parse_file(stdin, "stdin", stdout);
}
