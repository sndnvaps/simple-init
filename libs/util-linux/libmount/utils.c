/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * This file is part of libmount from util-linux project.
 *
 * Copyright (C) 2008-2018 Karel Zak <kzak@redhat.com>
 *
 * libmount is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 */

/**
 * SECTION: utils
 * @title: Utils
 * @short_description: misc utils.
 */

#define _GNU_SOURCE
#include <sched.h>
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <libblkid/blkid.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include "strutils.h"
#include "pathnames.h"
#include "mountP.h"
#include "mangle.h"
#include "match.h"
#include "fileutils.h"
#include "statfs_magic.h"
#include "sysfs.h"

/*
 * Return 1 if the file is not accessible or empty
 */
int is_file_empty(const char *name)
{
	struct stat st;
	return (stat(name, &st) != 0 || st.st_size == 0);
}

int mnt_valid_tagname(const char *tagname)
{
	if (tagname && *tagname && (
	    strcmp("ID", tagname) == 0 ||
	    strcmp("UUID", tagname) == 0 ||
	    strcmp("LABEL", tagname) == 0 ||
	    strcmp("PARTUUID", tagname) == 0 ||
	    strcmp("PARTLABEL", tagname) == 0))
		return 1;

	return 0;
}

/**
 * mnt_tag_is_valid:
 * @tag: NAME=value string
 *
 * Returns: 1 if the @tag is parsable and tag NAME= is supported by libmount, or 0.
 */
int mnt_tag_is_valid(const char *tag)
{
	char *t = NULL;
	int rc = tag && blkid_parse_tag_string(tag, &t, NULL) == 0
		     && mnt_valid_tagname(t);

	free(t);
	return rc;
}

int mnt_parse_offset(const char *str, size_t len, uintmax_t *res)
{
	char *p;
	int rc = 0;

	if (!str || !*str)
		return -EINVAL;

	p = strndup(str, len);
	if (!p)
		return -errno;

	if (strtosize(p, res))
		rc = -EINVAL;
	free(p);
	return rc;
}

/* used as a callback by bsearch in mnt_fstype_is_pseudofs() */
static int fstype_cmp(const void *v1, const void *v2)
{
	const char *s1 = *(char * const *)v1;
	const char *s2 = *(char * const *)v2;

	return strcmp(s1, s2);
}

int mnt_stat_mountpoint(const char *target, struct stat *st)
{
#ifdef AT_NO_AUTOMOUNT
	return fstatat(AT_FDCWD, target, st, AT_NO_AUTOMOUNT);
#else
	return stat(target, st);
#endif
}

int mnt_lstat_mountpoint(const char *target, struct stat *st)
{
#ifdef AT_NO_AUTOMOUNT
	return fstatat(AT_FDCWD, target, st, AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW);
#else
	return lstat(target, st);
#endif
}


/*
 * Note that the @target has to be an absolute path (so at least "/").  The
 * @filename returns an allocated buffer with the last path component, for example:
 *
 * mnt_chdir_to_parent("/mnt/test", &buf) ==> chdir("/mnt"), buf="test"
 */
int mnt_chdir_to_parent(const char *target, char **filename)
{
	char *buf, *parent, *last = NULL;
	char cwd[PATH_MAX];
	int rc = -EINVAL;

	if (!target || *target != '/')
		return -EINVAL;

	buf = strdup(target);
	if (!buf)
		return -ENOMEM;

	if (*(buf + 1) != '\0') {
		last = stripoff_last_component(buf);
		if (!last)
			goto err;
	}

	parent = buf && *buf ? buf : "/";

	if (chdir(parent) == -1) {
		rc = -errno;
		goto err;
	}
	if (!getcwd(cwd, sizeof(cwd))) {
		rc = -errno;
		goto err;
	}
	if (strcmp(cwd, parent) != 0) {
		goto err;
	}

	if (filename) {
		*filename = buf;

		if (!last || !*last)
			memcpy(*filename, ".", 2);
		else
			memmove(*filename, last, strlen(last) + 1);
	} else
		free(buf);
	return 0;
err:
	free(buf);
	return rc;
}

