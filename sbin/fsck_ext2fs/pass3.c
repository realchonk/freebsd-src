/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fsck_ext2fs.h"

static struct inodesc idesc;
static ino_t *dstack;
static size_t dsp;
static ino_t lfino;			/* lost+found, 0 until located */

/* pass3 callback: enqueue directory entries not yet reached. */
static int
pass3check(struct inodesc *idp)
{
	struct ext2fs_direct_2 *dp = idp->id_dirp;
	ino_t ino = le32toh(dp->e2d_ino);

	if (ino != 0 && ino <= (ino_t)maxino &&
	    getinostat(ino)->ino_state == DSTATE) {
		getinostat(ino)->ino_state = DFOUND;
		dstack[dsp++] = ino;
	}
	return (KEEPON);
}

/*
 * The first inode from the unreserved range that no pass found
 * allocated.
 */
static ino_t
allocino(void)
{
	struct ext2fs *fs = &disk.d_fs;
	ino_t ino, firstino;

	firstino = le32toh(fs->e2fs_rev) == E2FS_REV0 ? EXT2_FIRSTINO :
	    le32toh(fs->e2fs_first_ino);
	for (ino = firstino; ino <= (ino_t)maxino; ino++)
		if (getinostat(ino)->ino_state == USTATE)
			return (ino);
	return (0);
}

/*
 * A free data block per the on-disk block bitmap, claimed for the new
 * owner so pass 5 keeps it allocated.
 */
static uint64_t
allocblk(void)
{
	struct ext2fs *fs = &disk.d_fs;
	uint64_t gfirst, b;
	uint32_t bsize, bpg, first, bcount, gblocks, i;
	char *bmap;

	bsize = 1024u << le32toh(fs->e2fs_log_bsize);
	bpg = le32toh(fs->e2fs_bpg);
	first = le32toh(fs->e2fs_first_dblock);
	bcount = le32toh(fs->e2fs_bcount);
	if ((bmap = malloc(bsize)) == NULL)
		err(8, "cannot allocate bitmap buffer");

	for (uint32_t g = 0; g < disk.d_gcount; g++) {
		gfirst = first + (uint64_t)g * bpg;
		gblocks = bpg;
		if (gfirst + gblocks > bcount)
			gblocks = bcount - gfirst;
		if (ext2fs_bread(&disk, le32toh(disk.d_gd[g].ext2bgd_b_bitmap),
		    bmap, bsize) != (ssize_t)bsize)
			continue;
		for (i = 0; i < gblocks; i++) {
			b = gfirst + i;
			if (isclr(bmap, i) && !testbmap(b)) {
				setbmap(b);
				free(bmap);
				return (b);
			}
		}
	}
	free(bmap);
	return (0);
}

/*
 * Create lost+found in a fresh inode and block, and link it from the
 * root directory.
 */
static ino_t
mklf(void)
{
	struct ext2fs_dinode di;
	struct ext2fs_direct_2 *dp;
	struct inostat *stp;
	uint32_t bsize;
	uint64_t b;
	ino_t lf;
	char *buf;

	lf = allocino();
	b = allocblk();
	if (lf == 0 || b == 0)
		return (0);
	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate directory buffer");

	/* '.' and '..' lead the block; the tail stays unused. */
	memset(buf, 0, bsize);
	dp = (struct ext2fs_direct_2 *)buf;
	dp->e2d_ino = htole32((uint32_t)lf);
	dp->e2d_reclen = htole16(EXT2_DIR_REC_LEN(1));
	dp->e2d_namlen = 1;
	dp->e2d_type = EXT2_FT_DIR;
	memcpy(dp->e2d_name, ".", 1);
	dp = (struct ext2fs_direct_2 *)(buf + EXT2_DIR_REC_LEN(1));
	dp->e2d_ino = htole32((uint32_t)EXT2_ROOTINO);
	dp->e2d_namlen = 2;
	dp->e2d_type = EXT2_FT_DIR;
	memcpy(dp->e2d_name, "..", 2);
	if (le32toh(disk.d_fs.e2fs_features_rocompat) &
	    EXT2F_ROCOMPAT_METADATA_CKSUM) {
		dp->e2d_reclen =
		    htole16(bsize - 2 * EXT2_DIR_REC_LEN(1) -
		    sizeof(struct ext2fs_direct_tail));
		ext2_init_dirent_tail(EXT2_DIRENT_TAIL(buf, bsize));
		ext2_dirent_csum_update(disk.d_csum_seed, lf, 0, buf, bsize);
	} else
		dp->e2d_reclen = htole16(bsize - EXT2_DIR_REC_LEN(1));
	if (ext2fs_bwrite(&disk, b, buf, bsize) != (ssize_t)bsize) {
		free(buf);
		return (0);
	}
	free(buf);

	memset(&di, 0, sizeof(di));
	di.e2di_mode = htole16(S_IFDIR | 0755);
	di.e2di_nlink = htole16(2);
	di.e2di_size = htole32(bsize);
	di.e2di_nblock = htole32(bsize / 512);
	di.e2di_blocks[0] = htole32((uint32_t)b);
	if (ext2fs_iput(&disk, lf, &di) != 0)
		return (0);
	if (dir_add_entry(EXT2_ROOTINO, "lost+found", EXT2_FT_DIR, lf) != 0)
		return (0);

	/* The root gains lost+found's '..' reference: both the stored
	 * count and a real reference, so the residual stays as is. */
	if (ginode(EXT2_ROOTINO, &di) == 0) {
		di.e2di_nlink = htole16(le16toh(di.e2di_nlink) + 1);
		ext2fs_iput(&disk, EXT2_ROOTINO, &di);
	}

	stp = getinostat(lf);
	stp->ino_state = DFOUND;
	stp->ino_ftype = EXT2_FT_DIR;
	stp->ino_linkcnt = 0;		/* '.' plus the root entry */
	n_files++;
	fsmodified = 1;
	return (lf);
}

