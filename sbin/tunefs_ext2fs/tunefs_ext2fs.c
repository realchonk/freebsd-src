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
#include <sys/endian.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
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

enum feature_field {
	FEATURE_COMPAT,
	FEATURE_INCOMPAT,
	FEATURE_ROCOMPAT
};

struct feature_opt {
	const char	*name;
	enum feature_field field;
	uint32_t	mask;
	int		set_ok;
	int		clear_ok;
	int		needs_fsck;
};

#define	EXT4_EXT_MAGIC	0xf30a

struct ext4_extent_header {
	uint16_t	eh_magic;
	uint16_t	eh_entries;
	uint16_t	eh_max;
	uint16_t	eh_depth;
	uint32_t	eh_generation;
};

struct ext4_extent {
	uint32_t	e_blk;
	uint16_t	e_len;
	uint16_t	e_start_hi;
	uint32_t	e_start_lo;
};

struct jnl_superblock {
	uint32_t	h_magic;
	uint32_t	h_blocktype;
	uint32_t	h_sequence;
	uint32_t	s_blocksize;
	uint32_t	s_maxlen;
	uint32_t	s_first;
	uint32_t	s_sequence;
	uint32_t	s_start;
	uint32_t	s_errno;
	uint32_t	s_feature_compat;
	uint32_t	s_feature_incompat;
	uint32_t	s_feature_rocompat;
	uint8_t		s_uuid[16];
	uint32_t	s_nr_users;
	uint32_t	s_dynsuper;
	uint32_t	s_max_transaction;
	uint32_t	s_max_trans_data;
	uint8_t		s_checksum_type;
	uint8_t		s_padding2[3];
	uint32_t	s_num_fc_blks;
	uint32_t	s_head;
	uint32_t	s_padding[40];
	uint32_t	s_checksum;
	uint8_t		s_users[16 * 48];
};

static const struct feature_opt feature_opts[] = {
	{ "dir_index",		FEATURE_COMPAT,
	    EXT2F_COMPAT_DIRHASHINDEX,	1, 1, 1 },
	{ "has_journal",	FEATURE_COMPAT,
	    EXT2F_COMPAT_HASJOURNAL,	0, 0, 0 },
	{ "resize_inode",	FEATURE_COMPAT,
	    EXT2F_COMPAT_RESIZE,	0, 1, 1 },
	{ "sparse_super2",	FEATURE_COMPAT,
	    EXT2F_COMPAT_SPARSESUPER2,	0, 0, 0 },
	{ "filetype",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_FTYPE,	1, 1, 1 },
	{ "extent",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_EXTENTS,	0, 0, 0 },
	{ "extents",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_EXTENTS,	0, 0, 0 },
	{ "64bit",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_64BIT,	0, 0, 0 },
	{ "meta_bg",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_META_BG,	0, 0, 0 },
	{ "mmp",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_MMP,		0, 0, 0 },
	{ "flex_bg",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_FLEX_BG,	0, 0, 0 },
	{ "ea_inode",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_EA_INODE,	0, 0, 0 },
	{ "metadata_csum_seed",	FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_CSUM_SEED,	1, 1, 0 },
	{ "large_dir",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_LARGEDIR,	0, 0, 0 },
	{ "inline_data",	FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_INLINE_DATA,	0, 0, 0 },
	{ "encrypt",		FEATURE_INCOMPAT,
	    EXT2F_INCOMPAT_ENCRYPT,	0, 0, 0 },
	{ "sparse_super",	FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_SPARSESUPER,	1, 0, 1 },
	{ "large_file",		FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_LARGEFILE,	1, 1, 0 },
	{ "huge_file",		FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_HUGE_FILE,	1, 1, 0 },
	{ "uninit_bg",		FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_GDT_CSUM,	0, 0, 0 },
	{ "dir_nlink",		FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_DIR_NLINK,	1, 1, 0 },
	{ "extra_isize",	FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_EXTRA_ISIZE,	1, 1, 0 },
	{ "quota",		FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_QUOTA,	0, 0, 0 },
	{ "bigalloc",		FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_BIGALLOC,	0, 0, 0 },
	{ "metadata_csum",	FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_METADATA_CKSUM, 0, 0, 0 },
	{ "read-only",		FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_READONLY,	1, 1, 0 },
	{ "project",		FEATURE_ROCOMPAT,
	    EXT2F_ROCOMPAT_PROJECT,	0, 0, 0 },
};

