/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#ifndef _NEWFS_EXT2FS_H_
#define	_NEWFS_EXT2FS_H_

extern int		 Nflag;			/* do not write */
extern int		 nflag;			/* do not create lost+found */
extern int		 bsize;			/* block size */
extern int		 density;		/* bytes per inode */
extern int		 minfree;		/* reserved blocks % */
extern intmax_t		 fssize;			/* file system size in bytes */
extern u_char		*volumelabel;		/* volume label */
extern int		 sectorsize;
extern intmax_t		 mediasize;
extern struct ext2fsd	 disk;

void mkfs(const char *fsys);

#endif /* !_NEWFS_EXT2FS_H_ */
