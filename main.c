#include <string.h>
#include "tokenizer.h"

int main(int argc, char** argv) {
	struct tokenizer t;
	struct token curr;
	tokenizer_init(&t, stdin);
	tokenizer_register_marker(&t, MT_MULTILINE_COMMENT_START, "/*");
	tokenizer_register_marker(&t, MT_MULTILINE_COMMENT_END, "*/");
	tokenizer_register_marker(&t, MT_SINGLELINE_COMMENT_START, "//");
	int ret;
	while((ret = tokenizer_next(&t, &curr)) && curr.type != TT_EOF) {
		dprintf(1, "(stdin:%u,%u) ", curr.line, curr.column);
		if(curr.type == TT_SEP)
			dprintf(1, "separator: %c\n", curr.value == '\n'? ' ' : curr.value);
		else
			dprintf(1, "%s: %s\n", tokentype_to_str(curr.type), t.buf);
	}
	if(!ret) {
		dprintf(2, "error occured on %u:%u\n", curr.line, curr.column);
		dprintf(2, "%s\n", t.buf);
		for(int i = 0; i < strlen(t.buf); i++)
			dprintf(2, "^");
		dprintf(2, "\n");
	}
}

