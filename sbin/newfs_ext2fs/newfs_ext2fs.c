/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/param.h>
#include <sys/disk.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libutil.h>

#include <libext2fs.h>

#include "newfs_ext2fs.h"

int		 Nflag;
int		 nflag;
int		 bsize;
int		 density;
int		 minfree = 5;
intmax_t	 fssize;
u_char		*volumelabel;
int		 sectorsize;
intmax_t	 mediasize;
struct ext2fsd	 disk;

static void usage(void) __dead2;
static void getfssize(intmax_t *, const char *, intmax_t, intmax_t);

int
main(int argc, char *argv[])
{
	struct stat	 st;
	char		*special;
	int		 ch;

	while ((ch = getopt(argc, argv, "L:Nb:i:m:ns:")) != -1) {
		switch (ch) {
		case 'L':
			volumelabel = (u_char *)optarg;
			if (strlen(optarg) >= sizeof(disk.d_fs.e2fs_vname))
				errx(17, "volume label too long");
			{
				const u_char *p;

				for (p = volumelabel; *p != '\0'; p++)
					if (!isalnum(*p) && *p != '_' && *p != '-')
						errx(17, "bad volume label");
			}
			break;
		case 'N':
			Nflag = 1;
			break;
		case 'b':
			bsize = atoi(optarg);
			break;
		case 'i':
			density = atoi(optarg);
			break;
		case 'm':
			minfree = atoi(optarg);
			break;
		case 'n':
			nflag = 1;
			break;
		case 's': {
			int64_t size;

			if (expand_number(optarg, &size) < 0)
				errx(1, "invalid size %s", optarg);
			fssize = size;
			break;
		}
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc != 1)
		usage();
	special = argv[0];
	if (strchr(special, '/') == NULL) {
		char *s;

		if (asprintf(&s, "%s%s", _PATH_DEV, special) < 0)
			err(1, "asprintf");
		special = s;
	}

	if (ext2fs_disk_fillout_blank(&disk, special) == -1)
		errx(1, "%s: %s", special, disk.d_error);

	if (!Nflag && ext2fs_disk_write(&disk) == -1)
		errx(1, "%s: %s", special, disk.d_error);

	if (fstat(disk.d_fd, &st) == -1)
		err(1, "%s: fstat", special);
	if (S_ISCHR(st.st_mode)) {
		if (ioctl(disk.d_fd, DIOCGSECTORSIZE, &sectorsize) == -1 ||
		    ioctl(disk.d_fd, DIOCGMEDIASIZE, &mediasize) == -1)
			err(1, "%s: ioctl(DIOCGMEDIASIZE)", special);
	} else {
		sectorsize = 512;
		mediasize = st.st_size;
	}

	getfssize(&fssize, special, mediasize, 0);

	if (bsize == 0)
		bsize = 4096;
	if (density == 0)
		density = 16384;

	mkfs(special);
	ext2fs_disk_close(&disk);
	exit(0);
}

static void
usage(void)
{
	(void)fprintf(stderr,
	    "usage: newfs_ext2fs [-Nn] [-b block-size] [-i bytes-per-inode] "
	    "[-L volume-label]\n"
	    "                    [-m reserved-percent] [-s size] special\n");
	exit(1);
}

static void
getfssize(intmax_t *fssize, const char *special, intmax_t avail,
    intmax_t reserved)
{

	if (*fssize == 0)
		*fssize = avail - reserved;
	if (*fssize > avail - reserved)
		errx(1, "%s: maximum file system size is %jd bytes", special,
		    avail - reserved);
}
