/*
* Copyright (c) 2026 The FreeBSD Foundation
*
* SPDX-License-Identifier: BSD-2-Clause
*
* This software was developed by Benjamin Stürz <benni@stuerz.xyz>
* under sponsorship from the FreeBSD Foundation.
*/

#include <sys/types.h>
#include <sys/endian.h>
#include <sys/gsb_crc32.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_extents.h>

#include "libext2fs.h"

/*
 * Replay of the ext3/ext4 journal: the committed transactions of
 * the JBD2 log are written back to the filesystem and the log
 * emptied.  The on-disk format is big-endian throughout.
 */

#define JNL_MAGIC		0xc03b3998u

#define JNL_DESCRIPTOR		1
#define JNL_COMMIT		2
#define JNL_SUPERBLOCK_V1	3
#define JNL_SUPERBLOCK_V2	4
#define JNL_REVOKE		5

#define JNL_INCOMPAT_64BIT	0x00000002u
#define JNL_INCOMPAT_CSUM_V2	0x00000008u
#define JNL_INCOMPAT_CSUM_V3	0x00000010u
#define JNL_INCOMPAT_FAST_COMMIT	0x00000020u
#define JNL_FAST_COMMIT_BLKS	256u

#define JNL_TAG_ESCAPE		0x0001
#define JNL_TAG_SAME_UUID	0x0002
#define JNL_TAG_DELETED		0x0004
#define JNL_TAG_LAST		0x0008

struct jnl_header {
	uint32_t	h_magic;
	uint32_t	h_blocktype;
	uint32_t	h_sequence;
};

struct jnl_superblock {
	struct jnl_header s_header;	/* 0x00 */
	uint32_t	s_blocksize;	/* 0x0c */
	uint32_t	s_maxlen;	/* 0x10 */
	uint32_t	s_first;	/* 0x14 */
	uint32_t	s_sequence;	/* 0x18 */
	uint32_t	s_start;		/* 0x1c */
	uint32_t	s_errno;	/* 0x20 */
	uint32_t	s_feature_compat;
	uint32_t	s_feature_incompat;
	uint32_t	s_feature_rocompat;
	uint8_t		s_uuid[16];
	uint32_t	s_nr_users;
	uint32_t	s_dynsuper;
	uint32_t	s_max_transaction;
	uint32_t	s_max_trans_data;
	uint8_t		s_checksum_type;
	uint8_t		s_padding2[3];
	uint32_t	s_num_fc_blks;
	uint32_t	s_head;
	uint32_t	s_padding[40];
	uint32_t	s_checksum;	/* 0xfc */
	uint8_t		s_users[16 * 48];
};

struct jnl_tag {
	uint32_t	t_blocknr;
	uint16_t	t_checksum;
	uint16_t	t_flags;
	uint32_t	t_blocknr_high;	/* only present with 64BIT */
};

struct jnl_tag3 {
	uint32_t	t_blocknr;
	uint32_t	t_flags;
	uint32_t	t_blocknr_high;
	uint32_t	t_checksum;
};

struct jnl_revoke_header {
	struct jnl_header r_header;
	uint32_t	r_count;
};

struct jnl_write {
	uint64_t	w_blkno;
	uint32_t	w_seq;
	uint32_t	w_jblock;
	int		w_escape;
};

struct jnl_revocation {
	uint64_t	r_blkno;
	uint32_t	r_seq;
};

struct jnl {
	struct ext2fsd	*j_disk;
	uint64_t	*j_blks;
	uint32_t	j_nblks;
	char		*j_map;
	uint64_t	j_sbblk;
	uint32_t	j_bsize;
	uint32_t	j_first;
	uint32_t	j_last;
	uint32_t	j_tagbytes;
	int		j_csum23;
	int		j_64bit;
	struct jnl_superblock j_sb;
	struct jnl_write *j_writes;
	uint32_t	j_nwrites;
	struct jnl_revocation *j_revocations;
	uint32_t	j_nrevocations;
	uint32_t	j_nextseq;
};