/*
 * Check if @path is on a read-only filesystem independently of file permissions.
 */
int mnt_is_readonly(const char *path)
{
	if (access(path, W_OK) == 0)
		return 0;
	if (errno == EROFS)
		return 1;
	if (errno != EACCES)
		return 0;

	/*
	 * access(2) returns EACCES on read-only FS:
	 *
	 * - for set-uid application if one component of the path is not
	 *   accessible for the current rUID. (Note that euidaccess(2) does not
	 *   check for EROFS at all).
	 *
	 * - for a read-write filesystem with a read-only VFS node (aka -o remount,ro,bind)
	 */
	struct timespec times[2];

	times[0].tv_nsec = UTIME_NOW;	/* atime */
	times[1].tv_nsec = UTIME_OMIT;	/* mtime */

	if (utimensat(AT_FDCWD, path, times, 0) == -1)
		return errno == EROFS;
	return 0;
}

/**
 * mnt_mangle:
 * @str: string
 *
 * Encode @str to be compatible with fstab/mtab
 *
 * Returns: newly allocated string or NULL in case of error.
 */
char *mnt_mangle(const char *str)
{
	return mangle(str);
}

/**
 * mnt_unmangle:
 * @str: string
 *
 * Decode @str from fstab/mtab
 *
 * Returns: newly allocated string or NULL in case of error.
 */
char *mnt_unmangle(const char *str)
{
	return unmangle(str, NULL);
}

/**
 * mnt_fstype_is_pseudofs:
 * @type: filesystem name
 *
 * Returns: 1 for filesystems like proc, sysfs, ... or 0.
 */
int mnt_fstype_is_pseudofs(const char *type)
{
	/* This array must remain sorted when adding new fstypes */
	static const char *pseudofs[] = {
		"anon_inodefs",
		"apparmorfs",
		"autofs",
		"bdev",
		"binder",
		"binfmt_misc",
		"bpf",
		"cgroup",
		"cgroup2",
		"configfs",
		"cpuset",
		"debugfs",
		"devfs",
		"devpts",
		"devtmpfs",
		"dlmfs",
		"dmabuf",
		"drm",
		"efivarfs",
		"fuse", /* Fallback name of fuse used by many poorly written drivers. */
		"fuse.archivemount", /* Not a true pseudofs (has source), but source is not reported. */
		"fuse.avfsd", /* Not a true pseudofs (has source), but source is not reported. */
		"fuse.dumpfs", /* In fact, it is a netfs, but source is not reported. */
		"fuse.encfs", /* Not a true pseudofs (has source), but source is not reported. */
		"fuse.gvfs-fuse-daemon", /* Old name, not used by gvfs any more. */
		"fuse.gvfsd-fuse",
		"fuse.lxcfs",
		"fuse.rofiles-fuse",
		"fuse.vmware-vmblock",
		"fuse.xwmfs",
		"fusectl",
		"hugetlbfs",
		"ipathfs",
		"mqueue",
		"nfsd",
		"none",
		"nsfs",
		"overlay",
		"pipefs",
		"proc",
		"pstore",
		"ramfs",
		"resctrl",
		"rootfs",
		"rpc_pipefs",
		"securityfs",
		"selinuxfs",
		"smackfs",
		"sockfs",
		"spufs",
		"sysfs",
		"tmpfs",
		"tracefs",
		"vboxsf",
		"virtiofs"
	};

	return !(bsearch(&type, pseudofs, ARRLEN(pseudofs),
				sizeof(char*), fstype_cmp) == NULL);
}

/**
 * mnt_fstype_is_netfs:
 * @type: filesystem name
 *
 * Returns: 1 for filesystems like cifs, nfs, ... or 0.
 */
int mnt_fstype_is_netfs(const char *type)
{
	if (strcmp(type, "cifs")   == 0 ||
	    strcmp(type, "smb3")   == 0 ||
	    strcmp(type, "smbfs")  == 0 ||
	    strncmp(type,"nfs", 3) == 0 ||
	    strcmp(type, "afs")    == 0 ||
	    strcmp(type, "ncpfs")  == 0 ||
	    strcmp(type, "glusterfs")  == 0 ||
	    strcmp(type, "fuse.curlftpfs") == 0 ||
	    strcmp(type, "fuse.sshfs") == 0 ||
	    strncmp(type,"9p", 2)  == 0)
		return 1;
	return 0;
}