/*
 * The inode of lost+found per the root directory, 0 when absent.
 */
static ino_t
findlf(void)
{
	struct ext2fs_dinode di;
	struct ext2fs_direct_2 *dp;
	uint32_t bsize, reclen, i, off, nblocks;
	uint64_t b, blks[EXT2_NDIR_BLOCKS];
	ino_t found = 0;
	char *buf;

	if (ginode(EXT2_ROOTINO, &di) != 0)
		return (0);
	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate directory buffer");
	nblocks = dir_blocks(&di, blks, EXT2_NDIR_BLOCKS);
	for (i = 0; i < nblocks && found == 0; i++) {
		b = blks[i];
		if (b == 0 || b >= (uint64_t)maxfsblock)
			continue;
		if (ext2fs_bread(&disk, b, buf, bsize) != (ssize_t)bsize)
			continue;
		for (off = 0; off + EXT2_DIR_REC_LEN(0) <= bsize; ) {
			dp = (struct ext2fs_direct_2 *)(buf + off);
			reclen = le16toh(dp->e2d_reclen);
			if (reclen < EXT2_DIR_REC_LEN(0) ||
			    off + reclen > bsize)
				break;
			if (dp->e2d_namlen == 10 &&
			    memcmp(dp->e2d_name, "lost+found", 10) == 0) {
				found = le32toh(dp->e2d_ino);
				break;
			}
			off += reclen;
		}
	}
	free(buf);
	return (found);
}

/*
 * Point the '..' entry of directory dir at parent; *oldp receives the
 * inode '..' pointed at before.  Returns 0 on success.
 */
static int
setdotdot(ino_t dir, ino_t parent, ino_t *oldp)
{
	struct ext2fs_dinode di;
	struct ext2fs_direct_2 *dp;
	uint64_t blks[1];
	uint32_t bsize, reclen;
	char *buf;

	*oldp = 0;
	if (ginode(dir, &di) != 0)
		return (-1);
	bsize = 1024u << le32toh(disk.d_fs.e2fs_log_bsize);
	if (dir_blocks(&di, blks, 1) != 1 ||
	    blks[0] >= (uint64_t)maxfsblock)
		return (-1);
	if ((buf = malloc(bsize)) == NULL)
		err(8, "cannot allocate directory buffer");
	if (ext2fs_bread(&disk, blks[0], buf, bsize) != (ssize_t)bsize) {
		free(buf);
		return (-1);
	}
	dp = (struct ext2fs_direct_2 *)buf;
	reclen = le16toh(dp->e2d_reclen);
	if (reclen >= EXT2_DIR_REC_LEN(1) && dp->e2d_namlen == 1 &&
	    dp->e2d_name[0] == '.' && reclen + EXT2_DIR_REC_LEN(2) <= bsize) {
		dp = (struct ext2fs_direct_2 *)(buf + reclen);
		if (le16toh(dp->e2d_reclen) >= EXT2_DIR_REC_LEN(2) &&
		    dp->e2d_namlen == 2 && dp->e2d_name[0] == '.' &&
		    dp->e2d_name[1] == '.') {
			*oldp = le32toh(dp->e2d_ino);
			dp->e2d_ino = htole32((uint32_t)parent);
			if ((le32toh(disk.d_fs.e2fs_features_rocompat) &
			    EXT2F_ROCOMPAT_METADATA_CKSUM) != 0) {
				struct ext2fs_dinode pdi;

				if (ext2fs_iget(&disk, dir, &pdi) == 0) {
					uint32_t gen = le32toh(pdi.e2di_gen);

					ext2_dirent_csum_update(
					    disk.d_csum_seed, dir, gen,
					    buf, bsize);
				}
			}
			if (ext2fs_bwrite(&disk, blks[0], buf, bsize) !=
			    (ssize_t)bsize) {
				free(buf);
				return (-1);
			}
			free(buf);
			return (0);
		}
	}
	free(buf);
	return (-1);
}

