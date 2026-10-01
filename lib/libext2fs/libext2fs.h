/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#ifndef __LIBEXT2FS_H__
#define __LIBEXT2FS_H__

#include <sys/types.h>
#include <stddef.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2fs.h>

struct ext2fsd {
	union {
		struct ext2fs d_fs;	/* filesystem information */
		char d_sb[SBLOCKSIZE];	/* superblock as buffer */
	} d_sbunion;
	daddr_t		 d_sblock;	/* superblock location */
	off_t		 d_sblockloc;	/* where to look for the superblock */
	const char	*d_name;	/* disk name */
	const char	*d_error;	/* human readable disk error */
	int		 d_lookupflags;	/* flags to superblock lookup */
	int		 d_mine;	/* internal flags */
	int		 d_fd;		/* raw device file descriptor */
	int		 d_version;	/* 2=ext2, 3=ext3, 4=ext4 */
	struct ext2_gd	*d_gd;		/* block group descriptor table */
	uint32_t	 d_gcount;	/* number of block groups */
	uint32_t	 d_csum_seed;	/* metadata checksum seed */
};

#define	d_fs	d_sbunion.d_fs

#ifdef _LIBEXT2FS
/*
 * Trace steps through libext2fs, to be used at entry and erroneous return.
 */
static inline void
ERROR(struct ext2fsd *disk, const char *str)
{

#ifdef	_LIBEXT2FS_DEBUGGING
	if (str != NULL) {
		fprintf(stderr, "libext2fs: %s", str);
		if (errno != 0)
			fprintf(stderr, ": %s", strerror(errno));
		fprintf(stderr, "\n");
	}
#endif
	if (disk != NULL)
		disk->d_error = str;
}
#endif /* _LIBEXT2FS */

/*
 * type.c
 */
int ext2fs_disk_close(struct ext2fsd *);
int ext2fs_disk_fillout(struct ext2fsd *, const char *);
int ext2fs_disk_fillout_blank(struct ext2fsd *, const char *);
int ext2fs_disk_write(struct ext2fsd *);

/*
 * sblock.c
 */
int ext2fs_sbread(struct ext2fsd *);
/* low level superblock read/write functions */
int ext2fs_sbget(int, struct ext2fs **, off_t, int);
int ext2fs_sbwrite(struct ext2fsd *, off_t);
int ext2fs_sbverify(struct ext2fsd *);

/*
 * group.c
 */
int ext2fs_gdread(struct ext2fsd *);
int ext2fs_gdwrite(struct ext2fsd *);
int ext2fs_cg_hassb(const struct ext2fsd *, int);

/*
 * block.c
 */
ssize_t ext2fs_bread(struct ext2fsd *, uint64_t, void *, size_t);
ssize_t ext2fs_bwrite(struct ext2fsd *, uint64_t, const void *, size_t);

/*
 * inode.c
 */
int ext2fs_iget(struct ext2fsd *, ino_t, struct ext2fs_dinode *);
int ext2fs_iput(struct ext2fsd *, ino_t, const struct ext2fs_dinode *);
int ext2fs_icsum(struct ext2fsd *, ino_t);

/*
 * Metadata checksums (ext2_csum.c, shared with the kernel ext2fs
 * driver).  The seed comes from ext2_csum_seed() over the superblock.
 */
struct ext2fs_direct_tail;
uint32_t ext2_csum_seed(const struct ext2fs *);
int ext2_sb_csum_fits(const struct ext2fs *);
int ext2_sb_csum_check(const struct ext2fs *);
void ext2_sb_csum_update(struct ext2fs *);
uint32_t ext2_ei_csum_value(uint32_t, uint32_t, uint32_t,
    const struct ext2fs_dinode *, uint32_t);
int ext2_ei_csum_check(uint32_t, uint32_t, uint32_t,
    const struct ext2fs_dinode *, uint32_t);
void ext2_ei_csum_update(uint32_t, uint32_t, uint32_t,
    struct ext2fs_dinode *, uint32_t);
int ext2_dirent_has_tail(const void *, uint32_t);
int ext2_dirent_csum_check(uint32_t, uint32_t, uint32_t, void *, uint32_t);
void ext2_dirent_csum_update(uint32_t, uint32_t, uint32_t, void *, uint32_t);
void ext2_init_dirent_tail(struct ext2fs_direct_tail *);
uint16_t ext2_gd_csum_value(uint32_t, uint32_t, const struct ext2_gd *,
    uint16_t);
uint16_t ext2_gd_csum_legacy(const uint8_t *, uint32_t,
    const struct ext2_gd *, uint16_t);
uint32_t ext2_bitmap_csum_value(uint32_t, const void *, uint32_t);
void ext2_gd_bbitmap_csum_update(uint32_t, const void *, uint32_t,
    struct ext2_gd *, uint16_t);
void ext2_gd_ibitmap_csum_update(uint32_t, const void *, uint32_t,
    struct ext2_gd *, uint16_t);

#endif /* __LIBEXT2FS_H__ */
