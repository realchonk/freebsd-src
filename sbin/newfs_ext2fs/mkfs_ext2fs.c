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
#include <sys/stat.h>
#include <sys/types.h>

#include <err.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libext2fs.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_dir.h>

#include "newfs_ext2fs.h"

/* ext2 creator OS id for FreeBSD; not in the kernel headers yet. */
#define	EXT2_OS_FREEBSD	3

#define	LPFSIZE		(12 * 1024)

static void	fsinit(time_t utime);
static uint32_t	alloc(int mode);
static void	iput(const struct ext2fs_dinode *, ino_t);
static int	makedir(const struct ext2fs_direct_2 *, int);
static void	wtfs(uint64_t bno, int size, char *bf);
static void	wtsb(int cg);
static void	wtgdt(void);
static uint32_t	group_first(int cg);
static int	ilog2(int n);

static struct ext2fs *fs = &disk.d_fs;
static u_char *iobuf;
static u_char *bmap;		/* group 0 block bitmap */
static u_char *imap;		/* group 0 inode bitmap */
static u_char *itable0;		/* group 0 inode table */
static uint32_t bpg, ipg, itpg, ipb, gcount, gdbcount, descpb, first_dblock;

/*
 * Build an ext2 file system.  Everything is laid out in memory first, so
 * that the backup copies of the superblock and the group descriptor table
 * are written with the final allocation counters.
 */
