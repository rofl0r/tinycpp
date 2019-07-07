#include "preproc.h"

int main(int argc, char** argv) {
	struct cpp* cpp = cpp_new();
	int ret = cpp_run(cpp, stdin, stdout, "stdin");
	cpp_free(cpp);
	return !ret;
}