static int
jnl_tid_geq(uint32_t a, uint32_t b)
{

	return ((int32_t)(a - b) >= 0);
}

static uint32_t
jnl_next(const struct jnl *jp, uint32_t jblock)
{

	jblock++;
	if (jblock >= jp->j_last)
		jblock = jp->j_first;
	return (jblock);
}

static uint32_t
jnl_be32(const void *p)
{

	return (be32dec(p));
}

static uint64_t
jnl_tag_block(const struct jnl *jp, const char *tag)
{
	uint64_t blkno;

	blkno = jnl_be32(tag);
	if (jp->j_64bit)
		blkno |= (uint64_t)jnl_be32(tag + (jp->j_csum23 ?
		    offsetof(struct jnl_tag3, t_blocknr_high) :
		    offsetof(struct jnl_tag, t_blocknr_high))) << 32;
	return (blkno);
}

static uint32_t
jnl_tag_flags(const struct jnl *jp, const char *tag)
{

	if (jp->j_csum23)
		return (be32dec(tag + offsetof(struct jnl_tag3, t_flags)));
	return (be16dec(tag + offsetof(struct jnl_tag, t_flags)));
}

static uint32_t
jnl_tag_area(const struct jnl *jp)
{

	return (jp->j_bsize - sizeof(struct jnl_header) -
	    (jp->j_csum23 ? sizeof(uint32_t) : 0));
}

/* The superblock checksum is seeded with the journal uuid. */
/*
 * The superblock checksum covers the whole block with the checksum
 * field zeroed, the way e2fsck computes it.
 */
static uint32_t
jnl_sb_csum(const struct jnl *jp)
{
	struct jnl_superblock sb;
	uint32_t crc;

	sb = jp->j_sb;
	sb.s_checksum = 0;
	crc = calculate_crc32c(~0, (const void *)&sb, sizeof(sb));
	return (crc);
}

static void
jnl_sb_csum_set(struct jnl *jp)
{

	jp->j_sb.s_checksum = htobe32(jnl_sb_csum(jp));
}

static int
jnl_read(const struct jnl *jp, uint32_t jblock, char *buf)
{

	if (jblock < jp->j_first || jblock >= jp->j_last ||
	    jblock >= jp->j_nblks)
		return (-1);
	memcpy(buf, jp->j_map + (size_t)jblock * jp->j_bsize, jp->j_bsize);
	return (0);
}

static int
jnl_map_extents(const struct jnl *jp, const char *node, uint32_t max,
    uint32_t *np)
{
	const struct ext4_extent_header *eh;
	const char *p, *end;
	uint32_t bsize = jp->j_bsize;
	uint16_t ecount, depth;
	char *buf;
	int error = 0;

	eh = (const struct ext4_extent_header *)node;
	if (le16toh(eh->eh_magic) != EXT4_EXT_MAGIC ||
	    le16toh(eh->eh_ecount) > le16toh(eh->eh_max))
		return (EINVAL);
	ecount = le16toh(eh->eh_ecount);
	depth = le16toh(eh->eh_depth);
	end = node + bsize - sizeof(struct ext4_extent_tail);

	if (depth == 0) {
		const struct ext4_extent *ex;

		p = node + sizeof(*eh);
		for (; ecount > 0; ecount--, p += sizeof(*ex)) {
			uint64_t start;
			uint32_t len, k;

			ex = (const struct ext4_extent *)p;
			if (p + sizeof(*ex) > end)
				return (EINVAL);
			start = ((uint64_t)le16toh(ex->e_start_hi) << 32) |
			    le32toh(ex->e_start_lo);
			len = le16toh(ex->e_len) & (EXT_INIT_MAX_LEN - 1);
			for (k = 0; k < len && *np < max; k++)
				jp->j_blks[(*np)++] = start + k;
		}
		return (0);
	}

	if ((buf = malloc(bsize)) == NULL)
		return (ENOMEM);
	p = node + sizeof(*eh);
	for (; ecount > 0; ecount--, p += sizeof(struct ext4_extent_index)) {
		const struct ext4_extent_index *ix;
		uint64_t leaf;

		ix = (const struct ext4_extent_index *)p;
		if (p + sizeof(*ix) > end)
			break;
		leaf = ((uint64_t)le16toh(ix->ei_leaf_hi) << 32) |
		    le32toh(ix->ei_leaf_lo);
		if (leaf == 0)
			continue;
		if (ext2fs_bread(jp->j_disk, leaf, buf, bsize) ==
		    (ssize_t)bsize)
			error = jnl_map_extents(jp, buf, max, np);
		if (error != 0)
			break;
	}
	free(buf);
	return (error);
}

