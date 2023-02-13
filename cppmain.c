#include "preproc.h"
#include <string.h>
#include <unistd.h>

static int usage(char *a0) {
	fprintf(stderr,
			"example preprocessor\n"
			"usage: %s [-I includedir...] [-D define] file\n"
			"if no filename or '-' is passed, stdin is used.\n"
			, a0);
	return 1;
}

int main(int argc, char** argv) {
	int c; char* tmp;
	struct cpp* cpp = cpp_new();
	char *fn = "stdin";
	char *fnarg = NULL;
	int fnargc = 0;
	FILE *in = stdin;
	while (optind < argc) {
		if ((c = getopt(argc, argv, "D:I:")) != EOF) switch(c) {
		case 'I': cpp_add_includedir(cpp, optarg); break;
		case 'D':
			if((tmp = strchr(optarg, '='))) *tmp = ' ';
			cpp_add_define(cpp, optarg);
			break;
		default: return usage(argv[0]);
		}
		else {
			if (fnargc) return usage(argv[0]);
			if (strcmp(argv[optind], "-")) {
				fnarg = argv[optind];
			}
			fnargc++;
			optind++;
		}
	}
	if (fnarg) {
		fn = fnarg;
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

