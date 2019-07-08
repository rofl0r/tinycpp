#include "preproc.h"
#include <string.h>
#include <unistd.h>

static int usage(char *a0) {
	fprintf(stderr,
			"example preprocessor\n"
			"usage: %s [-I includedir...] file\n"
			"if no filename or '-' is passed, stdin is used.\n"
			, a0);
	return 1;
}

int main(int argc, char** argv) {
	int c;
	struct cpp* cpp = cpp_new();
	while ((c = getopt(argc, argv, "I:")) != EOF) switch(c) {
	case 'I': cpp_add_includedir(cpp, optarg); break;
	default: return usage(argv[0]);
	}
	char *fn = "stdin";
	FILE *in = stdin;
	if(argv[optind] && strcmp(argv[optind], "-")) {
		fn = argv[optind];
		in = fopen(fn, "r");
		if(!in) {
			perror("fopen");
			return 1;
		}
	}
	int ret = cpp_run(cpp, in, stdout, fn);
	cpp_free(cpp);
	if(in != stdin) fclose(in);
	return !ret;
}