void
mkfs(const char *fsys)
{
	time_t		utime;
	uint32_t	cg, i;
	uint64_t	bcount, fbcount, ficount;
	off_t		off;
	char		probe[512];

	if (fssize < 64 * 1024)
		errx(17, "file system too small");
	if (!Nflag) {
		/* Probe the last sector; md(4) needs aligned transfers. */
		off = fssize - 1;
		off -= off % 512;
		if (pread(disk.d_fd, probe, 512, off) != 512)
			err(17, "%s: last block", fsys);
	}

	switch (bsize) {
	case 1024:
	case 2048:
	case 4096:
		break;
	default:
		errx(16, "block size must be 1024, 2048 or 4096");
	}
	first_dblock = bsize == 1024 ? 1 : 0;

	bcount = fssize / bsize;
	bpg = bsize * 8;
	gcount = howmany(bcount - first_dblock, bpg);
	descpb = bsize / E2FS_REV0_GD_SIZE;
	gdbcount = howmany(gcount, descpb);
	ipb = bsize / E2FS_REV0_INODE_SIZE;

	/*
	 * One inode per "density" bytes, rounded down to whole inode-table
	 * blocks.  Keep at least enough inodes in group 0 for the reserved
	 * inodes, / and /lost+found, and make the table fit the last group.
	 */
	ipg = (uint32_t)((uint64_t)bpg * bsize / density);
	ipg -= ipg % ipb;
	i = howmany(EXT2_FIRSTINO + 2, ipb) * ipb;
	if (ipg < i)
		ipg = i;
	if (ipg > bpg)
		ipg = bpg;
	for (;;) {
		itpg = ipg / ipb;
		if (1 + gdbcount + 2 + itpg <= bpg)
			break;
		if (ipg <= ipb)
			errx(17, "file system too small");
		ipg -= ipb;
	}

	iobuf = calloc(1, bsize);
	bmap = calloc(1, bsize);
	imap = calloc(1, bsize);
	itable0 = calloc(itpg, bsize);
	if (iobuf == NULL || bmap == NULL || imap == NULL || itable0 == NULL)
		err(1, "calloc");

	utime = time(NULL);

	/* Group descriptors, one per group, in memory. */
	disk.d_gd = calloc(gcount, sizeof(struct ext2_gd));
	if (disk.d_gd == NULL)
		err(1, "calloc");
	disk.d_gcount = gcount;
	for (cg = 0; cg < gcount; cg++) {
		struct ext2_gd *gd = &disk.d_gd[cg];
		uint32_t first, meta, nblk;

		/*
		 * ext2fs_cg_hassb() reads the feature flags, so seed the
		 * sparse_super bit before laying out the groups.
		 */
		fs->e2fs_features_rocompat =
		    htole32(EXT2F_ROCOMPAT_SPARSESUPER);
		first = group_first(cg);
		meta = ext2fs_cg_hassb(&disk, cg) ? 1 + gdbcount : 0;
		nblk = bpg;
		if (first + nblk > bcount)
			nblk = bcount - first;
		gd->ext2bgd_b_bitmap = htole32(first + meta);
		gd->ext2bgd_i_bitmap = htole32(first + meta + 1);
		gd->ext2bgd_i_tables = htole32(first + meta + 2);
		gd->ext2bgd_nbfree = htole32(nblk - meta - 2 - itpg);
		gd->ext2bgd_nifree = htole32(ipg - (cg == 0 ?
		    EXT2_FIRSTINO - 1 : 0));
		gd->ext4bgd_flags = htole16(EXT2_BG_INODE_ZEROED);
	}

	/* Group 0 maps before fsinit() allocates from them. */
	for (i = 0; i < 1 + gdbcount + 2 + itpg; i++)
		setbit(bmap, i);
	for (i = 0; i < EXT2_FIRSTINO - 1; i++)
		setbit(imap, i);

	/* Superblock. */
	memset(&disk.d_fs, 0, SBLOCKSIZE);
	fs->e2fs_icount = htole32(gcount * ipg);
	fs->e2fs_bcount = htole32(bcount);
	fs->e2fs_rbcount = htole32(bcount * minfree / 100);
	fs->e2fs_first_dblock = htole32(first_dblock);
	fs->e2fs_log_bsize = htole32(ilog2(bsize) - 10);
	fs->e2fs_log_fsize = fs->e2fs_log_bsize;
	fs->e2fs_bpg = htole32(bpg);
	fs->e2fs_fpg = htole32(bpg);
	fs->e2fs_ipg = htole32(ipg);
	fs->e2fs_wtime = htole32(utime);
	fs->e2fs_max_mnt_count = htole16(20);
	fs->e2fs_magic = htole16(E2FS_MAGIC);
	fs->e2fs_state = htole16(E2FS_ISCLEAN);
	fs->e2fs_beh = htole16(1);
	fs->e2fs_rev = htole32(E2FS_REV1);
	fs->e2fs_first_ino = htole32(EXT2_FIRSTINO);
	fs->e2fs_inode_size = htole16(E2FS_REV0_INODE_SIZE);
	fs->e2fs_features_incompat = htole32(EXT2F_INCOMPAT_FTYPE);
	fs->e2fs_features_rocompat = htole32(EXT2F_ROCOMPAT_SPARSESUPER);
	arc4random_buf(fs->e2fs_uuid, sizeof(fs->e2fs_uuid));
	if (volumelabel != NULL)
		strlcpy(fs->e2fs_vname, (const char *)volumelabel,
		    sizeof(fs->e2fs_vname));
	fs->e2fs_lastfsck = htole32(utime);
	fs->e2fs_creator = htole32(EXT2_OS_FREEBSD);
	fs->e3fs_mkfs_time = htole32(utime);
	fs->e4fs_flags = htole32(E2FS_SIGNED_HASH);

	printf("%s: %.1fMB (%ju bytes) block size %d\n", fsys,
	    (double)fssize / 1024 / 1024, (uintmax_t)fssize, bsize);
	printf("\tusing %u block groups of %u blocks, %u inodes.\n", gcount,
	    bpg, ipg);

	if (Nflag) {
		printf("super-block backups (for fsck_ext2fs -b #) at:");
		for (cg = 1; cg < gcount; cg++)
			if (ext2fs_cg_hassb(&disk, cg))
				printf(" %u", group_first(cg));
		printf("\n");
		exit(0);
	}

	fsinit(utime);

	/* Allocation counters are final; refresh the superblock totals. */
	fbcount = ficount = 0;
	for (cg = 0; cg < gcount; cg++) {
		fbcount += le32toh(disk.d_gd[cg].ext2bgd_nbfree);
		ficount += le32toh(disk.d_gd[cg].ext2bgd_nifree);
	}
	fs->e2fs_fbcount = htole32(fbcount);
	fs->e2fs_ficount = htole32(ficount);

	/* Per-group metadata. */
	for (cg = 0; cg < gcount; cg++) {
		struct ext2_gd *gd = &disk.d_gd[cg];
		uint32_t first, meta, nblk, i;

		first = group_first(cg);
		meta = ext2fs_cg_hassb(&disk, cg) ? 1 + gdbcount : 0;
		nblk = bpg;
		if (first + nblk > bcount)
			nblk = bcount - first;

		memset(iobuf, 0, bsize);
		for (i = 0; i < meta + 2 + itpg; i++)
			setbit(iobuf, i);
		if (cg == gcount - 1)
			for (i = nblk; i < bpg; i++)
				setbit(iobuf, i);
		if (cg == 0)
			for (i = 0; i < bpg; i++)
				if (isset(bmap, i))
					setbit(iobuf, i);
		wtfs(le32toh(gd->ext2bgd_b_bitmap), bsize, (char *)iobuf);

		memset(iobuf, 0, bsize);
		/* Bits past the last inode are padding and must be set. */
		for (i = ipg; i < bpg; i++)
			setbit(iobuf, i);
		if (cg == 0)
			for (i = 0; i < ipg; i++)
				if (isset(imap, i))
					setbit(iobuf, i);
		wtfs(le32toh(gd->ext2bgd_i_bitmap), bsize, (char *)iobuf);

		if (cg == 0)
			wtfs(le32toh(gd->ext2bgd_i_tables), itpg * bsize,
			    (char *)itable0);
		else {
			memset(iobuf, 0, bsize);
			for (i = 0; i < itpg; i++)
				wtfs(le32toh(gd->ext2bgd_i_tables) + i, bsize,
				    (char *)iobuf);
		}
	}

	wtgdt();

	printf("super-block backups (for fsck_ext2fs -b #) at:");
	for (cg = 1; cg < gcount; cg++)
		if (ext2fs_cg_hassb(&disk, cg)) {
			wtsb(cg);
			printf(" %u", group_first(cg));
		}
	printf("\n");
	wtsb(0);
}