const char *mnt_statfs_get_fstype(struct statfs *vfs)
{
	switch (vfs->f_type) {
	case STATFS_ADFS_MAGIC:		return "adfs";
	case STATFS_AFFS_MAGIC:		return "affs";
	case STATFS_AFS_MAGIC:		return "afs";
	case STATFS_AUTOFS_MAGIC:	return "autofs";
	case STATFS_BDEVFS_MAGIC:	return "bdev";
	case STATFS_BEFS_MAGIC:		return "befs";
	case STATFS_BFS_MAGIC:		return "befs";
	case STATFS_BINFMTFS_MAGIC:	return "binfmt_misc";
	case STATFS_BTRFS_MAGIC:	return "btrfs";
	case STATFS_CEPH_MAGIC:		return "ceph";
	case STATFS_CGROUP_MAGIC:	return "cgroup";
	case STATFS_CIFS_MAGIC:		return "cifs";
	case STATFS_CODA_MAGIC:		return "coda";
	case STATFS_CONFIGFS_MAGIC:	return "configfs";
	case STATFS_CRAMFS_MAGIC:	return "cramfs";
	case STATFS_DEBUGFS_MAGIC:	return "debugfs";
	case STATFS_DEVPTS_MAGIC:	return "devpts";
	case STATFS_ECRYPTFS_MAGIC:	return "ecryptfs";
	case STATFS_EFIVARFS_MAGIC:	return "efivarfs";
	case STATFS_EFS_MAGIC:		return "efs";
	case STATFS_EXOFS_MAGIC:	return "exofs";
	case STATFS_EXT4_MAGIC:		return "ext4";	   /* all extN use the same magic */
	case STATFS_F2FS_MAGIC:		return "f2fs";
	case STATFS_FUSE_MAGIC:		return "fuse";
	case STATFS_FUTEXFS_MAGIC:	return "futexfs";
	case STATFS_GFS2_MAGIC:		return "gfs2";
	case STATFS_HFSPLUS_MAGIC:	return "hfsplus";
	case STATFS_HOSTFS_MAGIC:	return "hostfs";
	case STATFS_HPFS_MAGIC:		return "hpfs";
	case STATFS_HPPFS_MAGIC:	return "hppfs";
	case STATFS_HUGETLBFS_MAGIC:	return "hugetlbfs";
	case STATFS_ISOFS_MAGIC:	return "iso9660";
	case STATFS_JFFS2_MAGIC:	return "jffs2";
	case STATFS_JFS_MAGIC:		return "jfs";
	case STATFS_LOGFS_MAGIC:	return "logfs";
	case STATFS_MINIX2_MAGIC:
	case STATFS_MINIX2_MAGIC2:
	case STATFS_MINIX3_MAGIC:
	case STATFS_MINIX_MAGIC:
	case STATFS_MINIX_MAGIC2:	return "minix";
	case STATFS_MQUEUE_MAGIC:	return "mqueue";
	case STATFS_MSDOS_MAGIC:	return "vfat";
	case STATFS_NCP_MAGIC:		return "ncp";
	case STATFS_NFS_MAGIC:		return "nfs";
	case STATFS_NILFS_MAGIC:	return "nilfs2";
	case STATFS_NTFS_MAGIC:		return "ntfs";
	case STATFS_OCFS2_MAGIC:	return "ocfs2";
	case STATFS_OMFS_MAGIC:		return "omfs";
	case STATFS_OPENPROMFS_MAGIC:	return "openpromfs";
	case STATFS_PIPEFS_MAGIC:	return "pipefs";
	case STATFS_PROC_MAGIC:		return "proc";
	case STATFS_PSTOREFS_MAGIC:	return "pstore";
	case STATFS_QNX4_MAGIC:		return "qnx4";
	case STATFS_QNX6_MAGIC:		return "qnx6";
	case STATFS_RAMFS_MAGIC:	return "ramfs";
	case STATFS_REISERFS_MAGIC:	return "reiser4";
	case STATFS_ROMFS_MAGIC:	return "romfs";
	case STATFS_SECURITYFS_MAGIC:	return "securityfs";
	case STATFS_SELINUXFS_MAGIC:	return "selinuxfs";
	case STATFS_SMACKFS_MAGIC:	return "smackfs";
	case STATFS_SMB_MAGIC:		return "smb";
	case STATFS_SOCKFS_MAGIC:	return "sockfs";
	case STATFS_SQUASHFS_MAGIC:	return "squashfs";
	case STATFS_SYSFS_MAGIC:	return "sysfs";
	case STATFS_TMPFS_MAGIC:	return "tmpfs";
	case STATFS_UBIFS_MAGIC:	return "ubifs";
	case STATFS_UDF_MAGIC:		return "udf";
	case STATFS_UFS2_MAGIC:
	case STATFS_UFS_MAGIC:		return "ufs";
	case STATFS_V9FS_MAGIC:		return "9p";
	case STATFS_VXFS_MAGIC:		return "vxfs";
	case STATFS_XENFS_MAGIC:	return "xenfs";
	case STATFS_XFS_MAGIC:		return "xfs";
	default:
		break;
	}

	return NULL;
}

