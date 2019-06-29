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

#define MACRO_FLAG_OBJECTLIKE 1U<<31
#define MACRO_ARGCOUNT_MASK ~(0|(MACRO_FLAG_OBJECTLIKE))

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
	tokenizer_rewind(t);
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

static int is_whitespace_token(struct token *token)
{
	return token->type == TT_SEP &&
		(token->value == ' ' || token->value == '\t');
}

/* return index of matching item in values array, or -1 on error */
static int expect(struct tokenizer *t, enum tokentype tt, const char* values[], struct token *token)
{
	int ret;
	do {
		ret = tokenizer_next(t, token);
		if(ret == 0 || token->type == TT_EOF) goto err;
	} while(is_whitespace_token(token));

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

static int consume_nl_and_ws(struct tokenizer *t, struct token *tok, int expected) {
	if(!x_tokenizer_next(t, tok)) {
err:
		error("unexpected", t, tok);
		return 0;
	}
	if(expected) {
		if(tok->type != TT_SEP || tok->value != expected) goto err;
		switch(expected) {
			case '\\' : expected = '\n'; break;
			case '\n' : expected = 0; break;
		}
	} else {
		if(is_whitespace_token(tok)) ;
		else if(is_char(tok, '\\')) expected = '\n';
		else return 1;
	}
	return consume_nl_and_ws(t, tok, expected);
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
#ifdef DEBUG
	dprintf(2, "parsing macro %s\n", macroname);
#endif
	if(get_macro(macroname)) {
		char buf[128];
		sprintf(buf, "redefinition of macro %s", macroname);
		warning(buf, t, 0);
	}

	struct macro new = { 0 };
	unsigned macro_flags = MACRO_FLAG_OBJECTLIKE;
	List_init(&new.argnames, sizeof(char*));

	ret = x_tokenizer_next(t, &curr) && curr.type != TT_EOF;
	if(!ret) return ret;

	if (is_char(&curr, '(')) {
		macro_flags = 0;
		unsigned expected = 0;
		while(1) {
			/* process next function argument identifier */
			ret = consume_nl_and_ws(t, &curr, expected);
			if(!ret) {
				error("unexpected", t, &curr);
				return ret;
			}
			expected = 0;
			if(curr.type == TT_SEP) {
				switch(curr.value) {
				case '\\':
					expected = '\n';
					continue;
				case ',':
					continue;
				case ')':
					ret = tokenizer_skip_chars(t, " \t", &ws_count);
					if(!ret) return ret;
					goto break_loop1;
				default:
					error("unexpected character", t, &curr);
					return 0;
				}
			} else if(curr.type != TT_IDENTIFIER) {
				error("expected identifier for macro arg", t, &curr);
				return 0;
			}
			{
				const char *tmps = strdup(t->buf);
				List_add(&new.argnames, &tmps);
			}
			++new.num_args;
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
	new.num_args |= macro_flags;
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


struct macro_info {
	const char *name;
	unsigned nest;
	unsigned first;
	unsigned last;
};

unsigned get_macro_info(struct tokenizer *t,
	struct macro_info *mi_list, size_t *mi_cnt,
	unsigned nest, unsigned tpos, const char *name) {
	unsigned brace_lvl = 0;
	while(1) {
		struct token tok;
		int ret = tokenizer_next(t, &tok);
		if(!ret || tok.type == TT_EOF) break;
#ifdef DEBUG
		dprintf(2, "(%s) nest %d, brace %zu t: %s\n", name, nest, brace_lvl, t->buf);
#endif
		struct macro* m = 0;
		if(tok.type == TT_IDENTIFIER && (m = get_macro(t->buf))) {
			const char* newname = strdup(t->buf);
			if(!(m->num_args & MACRO_FLAG_OBJECTLIKE)) {
				if(tokenizer_peek(t) == '(') {
					unsigned tpos_save = tpos;
					tpos = get_macro_info(t, mi_list, mi_cnt, nest+1, tpos+1, newname);
					mi_list[*mi_cnt] = (struct macro_info) {
						.name = newname,
						.nest=nest+1,
						.first = tpos_save,
						.last = tpos + 1};
					++(*mi_cnt);
				}
			} else {
				mi_list[*mi_cnt] = (struct macro_info) {
					.name = newname,
					.nest=nest+1,
					.first = tpos,
					.last = tpos + 1};
				++(*mi_cnt);
			}
		} else if(is_char(&tok, '(')) {
			++brace_lvl;
		} else if(is_char(&tok, ')')) {
			--brace_lvl;
			if(brace_lvl == 0) break;
		}
		++tpos;
	}
	return tpos;
}

struct FILE_container {
	FILE *f;
	char *buf;
	size_t len;
	struct tokenizer t;
};

static void free_file_container(struct FILE_container *fc) {
	fclose(fc->f);
	free(fc->buf);
}

static int mem_tokenizers_join(
	struct FILE_container* org, struct FILE_container *inj,
	struct FILE_container* result,
	unsigned first, unsigned last) {
	result->f = open_memstream(&result->buf, &result->len);
	size_t i;
	struct token tok;
	int ret;
	tokenizer_rewind(&org->t);
	for(i=0; i<first; ++i) {
		ret = tokenizer_next(&org->t, &tok);
		assert(ret && tok.type != TT_EOF);
		emit_token(result->f, &tok, org->t.buf);
	}
	int cnt = 0;
	while(1) {
		ret = tokenizer_next(&inj->t, &tok);
		if(!ret || tok.type == TT_EOF) break;
		emit_token(result->f, &tok, inj->t.buf);
		++cnt;
	}
	int diff = cnt - ((int) last - (int) first);
	for(i = 0; i < last - first; ++i)
		tokenizer_next(&org->t, &tok);
	while(1) {
		ret = tokenizer_next(&org->t, &tok);
		if(!ret || tok.type == TT_EOF) break;
		emit_token(result->f, &tok, org->t.buf);
	}
	result->f = freopen_r(result->f, &result->buf, &result->len);
	tokenizer_from_file(&result->t, result->f);
	return diff;
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
	unsigned num_args = m->num_args & MACRO_ARGCOUNT_MASK;
	struct FILE_container *argvalues = calloc(num_args, sizeof(struct FILE_container));

	for(i=0; i < num_args; i++)
		argvalues[i].f = open_memstream(&argvalues[i].buf, &argvalues[i].len);

	/* replace named arguments in the contents of the macro call */
	if(!(m->num_args & MACRO_FLAG_OBJECTLIKE)) {
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
				if(curr_arg >= num_args) {
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
					if(curr_arg + num_args && curr_arg != num_args-1) {
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

	for(i=0; i < num_args; i++) {
		argvalues[i].f = freopen_r(argvalues[i].f, &argvalues[i].buf, &argvalues[i].len);
		tokenizer_from_file(&argvalues[i].t, argvalues[i].f);
#ifdef DEBUG
		dprintf(2, "macro argument %i: %s\n", (int) i, argvalues[i].buf);
#endif
	}

	if(!m->str_contents) goto cleanup;

	struct FILE_container cwae = {0}; /* contents_with_args_expanded */
	cwae.f = open_memstream(&cwae.buf, &cwae.len);
	FILE* output = cwae.f;

	struct tokenizer t2;
	tokenizer_from_file(&t2, m->str_contents);
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
				tokenizer_rewind(&argvalues[arg_nr].t);
				while(1) {
					ret = x_tokenizer_next(&argvalues[arg_nr].t, &tok);
					if(!ret) return ret;
					if(tok.type == TT_EOF) break;
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
err_hash_3:
			error("only two '#' characters allowed for macro expansion", &t2, &tok);
			return 0;
		}
		/* handle token concatention operator ## by emitting whitespace before/after*/
		if(!is_whitespace_token(&tok) && !ws_count) {
			flush_whitespace(output, &ws_count);
		} else if(ws_count && hash_count == 2) {
glue_eat_ws:
			if(tokenizer_peek(&t2) == '#') goto err_hash_3;
			ret = tokenizer_skip_chars(&t2, " \t", &ws_count);
			if(!ret) return ret;
			if(tokenizer_peek(&t2) == '\n') {
				x_tokenizer_next(&t2, &tok);
				goto glue_eat_ws;
			}
			ws_count = 0;
			hash_count = 0;
		}
	}
	flush_whitespace(output, &ws_count);

	/* we need to expand macros after the macro arguments have been inserted */
	if(1) {
		cwae.f = freopen_r(cwae.f, &cwae.buf, &cwae.len);
#ifdef DEBUG
		dprintf(2, "contents with args expanded: %s\n", cwae.buf);
#endif
		tokenizer_from_file(&cwae.t, cwae.f);
		size_t mac_cnt = 0;
		while(1) {
			int ret = x_tokenizer_next(&cwae.t, &tok);
			if(!ret || tok.type == TT_EOF) break;
			if(tok.type == TT_IDENTIFIER && get_macro(cwae.t.buf))
				++mac_cnt;
		}

		tokenizer_rewind(&cwae.t);
		struct macro_info *mcs = calloc(mac_cnt, sizeof(struct macro_info));
		{
			size_t mac_iter = 0;
			get_macro_info(&cwae.t, mcs, &mac_iter, 0, 0, "null");
			/* some of the macros might not expand at this stage (without braces)*/
			while(mac_cnt && mcs[mac_cnt-1].name == 0)
				--mac_cnt;
		}
		size_t i; int depth = 0;
		for(i = 0; i < mac_cnt; ++i) {
			if(mcs[i].nest > depth) depth = mcs[i].nest;
		}
		while(depth > -1) {
			for(i = 0; i < mac_cnt; ++i) if(mcs[i].nest == depth) {
				struct macro_info *mi = &mcs[i];
				tokenizer_rewind(&cwae.t);
				size_t j;
				struct token utok;
				for(j = 0; j < mi->first+1; ++j)
					tokenizer_next(&cwae.t, &utok);
				struct FILE_container t2 = {0}, tmp = {0};
				t2.f = open_memstream(&t2.buf, &t2.len);
				if(!expand_macro(&cwae.t, t2.f, mi->name, rec_level+1))
					return 0;
				t2.f = freopen_r(t2.f, &t2.buf, &t2.len);
				tokenizer_from_file(&t2.t, t2.f);
				tokenizer_rewind(&cwae.t);
				int diff = mem_tokenizers_join(&cwae, &t2, &tmp, mi->first, mi->last);
				free_file_container(&cwae);
				cwae = tmp;
				if(diff == 0) continue;
				for(j = 0; j < mac_cnt; ++j) {
					if(j == i) continue;
					struct macro_info *mi2 = &mcs[j];
					/* modified element mi can be either inside, after or before
					   another macro. the after case doesn't affect us. */
					if(mi->first >= mi2->first && mi->last <= mi2->last) {
						/* inside m2 */
						mi2->last += diff;
					} else if (mi->first < mi2->first) {
						/* before m2 */
						mi2->first += diff;
						mi2->last += diff;
					}
				}
			}
			--depth;
		}
		tokenizer_rewind(&cwae.t);
		while(1) {
			int ret = x_tokenizer_next(&cwae.t, &tok);
			if(!ret) return ret;
			if(tok.type == TT_EOF) break;
			emit_token(out, &tok, cwae.t.buf);
		}
		free(mcs);
	}

	free_file_container(&cwae);

cleanup:
	for(i=0; i < num_args; i++) {
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

int preprocessor_run(FILE* in, const char* inname, FILE* out) {
	macros = kh_init(macros);
	return parse_file(in, inname, out);
}

int main(int argc, char** argv) {
	return !preprocessor_run(stdin, "stdin", stdout);
}