/* Map the first max logical blocks of the journal inode. */
static int
jnl_bmap(struct jnl *jp, const struct ext2fs_dinode *di, uint32_t max)
{
	uint32_t bsize, perblk, i, n;

	bsize = 1024u << le32toh(jp->j_disk->d_fs.e2fs_log_bsize);
	perblk = bsize / 4;
	if ((jp->j_blks = calloc(max, sizeof(*jp->j_blks))) == NULL)
		return (ENOMEM);
	n = 0;

	if (le32toh(di->e2di_flags) & EXT4_EXTENTS) {
		char *buf;
		int error;

		if ((buf = malloc(bsize)) == NULL)
			return (ENOMEM);
		memcpy(buf, di->e2di_blocks, sizeof(di->e2di_blocks));
		error = jnl_map_extents(jp, buf, max, &n);
		free(buf);
		if (error != 0)
			return (error);
		goto done;
	}

	for (i = 0; i < EXT2_NDIR_BLOCKS && n < max; i++)
		jp->j_blks[n++] = le32toh(di->e2di_blocks[i]);
	{
		char *ind, *dind;
		uint64_t ib;

		if ((ind = malloc(bsize)) == NULL)
			return (ENOMEM);
		if ((dind = malloc(bsize)) == NULL) {
			free(ind);
			return (ENOMEM);
		}
		/* The single indirect block. */
		ib = le32toh(di->e2di_blocks[EXT2_NDIR_BLOCKS]);
		if (ib != 0 && ext2fs_bread(jp->j_disk, ib, ind, bsize) ==
		    (ssize_t)bsize)
			for (i = 0; i < perblk && n < max; i++)
				jp->j_blks[n++] = le32dec(ind + i * 4);
		/* The double indirect block. */
		ib = le32toh(di->e2di_blocks[EXT2_NDIR_BLOCKS + 1]);
		if (ib != 0 && ext2fs_bread(jp->j_disk, ib, dind, bsize) ==
		    (ssize_t)bsize) {
			for (i = 0; i < perblk && n < max; i++) {
				uint64_t sb = le32dec(dind + i * 4);
				uint32_t k;

				if (sb == 0)
					continue;
				if (ext2fs_bread(jp->j_disk, sb, ind,
				    bsize) != (ssize_t)bsize)
					continue;
				for (k = 0; k < perblk && n < max; k++)
					jp->j_blks[n++] = le32dec(ind + k * 4);
			}
		}
		free(dind);
		free(ind);
	}
done:
	jp->j_nblks = n;
	if (n == 0)
		return (EINVAL);
	return (0);
}

static int
jnl_read_log(struct jnl *jp)
{
	uint32_t i;

	if ((jp->j_map = malloc((size_t)jp->j_nblks * jp->j_bsize)) == NULL)
		return (ENOMEM);
	for (i = 0; i < jp->j_nblks; i++) {
		if (jp->j_blks[i] == 0) {
			memset(jp->j_map + (size_t)i * jp->j_bsize, 0,
			    jp->j_bsize);
			continue;
		}
		if (ext2fs_bread(jp->j_disk, jp->j_blks[i],
		    jp->j_map + (size_t)i * jp->j_bsize, jp->j_bsize) !=
		    (ssize_t)jp->j_bsize)
			return (EIO);
	}
	return (0);
}

