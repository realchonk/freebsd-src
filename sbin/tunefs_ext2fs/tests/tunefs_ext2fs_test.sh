#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 The FreeBSD Foundation
#
# This software was developed by Benjamin Stürz <benni@stuerz.xyz>
# under sponsorship from the FreeBSD Foundation.
#

# tunefs_ext2fs(8) tests.  Images are created with newfs_ext2fs(8) on
# regular files, so no privileges are needed except for the mounted
# filesystem check.  dumpfs_ext2fs(8) and fsck_ext2fs(8) verify the
# results; e2fsprogs is an optional oracle.

TUNEFS_EXT2FS="${TUNEFS_EXT2FS:-/sbin/tunefs_ext2fs}"
NEWFS_EXT2FS="${NEWFS_EXT2FS:-/sbin/newfs_ext2fs}"
DUMPFS_EXT2FS="${DUMPFS_EXT2FS:-/sbin/dumpfs_ext2fs}"
FSCK_EXT2FS="${FSCK_EXT2FS:-/sbin/fsck_ext2fs}"

export TZ=UTC

make_image()
{
	img=$1
	shift

	atf_check -s exit:0 truncate -s 32m "$img"
	atf_check -s exit:0 -o ignore -e ignore "$NEWFS_EXT2FS" "$@" "$img"
}

dump_super()
{
	atf_check -s exit:0 -o save:dump.out "$DUMPFS_EXT2FS" -s "$1"
}

# First token after the given key in a dumpfs_ext2fs line.
field()
{
	awk -v key="$1" '
	    {
		line = $0
		gsub(/[[:space:]]+/, " ", line)
		n = split(line, f, / +/)
		m = split(key, k, / +/)
		for (i = 1; i + m - 1 <= n; i++) {
		    ok = 1
		    for (j = 1; j <= m; j++)
			if (f[i + j - 1] != k[j]) {
			    ok = 0
			    break
			}
		    if (ok) {
			print f[i + m]
			exit
		    }
		}
	    }' "$2"
}

assert_clean()
{
	atf_check -s exit:0 -o save:fsck.log "$FSCK_EXT2FS" -n "$1"
	atf_check -s exit:1 grep -Eq \
	    'WRONG|BAD|UNREF|MISSING|SALVAGE|DUP|FREE SPACE' fsck.log
}

assert_e2fsck()
{
	if command -v e2fsck >/dev/null 2>&1; then
		atf_check -s exit:0 -o ignore -e ignore e2fsck -fn "$1"
	fi
}

atf_test_case print
print_head() { atf_set descr "-p prints the current parameters"; }
print_body()
{
	make_image ./print.img

	atf_check -s exit:0 -e save:tunefs.out "$TUNEFS_EXT2FS" -p "$img"
	atf_check -s exit:0 grep -q "mount count: (-C)" tunefs.out
	atf_check -s exit:0 grep -q "maximum mount count: (-c)" tunefs.out
	atf_check -s exit:0 grep -q "UUID: (-U)" tunefs.out
}

