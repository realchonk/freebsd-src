/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * This software was developed by Benjamin Stürz <benni@stuerz.xyz>
 * under sponsorship from the FreeBSD Foundation.
 */

/*
 * tunefs_ext2fs: change parameters of an existing ext2 file system.
 */

#include <sys/param.h>

#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void) __dead2;

int
main(int argc, char *argv[])
{
	const char *evalue, *ivalue, *Lvalue;
	const char *Mvalue, *Tvalue, *Uvalue;
	const char *special;
	const char *name;
	int Cflag, cflag, eflag, iflag;
	int Lflag, Mflag, mflag, rflag;
	int Tflag, Uflag;
	int pflag;
	int Cvalue, cvalue, mvalue, rvalue;
	int ch, found_arg, i;

	Cflag = cflag = eflag = iflag = Lflag = Mflag = mflag = rflag = 0;
	Tflag = Uflag = pflag = 0;
	Cvalue = cvalue = mvalue = rvalue = 0;
	evalue = ivalue = Lvalue = Mvalue = Tvalue = Uvalue = NULL;
	found_arg = 0;
	while ((ch = getopt(argc, argv, "C:c:e:i:L:M:m:pr:T:U:")) != -1)
		switch (ch) {

		case 'C':
			found_arg++;
			name = "mount count";
			Cvalue = atoi(optarg);
			if (Cvalue < 0)
				errx(10, "bad %s (%s)", name, optarg);
			Cflag = 1;
			break;

		case 'c':
			found_arg++;
			name = "maximum mount count";
			cvalue = atoi(optarg);
			if (cvalue < 0)
				errx(10, "bad %s (%s)", name, optarg);
			cflag = 1;
			break;

		case 'e':
			found_arg++;
			name = "behavior on errors";
			evalue = optarg;
			if (strcmp(evalue, "continue") &&
			    strcmp(evalue, "remount-ro") &&
			    strcmp(evalue, "panic"))
				errx(10, "bad %s (options are %s)", name,
				    "`continue', `remount-ro', or `panic'");
			eflag = 1;
			break;

		case 'i':
			found_arg++;
			name = "interval between checks";
			ivalue = optarg;
			iflag = 1;
			break;

		case 'L':
			found_arg++;
			name = "volume label";
			Lvalue = optarg;
			i = -1;
			while (isalnum(Lvalue[++i]) || Lvalue[i] == '_' ||
			    Lvalue[i] == '-')
				;
			if (Lvalue[i] != '\0')
				errx(10, "bad %s. Valid characters are "
				    "alphanumerics, dashes, and underscores.",
				    name);
			if (strlen(Lvalue) >= 16)
				errx(10, "bad %s. Length is longer than 15.",
				    name);
			Lflag = 1;
			break;

		case 'M':
			found_arg++;
			name = "last mounted directory";
			Mvalue = optarg;
			Mflag = 1;
			break;

		case 'm':
			found_arg++;
			name = "minimum percentage of free space";
			mvalue = atoi(optarg);
			if (mvalue < 0 || mvalue > 99)
				errx(10, "bad %s (%s)", name, optarg);
			mflag = 1;
			break;

		case 'p':
			found_arg++;
			pflag = 1;
			break;

		case 'r':
			found_arg++;
			name = "reserved block count";
			rvalue = atoi(optarg);
			if (rvalue < 0)
				errx(10, "bad %s (%s)", name, optarg);
			rflag = 1;
			break;

		case 'T':
			found_arg++;
			name = "last checked time";
			Tvalue = optarg;
			Tflag = 1;
			break;

		case 'U':
			found_arg++;
			name = "UUID";
			Uvalue = optarg;
			Uflag = 1;
			break;

		default:
			usage();
		}
	argc -= optind;
	argv += optind;
	if (found_arg == 0 || argc != 1)
		usage();

	special = argv[0];
	(void)special;

	/* TODO: open the file system and apply the requested changes. */
	return (0);
}

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: %s [-C mount-count] [-c max-mount-count]\n"
	    "       [-e continue|remount-ro|panic] [-i interval]\n"
	    "       [-L volume-label] [-M last-mounted-directory]\n"
	    "       [-m reserved-percent] [-p] [-r reserved-blocks]\n"
	    "       [-T last-check-time] [-U uuid] filesys\n", getprogname());
	exit(1);
}
