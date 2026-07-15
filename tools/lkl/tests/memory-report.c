// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>

#include <lkl.h>
#include <lkl_host.h>

int main(int argc, char **argv)
{
	const char *memory = "128M";
	long ret;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [memory-size]\n", argv[0]);
		return 2;
	}
	if (argc == 2)
		memory = argv[1];

	ret = lkl_init(&lkl_host_ops);
	if (ret < 0) {
		fprintf(stderr, "lkl_init failed: %s\n", lkl_strerror(ret));
		return 1;
	}

	ret = lkl_start_kernel("mem=%s loglevel=8", memory);
	if (ret < 0) {
		fprintf(stderr, "lkl_start_kernel failed: %s\n",
			lkl_strerror(ret));
		lkl_cleanup();
		return 1;
	}

	ret = lkl_sys_halt();
	if (ret < 0)
		fprintf(stderr, "lkl_sys_halt failed: %s\n",
			lkl_strerror(ret));
	lkl_cleanup();

	return ret < 0 ? 1 : 0;
}
