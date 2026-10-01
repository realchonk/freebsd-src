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
 * ext4 extent tree on disk.  The kernel's ext2_extents.h declares
 * kernel-only prototypes, so the on-disk layout is repeated here;
 * see sys/fs/ext2fs/ext2_extents.h.  All fields little-endian.
 */
#define	EXT4_EXT_MAGIC		0xf30a
#define	EXT_INIT_MAX_LEN	(1u << 15)

struct ext4_extent {
	uint32_t	e_blk;		/* first logical block */
	uint16_t	e_len;		/* number of blocks */
	uint16_t	e_start_hi;	/* high bits of physical block */
	uint32_t	e_start_lo;	/* low bits of physical block */
};

struct ext4_extent_index {
	uint32_t	ei_blk;		/* indexes logical blocks */
	uint32_t	ei_leaf_lo;	/* next level's physical block */
	uint16_t	ei_leaf_hi;	/* high bits of physical block */
	uint16_t	ei_unused;
};

struct ext4_extent_header {
	uint16_t	eh_magic;	/* EXT4_EXT_MAGIC */
	uint16_t	eh_ecount;	/* number of valid entries */
	uint16_t	eh_max;		/* capacity of store in entries */
	uint16_t	eh_depth;	/* depth of the extent tree */
	uint32_t	eh_gen;		/* generation of extent tree */
};

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

/*
 * Whether a problem may be fixed, per walk (id_fix) — see dofix().
 */
enum fixstate { DONTKNOW, FIX, NOFIX, IGNORE };

#define	MAXBAD	10			/* limit on bad blks (per inode) */
#define	MAXDUP	10			/* limit on dup blks (per inode) */

struct inostat {
	u_char	ino_state;
	u_char	ino_type;
	int16_t	ino_linkcnt;	/* stored count minus references found */
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
	enum fixstate id_fix;	/* may this walk fix what it finds? */
	uint64_t id_ptrblk;	/* ADDR: block holding the pointer to
				 * id_blkno, or 0 if it is in the inode */
	u_int	id_ptroff;	/* ADDR: byte offset of the pointer in it */
	u_int	id_ptridx;	/* ADDR: index in e2di_blocks[] when
				 * id_ptrblk == 0 */
};

/*
 * globals.c
 */
extern const char *cdevname;
extern int	bflag, ckclean, debug, fswritefd;
extern int	nflag, preen, skipclean, yflag;
extern int	maxfsblock, maxino;
extern int	fsmodified;	/* 1 == the filesystem has been written */
extern int	uncorrected;	/* 1 == a problem was left unfixed */
extern int64_t	n_blks, n_files;
extern char	*blockmap;
extern struct inostatlist *inostathead;
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
int	direntry_write(struct inodesc *);
int	dir_add_entry(ino_t, const char *, int, ino_t);
uint32_t dir_blocks(const struct ext2fs_dinode *, uint64_t *, uint32_t);

/*
 * pass4.c
 */
void	clri(struct inodesc *, ino_t, const char *);

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
