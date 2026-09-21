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

/*
 * Fatal inconsistency: abort in preen ("run fsck manually"), else just
 * report and let the caller decide.
 */
void
pfatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (!preen) {
		vfprintf(stdout, fmt, ap);
		va_end(ap);
		return;
	}
	if (cdevname == NULL)
		cdevname = "fsck";
	fprintf(stdout, "%s: ", cdevname);
	vfprintf(stdout, fmt, ap);
	fprintf(stdout, "\n%s: UNEXPECTED INCONSISTENCY; RUN fsck MANUALLY.\n",
	    cdevname);
	ckfini(0);
	exit(EEXIT);
}

void
pwarn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (preen)
		fprintf(stdout, "%s: ", cdevname);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
}

/*
 * Print the question and take the -y/-n/preen/interactive answer.
 * Persevere questions (CONTINUE) are answered even in -n mode.
 */
int
reply(const char *question)
{
	int persevere;
	char c;

	if (preen)
		pfatal("INTERNAL ERROR: GOT TO reply()");
	persevere = strcmp(question, "CONTINUE") == 0 ||
	    strcmp(question, "LOOK FOR ALTERNATE SUPERBLOCKS") == 0;
	printf("\n");
	if (!persevere && (nflag || fswritefd < 0)) {
		printf("%s? no\n\n", question);
		return (0);
	}
	if (yflag || (persevere && nflag)) {
		printf("%s? yes\n\n", question);
		return (1);
	}
	do {
		printf("%s? [yn] ", question);
		(void)fflush(stdout);
		c = getchar();
		while (c != '\n' && getchar() != '\n') {
			if (feof(stdin))
				return (0);
		}
	} while (c != 'y' && c != 'Y' && c != 'n' && c != 'N');
	printf("\n");
	if (c == 'y' || c == 'Y')
		return (1);
	return (0);
}

/*
 * Determine whether the problem msg may be fixed: preen salvages,
 * -y answers yes once and sticks, -n or no write access never fixes.
 * A declined or impossible fix leaves the filesystem uncorrected.
 */
int
dofix(struct inodesc *idesc, const char *msg)
{

	switch (idesc->id_fix) {
	case DONTKNOW:
		pwarn("%s", msg);
		if (preen) {
			printf(" (SALVAGED)\n");
			idesc->id_fix = FIX;
			return (ALTERED);
		}
		if (reply("SALVAGE") == 0) {
			idesc->id_fix = NOFIX;
			uncorrected = 1;
			return (0);
		}
		idesc->id_fix = FIX;
		return (ALTERED);
	case FIX:
		return (ALTERED);
	case NOFIX:
	case IGNORE:
		uncorrected = 1;
		return (0);
	default:
		errx(EEXIT, "UNKNOWN FIX STATE %d", idesc->id_fix);
	}
}

void
ckfini(int markclean)
{

	(void)markclean;
	ext2fs_disk_close(&disk);
}
