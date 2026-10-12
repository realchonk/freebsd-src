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
	case " $* " in
	*' -b 4096 '*)	blocks=8192 ;;
	*)		blocks=32768 ;;
	esac
	atf_check -s exit:0 -o ignore -e ignore \
	    mke2fs -q -F "$@" "$img" "$blocks"
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


# The first data block of path, via its inode's extent or block list.
first_block()
{
	img=$1
	path=$2

	ino=$(debugfs -R "stat $path" "$img" 2>/dev/null |
	    awk '/^Inode:/ { print $2; exit }')
	debugfs -R "blocks <$ino>" "$img" 2>/dev/null |
	    awk 'NR > 1 { exit }; { print $1 }'
}

# Byte offset of the directory entry named $2 inside block file $1.
dirent_off()
{
	bf=$1
	name=$2
	o=0

	while [ "$o" -lt 1024 ]; do
		rl=$(le16_file "$bf" $((o + 4)))
		nl=$(od -An -tu1 -j $((o + 6)) -N 1 "$bf" |
		    awk '{ print $1 }')
		[ -n "$rl" ] || break
		[ "$rl" -ge 8 ] || break
		nm=$(dd if="$bf" bs=1 skip=$((o + 8)) count="$nl" \
		    status=none)
		if [ "$nm" = "$name" ]; then
			echo "$o"
			return 0
		fi
		o=$((o + rl))
	done
	return 1
}

# Write one byte into a file image or block copy.
put_byte()
{
	f=$1
	off=$2
	val=$3

	printf "$(printf '\\%03o' "$val")" |
	    dd of="$f" bs=1 seek="$off" conv=notrunc status=none
}

# Write a 32-bit little-endian word into a file.
put_word32()
{
	f=$1
	off=$2
	val=$3

	printf "$(printf '\\%03o\\%03o\\%03o\\%03o' \
	    $((val & 255)) $(((val >> 8) & 255)) \
	    $(((val >> 16) & 255)) $(((val >> 24) & 255)))" |
	    dd of="$f" bs=1 seek="$off" conv=notrunc status=none
}

# Copy a patched 1024-byte directory block back into the image.
put_block()
{
	dd if="$2" of="$1" bs=1024 seek="$3" conv=notrunc status=none
}

# Toggle the bit for block $3 inside the bitmap at byte offset $2.
flip_bit()
{
	img=$1
	bmpoff=$2
	blk=$3

	byte=$((bmpoff + blk / 8))
	old=$(od -An -tu1 -j "$byte" -N 1 "$img" | awk '{ print $1 }')
	put_byte "$img" "$byte" $((old ^ (1 << (blk % 8))))
}