/**
 * mnt_match_fstype:
 * @type: filesystem type
 * @pattern: filesystem name or comma delimited list of names
 *
 * The @pattern list of filesystems can be prefixed with a global
 * "no" prefix to invert matching of the whole list. The "no" could
 * also be used for individual items in the @pattern list. So,
 * "nofoo,bar" has the same meaning as "nofoo,nobar".
 *
 * "bar"  : "nofoo,bar"		-> False   (global "no" prefix)
 *
 * "bar"  : "foo,bar"		-> True
 *
 * "bar" : "foo,nobar"		-> False
 *
 * Returns: 1 if type is matching, else 0. This function also returns
 *          0 if @pattern is NULL and @type is non-NULL.
 */
int mnt_match_fstype(const char *type, const char *pattern)
{
	return match_fstype(type, pattern);
}

void mnt_free_filesystems(char **filesystems)
{
	char **p;

	if (!filesystems)
		return;
	for (p = filesystems; *p; p++)
		free(*p);
	free(filesystems);
}

static int add_filesystem(char ***filesystems, char *name)
{
	int n = 0;

	if (*filesystems) {
		char **p;
		for (n = 0, p = *filesystems; *p; p++, n++) {
			if (strcmp(*p, name) == 0)
				return 0;
		}
	}

	#define MYCHUNK	16

	if (n == 0 || !((n + 1) % MYCHUNK)) {
		size_t items = ((n + 1 + MYCHUNK) / MYCHUNK) * MYCHUNK;
		char **x = realloc(*filesystems, items * sizeof(char *));

		if (!x)
			goto err;
		*filesystems = x;
	}
	name = strdup(name);
	(*filesystems)[n] = name;
	(*filesystems)[n + 1] = NULL;
	if (!name)
		goto err;
	return 0;
err:
	mnt_free_filesystems(*filesystems);
	return -ENOMEM;
}

static int get_filesystems(const char *filename, char ***filesystems, const char *pattern)
{
	int rc = 0;
	FILE *f;
	char line[129];

	f = fopen(filename, "re");
	if (!f)
		return 1;

	while (fgets(line, sizeof(line), f)) {
		char name[sizeof(line)];

		if (*line == '#' || strncmp(line, "nodev", 5) == 0)
			continue;
		if (sscanf(line, " %128[^\n ]\n", name) != 1)
			continue;
		if (strcmp(name, "*") == 0) {
			rc = 1;
			break;		/* end of the /etc/filesystems */
		}
		if (pattern && !mnt_match_fstype(name, pattern))
			continue;
		rc = add_filesystem(filesystems, name);
		if (rc)
			break;
	}

	fclose(f);
	return rc;
}

