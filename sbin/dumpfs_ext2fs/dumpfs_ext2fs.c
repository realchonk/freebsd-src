/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/param.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libext2fs.h>

static struct ext2fsd disk;

static int
usage(void)
{
	(void)fprintf(stderr, "usage: dumpfs_ext2fs filesys | device\n");
	return 1;
}

static const char *
ext2fserr(void)
{
	if (disk.d_error != NULL)
		return disk.d_error;
	if (errno)
		return strerror(errno);
	return "unknown error";
}

int
main (int argc, char *argv[])
{
	const char *name;
	int option, eval = 0;

	while ((option = getopt(argc, argv, "")) != -1) {
		switch (option) {
		default:
			return usage();
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1)
		return usage();

	while ((name = *argv++) != NULL) {
		if (ext2fs_disk_open(&disk, name) != 0) {
			printf("\n%s: %s\n", name, ext2fserr());
			eval |= 1;
			continue;
		}
		
		ext2fs_disk_close(&disk);
	}
	return eval;
}
