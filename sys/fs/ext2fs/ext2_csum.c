/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2017, Fedor Uporov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Metadata checksums for ext2/ext4, shared between the kernel file
 * system and libext2fs: the computations below the wrappers are pure
 * functions of the seed, the inode identity, and the buffers involved.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/endian.h>
#include <sys/gsb_crc32.h>

#include "ext2_csum.h"

#ifdef _KERNEL
#include <sys/systm.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/vnode.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/crc16.h>
#include <sys/mount.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_dir.h>
#include <fs/ext2fs/htree.h>
#include <fs/ext2fs/ext2_extattr.h>
#include <fs/ext2fs/ext2_extents.h>
#include <fs/ext2fs/ext2_extern.h>

SDT_PROVIDER_DECLARE(ext2fs);
/*
 * ext2fs trace probe:
 * arg0: verbosity. Higher numbers give more verbose messages
 * arg1: Textual message
 */
SDT_PROBE_DEFINE2(ext2fs, , trace, csum, "int", "char*");
#else /* !_KERNEL */
/* Userland (libext2fs) build. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/crc16.h>

/* ext2_dir.h uses the kernel-private doff_t; see ufs/ufs/dir.h. */
typedef int32_t doff_t;

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_dir.h>
#endif /* _KERNEL */

#define EXT2_BG_INODE_BITMAP_CSUM_HI_END	\
	(offsetof(struct ext2_gd, ext4bgd_i_bmap_csum_hi) + \
	 sizeof(uint16_t))

#define EXT2_INODE_CSUM_HI_EXTRA_END	\
	(offsetof(struct ext2fs_dinode, e2di_chksum_hi) + sizeof(uint16_t) - \
	 E2FS_REV0_INODE_SIZE)

#define EXT2_BG_BLOCK_BITMAP_CSUM_HI_LOCATION	\
	(offsetof(struct ext2_gd, ext4bgd_b_bmap_csum_hi) + \
	 sizeof(uint16_t))

/*
 * Checksum seeds and the superblock checksum are functions of the
 * superblock alone.
 */
uint32_t
ext2_csum_seed(const struct ext2fs *sb)
{

	if (le32toh(sb->e2fs_features_incompat) & EXT2F_INCOMPAT_CSUM_SEED)
		return (le32toh(sb->e4fs_chksum_seed));
	if (le32toh(sb->e2fs_features_rocompat) &
	    EXT2F_ROCOMPAT_METADATA_CKSUM)
		return (calculate_crc32c(~0, sb->e2fs_uuid,
		    sizeof(sb->e2fs_uuid)));
	return (0);
}

int
ext2_sb_csum_fits(const struct ext2fs *sb)
{

	return (sb->e4fs_chksum_type == EXT4_CRC32C_CHKSUM);
}

int
ext2_sb_csum_check(const struct ext2fs *sb)
{

	if (le32toh(sb->e4fs_sbchksum) ==
	    calculate_crc32c(~0, (const char *)sb,
	    offsetof(struct ext2fs, e4fs_sbchksum)))
		return (0);
	return (EIO);
}

void
ext2_sb_csum_update(struct ext2fs *sb)
{

	sb->e4fs_sbchksum =
	    htole32(calculate_crc32c(~0, (const char *)sb,
	    offsetof(struct ext2fs, e4fs_sbchksum)));
}

#ifdef _KERNEL
void
ext2_sb_csum_set_seed(struct m_ext2fs *fs)
{

	fs->e2fs_csum_seed = ext2_csum_seed(fs->e2fs);
}

int
ext2_sb_csum_verify(struct m_ext2fs *fs)
{

	if (!ext2_sb_csum_fits(fs->e2fs)) {
		printf(
"WARNING: mount of %s denied due bad sb csum type\n", fs->e2fs_fsmnt);
		return (EINVAL);
	}
	if (ext2_sb_csum_check(fs->e2fs) != 0) {
		printf(
"WARNING: mount of %s denied due bad sb csum=0x%x, expected=0x%x - run fsck\n",
		    fs->e2fs_fsmnt, le32toh(fs->e2fs->e4fs_sbchksum),
		    calculate_crc32c(~0, (const char *)fs->e2fs,
		    offsetof(struct ext2fs, e4fs_sbchksum)));
		return (EINVAL);
	}

	return (0);
}