/*
 * Always check the @filesystems pointer!
 *
 * man mount:
 *
 * ...mount will try to read the file /etc/filesystems, or, if that does not
 * exist, /proc/filesystems. All of the filesystem  types  listed  there  will
 * be tried,  except  for  those  that  are  labeled  "nodev"  (e.g.,  devpts,
 * proc  and  nfs).  If /etc/filesystems ends in a line with a single * only,
 * mount will read /proc/filesystems afterwards.
 */
int mnt_get_filesystems(char ***filesystems, const char *pattern)
{
	int rc;

	if (!filesystems)
		return -EINVAL;

	*filesystems = NULL;

	rc = get_filesystems(_PATH_FILESYSTEMS, filesystems, pattern);
	if (rc != 1)
		return rc;

	rc = get_filesystems(_PATH_PROC_FILESYSTEMS, filesystems, pattern);
	if (rc == 1 && *filesystems)
		rc = 0;			/* /proc/filesystems not found */

	return rc;
}

/*
 * Returns an allocated string with username or NULL.
 */
char *mnt_get_username(const uid_t uid)
{
        struct passwd pwd;
	struct passwd *res;
	char *buf, *username = NULL;

	buf = malloc(16384);
	if (!buf)
		return NULL;

	if (!getpwuid_r(uid, &pwd, buf, 16384, &res) && res)
		username = strdup(pwd.pw_name);

	free(buf);
	return username;
}

int mnt_get_uid(const char *username, uid_t *uid)
{
	int rc = -1;
        struct passwd pwd;
	struct passwd *pw;
	char *buf;

	if (!username || !uid)
		return -EINVAL;

	buf = malloc(16384);
	if (!buf)
		return -ENOMEM;

	if (!getpwnam_r(username, &pwd, buf, 16384, &pw) && pw) {
		*uid= pw->pw_uid;
		rc = 0;
	} else {
		rc = errno ? -errno : -EINVAL;
	}

	free(buf);
	return rc;
}

int mnt_get_gid(const char *groupname, gid_t *gid)
{
	int rc = -1;
        struct group grp;
	struct group *gr;
	char *buf;

	if (!groupname || !gid)
		return -EINVAL;

	buf = malloc(16384);
	if (!buf)
		return -ENOMEM;

	if (!getgrnam_r(groupname, &grp, buf, 16384, &gr) && gr) {
		*gid= gr->gr_gid;
		rc = 0;
	} else {
		rc = errno ? -errno : -EINVAL;
	}

	free(buf);
	return rc;
}

int mnt_in_group(gid_t gid)
{
	int rc = 0, n, i;
	gid_t *grps = NULL;

	if (getgid() == gid)
		return 1;

	n = getgroups(0, NULL);
	if (n <= 0)
		goto done;

	grps = malloc(n * sizeof(*grps));
	if (!grps)
		goto done;

	if (getgroups(n, grps) == n) {
		for (i = 0; i < n; i++) {
			if (grps[i] == gid) {
				rc = 1;
				break;
			}
		}
	}
done:
	free(grps);
	return rc;
}

static int try_write(const char *filename, const char *directory)
{
	int rc = 0;

	if (!filename)
		return -EINVAL;

	/* Try eaccess() first, because open() is overkill, may be monitored by
	 * audit and we don't want to fill logs by our checks...
	 */
	if (eaccess(filename, R_OK|W_OK) == 0) {
		return 0;
	}

	if (errno != ENOENT && errno != ENOSYS) {
		return -errno;
	}

	if (directory) {
		/* file does not exist; try if directory is writable */
		if (eaccess(directory, R_OK|W_OK) != 0)
			rc = -errno;
		return rc;
	}

	int fd = open(filename, O_RDWR|O_CREAT|O_CLOEXEC,
		    S_IWUSR|S_IRUSR|S_IRGRP|S_IROTH);
	if (fd < 0)
		rc = -errno;
	else
		close(fd);

	return rc;
}

/**
 * mnt_has_regular_mtab:
 * @mtab: returns path to mtab
 * @writable: returns 1 if the file is writable
 *
 * If the file does not exist and @writable argument is not NULL, then it will
 * try to create the file.
 *
 * Returns: 1 if /etc/mtab is a regular file, and 0 in case of error (check
 *          errno for more details).
 */