/* Read the journal superblock of ino and set up geometry and tags. */
static int
jnl_load(struct jnl *jp, uint32_t ino)
{
	struct ext2fs_dinode di;
	struct jnl_header *hdr;
	char *buf;
	uint32_t jbsize, maxlen, first, incompat;
	int error;

	jp->j_bsize = 1024u << le32toh(jp->j_disk->d_fs.e2fs_log_bsize);
	if (ext2fs_iget(jp->j_disk, ino, &di) != 0)
		return (EINVAL);
	if ((error = jnl_bmap(jp, &di, 1)) != 0)
		return (error);
	jp->j_sbblk = jp->j_blks[0];
	if ((buf = malloc(jp->j_bsize)) == NULL)
		return (ENOMEM);
	if (ext2fs_bread(jp->j_disk, jp->j_sbblk, buf, jp->j_bsize) !=
	    (ssize_t)jp->j_bsize) {
		free(buf);
		return (EIO);
	}
	memcpy(&jp->j_sb, buf, sizeof(jp->j_sb));
	free(buf);

	hdr = &jp->j_sb.s_header;
	if (jnl_be32(&hdr->h_magic) != JNL_MAGIC ||
	    (jnl_be32(&hdr->h_blocktype) != JNL_SUPERBLOCK_V1 &&
	    jnl_be32(&hdr->h_blocktype) != JNL_SUPERBLOCK_V2))
		return (EINVAL);

	jbsize = be32toh(jp->j_sb.s_blocksize);
	maxlen = be32toh(jp->j_sb.s_maxlen);
	first = be32toh(jp->j_sb.s_first);
	if (jbsize != jp->j_bsize || maxlen < 2 || first >= maxlen)
		return (EINVAL);

	jp->j_first = first;
	jp->j_last = maxlen;
	incompat = be32toh(jp->j_sb.s_feature_incompat);
	jp->j_csum23 = (incompat & (JNL_INCOMPAT_CSUM_V2 |
	    JNL_INCOMPAT_CSUM_V3)) != 0;
	jp->j_64bit = (incompat & JNL_INCOMPAT_64BIT) != 0;
	jp->j_tagbytes = jp->j_csum23 ? sizeof(struct jnl_tag3) :
	    sizeof(struct jnl_tag) - (jp->j_64bit ? 0 : 4);
	if ((incompat & JNL_INCOMPAT_FAST_COMMIT) != 0) {
		uint32_t fc = be32toh(jp->j_sb.s_num_fc_blks);

		if (fc >= maxlen - first)
			return (EINVAL);
		jp->j_last = maxlen - (fc != 0 ? fc : JNL_FAST_COMMIT_BLKS);
	}
	jp->j_nextseq = be32toh(jp->j_sb.s_sequence);
	return (0);
}

/*
 * Walk one descriptor block.  The data blocks it names follow it in
 * the log; *nextp advances past them.
 */
static int
jnl_scan_descriptor(struct jnl *jp, const char *desc, uint32_t seq,
    uint32_t *nextp, struct jnl_write **ws, uint32_t *nws)
{
	const char *tag, *end;
	uint32_t next = *nextp;

	end = desc + jnl_tag_area(jp);
	for (tag = desc + sizeof(struct jnl_header); tag < end;) {
		uint64_t blkno = jnl_tag_block(jp, tag);
		uint32_t flags = jnl_tag_flags(jp, tag);
		struct jnl_write *w;

		if ((flags & JNL_TAG_DELETED) == 0) {
			w = reallocarray(*ws, *nws + 1, sizeof(*w));
			if (w == NULL)
				return (ENOMEM);
			*ws = w;
			w = &(*ws)[(*nws)++];
			w->w_blkno = blkno;
			w->w_seq = seq;
			w->w_jblock = next;
			w->w_escape = (flags & JNL_TAG_ESCAPE) != 0;
			next = jnl_next(jp, next);
		}
		if ((flags & JNL_TAG_SAME_UUID) == 0)
			tag += 16;
		tag += jp->j_tagbytes;
		if (flags & JNL_TAG_LAST)
			break;
	}
	*nextp = next;
	return (0);
}

