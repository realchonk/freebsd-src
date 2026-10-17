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


# Journal tests: the images carry one hand-built log transaction.
# Journal block numbers come from the journal inode (8) via debugfs.

journal_setup()
{
	img=$1
	fstype=${2:-ext3}

	make_image "$img" -t "$fstype"
	populate_image "$img"
	hino=$(debugfs -R 'stat /h.txt' "$img" 2>/dev/null |
	    awk '/^Inode:/ { print $2; exit }')
	hblk=$(debugfs -R "blocks <$hino>" "$img" 2>/dev/null |
	    awk '{ print $1; exit }')
	jsb=$(debugfs -R 'blocks <8>' "$img" 2>/dev/null | awk '{ print $1 }')
	jb1=$(debugfs -R 'blocks <8>' "$img" 2>/dev/null | awk '{ print $2 }')
	atf_check test -n "$hblk"
	atf_check test -n "$jsb"
}

# Write a log block: a 12-byte big-endian header plus payload.
journal_header()
{
	file=$1
	type=$2
	seq=$3

	printf '\300\073\071\230' > "$file"
	be_bytes4 "$type" >> "$file"
	be_bytes4 "$seq" >> "$file"
}

# One 8-byte tag: target block, checksum, flags.
journal_tag()
{
	file=$1
	blk=$2
	flags=$3

	be_bytes4 "$blk" >> "$file"
	printf '\000\000' >> "$file"
	be_bytes2 "$flags" >> "$file"
}

# Mark the image as needing recovery and not clean.
journal_mark()
{
	img=$1

	inc=$(le32 "$img" $((1024 + 96)))
	put_word32 "$img" $((1024 + 96)) $((inc | 4))
	put_byte "$img" $((1024 + 58)) 0
	put_byte "$img" $((1024 + 59)) 0
}

# Point the journal superblock at log block $2, sequence $3.  The
# journal is big-endian throughout.
journal_start()
{
	img=$1
	start=$2
	seq=$3

	put_be32 "$img" $(($jsb * 1024 + 0x18)) "$seq"
	put_be32 "$img" $(($jsb * 1024 + 0x1c)) "$start"
}

# Write a 32-bit big-endian word into a file.
put_be32()
{
	f=$1
	off=$2
	val=$3

	printf "$(printf '\\%03o\\%03o\\%03o\\%03o' \
	    $(((val >> 24) & 255)) $(((val >> 16) & 255)) \
	    $(((val >> 8) & 255)) $((val & 255)))" |
	    dd of="$f" bs=1 seek="$off" conv=notrunc status=none
}

be_bytes2()
{
	v=$1

	printf "$(printf '\\%03o\\%03o' \
	    $((v >> 8)) $((v & 255)))"
}

be_bytes4()
{
	v=$1

	printf "$(printf '\\%03o\\%03o\\%03o\\%03o' \
	    $(((v >> 24) & 255)) $(((v >> 16) & 255)) \
	    $(((v >> 8) & 255)) $((v & 255)))"
}

atf_test_case journal_replay
journal_replay_head() { atf_set descr "ext3: journal transaction replayed"; }