/*
 * Create / (inode 2) and /lost+found (inode 11) with their directory
 * blocks.  Only in-memory state is updated; the write pass in mkfs()
 * stores it.
 */
static void
fsinit(time_t utime)
{
	struct ext2fs_dinode node;
	struct ext2fs_direct_2 root_dir[] = {
		{ htole32(EXT2_ROOTINO), htole16(12), 1, EXT2_FT_DIR, "." },
		{ htole32(EXT2_ROOTINO), htole16(12), 2, EXT2_FT_DIR, ".." },
		{ htole32(EXT2_FIRSTINO), 0, 10, EXT2_FT_DIR, "lost+found" },
	};
	struct ext2fs_direct_2 lpf_dir[] = {
		{ htole32(EXT2_FIRSTINO), htole16(12), 1, EXT2_FT_DIR, "." },
		{ htole32(EXT2_ROOTINO), 0, 2, EXT2_FT_DIR, ".." },
	};
	uint32_t rootblk, lpfblk[LPFSIZE / 1024];
	uint32_t i, nlpf;

	/* / */
	memset(&node, 0, sizeof(node));
	node.e2di_mode = htole16(S_IFDIR | 0755);
	node.e2di_nlink = htole16(nflag ? 2 : 3);
	node.e2di_size = htole32(bsize);
	node.e2di_atime = node.e2di_ctime = node.e2di_mtime = htole32(utime);
	rootblk = alloc(S_IFDIR);
	node.e2di_blocks[0] = htole32(rootblk);
	node.e2di_nblock = htole32(bsize / 512);
	node.e2di_gen = htole32(arc4random());
	iput(&node, EXT2_ROOTINO);

	/* /lost+found */
	if (!nflag) {
		nlpf = LPFSIZE / bsize;
		memset(&node, 0, sizeof(node));
		node.e2di_mode = htole16(S_IFDIR | 0755);
		node.e2di_nlink = htole16(2);
		node.e2di_size = htole32(LPFSIZE);
		node.e2di_atime = node.e2di_ctime = node.e2di_mtime =
		    htole32(utime);
		for (i = 0; i < nlpf; i++)
			lpfblk[i] = alloc(i == 0 ? S_IFDIR : 0);
		for (i = 0; i < nlpf; i++)
			node.e2di_blocks[i] = htole32(lpfblk[i]);
		node.e2di_nblock = htole32(LPFSIZE / 512);
		node.e2di_gen = htole32(arc4random());
		iput(&node, EXT2_FIRSTINO);

		makedir(lpf_dir, 2);
		wtfs(lpfblk[0], bsize, (char *)iobuf);
		/* Trailing blocks hold one empty entry spanning the block. */
		memset(iobuf, 0, bsize);
		*(uint16_t *)(iobuf + offsetof(struct ext2fs_direct_2,
		    e2d_reclen)) = htole16(bsize);
		for (i = 1; i < nlpf; i++)
			wtfs(lpfblk[i], bsize, (char *)iobuf);
	}

	makedir(root_dir, nflag ? 2 : 3);
	wtfs(rootblk, bsize, (char *)iobuf);
}