/*
 * Reconnect an orphan directory into lost+found: name it by its inode
 * number, transfer its '..' reference, and account the new entry.
 */
static int
reattach(ino_t orphan)
{
	struct ext2fs_dinode di;
	char name[16];
	ino_t old;

	if (lfino == 0)
		lfino = findlf();
	if (lfino == 0) {
		pwarn("LOST+FOUND NOT FOUND\n");
		if (preen || reply("CREATE") == 1)
			lfino = mklf();
	}
	if (lfino == 0)
		return (-1);

	snprintf(name, sizeof(name), "#%ju", (uintmax_t)orphan);
	if (dir_add_entry(lfino, name, EXT2_FT_DIR, orphan) != 0)
		return (-1);
	if (setdotdot(orphan, lfino, &old) != 0)
		return (-1);
	if (old != 0 && old != lfino && old <= (ino_t)maxino &&
	    getinostat(old)->ino_state != USTATE)
		getinostat(old)->ino_linkcnt++;

	/* lost+found gains the '..' reference and one stored link: the
	 * residual stays as is; the new entry alone shifts the orphan's. */
	if (ginode(lfino, &di) == 0) {
		di.e2di_nlink = htole16(le16toh(di.e2di_nlink) + 1);
		ext2fs_iput(&disk, lfino, &di);
	}
	getinostat(orphan)->ino_linkcnt--;	/* the new entry */
	getinostat(orphan)->ino_state = DFOUND;
	parentof[orphan] = lfino;
	fsmodified = 1;
	return (0);
}

/*
 * Check directory connectivity: mark every directory reachable from
 * the root by walking directory entries; the remainder are orphans,
 * reattached in lost+found or cleared.
 */
void
pass3(void)
{
	ino_t ino;

	dstack = calloc(maxino + 1, sizeof(*dstack));
	if (dstack == NULL)
		err(8, "cannot allocate directory stack");
	memset(&idesc, 0, sizeof(idesc));
	idesc.id_type = DATA;
	idesc.id_func = pass3check;
	idesc.id_fix = nflag ? IGNORE : DONTKNOW;

	if (getinostat(EXT2_ROOTINO)->ino_state == DSTATE) {
		getinostat(EXT2_ROOTINO)->ino_state = DFOUND;
		dstack[dsp++] = EXT2_ROOTINO;
	}
reached:
	while (dsp > 0) {
		struct ext2fs_dinode di;

		ino = dstack[--dsp];
		if (ginode(ino, &di) != 0)
			continue;
		uint64_t blk1[1];

		idesc.id_ino = ino;
		idesc.id_entryno = 0;
		if (dir_blocks(&di, blk1, 1) == 1)
			idesc.id_firstblock = blk1[0];
		else
			idesc.id_firstblock = 0;
		idesc.id_filesize = le32toh(di.e2di_size);
		ckinode(&di, &idesc);
	}

	for (ino = EXT2_ROOTINO + 1; ino <= (ino_t)maxino; ino++) {
		if (getinostat(ino)->ino_state != DSTATE)
			continue;
		pwarn("UNREF DIR I=%ju\n", (uintmax_t)ino);
		if (dofix(&idesc, "RECONNECT IN LOST+FOUND") != 0) {
			if (reattach(ino) == 0) {
				/*
				 * The subtree below the reattached
				 * directory is reachable through it.
				 */
				dstack[dsp++] = ino;
				goto reached;
			}
			pwarn("RECONNECT I=%ju FAILED\n", (uintmax_t)ino);
		}
		clri(&idesc, ino, "DIR");
	}
	free(dstack);
	dstack = NULL;
}