void
ext2_sb_csum_set(struct m_ext2fs *fs)
{

	ext2_sb_csum_update(fs->e2fs);
}

static uint32_t
ext2_extattr_blk_csum(struct inode *ip, uint64_t facl,
    struct ext2fs_extattr_header *header)
{
	struct m_ext2fs *fs;
	uint32_t crc, dummy_crc = 0;
	uint64_t facl_bn = htole64(facl);
	int offset = offsetof(struct ext2fs_extattr_header, h_checksum);

	fs = ip->i_e2fs;

	crc = calculate_crc32c(fs->e2fs_csum_seed, (uint8_t *)&facl_bn,
	    sizeof(facl_bn));
	crc = calculate_crc32c(crc, (uint8_t *)header, offset);
	crc = calculate_crc32c(crc, (uint8_t *)&dummy_crc,
	    sizeof(dummy_crc));
	offset += sizeof(dummy_crc);
	crc = calculate_crc32c(crc, (uint8_t *)header + offset,
	    fs->e2fs_bsize - offset);

	return (htole32(crc));
}

int
ext2_extattr_blk_csum_verify(struct inode *ip, struct buf *bp)
{
	struct ext2fs_extattr_header *header;

	header = (struct ext2fs_extattr_header *)bp->b_data;

	if (EXT2_HAS_RO_COMPAT_FEATURE(ip->i_e2fs, EXT2F_ROCOMPAT_METADATA_CKSUM) &&
	    (header->h_checksum != ext2_extattr_blk_csum(ip, ip->i_facl, header))) {
		SDT_PROBE2(ext2fs, , trace, csum, 1, "bad extattr csum detected");
		return (EIO);
	}

	return (0);
}