static int apply_features(struct ext2fsd *, uint64_t, uint64_t);
static int create_journal(struct ext2fsd *, uint64_t);
static const char *errbehavior(uint16_t);
static int find_feature(const char *);
static uint64_t fs_bcount(const struct ext2fs *);
static uint64_t fs_fbcount(const struct ext2fs *);
static uint64_t fs_rbcount(const struct ext2fs *);
static void fs_set_rbcount(struct ext2fs *, uint64_t);
static void printfs(void);
static void seterr(struct ext2fsd *, const char *);
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
	int jflag;
	int Lflag, Mflag, mflag, rflag;
	int Tflag, Uflag;
	int pflag, gflag, uflag;
	int Cvalue, cvalue, mvalue;
	uint32_t gvalue, tvalue, uvalue;
	uint64_t Oclear, Oset, ivalue, jsize_mb, mult, rvalue;
	uint16_t state;
	int ch, changed, found_arg, i, needs_fsck;

	Cflag = cflag = eflag = fflag = iflag = jflag = 0;
	Lflag = Mflag = mflag = rflag = 0;
	Tflag = Uflag = pflag = gflag = uflag = 0;
	Cvalue = cvalue = mvalue = 0;
	gvalue = tvalue = uvalue = 0;
	Oclear = Oset = ivalue = jsize_mb = mult = rvalue = 0;
	evalue = Lvalue = Mvalue = NULL;
	memset(uuid, 0, sizeof(uuid));
	changed = found_arg = needs_fsck = 0;
	while ((ch = getopt(argc, argv, "C:c:e:fg:i:jJ:L:M:m:O:pr:T:U:u:")) != -1)
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

		case 'g': {
			struct group *gr;
			char *p;

			found_arg++;
			name = "reserved block group";
			gr = getgrnam(optarg);
			if (gr != NULL) {
				gvalue = gr->gr_gid;
			} else {
				errno = 0;
				gvalue = strtoul(optarg, &p, 10);
				if (optarg[0] == '-' || *optarg == '\0' ||
				    errno != 0 || *p != '\0')
					errx(10, "bad %s (%s)", name, optarg);
			}
			if (gvalue > UINT16_MAX)
				errx(10, "bad %s (%s)", name, optarg);
			gflag = 1;
			break;
		}

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

		case 'j':
			found_arg++;
			jflag = 1;
			break;

		case 'J': {
			char *copy, *p, *token, *sizeend;
			unsigned long size;

			found_arg++;
			jflag = 1;
			copy = strdup(optarg);
			if (copy == NULL)
				err(1, "strdup");
			p = copy;
			while ((token = strsep(&p, ",")) != NULL) {
				if (strncmp(token, "size=", 5) != 0)
					errx(10, "unsupported journal option %s",
					    token);
				errno = 0;
				size = strtoul(token + 5, &sizeend, 10);
				if (token[5] == '\0' || *sizeend != '\0' ||
				    errno != 0 || size == 0)
					errx(10, "bad journal size %s",
					    token + 5);
				jsize_mb = size;
			}
			free(copy);
			break;
		}

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

		case 'O': {
			char *copy, *p, *token;
			int clear, idx;

			found_arg++;
			copy = strdup(optarg);
			if (copy == NULL)
				err(1, "strdup");
			p = copy;
			while ((token = strsep(&p, ",")) != NULL) {
				clear = 0;
				if (*token == '^') {
					clear = 1;
					token++;
				} else if (*token == '+')
					token++;
				if (*token == '\0')
					errx(10, "empty feature");
				if (strcmp(token, "has_journal") == 0) {
					if (clear)
						errx(10, "cannot clear feature "
						    "has_journal");
					jflag = 1;
					continue;
				}
				idx = find_feature(token);
				if (idx < 0)
					errx(10, "unknown feature %s", token);
				if (clear && !feature_opts[idx].clear_ok)
					errx(10, "cannot clear feature %s", token);
				if (!clear && !feature_opts[idx].set_ok)
					errx(10, "cannot set feature %s", token);
				if (clear)
					Oclear |= 1ULL << idx;
				else
					Oset |= 1ULL << idx;
			}
			free(copy);
			break;
		}

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

		case 'u': {
			struct passwd *pw;
			char *p;

			found_arg++;
			name = "reserved block user";
			pw = getpwnam(optarg);
			if (pw != NULL) {
				uvalue = pw->pw_uid;
			} else {
				errno = 0;
				uvalue = strtoul(optarg, &p, 10);
				if (optarg[0] == '-' || *optarg == '\0' ||
				    errno != 0 || *p != '\0')
					errx(10, "bad %s (%s)", name, optarg);
			}
			if (uvalue > UINT16_MAX)
				errx(10, "bad %s (%s)", name, optarg);
			uflag = 1;
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
	if (Oset != 0 || Oclear != 0) {
		if (apply_features(&disk, Oset, Oclear) != 0)
			needs_fsck = 1;
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
	if (uflag) {
		fs->e2fs_ruid = htole16((uint16_t)uvalue);
		changed = 1;
	}
	if (gflag) {
		fs->e2fs_rgid = htole16((uint16_t)gvalue);
		changed = 1;
	}
	if (jflag) {
		if (ext2fs_disk_write(&disk) == -1)
			errx(12, "%s: %s", special, disk.d_error);
		if (create_journal(&disk, jsize_mb) == -1)
			errx(13, "%s: %s", special,
			    disk.d_error != NULL ? disk.d_error :
			    "cannot create journal");
		changed = 1;
	}

	if (changed) {
		if (ext2fs_disk_write(&disk) == -1)
			errx(12, "%s: %s", special, disk.d_error);
		if (write_superblocks(&disk) == -1)
			errx(13, "%s: %s", special, disk.d_error);
		if (fsync(disk.d_fd) == -1)
			err(13, "%s: fsync", special);
	}
	if (needs_fsck)
		warnx("run fsck_ext2fs after changing file system features");
	ext2fs_disk_close(&disk);
	return (0);
}

static void
seterr(struct ext2fsd *disk, const char *msg)
{

	disk->d_error = msg;
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

static int
create_journal(struct ext2fsd *disk, uint64_t size_mb)
{
	struct ext2fs *fs;
	struct ext2_gd *gd;
	struct ext2fs_dinode di;
	struct ext2fs_dinode olddi;
	struct jnl_superblock jsb;
	struct ext4_extent_header *eh;
	struct ext4_extent *ex;
	char *bbuf, *ibuf, *zbuf;
	uint64_t bcount, base, fbcount, jstart;
	uint64_t jblocks64;
	uint32_t bsize, bpg, fpg, ipg, desc_size;
	uint32_t g, i, jblocks, nblks, nbfree, nifree, run, run_start;
	uint16_t incompat, rocompat;
	int error, found, has_csum, isize;
	time_t now;

	fs = &disk->d_fs;
	if ((le32toh(fs->e2fs_features_compat) &
	    EXT2F_COMPAT_HASJOURNAL) != 0) {
		seterr(disk, "file system already has a journal");
		return (-1);
	}
	if ((le32toh(fs->e2fs_features_incompat) &
	    EXT2F_INCOMPAT_EXTENTS) == 0) {
		seterr(disk, "journal creation requires the extents feature");
		return (-1);
	}
	if (ext2fs_gdread(disk) == -1)
		return (-1);

	bsize = 1024u << le32toh(fs->e2fs_log_bsize);
	bpg = le32toh(fs->e2fs_bpg);
	fpg = le32toh(fs->e2fs_fpg);
	ipg = le32toh(fs->e2fs_ipg);
	bcount = fs_bcount(fs);
	incompat = le32toh(fs->e2fs_features_incompat);
	rocompat = le32toh(fs->e2fs_features_rocompat);
	has_csum = (rocompat & EXT2F_ROCOMPAT_METADATA_CKSUM) != 0;
	desc_size = le16toh(fs->e3fs_desc_size);
	if (desc_size < E2FS_REV0_GD_SIZE)
		desc_size = E2FS_REV0_GD_SIZE;
	isize = le32toh(fs->e2fs_rev) == E2FS_REV0 ?
	    E2FS_REV0_INODE_SIZE : le16toh(fs->e2fs_inode_size);

	if (size_mb != 0) {
		if (size_mb > UINT64_MAX / (1024 * 1024)) {
			seterr(disk, "invalid journal size");
			return (-1);
		}
		jblocks64 = size_mb * 1024 * 1024 / bsize;
		if (jblocks64 < 1024)
			jblocks64 = 1024;
	} else
		jblocks64 = 1024;
	if (jblocks64 > 32768 || jblocks64 > bcount / 2) {
		seterr(disk, "invalid journal size");
		return (-1);
	}
	jblocks = (uint32_t)jblocks64;

	bbuf = ibuf = zbuf = NULL;
	bbuf = malloc(bsize);
	ibuf = malloc(bsize);
	zbuf = calloc(1, bsize);
	if (bbuf == NULL || ibuf == NULL || zbuf == NULL) {
		seterr(disk, "out of memory");
		goto fail;
	}

	/* Claim the reserved journal inode in group 0. */
	gd = &disk->d_gd[0];
	if (ext2fs_iget(disk, EXT2_JOURNALINO, &olddi) == -1) {
		seterr(disk, "journal inode read failed");
		goto fail;
	}
	if (le16toh(olddi.e2di_nlink) != 0 ||
	    le16toh(olddi.e2di_mode) != 0) {
		seterr(disk, "journal inode is already allocated");
		goto fail;
	}
	if (ext2fs_bread(disk, le32toh(gd->ext2bgd_i_bitmap), ibuf, bsize)
	    != (ssize_t)bsize) {
		seterr(disk, "inode bitmap read failed");
		goto fail;
	}
	if (!isset(ibuf, EXT2_JOURNALINO - 1)) {
		setbit(ibuf, EXT2_JOURNALINO - 1);
		nifree = ((uint32_t)le16toh(gd->ext4bgd_nifree_hi) << 16) |
		    le16toh(gd->ext2bgd_nifree);
		if (nifree == 0) {
			seterr(disk, "no free inodes in group 0");
			goto fail;
		}
		nifree--;
		gd->ext2bgd_nifree = htole16(nifree & 0xffff);
		gd->ext4bgd_nifree_hi = htole16(nifree >> 16);
		if (has_csum)
			ext2_gd_ibitmap_csum_update(disk->d_csum_seed, ibuf,
			    ipg / 8, gd, desc_size);
		if (ext2fs_bwrite(disk,
		    le32toh(gd->ext2bgd_i_bitmap), ibuf, bsize)
		    != (ssize_t)bsize) {
			seterr(disk, "inode bitmap write failed");
			goto fail;
		}
		if (le32toh(fs->e2fs_ficount) == 0) {
			seterr(disk, "free inode count underflow");
			goto fail;
		}
		fs->e2fs_ficount =
		    htole32(le32toh(fs->e2fs_ficount) - 1);
	}

	/* Find a contiguous run of free blocks in one group. */
	found = 0;
	for (g = 0; g < disk->d_gcount && !found; g++) {
		gd = &disk->d_gd[g];
		base = le32toh(fs->e2fs_first_dblock) +
		    (uint64_t)g * bpg;
		if (base >= bcount)
			break;
		nblks = bpg;
		if (base + nblks > bcount)
			nblks = (uint32_t)(bcount - base);
		if (ext2fs_bread(disk,
		    le32toh(gd->ext2bgd_b_bitmap), bbuf, bsize)
		    != (ssize_t)bsize) {
			seterr(disk, "block bitmap read failed");
			goto fail;
		}
		for (i = 0; i + jblocks <= nblks; ) {
			if (!isclr(bbuf, i)) {
				i++;
				continue;
			}
			for (run = 1; run < jblocks; run++)
				if (!isclr(bbuf, i + run))
					break;
			if (run == jblocks) {
				run_start = i;
				found = 1;
				break;
			}
			i += run + 1;
		}
	}
	if (!found) {
		seterr(disk, "no contiguous free space for the journal");
		goto fail;
	}

	jstart = base + run_start;
	for (i = 0; i < jblocks; i++)
		setbit(bbuf, run_start + i);
	nbfree = ((uint32_t)le16toh(gd->ext4bgd_nbfree_hi) << 16) |
	    le16toh(gd->ext2bgd_nbfree);
	if (nbfree < jblocks) {
		seterr(disk, "free block count underflow");
		goto fail;
	}
	nbfree -= jblocks;
	gd->ext2bgd_nbfree = htole16(nbfree & 0xffff);
	gd->ext4bgd_nbfree_hi = htole16(nbfree >> 16);
	if (has_csum)
		ext2_gd_bbitmap_csum_update(disk->d_csum_seed, bbuf,
		    fpg / 8, gd, desc_size);
	if (ext2fs_bwrite(disk, le32toh(gd->ext2bgd_b_bitmap), bbuf, bsize)
	    != (ssize_t)bsize) {
		seterr(disk, "block bitmap write failed");
		goto fail;
	}
	fbcount = fs_fbcount(fs);
	if (fbcount < jblocks) {
		seterr(disk, "free block count underflow");
		goto fail;
	}
	fbcount -= jblocks;
	fs->e2fs_fbcount = htole32((uint32_t)fbcount);
	if ((incompat & EXT2F_INCOMPAT_64BIT) != 0)
		fs->e4fs_fbcount_hi = htole32((uint32_t)(fbcount >> 32));

	/* Write the journal inode with a single extent. */
	memset(&di, 0, sizeof(di));
	now = time(NULL);
	di.e2di_mode = htole16(S_IFREG | 0600);
	di.e2di_size = htole32(jblocks * bsize);
	di.e2di_atime = htole32((uint32_t)now);
	di.e2di_ctime = htole32((uint32_t)now);
	di.e2di_mtime = htole32((uint32_t)now);
	di.e2di_crtime = htole32((uint32_t)now);
	di.e2di_nlink = htole16(1);
	di.e2di_nblock = htole32(jblocks * bsize / 512);
	di.e2di_flags = htole32(EXT4_EXTENTS);
	if (isize > E2FS_REV0_INODE_SIZE &&
	    (rocompat & EXT2F_ROCOMPAT_EXTRA_ISIZE) != 0)
		di.e2di_extra_isize = htole16(32);
	eh = (struct ext4_extent_header *)di.e2di_blocks;
	eh->eh_magic = htole16(EXT4_EXT_MAGIC);
	eh->eh_entries = htole16(1);
	eh->eh_max = htole16(4);
	eh->eh_depth = htole16(0);
	eh->eh_generation = htole32(0);
	ex = (struct ext4_extent *)(eh + 1);
	ex->e_blk = htole32(0);
	ex->e_len = htole16((uint16_t)jblocks);
	ex->e_start_hi = htole16((uint16_t)(jstart >> 32));
	ex->e_start_lo = htole32((uint32_t)jstart);
	if (ext2fs_iput(disk, EXT2_JOURNALINO, &di) == -1) {
		seterr(disk, "journal inode write failed");
		goto fail;
	}

	/* Clear the log and write the JBD2 superblock into its first block. */
	for (i = 0; i < jblocks; i++)
		if (ext2fs_bwrite(disk, jstart + i, zbuf, bsize) !=
		    (ssize_t)bsize) {
			seterr(disk, "journal block write failed");
			goto fail;
		}
	memset(&jsb, 0, sizeof(jsb));
	jsb.h_magic = htobe32(0xc03b3998);
	jsb.h_blocktype = htobe32(4);
	jsb.s_blocksize = htobe32(bsize);
	jsb.s_maxlen = htobe32(jblocks);
	jsb.s_first = htobe32(1);
	jsb.s_sequence = htobe32(1);
	jsb.s_nr_users = htobe32(1);
	memcpy(jsb.s_uuid, fs->e2fs_uuid, sizeof(jsb.s_uuid));
	memcpy(zbuf, &jsb, sizeof(jsb));
	if (ext2fs_bwrite(disk, jstart, zbuf, bsize) != (ssize_t)bsize) {
		seterr(disk, "journal superblock write failed");
		goto fail;
	}

	if (ext2fs_gdwrite(disk) == -1) {
		seterr(disk, "group descriptor write failed");
		goto fail;
	}

	fs->e2fs_features_compat = htole32(
	    le32toh(fs->e2fs_features_compat) | EXT2F_COMPAT_HASJOURNAL);
	fs->e3fs_journal_inum = htole32(EXT2_JOURNALINO);
	fs->e3fs_journal_dev = htole32(0);
	memset(fs->e3fs_journal_uuid, 0, sizeof(fs->e3fs_journal_uuid));
	fs->e2fs_wtime = htole32((uint32_t)now);

	free(bbuf);
	free(ibuf);
	free(zbuf);
	return (0);

fail:
	error = errno;
	free(bbuf);
	free(ibuf);
	free(zbuf);
	errno = error;
	return (-1);
}

static int
find_feature(const char *name)
{
	size_t i;

	for (i = 0; i < nitems(feature_opts); i++)
		if (strcmp(feature_opts[i].name, name) == 0)
			return ((int)i);
	return (-1);
}

static int
apply_features(struct ext2fsd *disk, uint64_t set, uint64_t clear)
{
	struct ext2fs *fs;
	uint32_t compat, incompat, rocompat;
	uint64_t bit;
	size_t i;
	int fsck;

	fs = &disk->d_fs;
	compat = le32toh(fs->e2fs_features_compat);
	incompat = le32toh(fs->e2fs_features_incompat);
	rocompat = le32toh(fs->e2fs_features_rocompat);
	fsck = 0;

	for (i = 0; i < nitems(feature_opts); i++) {
		const struct feature_opt *fo = &feature_opts[i];

		bit = 1ULL << i;
		if ((set & bit) != 0 && (clear & bit) != 0)
			errx(10, "cannot both set and clear feature %s",
			    fo->name);
		if ((set & bit) == 0 && (clear & bit) == 0)
			continue;

		if (fo->mask == EXT2F_INCOMPAT_CSUM_SEED) {
			if ((set & bit) != 0) {
				if ((rocompat &
				    EXT2F_ROCOMPAT_METADATA_CKSUM) == 0)
					errx(10, "%s requires metadata_csum",
					    fo->name);
				if ((incompat & EXT2F_INCOMPAT_CSUM_SEED) == 0) {
					fs->e4fs_chksum_seed =
					    htole32(ext2_csum_seed(fs));
					incompat |=
					    EXT2F_INCOMPAT_CSUM_SEED;
				}
			} else if ((incompat &
			    EXT2F_INCOMPAT_CSUM_SEED) != 0) {
				struct ext2fs tmp;
				uint32_t seed;

				tmp = *fs;
				tmp.e2fs_features_incompat = htole32(
				    incompat & ~EXT2F_INCOMPAT_CSUM_SEED);
				seed = ext2_csum_seed(&tmp);
				if (le32toh(fs->e4fs_chksum_seed) != seed)
					errx(10, "cannot clear %s: checksum "
					    "seed differs from UUID", fo->name);
				incompat &= ~EXT2F_INCOMPAT_CSUM_SEED;
			}
			continue;
		}

		if ((set & bit) != 0) {
			switch (fo->field) {
			case FEATURE_COMPAT:
				compat |= fo->mask;
				break;
			case FEATURE_INCOMPAT:
				incompat |= fo->mask;
				break;
			case FEATURE_ROCOMPAT:
				rocompat |= fo->mask;
				break;
			}
			fsck |= fo->needs_fsck;
		}
		if ((clear & bit) != 0) {
			switch (fo->field) {
			case FEATURE_COMPAT:
				compat &= ~fo->mask;
				break;
			case FEATURE_INCOMPAT:
				incompat &= ~fo->mask;
				break;
			case FEATURE_ROCOMPAT:
				rocompat &= ~fo->mask;
				break;
			}
			fsck |= fo->needs_fsck;
		}
	}

	fs->e2fs_features_compat = htole32(compat);
	fs->e2fs_features_incompat = htole32(incompat);
	fs->e2fs_features_rocompat = htole32(rocompat);
	return (fsck);
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
fs_fbcount(const struct ext2fs *fs)
{
	uint64_t fbcount;

	fbcount = le32toh(fs->e2fs_fbcount);
	if (le32toh(fs->e2fs_features_incompat) & EXT2F_INCOMPAT_64BIT)
		fbcount |= (uint64_t)le32toh(fs->e4fs_fbcount_hi) << 32;
	return (fbcount);
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
	warnx("%-52s %u", "reserved block user: (-u)",
	    le16toh(fs->e2fs_ruid));
	warnx("%-52s %u", "reserved block group: (-g)",
	    le16toh(fs->e2fs_rgid));
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
	    "       [-e continue|remount-ro|panic] [-f] [-g group]\n"
	    "       [-i interval] [-j] [-J journal-options]\n"
	    "       [-L volume-label] [-M last-mounted-directory]\n"
	    "       [-m reserved-percent] [-O feature[,...]] [-p]\n"
	    "       [-r reserved-blocks] [-T last-check-time] [-U uuid]\n"
	    "       [-u user] filesys\n", getprogname());
	exit(1);
}
