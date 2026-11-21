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
#include <sys/mount.h>

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
#include <uuid.h>

#include <libext2fs.h>
#include <mntopts.h>

static struct ext2fsd disk;

static const char *errbehavior(uint16_t);
static uint64_t fs_bcount(const struct ext2fs *);
static uint64_t fs_rbcount(const struct ext2fs *);
static void fs_set_rbcount(struct ext2fs *, uint64_t);
static void printfs(void);
static off_t sblockloc(const struct ext2fsd *, uint32_t);
static const char *timestr(time_t);
static void uuidstr(const uint8_t *, char *, size_t);
static int write_superblocks(struct ext2fsd *);
static void usage(void) __dead2;

int
main(int argc, char *argv[])
{
	const char *evalue, *Lvalue;
	const char *Mvalue;
	const char *special;
	const char *name;
	char *end;
	struct ext2fs *fs;
	struct statfs *mnt;
	uint8_t uuid[16];
	int Cflag, cflag, eflag, fflag, iflag;
	int Lflag, Mflag, mflag, rflag;
	int Tflag, Uflag;
	int pflag;
	int Cvalue, cvalue, mvalue;
	uint32_t tvalue;
	uint64_t ivalue, mult, rvalue;
	uint16_t state;
	int ch, changed, found_arg, i;

	Cflag = cflag = eflag = fflag = iflag = Lflag = Mflag = mflag = rflag = 0;
	Tflag = Uflag = pflag = 0;
	Cvalue = cvalue = mvalue = 0;
	tvalue = 0;
	ivalue = mult = rvalue = 0;
	evalue = Lvalue = Mvalue = NULL;
	memset(uuid, 0, sizeof(uuid));
	changed = found_arg = 0;
	while ((ch = getopt(argc, argv, "C:c:e:fi:L:M:m:pr:T:U:")) != -1)
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
			if (cvalue < 0 || cvalue > UINT16_MAX)
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

		case 'f':
			found_arg++;
			fflag = 1;
			break;

		case 'i':
			found_arg++;
			name = "interval between checks";
			errno = 0;
			ivalue = strtoull(optarg, &end, 10);
			if (optarg[0] == '-' || *optarg == '\0' || errno != 0)
				errx(10, "bad %s (%s)", name, optarg);
			if (*end == '\0' || strcmp(end, "d") == 0)
				mult = 86400;
			else if (strcmp(end, "m") == 0)
				mult = 2592000;
			else if (strcmp(end, "w") == 0)
				mult = 604800;
			else
				errx(10, "bad %s (%s)", name, optarg);
			if (ivalue > UINT32_MAX / mult)
				errx(10, "bad %s (%s)", name, optarg);
			ivalue *= mult;
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
			if (strlen(Mvalue) >= sizeof(disk.d_fs.e2fs_fsmnt))
				errx(10, "bad %s. Length is longer than %zu.",
				    name, sizeof(disk.d_fs.e2fs_fsmnt) - 1);
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
			errno = 0;
			rvalue = strtoull(optarg, &end, 0);
			if (optarg[0] == '-' || *optarg == '\0' || errno != 0 ||
			    *end != '\0')
				errx(10, "bad %s (%s)", name, optarg);
			rflag = 1;
			break;

		case 'T': {
			struct tm tm;
			time_t t;
			char *p;

			found_arg++;
			name = "last checked time";
			if (strcmp(optarg, "now") == 0) {
				t = time(NULL);
			} else {
				memset(&tm, 0, sizeof(tm));
				p = strptime(optarg, "%Y%m%d%H%M%S", &tm);
				if (p == NULL || *p != '\0')
					errx(10, "bad %s (%s)", name, optarg);
				tm.tm_isdst = -1;
				t = mktime(&tm);
			}
			if (t == (time_t)-1 || t < 0 ||
			    (uint64_t)t > UINT32_MAX)
				errx(10, "bad %s (%s)", name, optarg);
			tvalue = (uint32_t)t;
			Tflag = 1;
			break;
		}

		case 'U': {
			uint32_t status;
			uuid_t u;

			found_arg++;
			name = "UUID";
			if (strcmp(optarg, "clear") == 0) {
				memset(uuid, 0, sizeof(uuid));
			} else if (strcmp(optarg, "random") == 0) {
				arc4random_buf(uuid, sizeof(uuid));
				uuid[6] = (uuid[6] & 0x0f) | 0x40;
				uuid[8] = (uuid[8] & 0x3f) | 0x80;
			} else if (strcmp(optarg, "time") == 0) {
				uuid_create(&u, &status);
				if (status != uuid_s_ok)
					errx(10, "bad %s (%s)", name, optarg);
				uuid_enc_be(uuid, &u);
			} else {
				uuid_from_string(optarg, &u, &status);
				if (status != uuid_s_ok)
					errx(10, "bad %s (%s)", name, optarg);
				uuid_enc_be(uuid, &u);
			}
			Uflag = 1;
			break;
		}

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

	if ((mnt = getmntpoint(disk.d_name)) != NULL)
		errx(1, "%s is mounted on %s", special, mnt->f_mntonname);

	fs = &disk.d_fs;
	if (!fflag) {
		state = le16toh(fs->e2fs_state);
		if ((le32toh(fs->e2fs_features_incompat) &
		    EXT2F_INCOMPAT_RECOVER) != 0 ||
		    (state & E2FS_ISCLEAN) == 0)
			errx(1, "%s is not clean - run fsck", special);
		if ((state & E2FS_ERRORS) != 0)
			errx(1, "%s has errors - run fsck", special);
	}
	if (Cflag) {
		fs->e2fs_mnt_count = htole16((uint16_t)Cvalue);
		changed = 1;
	}
	if (cflag) {
		fs->e2fs_max_mnt_count = htole16((uint16_t)cvalue);
		changed = 1;
	}
	if (eflag) {
		if (strcmp(evalue, "continue") == 0)
			fs->e2fs_beh = htole16(1);
		else if (strcmp(evalue, "remount-ro") == 0)
			fs->e2fs_beh = htole16(2);
		else
			fs->e2fs_beh = htole16(3);
		changed = 1;
	}
	if (Lflag) {
		memset(fs->e2fs_vname, 0, sizeof(fs->e2fs_vname));
		strlcpy(fs->e2fs_vname, Lvalue, sizeof(fs->e2fs_vname));
		changed = 1;
	}
	if (Mflag) {
		memset(fs->e2fs_fsmnt, 0, sizeof(fs->e2fs_fsmnt));
		strlcpy(fs->e2fs_fsmnt, Mvalue, sizeof(fs->e2fs_fsmnt));
		changed = 1;
	}
	if (mflag) {
		fs_set_rbcount(fs, fs_bcount(fs) * (uint64_t)mvalue / 100);
		changed = 1;
	}
	if (rflag) {
		if (rvalue > fs_bcount(fs))
			errx(10, "bad reserved block count (%ju)",
			    (uintmax_t)rvalue);
		fs_set_rbcount(fs, rvalue);
		changed = 1;
	}
	if (iflag) {
		fs->e2fs_fsckintv = htole32((uint32_t)ivalue);
		changed = 1;
	}
	if (Tflag) {
		fs->e2fs_lastfsck = htole32(tvalue);
		changed = 1;
	}
	if (Uflag && memcmp(fs->e2fs_uuid, uuid, sizeof(fs->e2fs_uuid)) != 0) {
		if ((le32toh(fs->e2fs_features_rocompat) &
		    EXT2F_ROCOMPAT_METADATA_CKSUM) != 0 &&
		    (le32toh(fs->e2fs_features_incompat) &
		    EXT2F_INCOMPAT_CSUM_SEED) == 0) {
			fs->e4fs_chksum_seed = htole32(ext2_csum_seed(fs));
			fs->e2fs_features_incompat = htole32(
			    le32toh(fs->e2fs_features_incompat) |
			    EXT2F_INCOMPAT_CSUM_SEED);
		}
		memcpy(fs->e2fs_uuid, uuid, sizeof(fs->e2fs_uuid));
		changed = 1;
	}

	if (changed) {
		if (ext2fs_disk_write(&disk) == -1)
			errx(12, "%s: %s", special, disk.d_error);
		if (write_superblocks(&disk) == -1)
			errx(13, "%s: %s", special, disk.d_error);
	}
	ext2fs_disk_close(&disk);
	return (0);
}