journal_replay_body()
{
	img=jnl.img

	journal_setup "$img"
	# Descriptor, data, commit at log blocks 1-3, sequence 5.
	journal_header desc.blk 1 5
	journal_tag desc.blk "$hblk" 10
	put_block "$img" desc.blk "$jb1"
	printf 'REPLAYED-CONTENT\n' > data.blk
	truncate -s 1024 data.blk
	put_block "$img" data.blk $(($jb1 + 1))
	journal_header commit.blk 2 5
	truncate -s 1024 commit.blk
	put_block "$img" commit.blk $(($jb1 + 2))
	journal_mark "$img"
	journal_start "$img" 1 5

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'REPLAYED JOURNAL: 1 TRANSACTION' \
	    fsck.log
	atf_check -o match:'REPLAYED-' -e ignore \
	    debugfs -R 'cat /h.txt' "$img" 2>/dev/null
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case journal_revoke
journal_revoke_head() { atf_set descr "ext3: revoked write not replayed"; }

journal_revoke_body()
{
	img=jrvk.img

	journal_setup "$img"
	# Transaction 5 writes h.txt's block; transaction 6 revokes it.
	journal_header desc.blk 1 5
	journal_tag desc.blk "$hblk" 10
	put_block "$img" desc.blk "$jb1"
	printf 'REPLAYED-CONTENT\n' > data.blk
	truncate -s 1024 data.blk
	put_block "$img" data.blk $(($jb1 + 1))
	journal_header commit.blk 2 5
	truncate -s 1024 commit.blk
	put_block "$img" commit.blk $(($jb1 + 2))
	journal_header rev.blk 5 6
	be_bytes4 20 >> rev.blk
	be_bytes4 "$hblk" >> rev.blk
	truncate -s 1024 rev.blk
	put_block "$img" rev.blk $(($jb1 + 3))
	journal_header commit2.blk 2 6
	truncate -s 1024 commit2.blk
	put_block "$img" commit2.blk $(($jb1 + 4))
	journal_mark "$img"
	journal_start "$img" 1 5

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'REPLAYED JOURNAL: 2 TRANSACTIONS' \
	    fsck.log
	atf_check -o match:'contents' -e ignore \
	    debugfs -R 'cat /h.txt' "$img" 2>/dev/null
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case journal_nowrite
journal_nowrite_head() { atf_set descr "ext3: -n reports but never replays"; }

journal_nowrite_body()
{
	img=jnwr.img

	journal_setup "$img"
	journal_header desc.blk 1 5
	journal_tag desc.blk "$hblk" 10
	put_block "$img" desc.blk "$jb1"
	printf 'REPLAYED-CONTENT\n' > data.blk
	truncate -s 1024 data.blk
	put_block "$img" data.blk $(($jb1 + 1))
	journal_header commit.blk 2 5
	truncate -s 1024 commit.blk
	put_block "$img" commit.blk $(($jb1 + 2))
	journal_mark "$img"
	journal_start "$img" 1 5

	before=$(md5 -q "$img")
	atf_check -s exit:4 -o save:fsck.log "$FSCK_EXT2FS" -n "$img"
	atf_check -s exit:0 grep -Eq 'JOURNAL RECOVERY NEEDED' fsck.log
	atf_check -o inline:"$before"$'\n' md5 -q "$img"
}

atf_test_case journal_reset
journal_reset_head() { atf_set descr "ext3: unreadable journal reset"; }

journal_reset_body()
{
	img=jrst.img

	journal_setup "$img"
	# Garbage over the journal superblock, with recovery requested.
	printf 'NOT-A-JOURNAL-AT-ALL' > bad.blk
	truncate -s 1024 bad.blk
	put_block "$img" bad.blk "$jsb"
	journal_mark "$img"

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'JOURNAL RESET' fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}


# Physical journal block for logical log index $2.
jblock()
{
	debugfs -R "bmap <8> $2" "$1" 2>/dev/null
}

# Set bits in a big-endian 32-bit word of the image.
or_be32()
{
	img=$1
	off=$2
	bits=$3
	v=$(od -An -tx1 -j "$off" -N 4 "$img" | tr -d ' \n')

	put_be32 "$img" "$off" $((0x$v | bits))
}

# One 12-byte tag: 8-byte tag plus the high block word.
journal_tag64()
{
	file=$1
	blk=$2
	flags=$3

	journal_tag "$file" "$blk" "$flags"
	be_bytes4 0 >> "$file"
}

# One 16-byte v3 tag: block, flags, high word, checksum.
journal_tag3()
{
	file=$1
	blk=$2
	flags=$3

	be_bytes4 "$blk" >> "$file"
	be_bytes4 "$flags" >> "$file"
	be_bytes4 0 >> "$file"
	be_bytes4 0 >> "$file"
}

# Write data block, commit block and journal state around a
# descriptor already sitting at log block $2; last is one past the
# end of the log area, and the commit block wraps to its start.
journal_commit_after()
{
	img=$1
	descblk=$2
	last=$3
	commitblk=$(($descblk + 2))

	if [ "$commitblk" -ge "$last" ]; then
		commitblk=$(($commitblk - $last + 1))
	fi
	printf 'REPLAYED-CONTENT\n' > data.blk
	truncate -s 1024 data.blk
	put_block "$img" data.blk $(jblock "$img" $(($descblk + 1)))
	journal_header commit.blk 2 5
	truncate -s 1024 commit.blk
	put_block "$img" commit.blk $(jblock "$img" "$commitblk")
	journal_mark "$img"
	journal_start "$img" "$descblk" 5
}

atf_test_case journal_escape
journal_escape_head() { atf_set descr "ext3: escaped data block restored"; }

journal_escape_body()
{
	img=jesc.img

	journal_setup "$img"
	journal_header desc.blk 1 5
	journal_tag desc.blk "$hblk" 11	# ESCAPE too
	put_block "$img" desc.blk "$jb1"
	printf '\300\073\071\230ESCAPED-DATA\n' > data.blk
	truncate -s 1024 data.blk
	put_block "$img" data.blk $(($jb1 + 1))
	journal_header commit.blk 2 5
	truncate -s 1024 commit.blk
	put_block "$img" commit.blk $(($jb1 + 2))
	journal_mark "$img"
	journal_start "$img" 1 5

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'REPLAYED JOURNAL: 1 TRANSACTION' \
	    fsck.log
	# The escape magic leads the restored data block.
	atf_check -o save:esc.hex sh -c "dd if=$img bs=1024 skip=$hblk \
	    count=1 status=none | od -An -tx1 -N8"
	atf_check -o inline:"c03b399845534341\n" \
	    sh -c "tr -d ' \n' < esc.hex; echo"
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case journal_tag64
journal_tag64_head() { atf_set descr "ext3: 64-bit tags replayed"; }

journal_tag64_body()
{
	img=jt64.img

	journal_setup "$img"
	journal_header desc.blk 1 5
	journal_tag64 desc.blk "$hblk" 10
	put_block "$img" desc.blk "$jb1"
	or_be32 "$img" $(($jsb * 1024 + 0x28)) 2
	journal_commit_after "$img" 1 4096

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'REPLAYED JOURNAL: 1 TRANSACTION' \
	    fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case journal_tag3
journal_tag3_head() { atf_set descr "ext3: v3 checksum tags replayed"; }

journal_tag3_body()
{
	img=jt3.img

	journal_setup "$img"
	journal_header desc.blk 1 5
	journal_tag3 desc.blk "$hblk" 10
	put_block "$img" desc.blk "$jb1"
	or_be32 "$img" $(($jsb * 1024 + 0x28)) 16
	# v3 journals carry crc32c checksums, and e2fsck checks the type.
	put_byte "$img" $(($jsb * 1024 + 0x50)) 4
	journal_commit_after "$img" 1 4096

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'REPLAYED JOURNAL: 1 TRANSACTION' \
	    fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case journal_wrap
journal_wrap_head() { atf_set descr "ext3: log wraparound replayed"; }

journal_wrap_body()
{
	img=jwrp.img

	journal_setup "$img"
	# The log area ends at 4095; the commit block wraps to block 1.
	journal_header desc.blk 1 5
	journal_tag desc.blk "$hblk" 10
	put_block "$img" desc.blk $(jblock "$img" 4094)
	journal_commit_after "$img" 4094 4096

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'REPLAYED JOURNAL: 1 TRANSACTION' \
	    fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case journal_fastcommit
journal_fastcommit_head() { atf_set descr "ext3: fast commit area excluded"; }

journal_fastcommit_body()
{
	img=jfc.img

	journal_setup "$img"
	# 64 fast commit blocks move the end of the log area to 4031;
	# the commit block after the data at 4031 wraps to block 1.
	or_be32 "$img" $(($jsb * 1024 + 0x28)) 32
	put_be32 "$img" $(($jsb * 1024 + 0x54)) 64
	journal_header desc.blk 1 5
	journal_tag desc.blk "$hblk" 10
	put_block "$img" desc.blk $(jblock "$img" 4030)
	journal_commit_after "$img" 4030 4032

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'REPLAYED JOURNAL: 1 TRANSACTION' \
	    fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
}

atf_test_case journal_ext4
journal_ext4_head() { atf_set descr "ext4: extent-mapped journal replayed"; }

journal_ext4_body()
{
	img=je4.img

	journal_setup "$img" ext4
	journal_header desc.blk 1 5
	journal_tag desc.blk "$hblk" 10
	put_block "$img" desc.blk "$jb1"
	journal_commit_after "$img" 1 4096

	atf_check -s exit:1 -o save:fsck.log "$FSCK_EXT2FS" -y "$img"
	atf_check -s exit:0 grep -Eq 'REPLAYED JOURNAL: 1 TRANSACTION' \
	    fsck.log
	oracle_verdict "$img"
	rerun_silent "$img"
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
	atf_add_test_case journal_replay
	atf_add_test_case journal_revoke
	atf_add_test_case journal_nowrite
	atf_add_test_case journal_reset
	atf_add_test_case journal_escape
	atf_add_test_case journal_tag64
	atf_add_test_case journal_tag3
	atf_add_test_case journal_wrap
	atf_add_test_case journal_fastcommit
	atf_add_test_case journal_ext4
}
