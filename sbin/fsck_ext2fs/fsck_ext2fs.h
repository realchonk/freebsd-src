/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#ifndef __FSCK_EXT2FS_H__
#define __FSCK_EXT2FS_H__

#include <sys/param.h>
#include <sys/endian.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <stdint.h>
#include <stdio.h>

/* ext2_dir.h uses the kernel-private doff_t; see ufs/ufs/dir.h. */
typedef int32_t doff_t;

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_dir.h>
#include <fs/ext2fs/ext2fs.h>
#include <libext2fs.h>

/*
 * Exit codes (rc.d/fsck dispatches on these).
 */
#define	EEXIT	8		/* standard error exit */
#define	ERERUN	16		/* rerun needed */

/*
 * Inode states.  DFOUND is DSTATE with the low bit set; DVALID is DSTATE
 * with the low two bits clear, as in fsck_ffs.
 */
#define	USTATE	0x1		/* unallocated */
#define	FSTATE	0x2		/* file */
#define	FZLINK	0x3		/* file, zero link count */
#define	DSTATE	0x4		/* directory, not yet reached */
#define	DZLINK	0x5		/* directory, zero link count */
#define	DFOUND	0x6		/* directory reached by descent */
#define	DCLEAR	0x8		/* directory to be cleared */
#define	FCLEAR	0x9		/* file to be cleared */

#define	S_IS_DUNFOUND(s)	(((s) & ~0x1) == DSTATE)
#define	S_IS_DVALID(s)	(((s) & ~0x3) == DSTATE)

/*
 * ckinode()/dirscan() callback returns.
 */
#define	STOP	0x01
#define	SKIP	0x04
#define	KEEPON	0x10
#define	ALTERED	0x20
#define	FOUND	0x40

struct inostat {
	u_char	ino_state;
	u_char	ino_type;
	u_short	ino_linkcnt;
	u_short	ino_ftype;	/* EXT2_FT_* from the inode mode */
};

struct inostatlist {
	u_long	il_numalloced;
	struct inostat *il_stat;
};

struct inoinfo {
	ino_t	i_number;
	ino_t	i_parent;
	ino_t	i_dotdot;
	size_t	i_isize;
	u_int	i_numblks;
	uint64_t i_blks[1];
};

struct dups {
	struct dups *next;
	uint64_t	dup;
};

/*
 * Block-walk closure passed to ckinode(); id_func runs per block.
 */
enum idtype { ADDR, DATA };

struct inodesc {
	enum idtype id_type;
	int	(*id_func)(struct inodesc *);
	off_t	id_filesize;
	ino_t	id_ino;
	ino_t	id_parent;	/* DIR: parent inode we are checking */
	int	id_loc;		/* DIR: offset within the current block */
	int	id_entryno;	/* DIR: entry number within the directory */
	struct ext2fs_direct_2 *id_dirp;	/* DIR: current entry */
	uint64_t id_blkno;
	uint64_t id_firstblock;
	uint64_t id_numblocks;
};

/*
 * globals.c
 */
extern const char *cdevname;
extern int	bflag, ckclean, debug, fswritefd;
extern int	nflag, preen, skipclean, yflag;
extern int	maxfsblock, maxino;
extern int64_t	n_blks, n_files;
extern char	*blockmap;
extern struct inostatlist *inostathead;
extern struct dups *duplist, *muldup;
extern struct ext2fsd disk;
extern ino_t	*parentof;	/* containing directory per inode, pass2 */

#define	setbmap(b)	(blockmap[(b) >> 3] |= 1u << ((b) & 7))
#define	clrbmap(b)	(blockmap[(b) >> 3] &= ~(1u << ((b) & 7)))
#define	testbmap(b)	(blockmap[(b) >> 3] & (1u << ((b) & 7)))

/* Per-inode state for the inode number ino (1-based). */
struct inostat *getinostat(ino_t);

/*
 * Group descriptor getters (kernel versions live in ext2_alloc.c).
 */
static inline uint32_t
gd_nbfree(const struct ext2_gd *gd)
{

	return (((uint32_t)le16toh(gd->ext4bgd_nbfree_hi) << 16) |
	    le16toh(gd->ext2bgd_nbfree));
}

static inline uint32_t
gd_nifree(const struct ext2_gd *gd)
{

	return (((uint32_t)le16toh(gd->ext4bgd_nifree_hi) << 16) |
	    le16toh(gd->ext2bgd_nifree));
}

static inline uint32_t
gd_ndirs(const struct ext2_gd *gd)
{

	return (((uint32_t)le16toh(gd->ext4bgd_ndirs_hi) << 16) |
	    le16toh(gd->ext2bgd_ndirs));
}

/*
 * main.c
 */
int	checkfilesys(const char *);
void	usage(void) __dead2;

/*
 * globals.c
 */
void	fsckinit(void);

/*
 * setup.c
 */
int	openfilesys(const char *);
int	readsb(void);
int	setup(const char *);

/*
 * fsutil.c
 */
void	pfatal(const char *, ...) __printflike(1, 2);
void	pwarn(const char *, ...) __printflike(1, 2);
int	reply(const char *);
int	dofix(struct inodesc *, const char *);
void	ckfini(int markclean);

/*
 * inode.c
 */
int	ckinode(struct ext2fs_dinode *, struct inodesc *);
int	ginode(ino_t, struct ext2fs_dinode *);

/*
 * pass1.c
 */
int	mode2ftype(mode_t);

/*
 * pass2.c
 */
void	pass2(void);

/*
 * pass3.c
 */
void	pass3(void);

/*
 * pass4.c
 */
void	pass4(void);

/*
 * pass1.c
 */
void	pass1(void);

/*
 * pass5.c
 */
void	pass5(void);

#endif /* __FSCK_EXT2FS_H__ */
