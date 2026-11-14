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
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libext2fs.h>

static struct ext2fsd disk;

static const char *errbehavior(uint16_t);
static void printfs(void);
static const char *timestr(time_t);
static void uuidstr(const uint8_t *, char *, size_t);
static void usage(void) __dead2;

int
main(int argc, char *argv[])
{
	const char *evalue, *ivalue, *Lvalue;
	const char *Mvalue, *Tvalue, *Uvalue;
	const char *special;
	const char *name;
	struct ext2fs *fs;
	int Cflag, cflag, eflag, iflag;
	int Lflag, Mflag, mflag, rflag;
	int Tflag, Uflag;
	int pflag;
	int Cvalue, cvalue, mvalue, rvalue;
	int ch, changed, found_arg, i;

	Cflag = cflag = eflag = iflag = Lflag = Mflag = mflag = rflag = 0;
	Tflag = Uflag = pflag = 0;
	Cvalue = cvalue = mvalue = rvalue = 0;
	evalue = ivalue = Lvalue = Mvalue = Tvalue = Uvalue = NULL;
	changed = found_arg = 0;
	while ((ch = getopt(argc, argv, "C:c:e:i:L:M:m:pr:T:U:")) != -1)
		switch (ch) {

		case 'C':
			found_arg++;
			name = "mount count";
			Cvalue = atoi(optarg);
			if (Cvalue < 0 || Cvalue > UINT16_MAX)
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
	if (ext2fs_disk_fillout(&disk, special) == -1) {
		if (disk.d_error != NULL)
			errx(11, "%s: %s", special, disk.d_error);
		if (errno != 0)
			err(12, "%s", special);
		errx(12, "%s: unknown error", special);
	}
	if (pflag) {
		printfs();
		ext2fs_disk_close(&disk);
		exit(0);
	}

	fs = &disk.d_fs;
	if (Cflag) {
		fs->e2fs_mnt_count = htole16((uint16_t)Cvalue);
		changed = 1;
	}

	if (changed) {
		if (ext2fs_disk_write(&disk) == -1)
			errx(12, "%s: %s", special, disk.d_error);
		if (ext2fs_sbwrite(&disk, disk.d_sblockloc) == -1)
			errx(13, "%s: %s", special, disk.d_error);
	}
	ext2fs_disk_close(&disk);
	return (0);
}

static const char *
errbehavior(uint16_t behavior)
{

	switch (behavior) {
	case 1:
		return ("continue");
	case 2:
		return ("remount-ro");
	case 3:
		return ("panic");
	default:
		return ("unknown");
	}
}

static const char *
timestr(time_t t)
{
	static char buf[26];
	char *p;

	p = ctime(&t);
	if (p == NULL)
		return ("unknown");
	strlcpy(buf, p, sizeof(buf));
	buf[strcspn(buf, "\n")] = '\0';
	return (buf);
}

static void
uuidstr(const uint8_t *u, char *buf, size_t len)
{

	snprintf(buf, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
	    "%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5],
	    u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14],
	    u[15]);
}

static void
printfs(void)
{
	struct ext2fs *fs;
	char uuid[37];
	uint64_t bcount, rbcount;
	uint32_t percent;
	time_t t;

	fs = &disk.d_fs;
	bcount = le32toh(fs->e2fs_bcount);
	rbcount = le32toh(fs->e2fs_rbcount);
	if (le32toh(fs->e2fs_features_incompat) & EXT2F_INCOMPAT_64BIT) {
		bcount |= (uint64_t)le32toh(fs->e4fs_bcount_hi) << 32;
		rbcount |= (uint64_t)le32toh(fs->e4fs_rbcount_hi) << 32;
	}
	percent = bcount != 0 ?
	    (uint32_t)((rbcount * 100 + bcount / 2) / bcount) : 0;

	warnx("%-52s %u", "mount count: (-C)",
	    le16toh(fs->e2fs_mnt_count));
	warnx("%-52s %u", "maximum mount count: (-c)",
	    le16toh(fs->e2fs_max_mnt_count));
	warnx("%-52s %s", "behavior on errors: (-e)",
	    errbehavior(le16toh(fs->e2fs_beh)));
	warnx("%-52s %u", "interval between checks: (-i)",
	    le32toh(fs->e2fs_fsckintv));
	warnx("%-52s %.16s", "volume label: (-L)", fs->e2fs_vname);
	warnx("%-52s %.64s", "last mounted directory: (-M)", fs->e2fs_fsmnt);
	warnx("%-52s %u%%", "minimum percentage of free space: (-m)",
	    percent);
	warnx("%-52s %ju", "reserved block count: (-r)",
	    (uintmax_t)rbcount);
	t = (time_t)le32toh(fs->e2fs_lastfsck);
	warnx("%-52s %s", "last checked time: (-T)", timestr(t));
	uuidstr(fs->e2fs_uuid, uuid, sizeof(uuid));
	warnx("%-52s %s", "UUID: (-U)", uuid);
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