static int
jnl_scan_revoke(struct jnl *jp, const char *rev, uint32_t seq,
    struct jnl_revocation **rs, uint32_t *nrs)
{
	uint32_t count, entsize, n, i;
	struct jnl_revocation *r;

	count = jnl_be32(rev + offsetof(struct jnl_revoke_header, r_count));
	if (count > jp->j_bsize)
		count = jp->j_bsize;
	entsize = jp->j_64bit ? 8 : 4;
	if (count < sizeof(struct jnl_revoke_header))
		return (0);
	n = (count - sizeof(struct jnl_revoke_header)) / entsize;
	r = reallocarray(*rs, *nrs + n, sizeof(*r));
	if (r == NULL)
		return (ENOMEM);
	*rs = r;
	for (i = 0; i < n; i++) {
		const char *ent = rev + sizeof(struct jnl_revoke_header) +
		    (size_t)i * entsize;

		r = &(*rs)[(*nrs)++];
		r->r_blkno = jp->j_64bit ? be64dec(ent) : jnl_be32(ent);
		r->r_seq = seq;
	}
	return (0);
}

/*
 * Walk the committed transactions from the log start.  A
 * transaction may open with a descriptor or a revoke block and ends
 * at its commit block; the first incomplete one stops the walk.
 */
static int
jnl_scan(struct jnl *jp)
{
	char *buf;
	uint32_t next, seq;
	int error = 0;

	if ((buf = malloc(jp->j_bsize)) == NULL)
		return (ENOMEM);
	next = be32toh(jp->j_sb.s_start);
	seq = be32toh(jp->j_sb.s_sequence);

	while (next >= jp->j_first && next < jp->j_last) {
		struct jnl_header hdr;
		struct jnl_write *ws = NULL;
		uint32_t nws = 0;
		struct jnl_revocation *rs = NULL;
		uint32_t nrs = 0;
		uint32_t cur = next;
		uint32_t after;
		uint32_t commit_blk = 0;
		int stop = 0;

		while (cur >= jp->j_first && cur < jp->j_last) {
			if (jnl_read(jp, cur, buf) != 0) {
				error = EIO;
				break;
			}
			memcpy(&hdr, buf, sizeof(hdr));
			if (jnl_be32(&hdr.h_magic) != JNL_MAGIC ||
			    jnl_be32(&hdr.h_sequence) != seq) {
				stop = 1;
				break;
			}
			switch (jnl_be32(&hdr.h_blocktype)) {
			case JNL_DESCRIPTOR:
				after = jnl_next(jp, cur);
				error = jnl_scan_descriptor(jp, buf, seq,
				    &after, &ws, &nws);
				cur = after;
				break;
			case JNL_REVOKE:
				error = jnl_scan_revoke(jp, buf, seq,
				    &rs, &nrs);
				cur = jnl_next(jp, cur);
				break;
			case JNL_COMMIT:
				commit_blk = cur;
				cur = jnl_next(jp, cur);
				break;
			default:
				stop = 1;
				break;
			}
			if (error != 0 || stop != 0 || commit_blk != 0)
				break;
		}

		if (error == 0 && commit_blk != 0) {
			struct jnl_write *aw;
			struct jnl_revocation *ar;

			aw = reallocarray(jp->j_writes, jp->j_nwrites + nws,
			    sizeof(*aw));
			if (aw == NULL)
				error = ENOMEM;
			else {
				jp->j_writes = aw;
				memcpy(&jp->j_writes[jp->j_nwrites], ws,
				    (size_t)nws * sizeof(*ws));
				jp->j_nwrites += nws;
			}
			if (error == 0) {
				ar = reallocarray(jp->j_revocations,
				    jp->j_nrevocations + nrs, sizeof(*ar));
				if (ar == NULL)
					error = ENOMEM;
				else {
					jp->j_revocations = ar;
					memcpy(&jp->j_revocations[
					    jp->j_nrevocations], rs,
					    (size_t)nrs * sizeof(*rs));
					jp->j_nrevocations += nrs;
				}
			}
			jp->j_nextseq = seq + 1;
			next = cur;
			seq++;
		} else if (error == 0)
			next = 0;
		free(ws);
		free(rs);
		if (error != 0)
			break;
	}

	free(buf);
	return (error);
}

