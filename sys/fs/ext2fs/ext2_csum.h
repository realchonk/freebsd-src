/*-
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by Benjamin Stürz <benni@stuerz.xyz>
 * under sponsorship from the FreeBSD Foundation.
 */

#ifndef _FS_EXT2FS_EXT2_CSUM_H_
#define	_FS_EXT2FS_EXT2_CSUM_H_

#include <sys/types.h>

#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>

/*
 * Metadata checksums shared between the kernel file system and
 * libext2fs (ext2_csum.c).  The computations are pure functions of
 * the seed, the inode identity, and the buffers involved; the seed
 * comes from the superblock (uuid, or the CSUM_SEED field).
 */
uint32_t ext2_csum_seed(const struct ext2fs *);
int ext2_sb_csum_fits(const struct ext2fs *);
int ext2_sb_csum_check(const struct ext2fs *);
void ext2_sb_csum_update(struct ext2fs *);

uint32_t ext2_ei_csum_value(uint32_t, uint32_t, uint32_t,
    const struct ext2fs_dinode *, uint32_t);

uint16_t ext2_gd_csum_value(uint32_t, uint32_t, const struct ext2_gd *,
    uint16_t);
uint32_t ext2_bitmap_csum_value(uint32_t, const void *, uint32_t);
void ext2_gd_bbitmap_csum_update(uint32_t, const void *, uint32_t,
    struct ext2_gd *, uint16_t);
void ext2_gd_ibitmap_csum_update(uint32_t, const void *, uint32_t,
    struct ext2_gd *, uint16_t);

#ifndef _KERNEL
struct ext2fs_direct_tail;

uint16_t ext2_gd_csum_legacy(const uint8_t *, uint32_t,
    const struct ext2_gd *, uint16_t);
int ext2_dirent_has_tail(const void *, uint32_t);
int ext2_dirent_csum_check(uint32_t, uint32_t, uint32_t, void *, uint32_t);
void ext2_dirent_csum_update(uint32_t, uint32_t, uint32_t, void *, uint32_t);
void ext2_init_dirent_tail(struct ext2fs_direct_tail *);
int ext2_ei_csum_check(uint32_t, uint32_t, uint32_t,
    const struct ext2fs_dinode *, uint32_t);
void ext2_ei_csum_update(uint32_t, uint32_t, uint32_t,
    struct ext2fs_dinode *, uint32_t);
#endif /* !_KERNEL */

#endif /* !_FS_EXT2FS_EXT2_CSUM_H_ */