void
ext2_extattr_blk_csum_set(struct inode *ip, struct buf *bp)
{
	struct ext2fs_extattr_header *header;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(ip->i_e2fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return;

	header = (struct ext2fs_extattr_header *)bp->b_data;
	header->h_checksum = ext2_extattr_blk_csum(ip, ip->i_facl, header);
}
#endif /* _KERNEL */

void
ext2_init_dirent_tail(struct ext2fs_direct_tail *tp)
{
	memset(tp, 0, sizeof(struct ext2fs_direct_tail));
	tp->e2dt_rec_len = le16toh(sizeof(struct ext2fs_direct_tail));
	tp->e2dt_reserved_ft = EXT2_FT_DIR_CSUM;
}

/*
 * Does tp look like a directory block checksum tail?
 */
static int
dirent_tail_pattern(const struct ext2fs_direct_tail *tp)
{

	if (tp->e2dt_reserved_zero1 == 0 &&
	    le16toh(tp->e2dt_rec_len) == sizeof(struct ext2fs_direct_tail) &&
	    tp->e2dt_reserved_zero2 == 0 &&
	    tp->e2dt_reserved_ft == EXT2_FT_DIR_CSUM)
		return (1);

	return (0);
}

/*
 * Walk the entries of a directory block to its expected tail position.
 */
static struct ext2fs_direct_tail *
dirent_get_tail(struct ext2fs_direct_2 *ep, uint32_t bsize)
{
	struct ext2fs_direct_2 *dep;
	void *top;
	unsigned int rec_len;

	dep = ep;
	top = EXT2_DIRENT_TAIL(ep, bsize);
	rec_len = le16toh(dep->e2d_reclen);

	while (rec_len && !(rec_len & 0x3)) {
		dep = (struct ext2fs_direct_2 *)(((char *)dep) + rec_len);
		if ((void *)dep >= top)
			break;
		rec_len = le16toh(dep->e2d_reclen);
	}

	if (dep != top)
		return (NULL);

	if (dirent_tail_pattern((struct ext2fs_direct_tail *)dep))
		return ((struct ext2fs_direct_tail *)dep);

	return (NULL);
}

#ifdef _KERNEL
int
ext2_is_dirent_tail(struct inode *ip, struct ext2fs_direct_2 *ep)
{
	struct m_ext2fs *fs;

	fs = ip->i_e2fs;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return (0);

	return (dirent_tail_pattern((struct ext2fs_direct_tail *)ep));
}

struct ext2fs_direct_tail *
ext2_dirent_get_tail(struct inode *ip, struct ext2fs_direct_2 *ep)
{

	return (dirent_get_tail(ep, ip->i_e2fs->e2fs_bsize));
}
#endif /* _KERNEL */

static uint32_t
dirent_csum(uint32_t seed, uint32_t ino, uint32_t gen, char *buf, int size)
{
	uint32_t crc;

	ino = htole32(ino);
	gen = htole32(gen);
	crc = calculate_crc32c(seed, (uint8_t *)&ino, sizeof(ino));
	crc = calculate_crc32c(crc, (uint8_t *)&gen, sizeof(gen));
	crc = calculate_crc32c(crc, (uint8_t *)buf, size);

	return (crc);
}

#ifdef _KERNEL
static uint32_t
ext2_dirent_csum(struct inode *ip, struct ext2fs_direct_2 *ep, int size)
{

	return (dirent_csum(ip->i_e2fs->e2fs_csum_seed, ip->i_number,
	    ip->i_gen, (char *)ep, size));
}

int
ext2_dirent_csum_verify(struct inode *ip, struct ext2fs_direct_2 *ep)
{
	uint32_t calculated;
	struct ext2fs_direct_tail *tp;

	tp = ext2_dirent_get_tail(ip, ep);
	if (tp == NULL)
		return (0);

	calculated = ext2_dirent_csum(ip, ep, (char *)tp - (char *)ep);
	if (calculated != le32toh(tp->e2dt_checksum))
		return (EIO);

	return (0);
}
#endif /* _KERNEL */

#ifdef _KERNEL
static struct ext2fs_htree_count *
ext2_get_dx_count(struct inode *ip, struct ext2fs_direct_2 *ep, int *offset)
{
	struct ext2fs_direct_2 *dp;
	struct ext2fs_htree_root_info *root;
	int count_offset;

	if (le16toh(ep->e2d_reclen) == EXT2_BLOCK_SIZE(ip->i_e2fs))
		count_offset = 8;
	else if (le16toh(ep->e2d_reclen) == 12) {
		dp = (struct ext2fs_direct_2 *)(((char *)ep) + 12);
		if (le16toh(dp->e2d_reclen) != EXT2_BLOCK_SIZE(ip->i_e2fs) - 12)
			return (NULL);

		root = (struct ext2fs_htree_root_info *)(((char *)dp + 12));
		if (root->h_reserved1 ||
		    root->h_info_len != sizeof(struct ext2fs_htree_root_info))
			return (NULL);

		count_offset = 32;
	} else
		return (NULL);

	if (offset)
		*offset = count_offset;

	return ((struct ext2fs_htree_count *)(((char *)ep) + count_offset));
}

static uint32_t
ext2_dx_csum(struct inode *ip, struct ext2fs_direct_2 *ep, int count_offset,
    int count, struct ext2fs_htree_tail *tp)
{
	struct m_ext2fs *fs;
	char *buf;
	int size;
	uint32_t inum, old_csum, gen, crc;

	fs = ip->i_e2fs;

	buf = (char *)ep;

	size = count_offset + (count * sizeof(struct ext2fs_htree_entry));
	old_csum = tp->ht_checksum;
	tp->ht_checksum = 0;

	inum = htole32(ip->i_number);
	gen = htole32(ip->i_gen);
	crc = calculate_crc32c(fs->e2fs_csum_seed, (uint8_t *)&inum, sizeof(inum));
	crc = calculate_crc32c(crc, (uint8_t *)&gen, sizeof(gen));
	crc = calculate_crc32c(crc, (uint8_t *)buf, size);
	crc = calculate_crc32c(crc, (uint8_t *)tp, sizeof(struct ext2fs_htree_tail));
	tp->ht_checksum = old_csum;

	return htole32(crc);
}

int
ext2_dx_csum_verify(struct inode *ip, struct ext2fs_direct_2 *ep)
{
	uint32_t calculated;
	struct ext2fs_htree_count *cp;
	struct ext2fs_htree_tail *tp;
	int count_offset, limit, count;

	cp = ext2_get_dx_count(ip, ep, &count_offset);
	if (cp == NULL)
		return (0);

	limit = le16toh(cp->h_entries_max);
	count = le16toh(cp->h_entries_num);
	if (count_offset + (limit * sizeof(struct ext2fs_htree_entry)) >
	    ip->i_e2fs->e2fs_bsize - sizeof(struct ext2fs_htree_tail))
		return (EIO);

	tp = (struct ext2fs_htree_tail *)(((struct ext2fs_htree_entry *)cp) + limit);
	calculated = ext2_dx_csum(ip, ep,  count_offset, count, tp);

	if (tp->ht_checksum != calculated)
		return (EIO);

	return (0);
}

int
ext2_dir_blk_csum_verify(struct inode *ip, struct buf *bp)
{
	struct m_ext2fs *fs;
	struct ext2fs_direct_2 *ep;
	int error = 0;

	fs = ip->i_e2fs;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return (error);

	ep = (struct ext2fs_direct_2 *)bp->b_data;

	if (ext2_dirent_get_tail(ip, ep) != NULL)
		error = ext2_dirent_csum_verify(ip, ep);
	else if (ext2_get_dx_count(ip, ep, NULL) != NULL)
		error = ext2_dx_csum_verify(ip, ep);

	if (error)
		SDT_PROBE2(ext2fs, , trace, csum, 1, "bad directory csum detected");

	return (error);
}

void
ext2_dirent_csum_set(struct inode *ip, struct ext2fs_direct_2 *ep)
{
	struct m_ext2fs *fs;
	struct ext2fs_direct_tail *tp;

	fs = ip->i_e2fs;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return;

	tp = ext2_dirent_get_tail(ip, ep);
	if (tp == NULL)
		return;

	tp->e2dt_checksum =
	    htole32(ext2_dirent_csum(ip, ep, (char *)tp - (char *)ep));
}

void
ext2_dx_csum_set(struct inode *ip, struct ext2fs_direct_2 *ep)
{
	struct m_ext2fs *fs;
	struct ext2fs_htree_count *cp;
	struct ext2fs_htree_tail *tp;
	int count_offset, limit, count;

	fs = ip->i_e2fs;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return;

	cp = ext2_get_dx_count(ip, ep, &count_offset);
	if (cp == NULL)
		return;

	limit = le16toh(cp->h_entries_max);
	count = le16toh(cp->h_entries_num);
	if (count_offset + (limit * sizeof(struct ext2fs_htree_entry)) >
	    ip->i_e2fs->e2fs_bsize - sizeof(struct ext2fs_htree_tail))
		return;

	tp = (struct ext2fs_htree_tail *)(((struct ext2fs_htree_entry *)cp) + limit);
	tp->ht_checksum = ext2_dx_csum(ip, ep,  count_offset, count, tp);
}

static uint32_t
ext2_extent_blk_csum(struct inode *ip, struct ext4_extent_header *ehp)
{
	struct m_ext2fs *fs;
	size_t size;
	uint32_t inum, gen, crc;

	fs = ip->i_e2fs;

	size = EXT4_EXTENT_TAIL_OFFSET(ehp) +
	    offsetof(struct ext4_extent_tail, et_checksum);

	inum = htole32(ip->i_number);
	gen = htole32(ip->i_gen);
	crc = calculate_crc32c(fs->e2fs_csum_seed, (uint8_t *)&inum, sizeof(inum));
	crc = calculate_crc32c(crc, (uint8_t *)&gen, sizeof(gen));
	crc = calculate_crc32c(crc, (uint8_t *)ehp, size);

	return (crc);
}

int
ext2_extent_blk_csum_verify(struct inode *ip, void *data)
{
	struct m_ext2fs *fs;
	struct ext4_extent_header *ehp;
	struct ext4_extent_tail *etp;
	uint32_t provided, calculated;

	fs = ip->i_e2fs;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return (0);

	ehp = (struct ext4_extent_header *)data;
	etp = (struct ext4_extent_tail *)(((char *)ehp) +
	    EXT4_EXTENT_TAIL_OFFSET(ehp));

	provided = le32toh(etp->et_checksum);
	calculated = ext2_extent_blk_csum(ip, ehp);

	if (provided != calculated) {
		SDT_PROBE2(ext2fs, , trace, csum, 1, "bad extent csum detected");
		return (EIO);
	}

	return (0);
}

void
ext2_extent_blk_csum_set(struct inode *ip, void *data)
{
	struct m_ext2fs *fs;
	struct ext4_extent_header *ehp;
	struct ext4_extent_tail *etp;

	fs = ip->i_e2fs;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return;

	ehp = (struct ext4_extent_header *)data;
	etp = (struct ext4_extent_tail *)(((char *)data) +
	    EXT4_EXTENT_TAIL_OFFSET(ehp));

	etp->et_checksum = htole32(ext2_extent_blk_csum(ip,
	    (struct ext4_extent_header *)data));
}
#endif /* _KERNEL */

/*
 * The block and inode bitmap checksums live in their group descriptor.
 */
uint32_t
ext2_bitmap_csum_value(uint32_t seed, const void *data, uint32_t size)
{

	return (calculate_crc32c(seed, data, size));
}

void
ext2_gd_bbitmap_csum_update(uint32_t seed, const void *data, uint32_t size,
    struct ext2_gd *gd, uint16_t desc_size)
{
	uint32_t csum;

	csum = ext2_bitmap_csum_value(seed, data, size);
	gd->ext4bgd_b_bmap_csum = htole16(csum & 0xFFFF);
	if (desc_size >= EXT2_BG_BLOCK_BITMAP_CSUM_HI_LOCATION)
		gd->ext4bgd_b_bmap_csum_hi = htole16(csum >> 16);
}

void
ext2_gd_ibitmap_csum_update(uint32_t seed, const void *data, uint32_t size,
    struct ext2_gd *gd, uint16_t desc_size)
{
	uint32_t csum;

	csum = ext2_bitmap_csum_value(seed, data, size);
	gd->ext4bgd_i_bmap_csum = htole16(csum & 0xFFFF);
	if (desc_size >= EXT2_BG_INODE_BITMAP_CSUM_HI_END)
		gd->ext4bgd_i_bmap_csum_hi = htole16(csum >> 16);
}

#ifdef _KERNEL
int
ext2_gd_i_bitmap_csum_verify(struct m_ext2fs *fs, int cg, struct buf *bp)
{
	uint32_t hi, provided, calculated;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return (0);

	provided = le16toh(fs->e2fs_gd[cg].ext4bgd_i_bmap_csum);
	calculated = calculate_crc32c(fs->e2fs_csum_seed, bp->b_data,
	    fs->e2fs_ipg / 8);
	if (le16toh(fs->e2fs->e3fs_desc_size) >=
	    EXT2_BG_INODE_BITMAP_CSUM_HI_END) {
		hi = le16toh(fs->e2fs_gd[cg].ext4bgd_i_bmap_csum_hi);
		provided |= (hi << 16);
	} else
		calculated &= 0xFFFF;

	if (provided != calculated) {
		SDT_PROBE2(ext2fs, , trace, csum, 1, "bad inode bitmap csum detected");
		return (EIO);
	}

	return (0);
}

void
ext2_gd_i_bitmap_csum_set(struct m_ext2fs *fs, int cg, struct buf *bp)
{

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return;

	ext2_gd_ibitmap_csum_update(fs->e2fs_csum_seed, bp->b_data,
	    fs->e2fs_ipg / 8, &fs->e2fs_gd[cg],
	    le16toh(fs->e2fs->e3fs_desc_size));
}

int
ext2_gd_b_bitmap_csum_verify(struct m_ext2fs *fs, int cg, struct buf *bp)
{
	uint32_t hi, provided, calculated, size;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return (0);

	size = fs->e2fs_fpg / 8;
	provided = le16toh(fs->e2fs_gd[cg].ext4bgd_b_bmap_csum);
	calculated = calculate_crc32c(fs->e2fs_csum_seed, bp->b_data, size);
	if (le16toh(fs->e2fs->e3fs_desc_size) >=
	    EXT2_BG_BLOCK_BITMAP_CSUM_HI_LOCATION) {
		hi = le16toh(fs->e2fs_gd[cg].ext4bgd_b_bmap_csum_hi);
		provided |= (hi << 16);
	} else
		calculated &= 0xFFFF;

	if (provided != calculated) {
		SDT_PROBE2(ext2fs, , trace, csum, 1, "bad block bitmap csum detected");
		return (EIO);
	}

	return (0);
}

void
ext2_gd_b_bitmap_csum_set(struct m_ext2fs *fs, int cg, struct buf *bp)
{

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return;

	ext2_gd_bbitmap_csum_update(fs->e2fs_csum_seed, bp->b_data,
	    fs->e2fs_fpg / 8, &fs->e2fs_gd[cg],
	    le16toh(fs->e2fs->e3fs_desc_size));
}
#endif /* _KERNEL */

/*
 * The crc32c group descriptor checksum: the seed, the group number,
 * and the descriptor bytes with the checksum field skipped.
 */
uint16_t
ext2_gd_csum_value(uint32_t seed, uint32_t block_group,
    const struct ext2_gd *gd, uint16_t desc_size)
{
	size_t offset;
	uint32_t csum32;
	uint16_t crc, dummy_csum;

	offset = offsetof(struct ext2_gd, ext4bgd_csum);

	block_group = htole32(block_group);

	csum32 = calculate_crc32c(seed, (uint8_t *)&block_group,
	    sizeof(block_group));
	csum32 = calculate_crc32c(csum32, (const uint8_t *)gd, offset);
	dummy_csum = 0;
	csum32 = calculate_crc32c(csum32, (uint8_t *)&dummy_csum,
	    sizeof(dummy_csum));
	offset += sizeof(dummy_csum);
	if (offset < desc_size)
		csum32 = calculate_crc32c(csum32, (const uint8_t *)gd + offset,
		    desc_size - offset);

	crc = csum32 & 0xFFFF;
	return (htole16(crc));
}

#ifdef _KERNEL
static uint16_t
ext2_gd_csum(struct m_ext2fs *fs, uint32_t block_group, struct ext2_gd *gd)
{
	size_t offset;
	uint16_t crc;

	if (EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return (ext2_gd_csum_value(fs->e2fs_csum_seed, block_group,
		    gd, le16toh(fs->e2fs->e3fs_desc_size)));
	else if (EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_GDT_CSUM)) {
		offset = offsetof(struct ext2_gd, ext4bgd_csum);

		block_group = htole32(block_group);
		crc = crc16(~0, fs->e2fs->e2fs_uuid,
		    sizeof(fs->e2fs->e2fs_uuid));
		crc = crc16(crc, (uint8_t *)&block_group,
		    sizeof(block_group));
		crc = crc16(crc, (uint8_t *)gd, offset);
		offset += sizeof(gd->ext4bgd_csum); /* skip checksum */
		if (EXT2_HAS_INCOMPAT_FEATURE(fs, EXT2F_INCOMPAT_64BIT) &&
		    offset < le16toh(fs->e2fs->e3fs_desc_size))
			crc = crc16(crc, (uint8_t *)gd + offset,
			    le16toh(fs->e2fs->e3fs_desc_size) - offset);
		return (htole16(crc));
	}

	return (0);
}

int
ext2_gd_csum_verify(struct m_ext2fs *fs, struct cdev *dev)
{
	unsigned int i;
	int error = 0;

	for (i = 0; i < fs->e2fs_gcount; i++) {
		if (fs->e2fs_gd[i].ext4bgd_csum !=
		    ext2_gd_csum(fs, i, &fs->e2fs_gd[i])) {
			printf(
"WARNING: mount of %s denied due bad gd=%d csum=0x%x, expected=0x%x - run fsck\n",
			    devtoname(dev), i, fs->e2fs_gd[i].ext4bgd_csum,
			    ext2_gd_csum(fs, i, &fs->e2fs_gd[i]));
			error = EIO;
			break;
		}
	}

	return (error);
}

void
ext2_gd_csum_set(struct m_ext2fs *fs)
{
	unsigned int i;

	for (i = 0; i < fs->e2fs_gcount; i++)
		fs->e2fs_gd[i].ext4bgd_csum = ext2_gd_csum(fs, i, &fs->e2fs_gd[i]);
}
#endif /* _KERNEL */

/*
 * Inode checksums: the checksum fields at offsets 124 (and 130, for
 * larger checksummed inodes) are skipped; gen is the inode generation.
 */
uint32_t
ext2_ei_csum_value(uint32_t seed, uint32_t ino, uint32_t gen,
    const struct ext2fs_dinode *ei, uint32_t isize)
{
	uint32_t inode_csum_seed, crc;
	uint16_t dummy_csum = 0;
	unsigned int offset, csum_size;

	offset = offsetof(struct ext2fs_dinode, e2di_chksum_lo);
	csum_size = sizeof(dummy_csum);
	ino = htole32(ino);
	crc = calculate_crc32c(seed, (uint8_t *)&ino, sizeof(ino));
	gen = htole32(gen);
	inode_csum_seed = calculate_crc32c(crc, (uint8_t *)&gen, sizeof(gen));

	crc = calculate_crc32c(inode_csum_seed, (const uint8_t *)ei, offset);
	crc = calculate_crc32c(crc, (uint8_t *)&dummy_csum, csum_size);
	offset += csum_size;
	crc = calculate_crc32c(crc, (const uint8_t *)ei + offset,
	    E2FS_REV0_INODE_SIZE - offset);

	if (isize > E2FS_REV0_INODE_SIZE) {
		offset = offsetof(struct ext2fs_dinode, e2di_chksum_hi);
		crc = calculate_crc32c(crc, (const uint8_t *)ei +
		    E2FS_REV0_INODE_SIZE, offset - E2FS_REV0_INODE_SIZE);

		if (le16toh(ei->e2di_extra_isize) >=
		    EXT2_INODE_CSUM_HI_EXTRA_END) {
			crc = calculate_crc32c(crc, (uint8_t *)&dummy_csum,
			    csum_size);
			offset += csum_size;
		}

		crc = calculate_crc32c(crc, (const uint8_t *)ei + offset,
		    isize - offset);
	}

	return (crc);
}

#ifdef _KERNEL
int
ext2_ei_csum_verify(struct inode *ip, struct ext2fs_dinode *ei)
{
	struct m_ext2fs *fs;
	const static struct ext2fs_dinode ei_zero;
	uint32_t hi, provided, calculated;

	fs = ip->i_e2fs;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return (0);

	provided = le16toh(ei->e2di_chksum_lo);
	calculated = ext2_ei_csum_value(fs->e2fs_csum_seed, ip->i_number,
	    ip->i_gen, ei, EXT2_INODE_SIZE(fs));

	if ((EXT2_INODE_SIZE(fs) > E2FS_REV0_INODE_SIZE &&
	    le16toh(ei->e2di_extra_isize) >= EXT2_INODE_CSUM_HI_EXTRA_END)) {
		hi = le16toh(ei->e2di_chksum_hi);
		provided |= hi << 16;
	} else
		calculated &= 0xFFFF;

	if (provided != calculated) {
		/*
		 * If it is first time used dinode,
		 * it is expected that it will be zeroed
		 * and we will not return checksum error in this case.
		 */
		if (!memcmp(ei, &ei_zero, sizeof(struct ext2fs_dinode)))
			return (0);

		SDT_PROBE2(ext2fs, , trace, csum, 1, "bad inode csum");

		return (EIO);
	}

	return (0);
}

void
ext2_ei_csum_set(struct inode *ip, struct ext2fs_dinode *ei)
{
	struct m_ext2fs *fs;
	uint32_t crc;

	fs = ip->i_e2fs;

	if (!EXT2_HAS_RO_COMPAT_FEATURE(fs, EXT2F_ROCOMPAT_METADATA_CKSUM))
		return;

	crc = ext2_ei_csum_value(fs->e2fs_csum_seed, ip->i_number, ip->i_gen,
	    ei, EXT2_INODE_SIZE(fs));

	ei->e2di_chksum_lo = htole16(crc & 0xFFFF);
	if ((EXT2_INODE_SIZE(fs) > E2FS_REV0_INODE_SIZE &&
	    le16toh(ei->e2di_extra_isize) >= EXT2_INODE_CSUM_HI_EXTRA_END))
		ei->e2di_chksum_hi = htole16(crc >> 16);
}
#endif /* _KERNEL */

#ifndef _KERNEL
/*
 * The legacy crc16 group descriptor checksum of the GDT_CSUM feature
 * (uninit_bg), without metadata_csum.
 */
uint16_t
ext2_gd_csum_legacy(const uint8_t uuid[16], uint32_t block_group,
    const struct ext2_gd *gd, uint16_t desc_size)
{
	size_t offset;
	uint16_t crc;

	offset = offsetof(struct ext2_gd, ext4bgd_csum);

	block_group = htole32(block_group);
	crc = crc16(~0, uuid, 16);
	crc = crc16(crc, (uint8_t *)&block_group, sizeof(block_group));
	crc = crc16(crc, (const uint8_t *)gd, offset);
	offset += sizeof(gd->ext4bgd_csum);	/* skip checksum */
	if (offset < desc_size)
		crc = crc16(crc, (const uint8_t *)gd + offset,
		    desc_size - offset);
	return (htole16(crc));
}

/*
 * Userland (libext2fs) directory block checksums: the tail is located
 * in the block itself, so both verification and updates work on the
 * block buffer.
 */
int
ext2_dirent_has_tail(const void *block, uint32_t bsize)
{

	return (dirent_get_tail((struct ext2fs_direct_2 *)
	    __DECONST(void *, block), bsize) != NULL);
}

int
ext2_dirent_csum_check(uint32_t seed, uint32_t ino, uint32_t gen,
    void *block, uint32_t bsize)
{
	struct ext2fs_direct_tail *tp;

	tp = dirent_get_tail(block, bsize);
	if (tp == NULL)
		return (0);
	if (dirent_csum(seed, ino, gen, block, (char *)tp - (char *)block) !=
	    le32toh(tp->e2dt_checksum))
		return (EIO);
	return (0);
}

void
ext2_dirent_csum_update(uint32_t seed, uint32_t ino, uint32_t gen,
    void *block, uint32_t bsize)
{
	struct ext2fs_direct_tail *tp;

	tp = dirent_get_tail(block, bsize);
	if (tp == NULL)
		return;
	tp->e2dt_checksum =
	    htole32(dirent_csum(seed, ino, gen, block,
	    (char *)tp - (char *)block));
}

int
ext2_ei_csum_check(uint32_t seed, uint32_t ino, uint32_t gen,
    const struct ext2fs_dinode *ei, uint32_t isize)
{
	uint32_t hi, provided, calculated;

	provided = le16toh(ei->e2di_chksum_lo);
	calculated = ext2_ei_csum_value(seed, ino, gen, ei, isize);

	if (isize > E2FS_REV0_INODE_SIZE &&
	    le16toh(ei->e2di_extra_isize) >= EXT2_INODE_CSUM_HI_EXTRA_END) {
		hi = le16toh(ei->e2di_chksum_hi);
		provided |= hi << 16;
	} else
		calculated &= 0xFFFF;

	if (provided != calculated) {
		/* A first-time inode is zeroed and carries no checksum. */
		const static struct ext2fs_dinode ei_zero;

		if (memcmp(ei, &ei_zero, sizeof(ei_zero)) == 0)
			return (0);
		return (EIO);
	}

	return (0);
}

void
ext2_ei_csum_update(uint32_t seed, uint32_t ino, uint32_t gen,
    struct ext2fs_dinode *ei, uint32_t isize)
{
	uint32_t crc;

	crc = ext2_ei_csum_value(seed, ino, gen, ei, isize);

	ei->e2di_chksum_lo = htole16(crc & 0xFFFF);
	if (isize > E2FS_REV0_INODE_SIZE &&
	    le16toh(ei->e2di_extra_isize) >= EXT2_INODE_CSUM_HI_EXTRA_END)
		ei->e2di_chksum_hi = htole16(crc >> 16);
}
#endif /* !_KERNEL */
