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
static int	dumpgroups(void);
static void	dump_features(const char *, uint32_t,
		    const struct ext2_feature *, size_t);
static void	dump_bg_flags(uint16_t);
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

/*
 * The block group descriptor stores split low/high halves of each value so
 * the same struct serves 32-bit and 64-bit filesystems.  These local getters
 * mirror the kernel's e2fs_gd_get_* in ext2_alloc.c; for rev0 filesystems the
 * high halves are zero (left cleared by ext2fs_gdread) so the combines are
 * harmless.
 */
static uint64_t
gd_b_bitmap(const struct ext2_gd *gd)
{

	return (((uint64_t)le32toh(gd->ext4bgd_b_bitmap_hi) << 32) |
	    le32toh(gd->ext2bgd_b_bitmap));
}

static uint64_t
gd_i_bitmap(const struct ext2_gd *gd)
{

	return (((uint64_t)le32toh(gd->ext4bgd_i_bitmap_hi) << 32) |
	    le32toh(gd->ext2bgd_i_bitmap));
}

static uint64_t
gd_i_tables(const struct ext2_gd *gd)
{

	return (((uint64_t)le32toh(gd->ext4bgd_i_tables_hi) << 32) |
	    le32toh(gd->ext2bgd_i_tables));
}

static uint32_t
gd_nbfree(const struct ext2_gd *gd)
{

	return (((uint32_t)le16toh(gd->ext4bgd_nbfree_hi) << 16) |
	    le16toh(gd->ext2bgd_nbfree));
}

static uint32_t
gd_nifree(const struct ext2_gd *gd)
{

	return (((uint32_t)le16toh(gd->ext4bgd_nifree_hi) << 16) |
	    le16toh(gd->ext2bgd_nifree));
}

static uint32_t
gd_ndirs(const struct ext2_gd *gd)
{

	return (((uint32_t)le16toh(gd->ext4bgd_ndirs_hi) << 16) |
	    le16toh(gd->ext2bgd_ndirs));
}

static uint32_t
gd_i_unused(const struct ext2_gd *gd)
{

	return (((uint32_t)le16toh(gd->ext4bgd_i_unused_hi) << 16) |
	    le16toh(gd->ext4bgd_i_unused));
}

static void
dump_bg_flags(uint16_t flags)
{

	if (flags == 0) {
		printf("none");
		return;
	}
	if (flags & EXT2_BG_INODE_UNINIT)
		printf("inode_uninit ");
	if (flags & EXT2_BG_BLOCK_UNINIT)
		printf("block_uninit ");
	if (flags & EXT2_BG_INODE_ZEROED)
		printf("inode_zeroed ");
	flags &= ~(EXT2_BG_INODE_UNINIT | EXT2_BG_BLOCK_UNINIT |
	    EXT2_BG_INODE_ZEROED);
	if (flags != 0)
		printf("unknown (%#x)", flags);
}

/*
 * Dump the block group descriptor table, one stanza per group in the style of
 * dumpfs(8)'s cylinder-group listing.  Geometry is derived from the already
 * read superblock; the descriptors themselves come from ext2fs_gdread().
 */
static int
dumpgroups(void)
{
	struct ext2fs *fs;
	uint64_t bcount;
	uint32_t bsize, bpg, ipg, isize, ipb, itpg, first_dblock, g;
	int has64, has_csum;

	if (ext2fs_gdread(&disk) == -1) {
		printf("\n%s\n", ext2fserr());
		return (1);
	}

	fs = &disk.d_fs;
	bsize = 1024u << le32toh(fs->e2fs_log_bsize);
	bpg = le32toh(fs->e2fs_bpg);
	ipg = le32toh(fs->e2fs_ipg);
	isize = le32toh(fs->e2fs_rev) == E2FS_REV0 ? E2FS_REV0_INODE_SIZE :
	    le16toh(fs->e2fs_inode_size);
	ipb = bsize / isize;
	itpg = ipg / ipb;
	first_dblock = le32toh(fs->e2fs_first_dblock);

	has64 = le32toh(fs->e2fs_features_incompat) & EXT2F_INCOMPAT_64BIT;
	bcount = le32toh(fs->e2fs_bcount);
	if (has64)
		bcount |= (uint64_t)le32toh(fs->e4fs_bcount_hi) << 32;

	has_csum = (le32toh(fs->e2fs_features_rocompat) &
	    (EXT2F_ROCOMPAT_GDT_CSUM | EXT2F_ROCOMPAT_METADATA_CKSUM)) != 0;

	for (g = 0; g < disk.d_gcount; g++) {
		struct ext2_gd *gd;
		uint64_t first, last, itab;

		gd = &disk.d_gd[g];
		first = first_dblock + (uint64_t)g * bpg;
		last = first + bpg - 1;
		if (last > bcount - 1)
			last = bcount - 1;
		itab = gd_i_tables(gd);

		printf("\nbg %u:  (blocks %ju-%ju)\n", g, (uintmax_t)first,
		    (uintmax_t)last);
		printf("block bitmap\t%ju\tinode bitmap\t%ju\tinode table\t%ju",
		    (uintmax_t)gd_b_bitmap(gd), (uintmax_t)gd_i_bitmap(gd),
		    (uintmax_t)itab);
		if (itpg > 1 && itab != 0)
			printf("-%ju", (uintmax_t)(itab + itpg - 1));
		printf("\n");
		printf("free blocks\t%u\tfree inodes\t%u\tdirectories\t%u\n",
		    gd_nbfree(gd), gd_nifree(gd), gd_ndirs(gd));
		printf("flags\t");
		dump_bg_flags(le16toh(gd->ext4bgd_flags));
		printf("\tunused inodes\t%u\n", gd_i_unused(gd));
		if (has_csum)
			printf("checksum\t%#06x\n",
			    le16toh(gd->ext4bgd_csum));
	}
	if (disk.d_gcount > 0)
		printf("\n");
	return (0);
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
		eval |= dumpgroups();
		ext2fs_disk_close(&disk);
	}
	return eval;
}