int mnt_has_regular_mtab(const char **mtab, int *writable)
{
	struct stat st;
	int rc;
	const char *filename = mtab && *mtab ? *mtab : mnt_get_mtab_path();

	if (writable)
		*writable = 0;
	if (mtab && !*mtab)
		*mtab = filename;

	rc = lstat(filename, &st);

	if (rc == 0) {
		/* file exists */
		if (S_ISREG(st.st_mode)) {
			if (writable)
				*writable = !try_write(filename, NULL);
			return 1;
		}
		goto done;
	}

	/* try to create the file */
	if (writable) {
		*writable = !try_write(filename, NULL);
		if (*writable) {
			return 1;
		}
	}

done:
	return 0;
}

/*
 * Don't export this to libmount API -- utab is private library stuff.
 *
 * If the file does not exist and @writable argument is not NULL, then it will
 * try to create the directory (e.g. /run/mount) and the file.
 *
 * Returns: 1 if utab is a regular file, and 0 in case of
 *          error (check errno for more details).
 */
int mnt_has_regular_utab(const char **utab, int *writable)
{
	struct stat st;
	int rc;
	const char *filename = utab && *utab ? *utab : mnt_get_utab_path();

	if (writable)
		*writable = 0;
	if (utab && !*utab)
		*utab = filename;

	rc = lstat(filename, &st);

	if (rc == 0) {
		/* file exists */
		if (S_ISREG(st.st_mode)) {
			if (writable)
				*writable = !try_write(filename, NULL);
			return 1;
		}
		goto done;	/* it's not a regular file */
	}

	if (writable) {
		char *dirname = strdup(filename);

		if (!dirname)
			goto done;

		stripoff_last_component(dirname);	/* remove filename */

		rc = mkdir(dirname, S_IWUSR|
				    S_IRUSR|S_IRGRP|S_IROTH|
				    S_IXUSR|S_IXGRP|S_IXOTH);
		if (rc && errno != EEXIST) {
			free(dirname);
			goto done;			/* probably EACCES */
		}

		*writable = !try_write(filename, dirname);
		free(dirname);
		if (*writable)
			return 1;
	}
done:
	return 0;
}

/**
 * mnt_get_swaps_path:
 *
 * Returns: path to /proc/swaps or $LIBMOUNT_SWAPS.
 */
const char *mnt_get_swaps_path(void)
{
	const char *p = getenv("LIBMOUNT_SWAPS");
	return p ? : _PATH_PROC_SWAPS;
}

/**
 * mnt_get_fstab_path:
 *
 * Returns: path to /etc/fstab or $LIBMOUNT_FSTAB.
 */
const char *mnt_get_fstab_path(void)
{
	const char *p = getenv("LIBMOUNT_FSTAB");
	return p ? : _PATH_MNTTAB;
}

/**
 * mnt_get_mtab_path:
 *
 * This function returns the *default* location of the mtab file. The result does
 * not have to be writable. See also mnt_has_regular_mtab().
 *
 * Returns: path to /etc/mtab or $LIBMOUNT_MTAB.
 */
const char *mnt_get_mtab_path(void)
{
	const char *p = getenv("LIBMOUNT_MTAB");
	return p ? : _PATH_MOUNTED;
}

/*
 * Don't export this to libmount API -- utab is private library stuff.
 *
 * Returns: path to /run/mount/utab or $LIBMOUNT_UTAB.
 */
const char *mnt_get_utab_path(void)
{
	const char *p = getenv("LIBMOUNT_UTAB");
	return p ? : MNT_PATH_UTAB;
}


/* returns file descriptor or -errno, @name returns a unique filename
 */
int mnt_open_uniq_filename(const char *filename, char **name)
{
	int rc, fd;
	char *n;
	mode_t oldmode;

	if (!filename)
		return -EINVAL;
	if (name)
		*name = NULL;

	rc = asprintf(&n, "%s.XXXXXX", filename);
	if (rc <= 0)
		return -errno;

	/* This is for very old glibc and for compatibility with Posix, which says
	 * nothing about mkstemp() mode. All sane glibc use secure mode (0600).
	 */
	oldmode = umask(S_IRGRP|S_IWGRP|S_IXGRP|
			S_IROTH|S_IWOTH|S_IXOTH);

	fd = mkstemp_cloexec(n);
	if (fd < 0)
		fd = -errno;
	umask(oldmode);

	if (fd >= 0 && name)
		*name = n;
	else
		free(n);

	return fd;
}

