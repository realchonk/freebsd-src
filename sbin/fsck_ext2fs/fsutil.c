/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsck_ext2fs.h"

void
pfatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (preen)
		fprintf(stderr, "\nYOU ARE REBOOTING WITH A DIRTY FILESYSTEM\n");
}

void
pwarn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* Print the question and take the -y/-n/preen answer. */
int
reply(const char *question)
{
	int c;

	if (preen)
		pwarn(" (IGNORED)\n");
	printf(" (IGNORED)\n");
	printf("%s? ", question);
	(void)c;
	return (0);
}

int
dofix(struct inodesc *idesc, const char *msg)
{

	(void)idesc;
	pwarn("%s (NOT FIXED)\n", msg);
	return (0);
}

void
ckfini(int markclean)
{

	(void)markclean;
	ext2fs_disk_close(&disk);
}