static int
jnl_revoked(const struct jnl *jp, uint64_t blkno, uint32_t seq)
{
	uint32_t i;

	/* A revoke from the same or a later transaction cancels it. */
	for (i = 0; i < jp->j_nrevocations; i++)
		if (jp->j_revocations[i].r_blkno == blkno &&
		    jnl_tid_geq(jp->j_revocations[i].r_seq, seq))
			return (1);
	return (0);
}

static int
jnl_apply(struct jnl *jp, struct ext2fs_journal_info *info)
{
	char *buf;
	uint32_t i;
	int error = 0;

	if ((buf = malloc(jp->j_bsize)) == NULL)
		return (ENOMEM);
	for (i = 0; i < jp->j_nwrites; i++) {
		const struct jnl_write *w = &jp->j_writes[i];

		if (w->w_jblock >= jp->j_nblks ||
		    jnl_revoked(jp, w->w_blkno, w->w_seq))
			continue;
		memcpy(buf, jp->j_map + (size_t)w->w_jblock * jp->j_bsize,
		    jp->j_bsize);
		if (w->w_escape) {
			/*
			 * The data block was escaped: the log kept it
			 * as it was, and the leading magic goes back.
			 */
			be32enc(buf, JNL_MAGIC);
		}
		if (ext2fs_bwrite(jp->j_disk, w->w_blkno, buf,
		    jp->j_bsize) != (ssize_t)jp->j_bsize) {
			error = EIO;
			break;
		}
		info->ji_blocks++;
	}
	free(buf);
	return (error);
}

static int
jnl_finish(struct jnl *jp)
{

	jp->j_sb.s_start = htobe32(0);
	jp->j_sb.s_sequence = htobe32(jp->j_nextseq);
	jp->j_sb.s_errno = 0;
	jnl_sb_csum_set(jp);
	if (ext2fs_bwrite(jp->j_disk, jp->j_sbblk, (const char *)&jp->j_sb,
	    jp->j_bsize) != (ssize_t)jp->j_bsize)
		return (EIO);
	return (0);
}

/*
 * Replay the journal and empty the log.  Returns 0 - with nothing
 * applied if the log is empty - or an errno if the journal cannot
 * be parsed; the caller clears the RECOVER flag.
 */
