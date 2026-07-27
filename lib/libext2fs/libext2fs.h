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


struct ext2fsd {
	const char	*d_error;
	int		 d_fd;
};

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

int ext2fs_disk_open(struct ext2fsd *, const char *);
int ext2fs_disk_close(struct ext2fsd *);

#endif /* __LIBEXT2FS_H__ */