static off_t
sblockloc(const struct ext2fsd *disk, uint32_t cg)
{
	uint32_t bsize;
	uint64_t first;

	bsize = 1024u << le32toh(disk->d_fs.e2fs_log_bsize);
	first = le32toh(disk->d_fs.e2fs_first_dblock) +
	    (uint64_t)cg * le32toh(disk->d_fs.e2fs_bpg);
	return ((off_t)first * bsize + (bsize == 1024 ? 0 : SBLOCKOFFSET));
}

static int
write_superblocks(struct ext2fsd *disk)
{
	struct ext2fs *fs;
	uint32_t cg;

	fs = &disk->d_fs;
	if (ext2fs_gdread(disk) == -1)
		return (-1);
	if (ext2fs_sbwrite(disk, disk->d_sblockloc) == -1)
		return (-1);
	for (cg = 1; cg < disk->d_gcount; cg++) {
		if (!ext2fs_cg_hassb(disk, cg))
			continue;
		fs->e2fs_block_group_nr = htole16((uint16_t)cg);
		if (ext2fs_sbwrite(disk, sblockloc(disk, cg)) == -1)
			return (-1);
	}
	fs->e2fs_block_group_nr = htole16(0);
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

static uint64_t
fs_bcount(const struct ext2fs *fs)
{
	uint64_t bcount;

	bcount = le32toh(fs->e2fs_bcount);
	if (le32toh(fs->e2fs_features_incompat) & EXT2F_INCOMPAT_64BIT)
		bcount |= (uint64_t)le32toh(fs->e4fs_bcount_hi) << 32;
	return (bcount);
}

static uint64_t
fs_rbcount(const struct ext2fs *fs)
{
	uint64_t rbcount;

	rbcount = le32toh(fs->e2fs_rbcount);
	if (le32toh(fs->e2fs_features_incompat) & EXT2F_INCOMPAT_64BIT)
		rbcount |= (uint64_t)le32toh(fs->e4fs_rbcount_hi) << 32;
	return (rbcount);
}

static void
fs_set_rbcount(struct ext2fs *fs, uint64_t rbcount)
{

	fs->e2fs_rbcount = htole32((uint32_t)rbcount);
	if (le32toh(fs->e2fs_features_incompat) & EXT2F_INCOMPAT_64BIT)
		fs->e4fs_rbcount_hi = htole32((uint32_t)(rbcount >> 32));
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
	bcount = fs_bcount(fs);
	rbcount = fs_rbcount(fs);
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
	    "       [-e continue|remount-ro|panic] [-f] [-i interval]\n"
	    "       [-L volume-label] [-M last-mounted-directory]\n"
	    "       [-m reserved-percent] [-p] [-r reserved-blocks]\n"
	    "       [-T last-check-time] [-U uuid] filesys\n", getprogname());
	exit(1);
}
