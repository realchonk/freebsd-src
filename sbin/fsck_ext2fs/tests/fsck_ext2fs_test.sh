#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 The FreeBSD Foundation
#
# This software was developed by Benjamin Stürz <benni@stuerz.xyz>
# under sponsorship from the FreeBSD Foundation.
#

# Test images are laid out with e2fsprogs (mke2fs, debugfs), corrupted
# in controlled ways, and repaired with fsck_ext2fs; e2fsck serves as
# the oracle for the repairs.  Everything works on plain image files,
# so no privileges are required.  e2fsprogs tools are located via PATH.

FSCK_EXT2FS="${FSCK_EXT2FS:-/sbin/fsck_ext2fs}"

# Messages that a healthy filesystem never produces.
BAD_OUTPUT='WRONG|BAD|UNREF|MISSING|SALVAGE|DUP|FREE SPACE'

atf_test_case clean_ext2
clean_ext2_head() { atf_set descr "ext2: clean image passes -n silently"; }
atf_test_case clean_ext3
clean_ext3_head() { atf_set descr "ext3: clean image passes -n silently"; }
atf_test_case clean_ext4
clean_ext4_head() { atf_set descr "ext4: clean image passes -n silently"; }
atf_test_case clean_ext4min
clean_ext4min_head() { atf_set descr "ext4 without extents/csum: clean -n"; }
atf_test_case clean_uninit
clean_uninit_head() { atf_set descr "uninit_bg: clean image passes -n"; }

clean_ext2_body() { clean_body -t ext2; }
clean_ext3_body() { clean_body -t ext3; }
clean_ext4_body() { clean_body -t ext4; }
clean_ext4min_body()
{
	clean_body -t ext4 -O \
	    '^extents,^64bit,^flex_bg,^metadata_csum,^dir_index,^has_journal'
}

clean_uninit_body()
{
	clean_body -O 'uninit_bg,^metadata_csum,^extents,^64bit,^flex_bg'
}

clean_body()
{
	img=clean.img
	make_image "$img" "$@"
	populate_image "$img"

	atf_check -s exit:0 -o save:fsck.log "$FSCK_EXT2FS" -n "$img"
	assert_no_problems fsck.log
}

atf_test_case nlink_ext2
nlink_ext2_head() { atf_set descr "ext2: wrong link count repaired"; }
atf_test_case nlink_ext3
nlink_ext3_head() { atf_set descr "ext3: wrong link count repaired"; }
atf_test_case nlink_ext4
nlink_ext4_head() { atf_set descr "ext4: wrong link count repaired"; }
atf_test_case nlink_ext4min
nlink_ext4min_head() { atf_set descr "ext4 minimal: link count repaired"; }
atf_test_case nlink_uninit
nlink_uninit_head() { atf_set descr "uninit_bg: link count repaired"; }

nlink_ext2_body() { nlink_body -t ext2; }
nlink_ext3_body() { nlink_body -t ext3; }
nlink_ext4_body() { nlink_body -t ext4; }
nlink_ext4min_body()
{
	nlink_body -t ext4 -O \
	    '^extents,^64bit,^flex_bg,^metadata_csum,^dir_index,^has_journal'
}

nlink_uninit_body()
{
	nlink_body -O 'uninit_bg,^metadata_csum,^extents,^64bit,^flex_bg'
}

nlink_body()
{
	img=nlink.img
	make_image "$img" "$@"
	populate_image "$img"

	# Bump the stored link count of h.txt above its directory entry.
	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'sif /h.txt links_count 6' "$img"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'LINK COUNT' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case orphan_ext4
orphan_ext4_head() { atf_set descr "ext4: orphaned dir reconnected"; }

orphan_ext4_body()
{
	img=orphan.img
	make_image "$img" -t ext4
	populate_image "$img"

	# Remove the directory entry, leaving the inode unreferenced.
	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'unlink sub' "$img"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'UNREF' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case inode_csum_ext4
inode_csum_ext4_head() { atf_set descr "ext4: wrong inode checksum repaired"; }

inode_csum_ext4_body()
{
	img=icsum.img
	make_image "$img" -t ext4
	populate_image "$img"

	corrupt_inode_csum "$img"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'INODE CHECKSUM WRONG' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case no_write
no_write_head() { atf_set descr "-n never writes to the image"; }

no_write_body()
{
	img=nowrite.img
	make_image "$img" -t ext2
	populate_image "$img"

	# A wrong free block count is reported but must not be fixed.
	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'ssv free_blocks_count 16000' "$img"
	before=$(md5 -q "$img")
	atf_check -s exit:4 -o ignore "$FSCK_EXT2FS" -n "$img"
	atf_check -o inline:"$before"$'\n' md5 -q "$img"
}

# Lay out a 32m image with the given mke2fs options.
make_image()
{
	atf_require_prog mke2fs
	img=$1
	shift
	atf_check -s exit:0 -o empty -e empty \
	    mke2fs -q -F "$@" -b 1024 "$img" 32768
}

# Overwrite the checksum word of /h.txt's inode with garbage.  The
# inode location comes from the superblock and the first group
# descriptor: 1024-byte blocks, inode table start at gd+8, and the
# checksum at inode offset 124 (e2di_chksum_lo).
corrupt_inode_csum()
{
	img=$1

	ino=$(debugfs -R 'stat /h.txt' "$img" 2>/dev/null |
	    awk '/^Inode:/ { print $2; exit }')
	atf_check test -n "$ino"
	isize=$(le16 "$img" $((1024 + 88)))
	itblk=$(le32 "$img" $((2048 + 8)))
	printf '\276\357' | dd of="$img" bs=1 \
	    seek=$((itblk * 1024 + (ino - 1) * isize + 124)) \
	    conv=notrunc status=none
}

le16()
{
	dd if="$1" bs=1 skip="$2" count=2 status=none |
	    od -An -tu2 | awk '{ print $1 }'
}

le32()
{
	dd if="$1" bs=1 skip="$2" count=4 status=none |
	    od -An -tu4 | awk '{ print $1 }'
}

# Give the image some content to check and repair.
populate_image()
{
	img=$1

	atf_require_prog debugfs
	echo contents > h.txt
	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'write h.txt h.txt' "$img"
	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'mkdir sub' "$img"
}

# The repaired image must satisfy e2fsck with nothing left to fix.
oracle_verdict()
{
	atf_require_prog e2fsck
	atf_check -s exit:0 -o save:oracle.log -e ignore \
	    e2fsck -n -f "$1"
	atf_check -s exit:1 \
	    grep -Eq 'Fix\?|Clear\?|Connect|errors?' oracle.log
}

# A second run must find nothing left to do.
rerun_silent()
{
	atf_check -s exit:0 -o save:rerun.log "$FSCK_EXT2FS" -y "$1"
	assert_no_problems rerun.log
}

assert_no_problems()
{
	atf_check -s exit:1 grep -Eq "$BAD_OUTPUT" "$1"
}

atf_init_test_cases()
{
	atf_add_test_case clean_ext2
	atf_add_test_case clean_ext3
	atf_add_test_case clean_ext4
	atf_add_test_case clean_ext4min
	atf_add_test_case clean_uninit
	atf_add_test_case nlink_ext2
	atf_add_test_case nlink_ext3
	atf_add_test_case nlink_ext4
	atf_add_test_case nlink_ext4min
	atf_add_test_case nlink_uninit
	atf_add_test_case orphan_ext4
	atf_add_test_case inode_csum_ext4
	atf_add_test_case no_write
}