atf_test_case mount_count
mount_count_head() { atf_set descr "-C sets the mount count"; }
mount_count_body()
{
	make_image ./mountcount.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -C 7 "$img"
	dump_super "$img"
	atf_check test "$(field "mount count" dump.out)" = 7
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case max_mount_count
max_mount_count_head() { atf_set descr "-c sets the maximum mount count"; }
max_mount_count_body()
{
	make_image ./maxmount.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -c 42 "$img"
	dump_super "$img"
	atf_check test "$(field "max mount count" dump.out)" = 42
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case error_behavior
error_behavior_head() { atf_set descr "-e sets the error behavior"; }
error_behavior_body()
{
	make_image ./errors.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -e remount-ro "$img"
	atf_check -s exit:0 -e match:"remount-ro" \
	    "$TUNEFS_EXT2FS" -p "$img"
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case label
label_head() { atf_set descr "-L sets the volume label"; }
label_body()
{
	make_image ./label.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -L TESTLABEL "$img"
	dump_super "$img"
	atf_check test "$(field "volume name" dump.out)" = TESTLABEL
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case last_mounted
last_mounted_head() { atf_set descr "-M sets the last mounted directory"; }
last_mounted_body()
{
	make_image ./lastmnt.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -M /mnt/test "$img"
	dump_super "$img"
	atf_check test "$(field "last mounted on" dump.out)" = /mnt/test
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case reserved_percent
reserved_percent_head() { atf_set descr "-m sets the reserved block percentage"; }
reserved_percent_body()
{
	make_image ./reservedpct.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -m 3 "$img"
	dump_super "$img"
	atf_check test "$(field "reserved blocks" dump.out)" = 245
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case reserved_blocks
reserved_blocks_head()
{
	atf_set descr "-r sets the reserved block count and overrides -m"
}
reserved_blocks_body()
{
	make_image ./reservedblk.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -m 5 -r 300 "$img"
	dump_super "$img"
	atf_check test "$(field "reserved blocks" dump.out)" = 300
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case reserved_owner
reserved_owner_head()
{
	atf_set descr "-u and -g set the reserved block owner"
}
reserved_owner_body()
{
	atf_require_prog dumpe2fs
	make_image ./reservedowner.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -u 1001 -g 1001 "$img"
	atf_check -s exit:0 -o save:dumpe.out -e ignore dumpe2fs -h "$img"
	atf_check -s exit:0 grep -q "Reserved blocks uid:      1001" dumpe.out
	atf_check -s exit:0 grep -q "Reserved blocks gid:      1001" dumpe.out
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case interval
interval_head() { atf_set descr "-i sets the check interval"; }
interval_body()
{
	make_image ./interval.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -i 1w "$img"
	dump_super "$img"
	atf_check test "$(field "check interval" dump.out)" = 604800
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case last_check
last_check_head() { atf_set descr "-T sets the last checked time"; }
last_check_body()
{
	make_image ./lastcheck.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -T 20200102030405 "$img"
	atf_check -s exit:0 -o save:dump.out "$DUMPFS_EXT2FS" -s "$img"
	atf_check -s exit:0 grep -q "Thu Jan  2 03:04:05 2020" dump.out
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case uuid
uuid_head() { atf_set descr "-U sets the UUID"; }
uuid_body()
{
	make_image ./uuid.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" \
	    -U 12345678-1234-5678-1234-567812345678 "$img"
	dump_super "$img"
	atf_check test "$(field "UUID" dump.out)" = \
	    12345678-1234-5678-1234-567812345678
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case uuid_csum
uuid_csum_head()
{
	atf_set descr "-U preserves metadata checksums without CSUM_SEED"
}
uuid_csum_body()
{
	atf_require_prog mke2fs
	atf_require_prog dumpe2fs
	atf_require_prog e2fsck

	img=./uuidcsum.img
	atf_check -s exit:0 truncate -s 32m "$img"
	atf_check -s exit:0 -o ignore -e ignore \
	    mke2fs -q -F -t ext4 -O ^metadata_csum_seed "$img" 8192

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -U random "$img"
	atf_check -s exit:0 -o save:dumpe.out -e ignore dumpe2fs -h "$img"
	atf_check -s exit:0 grep -q "metadata_csum_seed" dumpe.out
	atf_check -s exit:0 -o ignore -e ignore e2fsck -fn "$img"
}

atf_test_case features
features_head() { atf_set descr "-O sets and clears simple features"; }
features_body()
{
	make_image ./features.img

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -O dir_nlink "$img"
	dump_super "$img"
	atf_check -s exit:0 grep -q "dir_nlink" dump.out
	atf_check -s exit:0 "$TUNEFS_EXT2FS" -O ^dir_nlink "$img"
	dump_super "$img"
	atf_check -s exit:1 grep -q "dir_nlink" dump.out
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case feature_csum_seed
feature_csum_seed_head()
{
	atf_set descr "-O metadata_csum_seed stores the checksum seed"
}
feature_csum_seed_body()
{
	atf_require_prog mke2fs
	atf_require_prog dumpe2fs
	atf_require_prog e2fsck

	img=./featureseed.img
	atf_check -s exit:0 truncate -s 32m "$img"
	atf_check -s exit:0 -o ignore -e ignore \
	    mke2fs -q -F -t ext4 -O ^metadata_csum_seed "$img" 8192

	atf_check -s exit:0 "$TUNEFS_EXT2FS" \
	    -O metadata_csum_seed "$img"
	atf_check -s exit:0 -o save:dumpe.out -e ignore dumpe2fs -h "$img"
	atf_check -s exit:0 grep -q "metadata_csum_seed" dumpe.out
	atf_check -s exit:0 -o ignore -e ignore e2fsck -fn "$img"
}

atf_test_case feature_reject
feature_reject_head()
{
	atf_set descr "-O refuses unsupported and unknown features"
}
feature_reject_body()
{
	make_image ./featurerej.img

	atf_check -s exit:10 -e match:"cannot set feature 64bit" \
	    "$TUNEFS_EXT2FS" -O 64bit "$img"
	atf_check -s exit:10 -e match:"unknown feature" \
	    "$TUNEFS_EXT2FS" -O nosuch "$img"
}

atf_test_case journal
journal_head()
{
	atf_set descr "-j adds an internal journal to an ext4 file system"
}
journal_body()
{
	atf_require_prog mke2fs
	atf_require_prog dumpe2fs
	atf_require_prog e2fsck

	img=./journal.img
	atf_check -s exit:0 truncate -s 32m "$img"
	atf_check -s exit:0 -o ignore -e ignore \
	    mke2fs -q -F -t ext4 -O ^has_journal "$img" 8192

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -j "$img"
	atf_check -s exit:0 -o save:dumpe.out -e ignore dumpe2fs -h "$img"
	atf_check -s exit:0 grep -q "has_journal" dumpe.out
	atf_check -s exit:0 grep -Eq "Journal inode:[[:space:]]+8" dumpe.out
	atf_check -s exit:0 -o ignore -e ignore e2fsck -fn "$img"
	assert_clean "$img"
}

atf_test_case journal_size
journal_size_head()
{
	atf_set descr "-J size= sets the journal size"
}
journal_size_body()
{
	atf_require_prog mke2fs
	atf_require_prog dumpe2fs
	atf_require_prog e2fsck

	img=./journalsize.img
	atf_check -s exit:0 truncate -s 64m "$img"
	atf_check -s exit:0 -o ignore -e ignore \
	    mke2fs -q -F -t ext4 -O ^has_journal "$img" 16384

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -J size=4 "$img"
	atf_check -s exit:0 -o save:dumpe.out -e ignore dumpe2fs -h "$img"
	atf_check -s exit:0 grep -q "has_journal" dumpe.out
	atf_check -s exit:0 -o ignore -e ignore e2fsck -fn "$img"
}

atf_test_case journal_refuse
journal_refuse_head()
{
	atf_set descr "-j refuses a file system without extents"
}
journal_refuse_body()
{
	make_image ./journalrej.img

	atf_check -s exit:13 -e match:"requires the extents feature" \
	    "$TUNEFS_EXT2FS" -j "$img"
}

atf_test_case dirty
dirty_head() { atf_set descr "refuse to tune a dirty file system"; }
dirty_body()
{
	atf_require_prog debugfs
	make_image ./dirty.img

	atf_check -s exit:0 -o ignore -e ignore \
	    debugfs -w -R 'ssv state 0' "$img"
	atf_check -s exit:1 -e match:"not clean" \
	    "$TUNEFS_EXT2FS" -C 1 "$img"
	atf_check -s exit:0 "$TUNEFS_EXT2FS" -f -C 1 "$img"
	dump_super "$img"
	atf_check test "$(field "mount count" dump.out)" = 1
}

atf_test_case backup_superblocks
backup_superblocks_head()
{
	atf_set descr "backup superblocks receive the new label"
}
backup_superblocks_body()
{
	make_image ./backup.img -b 1024

	atf_check -s exit:0 "$TUNEFS_EXT2FS" -L BACKUP "$img"
	atf_check -s exit:0 -o save:label.out dd if="$img" bs=1 \
	    skip=$((8193 * 1024 + 120)) count=16 status=none
	atf_check -s exit:0 grep -q BACKUP label.out
	atf_check -s exit:0 -o save:bgnr.out dd if="$img" bs=1 \
	    skip=$((8193 * 1024 + 90)) count=2 status=none
	atf_check test "$(od -An -tu2 bgnr.out | tr -d ' ')" = 1
	assert_clean "$img"
	assert_e2fsck "$img"
}

atf_test_case mounted cleanup
mounted_head()
{
	atf_set descr "refuse to tune a mounted file system"
	atf_set require.user root
}
mounted_body()
{
	atf_require_prog mdconfig
	atf_require_prog mount
	make_image ./mounted.img

	unit=$(mdconfig -a -t vnode -f "$img")
	echo "$unit" > mounted_unit
	atf_check -s exit:0 -o ignore -e ignore \
	    "$NEWFS_EXT2FS" "/dev/$unit"
	atf_check -s exit:0 mkdir -p mnt
	atf_check -s exit:0 -o ignore -e ignore mount -t ext2fs \
	    "/dev/$unit" mnt
	atf_check -s exit:1 -e match:"mounted" \
	    "$TUNEFS_EXT2FS" -C 1 "/dev/$unit"
}

mounted_cleanup()
{
	if [ -f mounted_unit ]; then
		umount mnt 2>/dev/null || true
		mdconfig -d -u "$(cat mounted_unit)" 2>/dev/null || true
	fi
}

atf_init_test_cases()
{
	atf_add_test_case print
	atf_add_test_case mount_count
	atf_add_test_case max_mount_count
	atf_add_test_case error_behavior
	atf_add_test_case label
	atf_add_test_case last_mounted
	atf_add_test_case reserved_percent
	atf_add_test_case reserved_blocks
	atf_add_test_case reserved_owner
	atf_add_test_case interval
	atf_add_test_case last_check
	atf_add_test_case uuid
	atf_add_test_case uuid_csum
	atf_add_test_case features
	atf_add_test_case feature_csum_seed
	atf_add_test_case feature_reject
	atf_add_test_case journal
	atf_add_test_case journal_size
	atf_add_test_case journal_refuse
	atf_add_test_case dirty
	atf_add_test_case backup_superblocks
	atf_add_test_case mounted
}