/*
 * Take one free block from group 0 and return its absolute number.
 */
static uint32_t
alloc(int mode)
{
	struct ext2_gd *gd = &disk.d_gd[0];
	uint32_t i;

	for (i = 0; i < bpg; i++)
		if (!isset(bmap, i))
			break;
	if (i == bpg)
		errx(1, "alloc: no free blocks in group 0");
	setbit(bmap, i);
	gd->ext2bgd_nbfree = htole32(le32toh(gd->ext2bgd_nbfree) - 1);
	if (mode & S_IFDIR)
		gd->ext2bgd_ndirs = htole32(le32toh(gd->ext2bgd_ndirs) + 1);
	return (group_first(0) + i);
}

/*
 * Store one inode in group 0's inode table.
 */
static void
iput(const struct ext2fs_dinode *node, ino_t ino)
{
	struct ext2_gd *gd = &disk.d_gd[0];

	memcpy(itable0 + (size_t)(ino - 1) * E2FS_REV0_INODE_SIZE, node,
	    E2FS_REV0_INODE_SIZE);
	if (!isset(imap, ino - 1)) {
		setbit(imap, ino - 1);
		gd->ext2bgd_nifree = htole32(le32toh(gd->ext2bgd_nifree) - 1);
	}
}

/*
 * Pack directory entries into iobuf, one block.  All but the last entry
 * get EXT2_DIR_REC_LEN(namlen); the last absorbs the remainder.
 */
static int
makedir(const struct ext2fs_direct_2 *protodir, int entries)
{
	struct ext2fs_direct_2 *dp;
	int i, loc;

	memset(iobuf, 0, bsize);
	loc = 0;
	for (i = 0; i < entries; i++) {
		dp = (struct ext2fs_direct_2 *)(iobuf + loc);
		*dp = protodir[i];
		if (i == entries - 1)
			dp->e2d_reclen = htole16(bsize - loc);
		else
			dp->e2d_reclen = htole16(EXT2_DIR_REC_LEN(
			    protodir[i].e2d_namlen));
		loc += EXT2_DIR_REC_LEN(protodir[i].e2d_namlen);
	}
	return (bsize);
}

static void
wtfs(uint64_t bno, int size, char *bf)
{

	if (Nflag)
		return;
	if (ext2fs_bwrite(&disk, bno, bf, size) != size)
		err(36, "ext2fs_bwrite");
}

/*
 * Write the superblock copy of block group cg (cg 0 is the primary).  At
 * 1K blocks the copy fills the group's first block; otherwise it sits at
 * byte 1024 of it.
 */
static void
wtsb(int cg)
{
	uint64_t off;

	fs->e2fs_block_group_nr = htole16(cg);
	off = (uint64_t)group_first(cg) * bsize +
	    (bsize == 1024 ? 0 : SBLOCKOFFSET);
	if (ext2fs_sbwrite(&disk, off) == -1)
		err(36, "%s", disk.d_error);
	fs->e2fs_block_group_nr = 0;
}

/*
 * Write the group descriptor table: gdbcount blocks in the primary
 * location, plus a full copy in every group that carries a superblock.
 */
static void
wtgdt(void)
{
	uint32_t cg, i, j;

	for (i = 0; i < gdbcount; i++) {
		memset(iobuf, 0, bsize);
		for (j = 0; j < descpb && i * descpb + j < gcount; j++)
			memcpy(iobuf + (size_t)j * E2FS_REV0_GD_SIZE,
			    &disk.d_gd[i * descpb + j], E2FS_REV0_GD_SIZE);
		wtfs((bsize > SBLOCKSIZE ? 0 : 1) + i + 1, bsize,
		    (char *)iobuf);
		for (cg = 1; cg < gcount; cg++)
			if (ext2fs_cg_hassb(&disk, cg))
				wtfs(group_first(cg) + i + 1, bsize,
				    (char *)iobuf);
	}
}

static uint32_t
group_first(int cg)
{

	return (first_dblock + (uint32_t)cg * bpg);
}

static int
ilog2(int n)
{
	int i;

	for (i = 0; i < 40; i++)
		if ((1 << i) >= n)
			return (i);
	errx(1, "ilog2");
}