/**
 * mnt_get_mountpoint:
 * @path: pathname
 *
 * This function finds the mountpoint that a given path resides in. @path
 * should be canonicalized. The returned pointer should be freed by the caller.
 *
 * WARNING: the function compares st_dev of the @path elements. This traditional
 * way may be insufficient on filesystems like Linux "overlay". See also
 * mnt_table_find_target().
 *
 * Returns: allocated string with the target of the mounted device or NULL on error
 */
char *mnt_get_mountpoint(const char *path)
{
	char *mnt;
	struct stat st;
	dev_t dir, base;

	if (!path)
		return NULL;

	mnt = strdup(path);
	if (!mnt)
		return NULL;
	if (*mnt == '/' && *(mnt + 1) == '\0')
		goto done;

	if (mnt_stat_mountpoint(mnt, &st))
		goto err;
	base = st.st_dev;

	do {
		char *p = stripoff_last_component(mnt);

		if (!p)
			break;
		if (mnt_stat_mountpoint(*mnt ? mnt : "/", &st))
			goto err;
		dir = st.st_dev;
		if (dir != base) {
			if (p > mnt)
				*(p - 1) = '/';
			goto done;
		}
		base = dir;
	} while (mnt && *(mnt + 1) != '\0');

	memcpy(mnt, "/", 2);
done:
	return mnt;
err:
	free(mnt);
	return NULL;
}

/*
 * Search for @name kernel command parameter.
 *
 * Returns newly allocated string with a parameter argument if the @name is
 * specified as "name=" or returns pointer to @name or returns NULL if not
 * found.  If it is specified more than once, we grab the last copy.
 *
 * For example cmdline: "aaa bbb=BBB ccc"
 *
 *	@name is "aaa"	--returns--> "aaa" (pointer to @name)
 *	@name is "bbb=" --returns--> "BBB" (allocated)
 *	@name is "foo"  --returns--> NULL
 *
 * Note: It is not really feasible to parse the command line exactly the same
 * as the kernel does since we don't know which options are valid.  We can use
 * the -- marker though and not walk past that.
 */
char *mnt_get_kernel_cmdline_option(const char *name)
{
	FILE *f;
	size_t len;
	int val = 0;
	char *p, *res = NULL, *mem = NULL;
	char buf[BUFSIZ];	/* see kernel include/asm-generic/setup.h: COMMAND_LINE_SIZE */
	const char *path = _PATH_PROC_CMDLINE;

	if (!name || !name[0])
		return NULL;

	f = fopen(path, "re");
	if (!f)
		return NULL;

	p = fgets(buf, sizeof(buf), f);
	fclose(f);

	if (!p || !*p || *p == '\n')
		return NULL;

	p = strstr(p, " -- ");
	if (p) {
		/* no more kernel args after this */
		*p = '\0';
	} else {
		len = strlen(buf);
		buf[len - 1] = '\0';	/* remove last '\n' */
	}

	len = strlen(name);
	if (name[len - 1] == '=')
		val = 1;

	for (p = buf; p && *p; p++) {
		if (!(p = strstr(p, name)))
			break;			/* not found the option */
		if (p != buf && !isblank(*(p - 1)))
			continue;		/* no space before the option */
		if (!val && *(p + len) != '\0' && !isblank(*(p + len)))
			continue;		/* no space after the option */
		if (val) {
			char *v = p + len;
			int end;

			while (*p && !isblank(*p))	/* jump to the end of the argument */
				p++;
			end = (*p == '\0');
			*p = '\0';
			free(mem);
			res = mem = strdup(v);
			if (end)
				break;
		} else
			res = (char *) name;	/* option without '=' */
		/* don't break -- keep scanning for more options */
	}

	return res;
}

