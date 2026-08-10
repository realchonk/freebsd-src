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
#include <fs/ext2fs/inode.h>
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

/*
 * sblock.c
 */
int ext2fs_sbread(struct ext2fsd *);
/* low level superblock read/write functions */
int ext2fs_sbget(int, struct ext2fs **, off_t, int);

/*
 * group.c
 */
int ext2fs_gdread(struct ext2fsd *);

/*
 * block.c
 */
ssize_t ext2fs_bread(struct ext2fsd *, uint64_t, void *, size_t);

#endif /* __LIBEXT2FS_H__ */
