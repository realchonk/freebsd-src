/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/param.h>
#include <sys/endian.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libext2fs.h>

static struct ext2fsd disk;

static int	usage(void);
static const char *ext2fserr(void);
static const char *ext2fstype(void);
static int	dumpfs(const char *);
static void	dump_features(const char *, uint32_t,
		    const struct ext2_feature *, size_t);
static void	dump_uuid(const uint8_t *);

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

static const char *
ext2fstype(void)
{
	switch (disk.d_version) {
	case 4:
		return "ext4";
	case 3:
		return "ext3";
	case 2:
		return "ext2";
	default:
		return "unknown";
	}
}

static int
dumpfs(const char *name)
{
	struct ext2fs *fs;
	uint32_t bsize;
	time_t t;

	fs = &disk.d_fs;
	bsize = 1024u << le32toh(fs->e2fs_log_bsize);

	printf("# %s\n", name);
	printf("magic\t%04x\t(%s)\trev\t%u\n", le16toh(fs->e2fs_magic),
	    ext2fstype(), le32toh(fs->e2fs_rev));

	t = (time_t)le32toh(fs->e2fs_wtime);
	printf("last write time\t%s", ctime(&t));
	t = (time_t)le32toh(fs->e2fs_mtime);
	printf("last mount time\t%s", ctime(&t));

	printf("block size\t%u\tlog block size\t%u\tfirst data block\t%u\n",
	    bsize, le32toh(fs->e2fs_log_bsize), le32toh(fs->e2fs_first_dblock));
	printf("total blocks\t%u\treserved blocks\t%u\tfree blocks\t%u\n",
	    le32toh(fs->e2fs_bcount), le32toh(fs->e2fs_rbcount),
	    le32toh(fs->e2fs_fbcount));
	printf("total inodes\t%u\tfree inodes\t%u\n",
	    le32toh(fs->e2fs_icount), le32toh(fs->e2fs_ficount));
	printf("blocks/group\t%u\tfrags/group\t%u\tinodes/group\t%u\n",
	    le32toh(fs->e2fs_bpg), le32toh(fs->e2fs_fpg),
	    le32toh(fs->e2fs_ipg));
	printf("inode size\t%u\tfirst inode\t%u\tblock group nr\t%u\n",
	    le16toh(fs->e2fs_inode_size), le32toh(fs->e2fs_first_ino),
	    le16toh(fs->e2fs_block_group_nr));
	printf("mount count\t%u\tmax mount count\t%u\n",
	    le16toh(fs->e2fs_mnt_count), le16toh(fs->e2fs_max_mnt_count));
	t = (time_t)le32toh(fs->e2fs_lastfsck);
	printf("last checked\t%s", ctime(&t));
	printf("check interval\t%u\tstate\t%#x\tbehavior on errors\t%#x\n",
	    le32toh(fs->e2fs_fsckintv), le16toh(fs->e2fs_state),
	    le16toh(fs->e2fs_beh));
	printf("creator OS\t%#x\n", le32toh(fs->e2fs_creator));

	dump_features("compat", le32toh(fs->e2fs_features_compat), compat,
	    nitems(compat));
	dump_features("incompat", le32toh(fs->e2fs_features_incompat), incompat,
	    nitems(incompat));
	dump_features("ro_compat", le32toh(fs->e2fs_features_rocompat),
	    ro_compat, nitems(ro_compat));

	printf("volume name\t%.16s\n", fs->e2fs_vname);
	printf("last mounted on\t%.64s\n", fs->e2fs_fsmnt);
	printf("UUID\t");
	dump_uuid(fs->e2fs_uuid);
	printf("\n");

	return 0;
}

static void
dump_features(const char *label, uint32_t mask,
    const struct ext2_feature *feats, size_t nfeats)
{
	size_t i;

	printf("%-10s %#08x", label, mask);
	for (i = 0; i < nfeats; i++)
		if (mask & feats[i].mask)
			printf(" %s", feats[i].name);
	printf("\n");
}

static void
dump_uuid(const uint8_t *u)
{

	printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
	    "%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3], u[4], u[5],
	    u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
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
		/*
		 * Open the device first, then read the superblock separately so
		 * that a failure to find a valid ext2 superblock is reported
		 * with the specific error set by sbread rather than being
		 * clobbered by the close path.
		 */
		if (ext2fs_disk_fillout_blank(&disk, name) != 0 ||
		    ext2fs_sbread(&disk) == -1) {
			printf("\n%s: %s\n", name, ext2fserr());
			eval |= 1;
			continue;
		}

		eval |= dumpfs(name);
		ext2fs_disk_close(&disk);
	}
	return eval;
}