le16_file()
{
	od -An -tu2 -j "$2" -N 2 "$1" | awk '{ print $1 }'
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


atf_test_case bad_entry_ext4
bad_entry_ext4_head() { atf_set descr "ext4: entry to bad inode cleared"; }

bad_entry_ext4_body()
{
	img=badent.img
	make_image "$img" -t ext4
	populate_image "$img"

	blk=$(first_block "$img" /)
	atf_check test -n "$blk"
	dd if="$img" of=root.blk bs=1024 skip="$blk" count=1 status=none
	off=$(dirent_off root.blk h.txt)
	atf_check test -n "$off"
	put_word32 root.blk $((off + 0)) 99999
	put_block "$img" root.blk "$blk"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'BAD INODE' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case bad_dot_ext4
bad_dot_ext4_head() { atf_set descr "ext4: '.' pointing elsewhere fixed"; }

bad_dot_ext4_body()
{
	img=dot.img
	make_image "$img" -t ext4
	populate_image "$img"

	# The '.' entry leads the block; its target is the entry's inode.
	blk=$(first_block "$img" /)
	atf_check test -n "$blk"
	dd if="$img" of=root.blk bs=1024 skip="$blk" count=1 status=none
	put_word32 root.blk 0 13
	put_block "$img" root.blk "$blk"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq "BAD '.' INO" fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case bad_dotdot_ext4
bad_dotdot_ext4_head() { atf_set descr "ext4: wrong '..' target fixed"; }

bad_dotdot_ext4_body()
{
	img=dotdot.img
	make_image "$img" -t ext4
	populate_image "$img"

	# '..' follows '.'; point it at the directory itself.
	blk=$(first_block "$img" sub)
	atf_check test -n "$blk"
	dd if="$img" of=sub.blk bs=1024 skip="$blk" count=1 status=none
	rl=$(le16_file sub.blk 4)
	put_word32 sub.blk "$rl" 14
	put_block "$img" sub.blk "$blk"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq "BAD '\.\.' INO" fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case filetype_ext4
filetype_ext4_head() { atf_set descr "ext4: wrong filetype byte fixed"; }

filetype_ext4_body()
{
	img=ftype.img
	make_image "$img" -t ext4
	populate_image "$img"

	blk=$(first_block "$img" /)
	atf_check test -n "$blk"
	dd if="$img" of=root.blk bs=1024 skip="$blk" count=1 status=none
	off=$(dirent_off root.blk h.txt)
	atf_check test -n "$off"
	put_byte root.blk $((off + 7)) 7	# EXT2_FT_UNKNOWN+1.. bogus
	put_block "$img" root.blk "$blk"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'WRONG TYPE' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case dir_csum_ext4
dir_csum_ext4_head() { atf_set descr "ext4: directory block checksum fixed"; }

dir_csum_ext4_body()
{
	img=dircsum.img
	make_image "$img" -t ext4
	populate_image "$img"

	# Overwrite padding behind the name: structure stays valid,
	# only the block checksum breaks.
	blk=$(first_block "$img" /)
	atf_check test -n "$blk"
	dd if="$img" of=root.blk bs=1024 skip="$blk" count=1 status=none
	off=$(dirent_off root.blk h.txt)
	atf_check test -n "$off"
	put_byte root.blk $((off + 13)) 165
	put_block "$img" root.blk "$blk"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'DIRECTORY BLOCK .* CHECKSUM WRONG' \
	    fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case bitmap_ext2
bitmap_ext2_head() { atf_set descr "ext2: block bitmap rebuilt"; }

bitmap_ext2_body() { bitmap_body -t ext2; }

atf_test_case bitmap_ext4
bitmap_ext4_head() { atf_set descr "ext4: block bitmap rebuilt"; }

bitmap_ext4_body() { bitmap_body -t ext4; }

bitmap_body()
{
	img=bitmap.img
	make_image "$img" "$@"
	populate_image "$img"

	# Mark the root directory's block free in the bitmap.
	blk=$(first_block "$img" /)
	bb=$(le32 "$img" $((2048 + 0)))
	flip_bit "$img" $((bb * 1024)) "$blk"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case freecounts_ext2
freecounts_ext2_head() { atf_set descr "ext2: free counts repaired"; }

freecounts_ext2_body()
{
	img=freec.img
	make_image "$img" -t ext2
	populate_image "$img"

	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'ssv free_blocks_count 16000' "$img"
	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'ssv free_inodes_count 8000' "$img"

	atf_check -s exit:1 -o ignore "$FSCK_EXT2FS" -y "$img"
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case freecounts_ext4
freecounts_ext4_head() { atf_set descr "ext4: group counts repaired"; }

freecounts_ext4_body()
{
	img=freec4.img
	make_image "$img" -t ext4
	populate_image "$img"

	# Zero the group's free block count; this also invalidates the
	# group descriptor checksum, which the repair recomputes.
	put_word32 "$img" $((2048 + 12)) 0

	atf_check -s exit:1 -o ignore "$FSCK_EXT2FS" -y "$img"
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case lostfound_ext4
lostfound_ext4_head()
{
	atf_set descr "ext4: lost+found created, orphan reconnected"
}

lostfound_ext4_body()
{
	img=lf.img
	make_image "$img" -t ext4
	populate_image "$img"

	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'unlink lost+found' "$img"
	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'unlink sub' "$img"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'UNREF' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case orphan_ext2
orphan_ext2_head() { atf_set descr "ext2: orphaned dir reconnected"; }

orphan_ext2_body()
{
	img=orph2.img
	make_image "$img" -t ext2
	populate_image "$img"

	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'unlink sub' "$img"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'UNREF' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case preen_ext2
preen_ext2_head() { atf_set descr "ext2: preen salvages a wrong link count"; }

preen_ext2_body()
{
	img=preen.img
	make_image "$img" -t ext2
	populate_image "$img"

	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'sif /h.txt links_count 6' "$img"
	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'ssv state 0' "$img"

	atf_check -s exit:0 -o ignore "$FSCK_EXT2FS" -p "$img"
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case clean_4k_ext2
clean_4k_ext2_head() { atf_set descr "ext2 4K blocks: clean -n"; }

clean_4k_ext2_body() { clean_body -b 4096 -t ext2; }

atf_test_case clean_4k_ext4
clean_4k_ext4_head() { atf_set descr "ext4 4K blocks: clean -n"; }

clean_4k_ext4_body() { clean_body -b 4096 -t ext4; }

atf_test_case nlink_4k_ext4
nlink_4k_ext4_head() { atf_set descr "ext4 4K blocks: link count repaired"; }

nlink_4k_ext4_body() { nlink_body -b 4096 -t ext4; }

atf_test_case inode_csum_128_ext4
inode_csum_128_ext4_head()
{
	atf_set descr "ext4 128B inodes: checksum repaired"
}

inode_csum_128_ext4_body()
{
	img=icsum128.img
	make_image "$img" -t ext4 -I 128
	populate_image "$img"

	corrupt_inode_csum "$img"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'INODE CHECKSUM WRONG' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case clean_newfs
clean_newfs_head()
{
	atf_set descr "newfs_ext2fs image passes -n silently"
	atf_set require.user root
}

clean_newfs_body()
{
	img=newfs.img
	truncate -s 32m "$img"

	# newfs_ext2fs labels the device, so it needs a real node.
	unit=$(mdconfig -a -t vnode -f "$img")
	atf_check -s exit:0 -o ignore -e ignore \
	    "${NEWFS_EXT2FS:-/sbin/newfs_ext2fs}" "/dev/$unit"
	mdconfig -d -u "$unit"

	atf_check -s exit:0 -o save:fsck.log "$FSCK_EXT2FS" -n "$img"
	assert_no_problems fsck.log
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
	atf_add_test_case bad_entry_ext4
	atf_add_test_case bad_dot_ext4
	atf_add_test_case bad_dotdot_ext4
	atf_add_test_case filetype_ext4
	atf_add_test_case dir_csum_ext4
	atf_add_test_case bitmap_ext2
	atf_add_test_case bitmap_ext4
	atf_add_test_case freecounts_ext2
	atf_add_test_case freecounts_ext4
	atf_add_test_case lostfound_ext4
	atf_add_test_case orphan_ext2
	atf_add_test_case preen_ext2
	atf_add_test_case clean_4k_ext2
	atf_add_test_case clean_4k_ext4
	atf_add_test_case nlink_4k_ext4
	atf_add_test_case inode_csum_128_ext4
	atf_add_test_case clean_newfs
}
