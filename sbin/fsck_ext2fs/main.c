/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <err.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fsck_ext2fs.h"

int
main(int argc, char *argv[])
{
	int ch, ret = 0;

	/* Interleave reports, prompts, and fixes in order. */
	setvbuf(stdout, NULL, _IONBF, 0);

	while ((ch = getopt(argc, argv, "b:Cdfnpy")) != -1) {
		switch (ch) {
		case 'b':
			skipclean = 0;
			bflag = atoi(optarg);
			printf("Alternate super block location: %d\n", bflag);
			break;
		case 'C':
			ckclean++;
			break;
		case 'd':
			debug++;
			break;
		case 'f':
			skipclean = 0;
			break;
		case 'n':
			nflag++;
			yflag = 0;
			break;
		case 'p':
			preen++;
			ckclean++;
			break;
		case 'y':
			yflag++;
			nflag = 0;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1)
		usage();

	while (argc-- > 0)
		ret |= checkfilesys(*argv++);
	return (ret);
}

int
checkfilesys(const char *filesys)
{
	int ret;

	fsckinit();
	cdevname = filesys;

	switch (setup(filesys)) {
	case 0:
		pwarn("%s: unable to open or verify file system\n", filesys);
		if (preen)
			pfatal("CAN'T CHECK FILE SYSTEM.");
		return (EEXIT);
	case -1:
		pwarn("clean, %u free blocks, %u free inodes\n",
		    le32toh(disk.d_fs.e2fs_fbcount),
		    le32toh(disk.d_fs.e2fs_ficount));
		return (0);
	}

	printf("** Last Mounted %s\n", disk.d_fs.e2fs_fsmnt);
	pass1();
	pass2();
	pass3();
	pass4();
	pass5();

	/*
	 * A checked filesystem with nothing left uncorrected is clean:
	 * without the bit, the kernel keeps refusing read-write mounts.
	 */
	if (fswritefd >= 0 && uncorrected == 0 &&
	    (le16toh(disk.d_fs.e2fs_state) & E2FS_ISCLEAN) == 0) {
		disk.d_fs.e2fs_state =
		    htole16(le16toh(disk.d_fs.e2fs_state) | E2FS_ISCLEAN);
		if (ext2fs_sbwrite(&disk, disk.d_sblockloc) == 0)
			fsmodified = 1;
		else
			pwarn("SUPERBLOCK WRITE FAILED\n");
	}
	/* In-use counts as e2fsck reports them: total - free from the
	 * superblock counters; n_files/n_blks stay internal to the passes. */
	printf("%ju files, %ju blocks, %u free\n",
	    (uintmax_t)((uint64_t)disk.d_gcount * le32toh(disk.d_fs.e2fs_ipg) -
		le32toh(disk.d_fs.e2fs_ficount)),
	    (uintmax_t)(le32toh(disk.d_fs.e2fs_bcount) -
		le32toh(disk.d_fs.e2fs_fbcount)),
	    le32toh(disk.d_fs.e2fs_fbcount));
	ckfini(0);
	if (fsmodified)
		printf("***** FILE SYSTEM WAS MODIFIED *****\n");
	ret = 0;
	if (fsmodified)
		ret |= 1;			/* filesystem modified */
	if (uncorrected)
		ret |= 4;			/* errors left uncorrected */
	return (ret);
}

void
usage(void)
{
	(void)fprintf(stderr, "usage: fsck_ext2fs [-b #] [-dfnpy] filesys\n");
	exit(EEXIT);
}