int
ext2fs_journal_replay(struct ext2fsd *disk, struct ext2fs_journal_info *info)
{
	struct jnl jp;
	uint32_t ino;
	int error;

	memset(&jp, 0, sizeof(jp));
	memset(info, 0, sizeof(*info));
	jp.j_disk = disk;

	if ((le32toh(disk->d_fs.e2fs_features_incompat) &
	    EXT2F_INCOMPAT_JOURNAL_DEV) != 0)
		return (ENXIO);		/* external journal */
	ino = le32toh(disk->d_fs.e3fs_journal_inum);
	if (ino == 0)
		return (EINVAL);

	if ((error = jnl_load(&jp, ino)) != 0) {
		free(jp.j_blks);
		return (error);
	}
	if (be32toh(jp.j_sb.s_start) == 0) {
		free(jp.j_blks);
		return (0);
	}

	/* Map the whole log now that the geometry is known. */
	free(jp.j_blks);
	jp.j_blks = NULL;
	jp.j_nblks = 0;
	{
		struct ext2fs_dinode di;
		uint32_t want = be32toh(jp.j_sb.s_maxlen);

		if (ext2fs_iget(disk, ino, &di) != 0)
			return (EINVAL);
		if ((error = jnl_bmap(&jp, &di, want)) != 0)
			return (error);
	}
	if ((error = jnl_read_log(&jp)) != 0)
		goto out;

	if ((error = jnl_scan(&jp)) != 0)
		goto out;
	if ((error = jnl_apply(&jp, info)) != 0)
		goto out;
	info->ji_trans = jp.j_nextseq - be32toh(jp.j_sb.s_sequence);
	info->ji_ino = ino;
	if ((error = jnl_finish(&jp)) != 0)
		goto out;
out:
	free(jp.j_blks);
	free(jp.j_map);
	free(jp.j_writes);
	free(jp.j_revocations);
	return (error);
}

/*
 * Empty a journal that cannot be parsed, so the filesystem can
 * still be checked; a missing superblock gets a minimal one.
 */
int
ext2fs_journal_reset(struct ext2fsd *disk)
{
	struct jnl jp;
	struct ext2fs_dinode di;
	uint32_t ino;
	int error;

	memset(&jp, 0, sizeof(jp));
	jp.j_disk = disk;

	if ((le32toh(disk->d_fs.e2fs_features_incompat) &
	    EXT2F_INCOMPAT_JOURNAL_DEV) != 0)
		return (ENXIO);
	ino = le32toh(disk->d_fs.e3fs_journal_inum);
	if (ino == 0)
		return (EINVAL);

	if (ext2fs_iget(disk, ino, &di) != 0)
		return (EINVAL);
	jp.j_bsize = 1024u << le32toh(disk->d_fs.e2fs_log_bsize);
	if ((error = jnl_bmap(&jp, &di, 1)) != 0) {
		free(jp.j_blks);
		return (error);
	}
	jp.j_sbblk = jp.j_blks[0];
	free(jp.j_blks);
	jp.j_blks = NULL;

	{
		char *buf = malloc(jp.j_bsize);

		if (buf == NULL)
			return (ENOMEM);
		if (ext2fs_bread(disk, jp.j_sbblk, buf, jp.j_bsize) !=
		    (ssize_t)jp.j_bsize) {
			free(buf);
			return (EIO);
		}
		memcpy(&jp.j_sb, buf, sizeof(jp.j_sb));
		free(buf);
	}
	if (jnl_be32(&jp.j_sb.s_header.h_magic) != JNL_MAGIC) {
		uint32_t maxlen = (uint32_t)(le32toh(di.e2di_size) /
		    jp.j_bsize);

		if (maxlen < 2)
			maxlen = 2;
		memset(&jp.j_sb, 0, sizeof(jp.j_sb));
		jp.j_sb.s_header.h_magic = htobe32(JNL_MAGIC);
		jp.j_sb.s_header.h_blocktype = htobe32(JNL_SUPERBLOCK_V2);
		jp.j_sb.s_blocksize = htobe32(jp.j_bsize);
		jp.j_sb.s_maxlen = htobe32(maxlen);
		jp.j_sb.s_first = htobe32(1);
		jp.j_sb.s_sequence = htobe32(1);
	}
	jp.j_sb.s_start = htobe32(0);
	jp.j_sb.s_errno = 0;
	jnl_sb_csum_set(&jp);
	if (ext2fs_bwrite(disk, jp.j_sbblk, (const char *)&jp.j_sb,
	    jp.j_bsize) != (ssize_t)jp.j_bsize)
		return (EIO);
	return (0);
}