/**
 * mnt_guess_system_root:
 * @devno: device number or zero
 * @cache: paths cache or NULL
 * @path: returns allocated path
 *
 * Converts @devno to the real device name if devno major number is greater
 * than zero, otherwise use root= kernel cmdline option to get device name.
 *
 * The function uses /sys to convert devno to device name.
 *
 * Returns: 0 = success, 1 = not found, <0 = error
 *
 * Since: 2.34
 */
int mnt_guess_system_root(dev_t devno, struct libmnt_cache *cache, char **path)
{
	char buf[PATH_MAX];
	char *dev = NULL, *spec = NULL;
	unsigned int x, y;
	int allocated = 0;

	/* The pseudo-fs, net-fs or btrfs devno is useless, otherwise it
	 * usually matches with the source device, let's try to use it.
	 */
	if (major(devno) > 0) {
		dev = sysfs_devno_to_devpath(devno, buf, sizeof(buf));
		if (dev) {
			goto done;
		}
	}

	/* Let's try to use root= kernel command line option
	 */
	spec = mnt_get_kernel_cmdline_option("root=");
	if (!spec)
		goto done;

	/* maj:min notation */
	if (sscanf(spec, "%u:%u", &x, &y) == 2) {
		dev = sysfs_devno_to_devpath(makedev(x, y), buf, sizeof(buf));
		if (dev) {
			goto done;
		}

	/* hexhex notation */
	} else if (isxdigit_string(spec)) {
		char *end = NULL;
		uint32_t n;

		errno = 0;
		n = strtoul(spec, &end, 16);

		if (!(errno != 0 || spec == end || (end && *end))) {
			/* kernel new_decode_dev() */
			x = (n & 0xfff00) >> 8;
			y = (n & 0xff) | ((n >> 12) & 0xfff00);
			dev = sysfs_devno_to_devpath(makedev(x, y), buf, sizeof(buf));
			if (dev) {
				goto done;
			}
		}

	/* devname or PARTUUID= etc. */
	} else {

		dev = mnt_resolve_spec(spec, cache);
		if (dev && !cache)
			allocated = 1;
	}
done:
	free(spec);
	if (dev) {
		*path = allocated ? dev : strdup(dev);
		if (!*path)
			return -ENOMEM;
		return 0;
	}

	return 1;
}

/*
 * Initialize MNT_PATH_TMPTGT; mkdir, create a new namespace and
 * mark (bind mount) the directory as private.
 */
int mnt_tmptgt_unshare(int *old_ns_fd)
{
	int rc = 0, fd = -1;

	*old_ns_fd = -1;

	/* remember the current namespace */
	fd = open("/proc/self/ns/mnt", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		goto fail;

	/* create new namespace */
	if (unshare(CLONE_NEWNS) != 0)
		goto fail;

	/* create directory */
	rc = ul_mkdir_p(MNT_PATH_TMPTGT, S_IRWXU);
	if (rc)
		goto fail;

	/* try to set top-level directory as private, this is possible if
	 * MNT_RUNTIME_TOPDIR (/run) is a separated filesystem. */
	if (mount("none", MNT_RUNTIME_TOPDIR, NULL, MS_PRIVATE, NULL) != 0) {

		/* failed; create a mountpoint from MNT_PATH_TMPTGT */
		if (mount(MNT_PATH_TMPTGT, MNT_PATH_TMPTGT, "none", MS_BIND, NULL) != 0)
			goto fail;
		if (mount("none", MNT_PATH_TMPTGT, NULL, MS_PRIVATE, NULL) != 0)
			goto fail;
	}

	*old_ns_fd = fd;
	return 0;
fail:
	if (rc == 0)
		rc = errno ? -errno : -EINVAL;

	mnt_tmptgt_cleanup(fd);
	return rc;
}

/*
 * Clean up MNT_PATH_TMPTGT; umount and switch back to old namespace
 */
int mnt_tmptgt_cleanup(int old_ns_fd)
{
	umount(MNT_PATH_TMPTGT);

	if (old_ns_fd >= 0) {
		setns(old_ns_fd, CLONE_NEWNS);
		close(old_ns_fd);
	}

	return 0;
}
