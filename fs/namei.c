// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/namei.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * Some corrections by tytso.
 */

/* [Feb 1997 T. Schoebel-Theuer] Complete rewrite of the pathname
 * lookup logic.
 */
/* [Feb-Apr 2000, AV] Rewrite to the new namespace architecture.
 */

#include "linux/dcache.h"
#include "linux/printk.h"
#include <linux/dbg.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/wordpart.h>
#include <linux/fs.h>
#include <linux/filelock.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/sched/mm.h>
#include <linux/fsnotify.h>
#include <linux/personality.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/mount.h>
#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/device_cgroup.h>
#include <linux/fs_struct.h>
#include <linux/posix_acl.h>
#include <linux/hash.h>
#include <linux/bitops.h>
#include <linux/init_task.h>
#include <linux/uaccess.h>

#include <asm/runtime-const.h>

#include "internal.h"
#include "mount.h"

/* [Feb-1997 T. Schoebel-Theuer]
 * Fundamental changes in the pathname lookup mechanisms (namei)
 * were necessary because of omirr.  The reason is that omirr needs
 * to know the _real_ pathname, not the user-supplied one, in case
 * of symlinks (and also when transname replacements occur).
 *
 * The new code replaces the old recursive symlink resolution with
 * an iterative one (in case of non-nested symlink chains).  It does
 * this with calls to <fs>_follow_link().
 * As a side effect, dir_namei(), _namei() and follow_link() are now 
 * replaced with a single function lookup_dentry() that can handle all 
 * the special cases of the former code.
 *
 * With the new dcache, the pathname is stored at each inode, at least as
 * long as the refcount of the inode is positive.  As a side effect, the
 * size of the dcache depends on the inode cache and thus is dynamic.
 *
 * [29-Apr-1998 C. Scott Ananian] Updated above description of symlink
 * resolution to correspond with current state of the code.
 *
 * Note that the symlink resolution is not *completely* iterative.
 * There is still a significant amount of tail- and mid- recursion in
 * the algorithm.  Also, note that <fs>_readlink() is not used in
 * lookup_dentry(): lookup_dentry() on the result of <fs>_readlink()
 * may return different results than <fs>_follow_link().  Many virtual
 * filesystems (including /proc) exhibit this behavior.
 */

/* [24-Feb-97 T. Schoebel-Theuer] Side effects caused by new implementation:
 * New symlink semantics: when open() is called with flags O_CREAT | O_EXCL
 * and the name already exists in form of a symlink, try to create the new
 * name indicated by the symlink. The old code always complained that the
 * name already exists, due to not following the symlink even if its target
 * is nonexistent.  The new semantics affects also mknod() and link() when
 * the name is a symlink pointing to a non-existent name.
 *
 * I don't know which semantics is the right one, since I have no access
 * to standards. But I found by trial that HP-UX 9.0 has the full "new"
 * semantics implemented, while SunOS 4.1.1 and Solaris (SunOS 5.4) have the
 * "old" one. Personally, I think the new semantics is much more logical.
 * Note that "ln old new" where "new" is a symlink pointing to a non-existing
 * file does succeed in both HP-UX and SunOs, but not in Solaris
 * and in the old Linux semantics.
 */

/* [16-Dec-97 Kevin Buhr] For security reasons, we change some symlink
 * semantics.  See the comments in "open_namei" and "do_link" below.
 *
 * [10-Sep-98 Alan Modra] Another symlink change.
 */

/* [Feb-Apr 2000 AV] Complete rewrite. Rules for symlinks:
 *	inside the path - always follow.
 *	in the last component in creation/removal/renaming - never follow.
 *	if LOOKUP_FOLLOW passed - follow.
 *	if the pathname has trailing slashes - follow.
 *	otherwise - don't follow.
 * (applied in that order).
 *
 * [Jun 2000 AV] Inconsistent behaviour of open() in case if flags==O_CREAT
 * restored for 2.4. This is the last surviving part of old 4.2BSD bug.
 * During the 2.4 we need to fix the userland stuff depending on it -
 * hopefully we will be able to get rid of that wart in 2.5. So far only
 * XEmacs seems to be relying on it...
 */
/*
 * [Sep 2001 AV] Single-semaphore locking scheme (kudos to David Holland)
 * implemented.  Let's see if raised priority of ->s_vfs_rename_mutex gives
 * any extra contention...
 */

/* In order to reduce some races, while at the same time doing additional
 * checking and hopefully speeding things up, we copy filenames to the
 * kernel data space before using them..
 *
 * POSIX.1 2.4: an empty pathname is invalid (ENOENT).
 * PATH_MAX includes the nul terminator --RR.
 */

/* SLAB cache for struct filename instances */
static struct kmem_cache *__names_cache __ro_after_init;
#define names_cache	runtime_const_ptr(__names_cache)

/*
 * Type of the last component on LOOKUP_PARENT
 */
enum last_type {LAST_NORM, LAST_ROOT, LAST_DOT, LAST_DOTDOT};

void __init filename_init(void)
{
	__names_cache = kmem_cache_create_usercopy("names_cache", sizeof(struct filename), 0,
			 SLAB_HWCACHE_ALIGN|SLAB_PANIC, offsetof(struct filename, iname),
			 EMBEDDED_NAME_MAX, NULL);
	runtime_const_init(ptr, __names_cache);
}

static inline struct filename *alloc_filename(void)
{
	return kmem_cache_alloc(names_cache, GFP_KERNEL);
}

static inline void free_filename(struct filename *p)
{
	kmem_cache_free(names_cache, p);
}

static inline void initname(struct filename *name)
{
	name->aname = NULL;
	name->refcnt = 1;
}

static int getname_long(struct filename *name, const char __user *filename)
{
	int len;
	char *p __free(kfree) = kmalloc(PATH_MAX, GFP_KERNEL);
	if (unlikely(!p))
		return -ENOMEM;

	memcpy(p, &name->iname, EMBEDDED_NAME_MAX);
	len = strncpy_from_user(p + EMBEDDED_NAME_MAX,
				filename + EMBEDDED_NAME_MAX,
				PATH_MAX - EMBEDDED_NAME_MAX);
	if (unlikely(len < 0))
		return len;
	if (unlikely(len == PATH_MAX - EMBEDDED_NAME_MAX))
		return -ENAMETOOLONG;
	name->name = no_free_ptr(p);
	return 0;
}

static struct filename *
do_getname(const char __user *filename, int flags, bool incomplete)
{
	struct filename *result;
	char *kname;
	int len;

	result = alloc_filename();
	if (unlikely(!result))
		return ERR_PTR(-ENOMEM);

	/*
	 * First, try to embed the struct filename inside the names_cache
	 * allocation
	 */
	kname = (char *)result->iname;
	result->name = kname;

	len = strncpy_from_user(kname, filename, EMBEDDED_NAME_MAX);
	/*
	 * Handle both empty path and copy failure in one go.
	 */
	if (unlikely(len <= 0)) {
		/* The empty path is special. */
		if (!len && !(flags & LOOKUP_EMPTY))
			len = -ENOENT;
	}

	/*
	 * Uh-oh. We have a name that's approaching PATH_MAX. Allocate a
	 * separate struct filename so we can dedicate the entire
	 * names_cache allocation for the pathname, and re-do the copy from
	 * userland.
	 */
	if (unlikely(len == EMBEDDED_NAME_MAX))
		len = getname_long(result, filename);
	if (unlikely(len < 0)) {
		free_filename(result);
		return ERR_PTR(len);
	}

	initname(result);
	if (likely(!incomplete))
		audit_getname(result);
	return result;
}

struct filename *
getname_flags(const char __user *filename, int flags)
{
	return do_getname(filename, flags, false);
}

struct filename *getname_uflags(const char __user *filename, int uflags)
{
	int flags = (uflags & AT_EMPTY_PATH) ? LOOKUP_EMPTY : 0;

	return getname_flags(filename, flags);
}

struct filename *__getname_maybe_null(const char __user *pathname)
{
	char c;

	/* try to save on allocations; loss on um, though */
	if (get_user(c, pathname))
		return ERR_PTR(-EFAULT);
	if (!c)
		return NULL;

	CLASS(filename_flags, name)(pathname, LOOKUP_EMPTY);
	/* empty pathname translates to NULL */
	if (!IS_ERR(name) && !(name->name[0]))
		return NULL;
	return no_free_ptr(name);
}

static struct filename *do_getname_kernel(const char *filename, bool incomplete)
{
	struct filename *result;
	int len = strlen(filename) + 1;
	char *p;

	if (unlikely(len > PATH_MAX))
		return ERR_PTR(-ENAMETOOLONG);

	result = alloc_filename();
	if (unlikely(!result))
		return ERR_PTR(-ENOMEM);

	if (len <= EMBEDDED_NAME_MAX) {
		p = (char *)result->iname;
		memcpy(p, filename, len);
	} else {
		p = kmemdup(filename, len, GFP_KERNEL);
		if (unlikely(!p)) {
			free_filename(result);
			return ERR_PTR(-ENOMEM);
		}
	}
	result->name = p;
	initname(result);
	if (likely(!incomplete))
		audit_getname(result);
	return result;
}

struct filename *getname_kernel(const char *filename)
{
	return do_getname_kernel(filename, false);
}
EXPORT_SYMBOL(getname_kernel);

void putname(struct filename *name)
{
	int refcnt;

	if (IS_ERR_OR_NULL(name))
		return;

	refcnt = name->refcnt;
	if (unlikely(refcnt != 1)) {
		if (WARN_ON_ONCE(!refcnt))
			return;

		name->refcnt--;
		return;
	}

	if (unlikely(name->name != name->iname))
		kfree(name->name);
	free_filename(name);
}
EXPORT_SYMBOL(putname);

static inline int __delayed_getname(struct delayed_filename *v,
			   const char __user *string, int flags)
{
	v->__incomplete_filename = do_getname(string, flags, true);
	return PTR_ERR_OR_ZERO(v->__incomplete_filename);
}

int delayed_getname(struct delayed_filename *v, const char __user *string)
{
	return __delayed_getname(v, string, 0);
}

int delayed_getname_uflags(struct delayed_filename *v, const char __user *string,
			 int uflags)
{
	int flags = (uflags & AT_EMPTY_PATH) ? LOOKUP_EMPTY : 0;
	return __delayed_getname(v, string, flags);
}

int putname_to_delayed(struct delayed_filename *v, struct filename *name)
{
	if (likely(name->refcnt == 1)) {
		v->__incomplete_filename = name;
		return 0;
	}
	name->refcnt--;
	v->__incomplete_filename = do_getname_kernel(name->name, true);
	return PTR_ERR_OR_ZERO(v->__incomplete_filename);
}

void dismiss_delayed_filename(struct delayed_filename *v)
{
	putname(no_free_ptr(v->__incomplete_filename));
}

struct filename *complete_getname(struct delayed_filename *v)
{
	struct filename *res = no_free_ptr(v->__incomplete_filename);
	if (!IS_ERR(res))
		audit_getname(res);
	return res;
}

/**
 * check_acl - perform ACL permission checking
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	inode to check permissions on
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC ...)
 *
 * This function performs the ACL permission checking. Since this function
 * retrieve POSIX acls it needs to know whether it is called from a blocking or
 * non-blocking context and thus cares about the MAY_NOT_BLOCK bit.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
static int check_acl(struct mnt_idmap *idmap,
		     struct inode *inode, int mask)
{
#ifdef CONFIG_FS_POSIX_ACL
	struct posix_acl *acl;

	if (mask & MAY_NOT_BLOCK) {
		acl = get_cached_acl_rcu(inode, ACL_TYPE_ACCESS);
	        if (!acl)
	                return -EAGAIN;
		/* no ->get_inode_acl() calls in RCU mode... */
		if (is_uncached_acl(acl))
			return -ECHILD;
	        return posix_acl_permission(idmap, inode, acl, mask);
	}

	acl = get_inode_acl(inode, ACL_TYPE_ACCESS);
	if (IS_ERR(acl))
		return PTR_ERR(acl);
	if (acl) {
	        int error = posix_acl_permission(idmap, inode, acl, mask);
	        posix_acl_release(acl);
	        return error;
	}
#endif

	return -EAGAIN;
}

/*
 * Very quick optimistic "we know we have no ACL's" check.
 *
 * Note that this is purely for ACL_TYPE_ACCESS, and purely
 * for the "we have cached that there are no ACLs" case.
 *
 * If this returns true, we know there are no ACLs. But if
 * it returns false, we might still not have ACLs (it could
 * be the is_uncached_acl() case).
 */
static inline bool no_acl_inode(struct inode *inode)
{
#ifdef CONFIG_FS_POSIX_ACL
	return likely(!READ_ONCE(inode->i_acl));
#else
	return true;
#endif
}

/**
 * acl_permission_check - perform basic UNIX permission checking
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	inode to check permissions on
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC ...)
 *
 * This function performs the basic UNIX permission checking. Since this
 * function may retrieve POSIX acls it needs to know whether it is called from a
 * blocking or non-blocking context and thus cares about the MAY_NOT_BLOCK bit.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
static int acl_permission_check(struct mnt_idmap *idmap,
				struct inode *inode, int mask)
{
	unsigned int mode = inode->i_mode;
	vfsuid_t vfsuid;

	/*
	 * Common cheap case: everybody has the requested
	 * rights, and there are no ACLs to check. No need
	 * to do any owner/group checks in that case.
	 *
	 *  - 'mask&7' is the requested permission bit set
	 *  - multiplying by 0111 spreads them out to all of ugo
	 *  - '& ~mode' looks for missing inode permission bits
	 *  - the '!' is for "no missing permissions"
	 *
	 * After that, we just need to check that there are no
	 * ACL's on the inode - do the 'IS_POSIXACL()' check last
	 * because it will dereference the ->i_sb pointer and we
	 * want to avoid that if at all possible.
	 */
	if (!((mask & 7) * 0111 & ~mode)) {
		if (no_acl_inode(inode))
			return 0;
		if (!IS_POSIXACL(inode))
			return 0;
	}

	/* Are we the owner? If so, ACL's don't matter */
	vfsuid = i_uid_into_vfsuid(idmap, inode);
	if (likely(vfsuid_eq_kuid(vfsuid, current_fsuid()))) {
		mask &= 7;
		mode >>= 6;
		return (mask & ~mode) ? -EACCES : 0;
	}

	/* Do we have ACL's? */
	if (IS_POSIXACL(inode) && (mode & S_IRWXG)) {
		int error = check_acl(idmap, inode, mask);
		if (error != -EAGAIN)
			return error;
	}

	/* Only RWX matters for group/other mode bits */
	mask &= 7;

	/*
	 * Are the group permissions different from
	 * the other permissions in the bits we care
	 * about? Need to check group ownership if so.
	 */
	if (mask & (mode ^ (mode >> 3))) {
		vfsgid_t vfsgid = i_gid_into_vfsgid(idmap, inode);
		if (vfsgid_in_group_p(vfsgid))
			mode >>= 3;
	}

	/* Bits in 'mode' clear that we require? */
	return (mask & ~mode) ? -EACCES : 0;
}

/**
 * generic_permission -  check for access rights on a Posix-like filesystem
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	inode to check access rights for
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC,
 *		%MAY_NOT_BLOCK ...)
 *
 * Used to check for read/write/execute permissions on a file.
 * We use "fsuid" for this, letting us set arbitrary permissions
 * for filesystem access without changing the "normal" uids which
 * are used for other things.
 *
 * generic_permission is rcu-walk aware. It returns -ECHILD in case an rcu-walk
 * request cannot be satisfied (eg. requires blocking or too much complexity).
 * It would then be called again in ref-walk mode.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int generic_permission(struct mnt_idmap *idmap, struct inode *inode,
		       int mask)
{
	int ret;

	/*
	 * Do the basic permission checks.
	 */
	ret = acl_permission_check(idmap, inode, mask);
	if (ret != -EACCES)
		return ret;

	if (S_ISDIR(inode->i_mode)) {
		/* DACs are overridable for directories */
		if (!(mask & MAY_WRITE))
			if (capable_wrt_inode_uidgid(idmap, inode,
						     CAP_DAC_READ_SEARCH))
				return 0;
		if (capable_wrt_inode_uidgid(idmap, inode,
					     CAP_DAC_OVERRIDE))
			return 0;
		return -EACCES;
	}

	/*
	 * Searching includes executable on directories, else just read.
	 */
	mask &= MAY_READ | MAY_WRITE | MAY_EXEC;
	if (mask == MAY_READ)
		if (capable_wrt_inode_uidgid(idmap, inode,
					     CAP_DAC_READ_SEARCH))
			return 0;
	/*
	 * Read/write DACs are always overridable.
	 * Executable DACs are overridable when there is
	 * at least one exec bit set.
	 */
	if (!(mask & MAY_EXEC) || (inode->i_mode & S_IXUGO))
		if (capable_wrt_inode_uidgid(idmap, inode,
					     CAP_DAC_OVERRIDE))
			return 0;

	return -EACCES;
}
EXPORT_SYMBOL(generic_permission);

/**
 * do_inode_permission - UNIX permission checking
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	inode to check permissions on
 * @mask:	right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC ...)
 *
 * We _really_ want to just do "generic_permission()" without
 * even looking at the inode->i_op values. So we keep a cache
 * flag in inode->i_opflags, that says "this has not special
 * permission function, use the fast case".
 */
static inline int do_inode_permission(struct mnt_idmap *idmap,
				      struct inode *inode, int mask)
{
	if (unlikely(!(inode->i_opflags & IOP_FASTPERM))) {
		if (likely(inode->i_op->permission))
			return inode->i_op->permission(idmap, inode, mask);

		/* This gets set once for the inode lifetime */
		spin_lock(&inode->i_lock);
		inode->i_opflags |= IOP_FASTPERM;
		spin_unlock(&inode->i_lock);
	}
	return generic_permission(idmap, inode, mask);
}

/**
 * sb_permission - Check superblock-level permissions
 * @sb: Superblock of inode to check permission on
 * @inode: Inode to check permission on
 * @mask: Right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * Separate out file-system wide checks from inode-specific permission checks.
 *
 * Note: lookup_inode_permission_may_exec() does not call here. If you add
 * MAY_EXEC checks, adjust it.
 */
static int sb_permission(struct super_block *sb, struct inode *inode, int mask)
{
	if (mask & MAY_WRITE) {
		umode_t mode = inode->i_mode;

		/* Nobody gets write access to a read-only fs. */
		if (sb_rdonly(sb) && (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)))
			return -EROFS;
	}
	return 0;
}

/**
 * inode_permission - Check for access rights to a given inode
 * @idmap:	idmap of the mount the inode was found from
 * @inode:	Inode to check permission on
 * @mask:	Right to check for (%MAY_READ, %MAY_WRITE, %MAY_EXEC)
 *
 * Check for read/write/execute permissions on an inode.  We use fs[ug]id for
 * this, letting us set arbitrary permissions for filesystem access without
 * changing the "normal" UIDs which are used for other things.
 *
 * When checking for MAY_APPEND, MAY_WRITE must also be set in @mask.
 */
int inode_permission(struct mnt_idmap *idmap,
		     struct inode *inode, int mask)
{
	int retval;

	retval = sb_permission(inode->i_sb, inode, mask);
	if (unlikely(retval))
		return retval;

	if (mask & MAY_WRITE) {
		/*
		 * Nobody gets write access to an immutable file.
		 */
		if (unlikely(IS_IMMUTABLE(inode)))
			return -EPERM;

		/*
		 * Updating mtime will likely cause i_uid and i_gid to be
		 * written back improperly if their true value is unknown
		 * to the vfs.
		 */
		if (unlikely(HAS_UNMAPPED_ID(idmap, inode)))
			return -EACCES;
	}

	retval = do_inode_permission(idmap, inode, mask);
	if (unlikely(retval))
		return retval;

	retval = devcgroup_inode_permission(inode, mask);
	if (unlikely(retval))
		return retval;

	return security_inode_permission(inode, mask);
}
EXPORT_SYMBOL(inode_permission);

/*
 * lookup_inode_permission_may_exec - Check traversal right for given inode
 *
 * This is a special case routine for may_lookup() making assumptions specific
 * to path traversal. Use inode_permission() if you are doing something else.
 *
 * Work is shaved off compared to inode_permission() as follows:
 * - we know for a fact there is no MAY_WRITE to worry about
 * - it is an invariant the inode is a directory
 *
 * Since majority of real-world traversal happens on inodes which grant it for
 * everyone, we check it upfront and only resort to more expensive work if it
 * fails.
 *
 * Filesystems which have their own ->permission hook and consequently miss out
 * on IOP_FASTPERM can still get the optimization if they set IOP_FASTPERM_MAY_EXEC
 * on their directory inodes.
 */
static __always_inline int lookup_inode_permission_may_exec(struct mnt_idmap *idmap,
	struct inode *inode, int mask)
{
	/* Lookup already checked this to return -ENOTDIR */
	VFS_BUG_ON_INODE(!S_ISDIR(inode->i_mode), inode);
	VFS_BUG_ON((mask & ~MAY_NOT_BLOCK) != 0);

	mask |= MAY_EXEC;

	if (unlikely(!(inode->i_opflags & (IOP_FASTPERM | IOP_FASTPERM_MAY_EXEC))))
		return inode_permission(idmap, inode, mask);

	if (unlikely(((inode->i_mode & 0111) != 0111) || !no_acl_inode(inode)))
		return inode_permission(idmap, inode, mask);

	return security_inode_permission(inode, mask);
}

/**
 * path_get - get a reference to a path
 * @path: path to get the reference to
 *
 * Given a path increment the reference count to the dentry and the vfsmount.
 */
void path_get(const struct path *path)
{
	mntget(path->mnt);
	dget(path->dentry);
}
EXPORT_SYMBOL(path_get);

/**
 * path_put - put a reference to a path
 * @path: path to put the reference to
 *
 * Given a path decrement the reference count to the dentry and the vfsmount.
 */
void path_put(const struct path *path)
{
	dput(path->dentry);
	mntput(path->mnt);
}
EXPORT_SYMBOL(path_put);

#define EMBEDDED_LEVELS 2
struct nameidata {
	struct path	path;
	struct qstr	last;
	struct path	root;
	struct inode	*inode; /* path.dentry.d_inode */
	unsigned int	flags, state;
	unsigned	seq, next_seq, m_seq, r_seq;
	enum last_type	last_type;
	unsigned	depth;
	int		total_link_count;
	struct saved {
		struct path link;
		struct delayed_call done;
		const char *name;
		unsigned seq;
	} *stack, internal[EMBEDDED_LEVELS];
	struct filename	*name;
	const char *pathname;
	struct nameidata *saved;
	unsigned	root_seq;
	int		dfd;
	vfsuid_t	dir_vfsuid;
	umode_t		dir_mode;
} __randomize_layout;

#define ND_ROOT_PRESET 1
#define ND_ROOT_GRABBED 2
#define ND_JUMPED 4

static void __set_nameidata(struct nameidata *p, int dfd, struct filename *name)
{
	struct nameidata *old = current->nameidata;
	p->stack = p->internal;
	p->depth = 0;
	p->dfd = dfd;
	p->name = name;
	p->pathname = likely(name) ? name->name : "";
	p->path.mnt = NULL;
	p->path.dentry = NULL;
	p->total_link_count = old ? old->total_link_count : 0;
	p->saved = old;
	current->nameidata = p;
}

/**
 * 保存当前查找上下文，目的是遇到相对路径的符号链接，可以返回到之前的查找路径，
 * 如果只有一个全局的nameidata，那么在遇到相对路径的符号链接时，无法返回到之前的查找路径。
 */
static inline void set_nameidata(struct nameidata *p, int dfd, struct filename *name,
			  const struct path *root)
{
	__set_nameidata(p, dfd, name);
	p->state = 0;
	if (unlikely(root)) {
		p->state = ND_ROOT_PRESET;
		p->root = *root;
	}
}

static void restore_nameidata(void)
{
	struct nameidata *now = current->nameidata, *old = now->saved;

	current->nameidata = old;
	if (old)
		old->total_link_count = now->total_link_count;
	if (now->stack != now->internal)
		kfree(now->stack);
}

static bool nd_alloc_stack(struct nameidata *nd)
{
	struct saved *p;

	p= kmalloc_objs(struct saved, MAXSYMLINKS,
			nd->flags & LOOKUP_RCU ? GFP_ATOMIC : GFP_KERNEL);
	if (unlikely(!p))
		return false;
	memcpy(p, nd->internal, sizeof(nd->internal));
	nd->stack = p;
	return true;
}

/**
 * path_connected - Verify that a dentry is below mnt.mnt_root
 * @mnt: The mountpoint to check.
 * @dentry: The dentry to check.
 *
 * Rename can sometimes move a file or directory outside of a bind
 * mount, path_connected allows those cases to be detected.
 */
static bool path_connected(struct vfsmount *mnt, struct dentry *dentry)
{
	struct super_block *sb = mnt->mnt_sb;

	/* Bind mounts can have disconnected paths */
	if (mnt->mnt_root == sb->s_root)
		return true;

	return is_subdir(dentry, mnt->mnt_root);
}

static void drop_links(struct nameidata *nd)
{
	int i = nd->depth;
	while (i--) {
		struct saved *last = nd->stack + i;
		do_delayed_call(&last->done);
		clear_delayed_call(&last->done);
	}
}

static void leave_rcu(struct nameidata *nd)
{
	nd->flags &= ~LOOKUP_RCU;
	nd->seq = nd->next_seq = 0;
	rcu_read_unlock();
}

static void terminate_walk(struct nameidata *nd)
{
	if (unlikely(nd->depth))
		drop_links(nd);
	if (!(nd->flags & LOOKUP_RCU)) {
		int i;
		path_put(&nd->path);
		for (i = 0; i < nd->depth; i++)
			path_put(&nd->stack[i].link);
		if (nd->state & ND_ROOT_GRABBED) {
			path_put(&nd->root);
			nd->state &= ~ND_ROOT_GRABBED;
		}
	} else {
		leave_rcu(nd);
	}
	nd->depth = 0;
	nd->path.mnt = NULL;
	nd->path.dentry = NULL;
}

/* path_put is needed afterwards regardless of success or failure */
static bool __legitimize_path(struct path *path, unsigned seq, unsigned mseq)
{
	int res = __legitimize_mnt(path->mnt, mseq);
	if (unlikely(res)) {
		if (res > 0)
			path->mnt = NULL;
		path->dentry = NULL;
		return false;
	}
	if (unlikely(!lockref_get_not_dead(&path->dentry->d_lockref))) {
		path->dentry = NULL;
		return false;
	}
	return !read_seqcount_retry(&path->dentry->d_seq, seq);
}

static inline bool legitimize_path(struct nameidata *nd,
			    struct path *path, unsigned seq)
{
	return __legitimize_path(path, seq, nd->m_seq);
}

static bool legitimize_links(struct nameidata *nd)
{
	int i;

	VFS_BUG_ON(nd->flags & LOOKUP_CACHED);

	for (i = 0; i < nd->depth; i++) {
		struct saved *last = nd->stack + i;
		if (unlikely(!legitimize_path(nd, &last->link, last->seq))) {
			drop_links(nd);
			nd->depth = i + 1;
			return false;
		}
	}
	return true;
}

static bool legitimize_root(struct nameidata *nd)
{
	/* Nothing to do if nd->root is zero or is managed by the VFS user. */
	if (!nd->root.mnt || (nd->state & ND_ROOT_PRESET))
		return true;
	nd->state |= ND_ROOT_GRABBED;
	return legitimize_path(nd, &nd->root, nd->root_seq);
}

/*
 * Path walking has 2 modes, rcu-walk and ref-walk (see
 * Documentation/filesystems/path-lookup.txt).  In situations when we can't
 * continue in RCU mode, we attempt to drop out of rcu-walk mode and grab
 * normal reference counts on dentries and vfsmounts to transition to ref-walk
 * mode.  Refcounts are grabbed at the last known good point before rcu-walk
 * got stuck, so ref-walk may continue from there. If this is not successful
 * (eg. a seqcount has changed), then failure is returned and it's up to caller
 * to restart the path walk from the beginning in ref-walk mode.
 */

/**
 * try_to_unlazy - try to switch to ref-walk mode.
 * @nd: nameidata pathwalk data
 * Returns: true on success, false on failure
 *
 * try_to_unlazy attempts to legitimize the current nd->path and nd->root
 * for ref-walk mode.
 * Must be called from rcu-walk context.
 * Nothing should touch nameidata between try_to_unlazy() failure and
 * terminate_walk().
 */
static bool try_to_unlazy(struct nameidata *nd)
{
	struct dentry *parent = nd->path.dentry;

	VFS_BUG_ON(!(nd->flags & LOOKUP_RCU));

	if (unlikely(nd->flags & LOOKUP_CACHED)) {
		drop_links(nd);
		nd->depth = 0;
		goto out1;
	}
	if (unlikely(nd->depth && !legitimize_links(nd)))
		goto out1;
	if (unlikely(!legitimize_path(nd, &nd->path, nd->seq)))
		goto out;
	if (unlikely(!legitimize_root(nd)))
		goto out;
	leave_rcu(nd);
	BUG_ON(nd->inode != parent->d_inode);
	return true;

out1:
	nd->path.mnt = NULL;
	nd->path.dentry = NULL;
out:
	leave_rcu(nd);
	return false;
}

/**
 * try_to_unlazy_next - try to switch to ref-walk mode.
 * @nd: nameidata pathwalk data
 * @dentry: next dentry to step into
 * Returns: true on success, false on failure
 *
 * Similar to try_to_unlazy(), but here we have the next dentry already
 * picked by rcu-walk and want to legitimize that in addition to the current
 * nd->path and nd->root for ref-walk mode.  Must be called from rcu-walk context.
 * Nothing should touch nameidata between try_to_unlazy_next() failure and
 * terminate_walk().
 */
static bool try_to_unlazy_next(struct nameidata *nd, struct dentry *dentry)
{
	int res;

	VFS_BUG_ON(!(nd->flags & LOOKUP_RCU));

	if (unlikely(nd->flags & LOOKUP_CACHED)) {
		drop_links(nd);
		nd->depth = 0;
		goto out2;
	}
	if (unlikely(nd->depth && !legitimize_links(nd)))
		goto out2;
	res = __legitimize_mnt(nd->path.mnt, nd->m_seq);
	if (unlikely(res)) {
		if (res > 0)
			goto out2;
		goto out1;
	}
	if (unlikely(!lockref_get_not_dead(&nd->path.dentry->d_lockref)))
		goto out1;

	/*
	 * We need to move both the parent and the dentry from the RCU domain
	 * to be properly refcounted. And the sequence number in the dentry
	 * validates *both* dentry counters, since we checked the sequence
	 * number of the parent after we got the child sequence number. So we
	 * know the parent must still be valid if the child sequence number is
	 */
	if (unlikely(!lockref_get_not_dead(&dentry->d_lockref)))
		goto out;
	if (read_seqcount_retry(&dentry->d_seq, nd->next_seq))
		goto out_dput;
	/*
	 * Sequence counts matched. Now make sure that the root is
	 * still valid and get it if required.
	 */
	if (unlikely(!legitimize_root(nd)))
		goto out_dput;
	leave_rcu(nd);
	return true;

out2:
	nd->path.mnt = NULL;
out1:
	nd->path.dentry = NULL;
out:
	leave_rcu(nd);
	return false;
out_dput:
	leave_rcu(nd);
	dput(dentry);
	return false;
}

static inline int d_revalidate(struct inode *dir, const struct qstr *name,
			       struct dentry *dentry, unsigned int flags)
{
	if (unlikely(dentry->d_flags & DCACHE_OP_REVALIDATE))
		return dentry->d_op->d_revalidate(dir, name, dentry, flags);
	else
		return 1;
}

/**
 * complete_walk - successful completion of path walk
 * @nd:  pointer nameidata
 *
 * If we had been in RCU mode, drop out of it and legitimize nd->path.
 * Revalidate the final result, unless we'd already done that during
 * the path walk or the filesystem doesn't ask for it.  Return 0 on
 * success, -error on failure.  In case of failure caller does not
 * need to drop nd->path.
 */
static int complete_walk(struct nameidata *nd)
{
	struct dentry *dentry = nd->path.dentry;
	int status;

	if (nd->flags & LOOKUP_RCU) {
		/*
		 * We don't want to zero nd->root for scoped-lookups or
		 * externally-managed nd->root.
		 */
		if (likely(!(nd->state & ND_ROOT_PRESET)))
			if (likely(!(nd->flags & LOOKUP_IS_SCOPED)))
				nd->root.mnt = NULL;
		nd->flags &= ~LOOKUP_CACHED;
		if (!try_to_unlazy(nd))
			return -ECHILD;
	}

	if (unlikely(nd->flags & LOOKUP_IS_SCOPED)) {
		/*
		 * While the guarantee of LOOKUP_IS_SCOPED is (roughly) "don't
		 * ever step outside the root during lookup" and should already
		 * be guaranteed by the rest of namei, we want to avoid a namei
		 * BUG resulting in userspace being given a path that was not
		 * scoped within the root at some point during the lookup.
		 *
		 * So, do a final sanity-check to make sure that in the
		 * worst-case scenario (a complete bypass of LOOKUP_IS_SCOPED)
		 * we won't silently return an fd completely outside of the
		 * requested root to userspace.
		 *
		 * Userspace could move the path outside the root after this
		 * check, but as discussed elsewhere this is not a concern (the
		 * resolved file was inside the root at some point).
		 */
		if (!path_is_under(&nd->path, &nd->root))
			return -EXDEV;
	}

	if (likely(!(nd->state & ND_JUMPED)))
		return 0;

	if (likely(!(dentry->d_flags & DCACHE_OP_WEAK_REVALIDATE)))
		return 0;

	status = dentry->d_op->d_weak_revalidate(dentry, nd->flags);
	if (status > 0)
		return 0;

	if (!status)
		status = -ESTALE;

	return status;
}

static int set_root(struct nameidata *nd)
{
	struct fs_struct *fs = current->fs;

	/*
	 * Jumping to the real root in a scoped-lookup is a BUG in namei, but we
	 * still have to ensure it doesn't happen because it will cause a breakout
	 * from the dirfd.
	 */
	if (WARN_ON(nd->flags & LOOKUP_IS_SCOPED))
		return -ENOTRECOVERABLE;

	if (nd->flags & LOOKUP_RCU) {
		unsigned seq;

		do {
			seq = read_seqbegin(&fs->seq);
			nd->root = fs->root;
			nd->root_seq = __read_seqcount_begin(&nd->root.dentry->d_seq);
		} while (read_seqretry(&fs->seq, seq));
	} else {
		get_fs_root(fs, &nd->root);
		nd->state |= ND_ROOT_GRABBED;
	}
	return 0;
}

static int nd_jump_root(struct nameidata *nd)
{
	if (unlikely(nd->flags & LOOKUP_BENEATH))
		return -EXDEV;
	if (unlikely(nd->flags & LOOKUP_NO_XDEV)) {
		/* Absolute path arguments to path_init() are allowed. */
		if (nd->path.mnt != NULL && nd->path.mnt != nd->root.mnt)
			return -EXDEV;
	}
	if (!nd->root.mnt) {
		int error = set_root(nd);
		if (unlikely(error))
			return error;
	}
	if (nd->flags & LOOKUP_RCU) {
		struct dentry *d;
		nd->path = nd->root;
		d = nd->path.dentry;
		nd->inode = d->d_inode;
		nd->seq = nd->root_seq;
		if (read_seqcount_retry(&d->d_seq, nd->seq))
			return -ECHILD;
	} else {
		path_put(&nd->path);
		nd->path = nd->root;
		path_get(&nd->path);
		nd->inode = nd->path.dentry->d_inode;
	}
	nd->state |= ND_JUMPED;
	return 0;
}

/*
 * Helper to directly jump to a known parsed path from ->get_link,
 * caller must have taken a reference to path beforehand.
 */
int nd_jump_link(const struct path *path)
{
	int error = -ELOOP;
	struct nameidata *nd = current->nameidata;

	if (unlikely(nd->flags & LOOKUP_NO_MAGICLINKS))
		goto err;

	error = -EXDEV;
	if (unlikely(nd->flags & LOOKUP_NO_XDEV)) {
		if (nd->path.mnt != path->mnt)
			goto err;
	}
	/* Not currently safe for scoped-lookups. */
	if (unlikely(nd->flags & LOOKUP_IS_SCOPED))
		goto err;

	path_put(&nd->path);
	nd->path = *path;
	nd->inode = nd->path.dentry->d_inode;
	nd->state |= ND_JUMPED;
	return 0;

err:
	path_put(path);
	return error;
}

static inline void put_link(struct nameidata *nd)
{
	struct saved *last = nd->stack + --nd->depth;
	do_delayed_call(&last->done);
	if (!(nd->flags & LOOKUP_RCU))
		path_put(&last->link);
}

static int sysctl_protected_symlinks __read_mostly;
static int sysctl_protected_hardlinks __read_mostly;
static int sysctl_protected_fifos __read_mostly;
static int sysctl_protected_regular __read_mostly;

#ifdef CONFIG_SYSCTL
static const struct ctl_table namei_sysctls[] = {
	{
		.procname	= "protected_symlinks",
		.data		= &sysctl_protected_symlinks,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "protected_hardlinks",
		.data		= &sysctl_protected_hardlinks,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "protected_fifos",
		.data		= &sysctl_protected_fifos,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
	{
		.procname	= "protected_regular",
		.data		= &sysctl_protected_regular,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
};

static int __init init_fs_namei_sysctls(void)
{
	register_sysctl_init("fs", namei_sysctls);
	return 0;
}
fs_initcall(init_fs_namei_sysctls);

#endif /* CONFIG_SYSCTL */

/**
 * may_follow_link - Check symlink following for unsafe situations
 * @nd: nameidata pathwalk data
 * @inode: Used for idmapping.
 *
 * In the case of the sysctl_protected_symlinks sysctl being enabled,
 * CAP_DAC_OVERRIDE needs to be specifically ignored if the symlink is
 * in a sticky world-writable directory. This is to protect privileged
 * processes from failing races against path names that may change out
 * from under them by way of other users creating malicious symlinks.
 * It will permit symlinks to be followed only when outside a sticky
 * world-writable directory, or when the uid of the symlink and follower
 * match, or when the directory owner matches the symlink's owner.
 *
 * Returns 0 if following the symlink is allowed, -ve on error.
 */
static inline int may_follow_link(struct nameidata *nd, const struct inode *inode)
{
	struct mnt_idmap *idmap;
	vfsuid_t vfsuid;

	if (!sysctl_protected_symlinks)
		return 0;

	idmap = mnt_idmap(nd->path.mnt);
	vfsuid = i_uid_into_vfsuid(idmap, inode);
	/* Allowed if owner and follower match. */
	if (vfsuid_eq_kuid(vfsuid, current_fsuid()))
		return 0;

	/* Allowed if parent directory not sticky and world-writable. */
	if ((nd->dir_mode & (S_ISVTX|S_IWOTH)) != (S_ISVTX|S_IWOTH))
		return 0;

	/* Allowed if parent directory and link owner match. */
	if (vfsuid_valid(nd->dir_vfsuid) && vfsuid_eq(nd->dir_vfsuid, vfsuid))
		return 0;

	if (nd->flags & LOOKUP_RCU)
		return -ECHILD;

	audit_inode(nd->name, nd->stack[0].link.dentry, 0);
	audit_log_path_denied(AUDIT_ANOM_LINK, "follow_link");
	return -EACCES;
}

/**
 * safe_hardlink_source - Check for safe hardlink conditions
 * @idmap: idmap of the mount the inode was found from
 * @inode: the source inode to hardlink from
 *
 * Return false if at least one of the following conditions:
 *    - inode is not a regular file
 *    - inode is setuid
 *    - inode is setgid and group-exec
 *    - access failure for read and write
 *
 * Otherwise returns true.
 */
static bool safe_hardlink_source(struct mnt_idmap *idmap,
				 struct inode *inode)
{
	umode_t mode = inode->i_mode;

	/* Special files should not get pinned to the filesystem. */
	if (!S_ISREG(mode))
		return false;

	/* Setuid files should not get pinned to the filesystem. */
	if (mode & S_ISUID)
		return false;

	/* Executable setgid files should not get pinned to the filesystem. */
	if ((mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
		return false;

	/* Hardlinking to unreadable or unwritable sources is dangerous. */
	if (inode_permission(idmap, inode, MAY_READ | MAY_WRITE))
		return false;

	return true;
}

/**
 * may_linkat - Check permissions for creating a hardlink
 * @idmap: idmap of the mount the inode was found from
 * @link:  the source to hardlink from
 *
 * Block hardlink when all of:
 *  - sysctl_protected_hardlinks enabled
 *  - fsuid does not match inode
 *  - hardlink source is unsafe (see safe_hardlink_source() above)
 *  - not CAP_FOWNER in a namespace with the inode owner uid mapped
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 *
 * Returns 0 if successful, -ve on error.
 */
int may_linkat(struct mnt_idmap *idmap, const struct path *link)
{
	struct inode *inode = link->dentry->d_inode;

	/* Inode writeback is not safe when the uid or gid are invalid. */
	if (!vfsuid_valid(i_uid_into_vfsuid(idmap, inode)) ||
	    !vfsgid_valid(i_gid_into_vfsgid(idmap, inode)))
		return -EOVERFLOW;

	if (!sysctl_protected_hardlinks)
		return 0;

	/* Source inode owner (or CAP_FOWNER) can hardlink all they like,
	 * otherwise, it must be a safe source.
	 */
	if (safe_hardlink_source(idmap, inode) ||
	    inode_owner_or_capable(idmap, inode))
		return 0;

	audit_log_path_denied(AUDIT_ANOM_LINK, "linkat");
	return -EPERM;
}

/**
 * may_create_in_sticky - Check whether an O_CREAT open in a sticky directory
 *			  should be allowed, or not, on files that already
 *			  exist.
 * @idmap: idmap of the mount the inode was found from
 * @nd: nameidata pathwalk data
 * @inode: the inode of the file to open
 *
 * Block an O_CREAT open of a FIFO (or a regular file) when:
 *   - sysctl_protected_fifos (or sysctl_protected_regular) is enabled
 *   - the file already exists
 *   - we are in a sticky directory
 *   - we don't own the file
 *   - the owner of the directory doesn't own the file
 *   - the directory is world writable
 * If the sysctl_protected_fifos (or sysctl_protected_regular) is set to 2
 * the directory doesn't have to be world writable: being group writable will
 * be enough.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 *
 * Returns 0 if the open is allowed, -ve on error.
 */
static int may_create_in_sticky(struct mnt_idmap *idmap, struct nameidata *nd,
				struct inode *const inode)
{
	umode_t dir_mode = nd->dir_mode;
	vfsuid_t dir_vfsuid = nd->dir_vfsuid, i_vfsuid;

	if (likely(!(dir_mode & S_ISVTX)))
		return 0;

	if (S_ISREG(inode->i_mode) && !sysctl_protected_regular)
		return 0;

	if (S_ISFIFO(inode->i_mode) && !sysctl_protected_fifos)
		return 0;

	i_vfsuid = i_uid_into_vfsuid(idmap, inode);

	if (vfsuid_eq(i_vfsuid, dir_vfsuid))
		return 0;

	if (vfsuid_eq_kuid(i_vfsuid, current_fsuid()))
		return 0;

	if (likely(dir_mode & 0002)) {
		audit_log_path_denied(AUDIT_ANOM_CREAT, "sticky_create");
		return -EACCES;
	}

	if (dir_mode & 0020) {
		if (sysctl_protected_fifos >= 2 && S_ISFIFO(inode->i_mode)) {
			audit_log_path_denied(AUDIT_ANOM_CREAT,
					      "sticky_create_fifo");
			return -EACCES;
		}

		if (sysctl_protected_regular >= 2 && S_ISREG(inode->i_mode)) {
			audit_log_path_denied(AUDIT_ANOM_CREAT,
					      "sticky_create_regular");
			return -EACCES;
		}
	}

	return 0;
}

/*
 * follow_up - Find the mountpoint of path's vfsmount
 *
 * Given a path, find the mountpoint of its source file system.
 * Replace @path with the path of the mountpoint in the parent mount.
 * Up is towards /.
 *
 * Return 1 if we went up a level and 0 if we were already at the
 * root.
 */
int follow_up(struct path *path)
{
	struct mount *mnt = real_mount(path->mnt);
	struct mount *parent;
	struct dentry *mountpoint;

	read_seqlock_excl(&mount_lock);
	parent = mnt->mnt_parent;
	if (parent == mnt) {
		read_sequnlock_excl(&mount_lock);
		return 0;
	}
	mntget(&parent->mnt);
	mountpoint = dget(mnt->mnt_mountpoint);
	read_sequnlock_excl(&mount_lock);
	dput(path->dentry);
	path->dentry = mountpoint;
	mntput(path->mnt);
	path->mnt = &parent->mnt;
	return 1;
}
EXPORT_SYMBOL(follow_up);

static bool choose_mountpoint_rcu(struct mount *m, const struct path *root,
				  struct path *path, unsigned *seqp)
{
	while (mnt_has_parent(m)) {
		struct dentry *mountpoint = m->mnt_mountpoint;

		m = m->mnt_parent;
		if (unlikely(root->dentry == mountpoint &&
			     root->mnt == &m->mnt))
			break;
		if (mountpoint != m->mnt.mnt_root) {
			path->mnt = &m->mnt;
			path->dentry = mountpoint;
			*seqp = read_seqcount_begin(&mountpoint->d_seq);
			return true;
		}
	}
	return false;
}

static bool choose_mountpoint(struct mount *m, const struct path *root,
			      struct path *path)
{
	bool found;

	rcu_read_lock();
	while (1) {
		unsigned seq, mseq = read_seqbegin(&mount_lock);

		found = choose_mountpoint_rcu(m, root, path, &seq);
		if (unlikely(!found)) {
			if (!read_seqretry(&mount_lock, mseq))
				break;
		} else {
			if (likely(__legitimize_path(path, seq, mseq)))
				break;
			rcu_read_unlock();
			path_put(path);
			rcu_read_lock();
		}
	}
	rcu_read_unlock();
	return found;
}

/*
 * Perform an automount
 * - return -EISDIR to tell follow_managed() to stop and return the path we
 *   were called with.
 */
static int follow_automount(struct path *path, int *count, unsigned lookup_flags)
{
	struct dentry *dentry = path->dentry;

	/* We don't want to mount if someone's just doing a stat -
	 * unless they're stat'ing a directory and appended a '/' to
	 * the name.
	 *
	 * We do, however, want to mount if someone wants to open or
	 * create a file of any type under the mountpoint, wants to
	 * traverse through the mountpoint or wants to open the
	 * mounted directory.  Also, autofs may mark negative dentries
	 * as being automount points.  These will need the attentions
	 * of the daemon to instantiate them before they can be used.
	 */
	if (!(lookup_flags & (LOOKUP_PARENT | LOOKUP_DIRECTORY |
			   LOOKUP_OPEN | LOOKUP_CREATE | LOOKUP_AUTOMOUNT)) &&
	    dentry->d_inode)
		return -EISDIR;

	/* No need to trigger automounts if mountpoint crossing is disabled. */
	if (lookup_flags & LOOKUP_NO_XDEV)
		return -EXDEV;

	if (count && (*count)++ >= MAXSYMLINKS)
		return -ELOOP;

	return finish_automount(dentry->d_op->d_automount(path), path);
}

/*
 * mount traversal - out-of-line part.  One note on ->d_flags accesses -
 * dentries are pinned but not locked here, so negative dentry can go
 * positive right under us.  Use of smp_load_acquire() provides a barrier
 * sufficient for ->d_inode and ->d_flags consistency.
 */
static int __traverse_mounts(struct path *path, unsigned flags, bool *jumped,
			     int *count, unsigned lookup_flags)
{
	struct vfsmount *mnt = path->mnt;
	bool need_mntput = false;
	int ret = 0;

	dentry_dbg(path->dentry, "mnt->mnt_root.d_name.name %s\n", mnt->mnt_root->d_name.name);
	while (flags & DCACHE_MANAGED_DENTRY) {
		/* Allow the filesystem to manage the transit without i_rwsem
		 * being held. */
		if (flags & DCACHE_MANAGE_TRANSIT) {
			if (lookup_flags & LOOKUP_NO_XDEV) {
				ret = -EXDEV;
				break;
			}
			ret = path->dentry->d_op->d_manage(path, false);
			flags = smp_load_acquire(&path->dentry->d_flags);
			if (ret < 0)
				break;
		}

		if (flags & DCACHE_MOUNTED) {	// something's mounted on it..
			struct vfsmount *mounted = lookup_mnt(path);
			if (mounted) {		// ... in our namespace
				dput(path->dentry);
				if (need_mntput)
					mntput(path->mnt);
				path->mnt = mounted;
				path->dentry = dget(mounted->mnt_root);
				// here we know it's positive
				flags = path->dentry->d_flags;
				need_mntput = true;
				if (unlikely(lookup_flags & LOOKUP_NO_XDEV)) {
					ret = -EXDEV;
					break;
				}
				continue;
			}
		}

		if (!(flags & DCACHE_NEED_AUTOMOUNT))
			break;

		// uncovered automount point
		ret = follow_automount(path, count, lookup_flags);
		flags = smp_load_acquire(&path->dentry->d_flags);
		if (ret < 0)
			break;
	}

	if (ret == -EISDIR)
		ret = 0;
	// possible if you race with several mount --move
	if (need_mntput && path->mnt == mnt)
		mntput(path->mnt);
	if (!ret && unlikely(d_flags_negative(flags)))
		ret = -ENOENT;
	*jumped = need_mntput;
	return ret;
}

static inline int traverse_mounts(struct path *path, bool *jumped,
				  int *count, unsigned lookup_flags)
{
	unsigned flags = smp_load_acquire(&path->dentry->d_flags);

	dentry_dbg(path->dentry, "traverse\n");
	/* fastpath */
	if (likely(!(flags & DCACHE_MANAGED_DENTRY))) {
		*jumped = false;
		if (unlikely(d_flags_negative(flags)))
			return -ENOENT;
		return 0;
	}
	return __traverse_mounts(path, flags, jumped, count, lookup_flags);
}

int follow_down_one(struct path *path)
{
	struct vfsmount *mounted;

	mounted = lookup_mnt(path);
	if (mounted) {
		dput(path->dentry);
		mntput(path->mnt);
		path->mnt = mounted;
		path->dentry = dget(mounted->mnt_root);
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(follow_down_one);

/*
 * Follow down to the covering mount currently visible to userspace.  At each
 * point, the filesystem owning that dentry may be queried as to whether the
 * caller is permitted to proceed or not.
 */
int follow_down(struct path *path, unsigned int flags)
{
	struct vfsmount *mnt = path->mnt;
	bool jumped;
	int ret = traverse_mounts(path, &jumped, NULL, flags);

	if (path->mnt != mnt)
		mntput(mnt);
	return ret;
}
EXPORT_SYMBOL(follow_down);

/*
 * Try to skip to top of mountpoint pile in rcuwalk mode.  Fail if
 * we meet a managed dentry that would need blocking.
 */
static bool __follow_mount_rcu(struct nameidata *nd, struct path *path)
{
	struct dentry *dentry = path->dentry;
	unsigned int flags = dentry->d_flags;

	dentry_dbg(dentry, "dentry->d_flags: %x\n", flags);
	if (unlikely(nd->flags & LOOKUP_NO_XDEV))
		return false;

	for (;;) {
		/*
		 * Don't forget we might have a non-mountpoint managed dentry
		 * that wants to block transit.
		 */
		if (unlikely(flags & DCACHE_MANAGE_TRANSIT)) {
			int res = dentry->d_op->d_manage(path, true);
			if (res)
				return res == -EISDIR;
			flags = dentry->d_flags;
		}

		if (flags & DCACHE_MOUNTED) {
			struct mount *mounted = __lookup_mnt(path->mnt, dentry);
			if (mounted) {
				path->mnt = &mounted->mnt;
				dentry = path->dentry = mounted->mnt.mnt_root;
				dentry_dbg(dentry, "found mnt\n");
				nd->state |= ND_JUMPED;
				nd->next_seq = read_seqcount_begin(&dentry->d_seq);
				flags = dentry->d_flags;
				// makes sure that non-RCU pathwalk could reach
				// this state.
				if (read_seqretry(&mount_lock, nd->m_seq))
					return false;
				continue;
			}
			if (read_seqretry(&mount_lock, nd->m_seq))
				return false;
		}
		return !(flags & DCACHE_NEED_AUTOMOUNT);
	}
}

static inline int handle_mounts(struct nameidata *nd, struct dentry *dentry,
			  struct path *path)
{
	bool jumped;
	int ret;

	dentry_dbg(dentry, "nd->last.name: %s\n", nd->last.name);
	path->mnt = nd->path.mnt;
	path->dentry = dentry;
	if (nd->flags & LOOKUP_RCU) {
		unsigned int seq = nd->next_seq;
		if (likely(!d_managed(dentry)))
			return 0;
		if (likely(__follow_mount_rcu(nd, path)))
			return 0;
		// *path and nd->next_seq might've been clobbered
		path->mnt = nd->path.mnt;
		path->dentry = dentry;
		nd->next_seq = seq;
		if (unlikely(!try_to_unlazy_next(nd, dentry)))
			return -ECHILD;
	}
	ret = traverse_mounts(path, &jumped, &nd->total_link_count, nd->flags);
	if (jumped)
		nd->state |= ND_JUMPED;
	if (unlikely(ret)) {
		dput(path->dentry);
		if (path->mnt != nd->path.mnt)
			mntput(path->mnt);
	}
	return ret;
}

/*
 * This looks up the name in dcache and possibly revalidates the found dentry.
 * NULL is returned if the dentry does not exist in the cache.
 */
static struct dentry *lookup_dcache(const struct qstr *name,
				    struct dentry *dir,
				    unsigned int flags)
{
	struct dentry *dentry = d_lookup(dir, name);
	if (dentry) {
		int error = d_revalidate(dir->d_inode, name, dentry, flags);
		if (unlikely(error <= 0)) {
			if (!error)
				d_invalidate(dentry);
			dput(dentry);
			return ERR_PTR(error);
		}
	}
	return dentry;
}

/*
 * Parent directory has inode locked exclusive.  This is one
 * and only case when ->lookup() gets called on non in-lookup
 * dentries - as the matter of fact, this only gets called
 * when directory is guaranteed to have no in-lookup children
 * at all.
 * Will return -ENOENT if name isn't found and LOOKUP_CREATE wasn't passed.
 * Will return -EEXIST if name is found and LOOKUP_EXCL was passed.
 */
static struct dentry *lookup_one_qstr_excl(const struct qstr *name,
					   struct dentry *base, unsigned int flags)
{
	struct dentry *dentry;
	struct dentry *old;
	struct inode *dir;

	dentry = lookup_dcache(name, base, flags);
	if (dentry)
		goto found;

	/* Don't create child dentry for a dead directory. */
	dir = base->d_inode;
	if (unlikely(IS_DEADDIR(dir)))
		return ERR_PTR(-ENOENT);

	dentry = d_alloc(base, name);
	if (unlikely(!dentry))
		return ERR_PTR(-ENOMEM);

	old = dir->i_op->lookup(dir, dentry, flags);
	if (unlikely(old)) {
		dput(dentry);
		dentry = old;
	}
found:
	if (IS_ERR(dentry))
		return dentry;
	if (d_is_negative(dentry) && !(flags & LOOKUP_CREATE)) {
		dput(dentry);
		return ERR_PTR(-ENOENT);
	}
	if (d_is_positive(dentry) && (flags & LOOKUP_EXCL)) {
		dput(dentry);
		return ERR_PTR(-EEXIST);
	}
	return dentry;
}

/**
 * lookup_fast - do fast lockless (but racy) lookup of a dentry
 * @nd: current nameidata
 *
 * Do a fast, but racy lookup in the dcache for the given dentry, and
 * revalidate it. Returns a valid dentry pointer or NULL if one wasn't
 * found. On error, an ERR_PTR will be returned.
 *
 * If this function returns a valid dentry and the walk is no longer
 * lazy, the dentry will carry a reference that must later be put. If
 * RCU mode is still in force, then this is not the case and the dentry
 * must be legitimized before use. If this returns NULL, then the walk
 * will no longer be in RCU mode.
 */
/**
 * lookup_fast - 快速查找目录项
 * @nd: 当前路径查找数据结构，包含查找状态、路径和标志等信息
 *
 * 该函数用于在目录缓存中快速查找指定的目录项。根据查找标志的不同，
 * 它会在 RCU（Read-Copy-Update）模式或普通加锁模式下进行查找。
 * 如果在 RCU 模式下查找失败或遇到竞态条件，会尝试退回到非 RCU 模式。
 *
 * 返回值: 成功则返回找到的 dentry 指针；如果目录项不在缓存中则返回 NULL；
 *         如果发生错误则返回相应的 ERR_PTR。
 */
static struct dentry *lookup_fast(struct nameidata *nd)
{
	struct dentry *dentry, *parent = nd->path.dentry; /* dentry: 查找结果目录项，parent: 父目录项 */
	int status = 1; /* 目录项验证状态，默认为1（有效） */

	/* 打印调试信息，输出当前正在查找的文件/目录名 */
	nd_dbg(nd, "nd->last=%s\n",
		nd->last.name);
	/*
	 * Rename seqlock is not required here because in the off chance
	 * of a false negative due to a concurrent rename, the caller is
	 * going to fall back to non-racy lookup.
	 */
	/* 判断是否处于 RCU 查找模式 */
	if (nd->flags & LOOKUP_RCU) {
		// rcu_read_lock已经在path_init中调用
		/* 在 RCU 模式下进行无锁目录项查找 */
		dentry = __d_lookup_rcu(parent, &nd->last, &nd->next_seq);
		/* 如果查找失败，尝试退出 RCU 模式转为非竞态查找 */
		if (unlikely(!dentry)) {
			if (!try_to_unlazy(nd))
				return ERR_PTR(-ECHILD); /* 退出 RCU 失败，返回错误 */
			return NULL; /* 返回 NULL 提示调用者回退到常规查找 */
		}
		/*
		 * This sequence count validates that the parent had no
		 * changes while we did the lookup of the dentry above.
		 */
		/* 检查父目录的序列号，验证在查找期间父目录是否发生了改变 */
		if (read_seqcount_retry(&parent->d_seq, nd->seq))
			return ERR_PTR(-ECHILD); /* 发生竞态条件，返回错误要求重试 */

		// 验证目录是否还有效，FUSE，NFS等文件系统会用到
		status = d_revalidate(nd->inode, &nd->last, dentry, nd->flags);
		/* 如果验证成功，直接返回找到的目录项 */
		if (likely(status > 0))
			return dentry;
		/* 验证未通过，尝试将当前 nd 和 dentry 退出 RCU 模式 */
		if (!try_to_unlazy_next(nd, dentry))
			return ERR_PTR(-ECHILD); /* 退出失败，返回错误 */
		/* 如果是因为 RCU 模式导致验证失败，则在非 RCU 模式下重新验证 */
		if (status == -ECHILD)
			/* we'd been told to redo it in non-rcu mode */
			status = d_revalidate(nd->inode, &nd->last,
					      dentry, nd->flags);
	} else {
		/* 非 RCU 模式：使用常规的加锁方式在 dcache 中查找 */
		dentry = __d_lookup(parent, &nd->last);
		/* 如果目录项不在缓存中，直接返回 NULL */
		if (unlikely(!dentry))
			return NULL;
		/* 打印查找到的 dentry 调试信息 */
		dentry_dbg(dentry, "__d_lookup nd->last=%s\n", nd->last.name);
		/* 在非 RCU 模式下验证目录项的有效性 */
		status = d_revalidate(nd->inode, &nd->last, dentry, nd->flags);
	}
	/* 处理目录项验证失败的情况 */
	if (unlikely(status <= 0)) {
		// 如果无效，把目录invalidate掉
		if (!status)
			d_invalidate(dentry); /* status 为 0 表示目录项无效，将其失效 */
		dput(dentry); /* 释放目录项引用 */
		return ERR_PTR(status); /* 返回错误状态 */
	}
	/* 打印最终成功查找的调试信息 */
	dentry_dbg(dentry, "nd->last=%s\n",
		nd->last.name);
	return dentry; /* 返回成功查找到且有效的目录项 */
}


/* Fast lookup failed, do it the slow way */
/**
 * __lookup_slow - 在父目录中慢速查找指定名称的目录项
 * @name: 待查找的目录项名称（qstr结构体指针）
 * @dir: 父目录的目录项
 * @flags: 查找标志位
 *
 * 此函数用于在给定的父目录中查找指定名称的dentry。如果dentry不在缓存中，
 * 则会调用具体文件系统的lookup操作进行磁盘查找。若缓存中的dentry无效，
 * 则会进行失效处理并重新查找。
 *
 * Return: 成功时返回找到的dentry指针，失败时返回相应的错误指针(ERR_PTR)
 */
static struct dentry *__lookup_slow(const struct qstr *name,
				    struct dentry *dir,
				    unsigned int flags)
{
	struct dentry *dentry, *old;
	struct inode *inode = dir->d_inode;

	/* 调试信息：打印正在查找的目录项名称 */
	dentry_dbg(dir, "search for name = %s\n", name->name);
	/* 如果目录已经是无效（死亡）状态，直接返回-ENOENT错误 */
	if (unlikely(IS_DEADDIR(inode)))
		return ERR_PTR(-ENOENT);
again:
	/* 并行分配或查找指定名称的dentry，如果其他进程正在创建同一dentry，则在此等待 */
	dentry = d_alloc_parallel(dir, name);
	if (IS_ERR(dentry))
		return dentry;
	if (unlikely(!d_in_lookup(dentry))) {
		//非查找状态，校验目录，如果目录无效，重新查找
		int error = d_revalidate(inode, name, dentry, flags);
		if (unlikely(error <= 0)) {
			if (!error) {
				/* 校验返回0表示dentry已失效，需要使该缓存无效化并重试查找 */
				d_invalidate(dentry);
				dput(dentry);
				goto again;
			}
			/* 校验返回负值表示发生错误，释放dentry引用并返回错误指针 */
			dput(dentry);
			dentry = ERR_PTR(error);
		}
	} else {
		// 例如 ext4_lookup 函数
		/* 调用具体文件系统的lookup方法（如ext4_lookup）从磁盘查找并实例化inode */
		old = inode->i_op->lookup(inode, dentry, flags);
		// 唤醒等待在这个dentry wq的查找进程
		d_lookup_done(dentry);
		if (unlikely(old)) {
			/* 如果lookup返回了另一个dentry（通常是由于竞态条件），释放当前dentry，使用返回的old */
			dput(dentry);
			dentry = old;
		}
	}
	return dentry;
}


static noinline struct dentry *lookup_slow(const struct qstr *name,
				  struct dentry *dir,
				  unsigned int flags)
{
	struct inode *inode = dir->d_inode;
	struct dentry *res;

	// 信号量
	inode_lock_shared(inode);
	dentry_dbg(dir, "search for name = %s\n", name->name);
	res = __lookup_slow(name, dir, flags);
	inode_unlock_shared(inode);
	return res;
}

static struct dentry *lookup_slow_killable(const struct qstr *name,
					   struct dentry *dir,
					   unsigned int flags)
{
	struct inode *inode = dir->d_inode;
	struct dentry *res;

	if (inode_lock_shared_killable(inode))
		return ERR_PTR(-EINTR);
	res = __lookup_slow(name, dir, flags);
	inode_unlock_shared(inode);
	return res;
}

static inline int may_lookup(struct mnt_idmap *idmap,
			     struct nameidata *restrict nd)
{
	int err, mask;

	mask = nd->flags & LOOKUP_RCU ? MAY_NOT_BLOCK : 0;
	err = lookup_inode_permission_may_exec(idmap, nd->inode, mask);
	if (likely(!err))
		return 0;

	// If we failed, and we weren't in LOOKUP_RCU, it's final
	if (!(nd->flags & LOOKUP_RCU))
		return err;

	// Drop out of RCU mode to make sure it wasn't transient
	if (!try_to_unlazy(nd))
		return -ECHILD;	// redo it all non-lazy

	if (err != -ECHILD)	// hard error
		return err;

	return lookup_inode_permission_may_exec(idmap, nd->inode, 0);
}

static int reserve_stack(struct nameidata *nd, struct path *link)
{
	if (unlikely(nd->total_link_count++ >= MAXSYMLINKS))
		return -ELOOP;

	if (likely(nd->depth != EMBEDDED_LEVELS))
		return 0;
	if (likely(nd->stack != nd->internal))
		return 0;
	if (likely(nd_alloc_stack(nd)))
		return 0;

	if (nd->flags & LOOKUP_RCU) {
		// we need to grab link before we do unlazy.  And we can't skip
		// unlazy even if we fail to grab the link - cleanup needs it
		bool grabbed_link = legitimize_path(nd, link, nd->next_seq);

		if (!try_to_unlazy(nd) || !grabbed_link)
			return -ECHILD;

		if (nd_alloc_stack(nd))
			return 0;
	}
	return -ENOMEM;
}

enum {WALK_TRAILING = 1, WALK_MORE = 2, WALK_NOFOLLOW = 4};

static noinline const char *pick_link(struct nameidata *nd, struct path *link,
		     struct inode *inode, int flags)
{
	struct saved *last;
	const char *res;
	int error;

	if (nd->flags & LOOKUP_RCU) {
		/* make sure that d_is_symlink from step_into_slowpath() matches the inode */
		if (read_seqcount_retry(&link->dentry->d_seq, nd->next_seq))
			return ERR_PTR(-ECHILD);
	} else {
		if (link->mnt == nd->path.mnt)
			mntget(link->mnt);
	}

	error = reserve_stack(nd, link);
	if (unlikely(error)) {
		if (!(nd->flags & LOOKUP_RCU))
			path_put(link);
		return ERR_PTR(error);
	}
	last = nd->stack + nd->depth++;
	last->link = *link;
	clear_delayed_call(&last->done);
	last->seq = nd->next_seq;

	if (flags & WALK_TRAILING) {
		error = may_follow_link(nd, inode);
		if (unlikely(error))
			return ERR_PTR(error);
	}

	if (unlikely(nd->flags & LOOKUP_NO_SYMLINKS) ||
			unlikely(link->mnt->mnt_flags & MNT_NOSYMFOLLOW))
		return ERR_PTR(-ELOOP);

	if (unlikely(atime_needs_update(&last->link, inode))) {
		if (nd->flags & LOOKUP_RCU) {
			if (!try_to_unlazy(nd))
				return ERR_PTR(-ECHILD);
		}
		touch_atime(&last->link);
		cond_resched();
	}

	error = security_inode_follow_link(link->dentry, inode,
					   nd->flags & LOOKUP_RCU);
	if (unlikely(error))
		return ERR_PTR(error);

	res = READ_ONCE(inode->i_link);
	if (!res) {
		const char * (*get)(struct dentry *, struct inode *,
				struct delayed_call *);
		get = inode->i_op->get_link;
		if (nd->flags & LOOKUP_RCU) {
			res = get(NULL, inode, &last->done);
			if (res == ERR_PTR(-ECHILD) && try_to_unlazy(nd))
				res = get(link->dentry, inode, &last->done);
		} else {
			res = get(link->dentry, inode, &last->done);
		}
		if (!res)
			goto all_done;
		if (IS_ERR(res))
			return res;
	}
	if (*res == '/') {
		error = nd_jump_root(nd);
		if (unlikely(error))
			return ERR_PTR(error);
		while (unlikely(*++res == '/'))
			;
	}
	if (*res)
		return res;
all_done: // pure jump
	put_link(nd);
	return NULL;
}

/*
 * Do we need to follow links? We _really_ want to be able
 * to do this check without having to look at inode->i_op,
 * so we keep a cache of "no, this doesn't need follow_link"
 * for the common case.
 *
 * NOTE: dentry must be what nd->next_seq had been sampled from.
 */
static noinline const char *step_into_slowpath(struct nameidata *nd, int flags,
		     struct dentry *dentry)
{
	struct path path;
	struct inode *inode;
	int err;
	
	dentry_dbg(dentry, "nd->last.name: %s\n", nd->last.name);
	err = handle_mounts(nd, dentry, &path);
	if (unlikely(err < 0))
		return ERR_PTR(err);
	inode = path.dentry->d_inode;
	if (likely(!d_is_symlink(path.dentry)) ||
	   ((flags & WALK_TRAILING) && !(nd->flags & LOOKUP_FOLLOW)) ||
	   (flags & WALK_NOFOLLOW)) {
		/* not a symlink or should not follow */
		if (nd->flags & LOOKUP_RCU) {
			if (read_seqcount_retry(&path.dentry->d_seq, nd->next_seq))
				return ERR_PTR(-ECHILD);
			if (unlikely(!inode))
				return ERR_PTR(-ENOENT);
		} else {
			dput(nd->path.dentry);
			if (nd->path.mnt != path.mnt)
				mntput(nd->path.mnt);
		}
		nd->path = path;
		nd->inode = inode;
		nd->seq = nd->next_seq;
		return NULL;
	}
	return pick_link(nd, &path, inode, flags);
}

static __always_inline const char *step_into(struct nameidata *nd, int flags,
                    struct dentry *dentry)
{
	/*
	 * In the common case we are in rcu-walk and traversing over a non-mounted on
	 * directory (as opposed to e.g., a symlink).
	 *
	 * We can handle that and negative entries with the checks below.
	 */
	if (likely((nd->flags & LOOKUP_RCU) &&
	    !d_managed(dentry) && !d_is_symlink(dentry))) {
		struct inode *inode = dentry->d_inode;
		if (read_seqcount_retry(&dentry->d_seq, nd->next_seq))
			return ERR_PTR(-ECHILD);
		if (unlikely(!inode))
			return ERR_PTR(-ENOENT);
		nd->path.dentry = dentry;
		/* nd->path.mnt remains unchanged as no mount point was crossed */
		nd->inode = inode;
		nd->seq = nd->next_seq;
		return NULL;
	}
	return step_into_slowpath(nd, flags, dentry);
}

static struct dentry *follow_dotdot_rcu(struct nameidata *nd)
{
	struct dentry *parent, *old;

	if (path_equal(&nd->path, &nd->root))
		goto in_root;
	if (unlikely(nd->path.dentry == nd->path.mnt->mnt_root)) {
		struct path path;
		unsigned seq;
		if (!choose_mountpoint_rcu(real_mount(nd->path.mnt),
					   &nd->root, &path, &seq))
			goto in_root;
		if (unlikely(nd->flags & LOOKUP_NO_XDEV))
			return ERR_PTR(-ECHILD);
		nd->path = path;
		nd->inode = path.dentry->d_inode;
		nd->seq = seq;
		// makes sure that non-RCU pathwalk could reach this state
		if (read_seqretry(&mount_lock, nd->m_seq))
			return ERR_PTR(-ECHILD);
		/* we know that mountpoint was pinned */
	}
	old = nd->path.dentry;
	parent = old->d_parent;
	nd->next_seq = read_seqcount_begin(&parent->d_seq);
	// makes sure that non-RCU pathwalk could reach this state
	if (read_seqcount_retry(&old->d_seq, nd->seq))
		return ERR_PTR(-ECHILD);
	if (unlikely(!path_connected(nd->path.mnt, parent)))
		return ERR_PTR(-ECHILD);
	return parent;
in_root:
	if (read_seqretry(&mount_lock, nd->m_seq))
		return ERR_PTR(-ECHILD);
	if (unlikely(nd->flags & LOOKUP_BENEATH))
		return ERR_PTR(-ECHILD);
	nd->next_seq = nd->seq;
	return nd->path.dentry;
}

static struct dentry *follow_dotdot(struct nameidata *nd)
{
	struct dentry *parent;

	if (path_equal(&nd->path, &nd->root))
		goto in_root;
	if (unlikely(nd->path.dentry == nd->path.mnt->mnt_root)) {
		struct path path;

		if (!choose_mountpoint(real_mount(nd->path.mnt),
				       &nd->root, &path))
			goto in_root;
		path_put(&nd->path);
		nd->path = path;
		nd->inode = path.dentry->d_inode;
		if (unlikely(nd->flags & LOOKUP_NO_XDEV))
			return ERR_PTR(-EXDEV);
	}
	/* rare case of legitimate dget_parent()... */
	parent = dget_parent(nd->path.dentry);
	if (unlikely(!path_connected(nd->path.mnt, parent))) {
		dput(parent);
		return ERR_PTR(-ENOENT);
	}
	return parent;

in_root:
	if (unlikely(nd->flags & LOOKUP_BENEATH))
		return ERR_PTR(-EXDEV);
	return dget(nd->path.dentry);
}

static const char *handle_dots(struct nameidata *nd, enum last_type type)
{
	if (type == LAST_DOTDOT) {
		const char *error = NULL;
		struct dentry *parent;

		if (!nd->root.mnt) {
			error = ERR_PTR(set_root(nd));
			if (unlikely(error))
				return error;
		}
		if (nd->flags & LOOKUP_RCU)
			parent = follow_dotdot_rcu(nd);
		else
			parent = follow_dotdot(nd);
		if (IS_ERR(parent))
			return ERR_CAST(parent);
		error = step_into(nd, WALK_NOFOLLOW, parent);
		if (unlikely(error))
			return error;

		if (unlikely(nd->flags & LOOKUP_IS_SCOPED)) {
			/*
			 * If there was a racing rename or mount along our
			 * path, then we can't be sure that ".." hasn't jumped
			 * above nd->root (and so userspace should retry or use
			 * some fallback).
			 */
			smp_rmb();
			if (__read_seqcount_retry(&mount_lock.seqcount, nd->m_seq))
				return ERR_PTR(-EAGAIN);
			if (__read_seqcount_retry(&rename_lock.seqcount, nd->r_seq))
				return ERR_PTR(-EAGAIN);
		}
	}
	return NULL;
}

/**
 * walk_component - 处理路径查找中的单个组件
 * @nd: 名字查找数据结构，保存当前查找的状态和路径信息
 * @flags: 查找标志位，控制查找行为（如是否继续查找等）
 *
 * 该函数用于解析路径名中的单个组件。对于特殊的"."和".."目录项会进行
 * 特殊处理；对于普通的目录项，会先尝试从dentry缓存中快速查找，
 * 若缓存未命中，再回退到慢速路径从磁盘文件系统中查找。
 *
 * Return: 成功时返回下一步的文件名指针，失败时返回相应的错误指针
 */
static __always_inline const char *walk_component(struct nameidata *nd, int flags)
{
	struct dentry *dentry;
	/*
	 * "." and ".." are special - ".." especially so because it has
	 * to be able to know about the current root directory and
	 * parent relationships.
	 */
	if (unlikely(nd->last_type != LAST_NORM)) {
		/* 如果处于深度遍历状态且没有WALK_MORE标志，则释放当前的链接 */
		if (unlikely(nd->depth) && !(flags & WALK_MORE))
			put_link(nd);
		/* 处理特殊的"."和".."目录项 */
		return handle_dots(nd, nd->last_type);
	}
	nd_dbg(nd, "nd->last_name = %s\n", nd->last.name);
	// 从dentry_cache中查找dentry
	dentry = lookup_fast(nd);
	/* 如果快速查找返回错误，则将错误指针转换为对应的字符串指针并返回 */
	if (IS_ERR(dentry))
		return ERR_CAST(dentry);
	if (unlikely(!dentry)) {
		// 缓存中不存在，则从磁盘查找
		dentry = lookup_slow(&nd->last, nd->path.dentry, nd->flags);
		/* 如果慢速查找返回错误，则将错误指针转换为对应的字符串指针并返回 */
		if (IS_ERR(dentry))
			return ERR_CAST(dentry);
		/* 如果在磁盘中成功找到dentry，打印调试信息 */
		if (dentry)
			dentry_dbg(dentry, "found\n");
	}
	/* 如果处于深度遍历状态且没有WALK_MORE标志，则释放当前的链接 */
	if (unlikely(nd->depth) && !(flags & WALK_MORE))
		put_link(nd);
	/* 进入找到的目录项，继续路径遍历的下一步 */
	return step_into(nd, flags, dentry);
}


/*
 * We can do the critical dentry name comparison and hashing
 * operations one word at a time, but we are limited to:
 *
 * - Architectures with fast unaligned word accesses. We could
 *   do a "get_unaligned()" if this helps and is sufficiently
 *   fast.
 *
 * - non-CONFIG_DEBUG_PAGEALLOC configurations (so that we
 *   do not trap on the (extremely unlikely) case of a page
 *   crossing operation.
 *
 * - Furthermore, we need an efficient 64-bit compile for the
 *   64-bit case in order to generate the "number of bytes in
 *   the final mask". Again, that could be replaced with a
 *   efficient population count instruction or similar.
 */
#ifdef CONFIG_DCACHE_WORD_ACCESS

#include <asm/word-at-a-time.h>

#ifdef HASH_MIX

/* Architecture provides HASH_MIX and fold_hash() in <asm/hash.h> */

#elif defined(CONFIG_64BIT)
/*
 * Register pressure in the mixing function is an issue, particularly
 * on 32-bit x86, but almost any function requires one state value and
 * one temporary.  Instead, use a function designed for two state values
 * and no temporaries.
 *
 * This function cannot create a collision in only two iterations, so
 * we have two iterations to achieve avalanche.  In those two iterations,
 * we have six layers of mixing, which is enough to spread one bit's
 * influence out to 2^6 = 64 state bits.
 *
 * Rotate constants are scored by considering either 64 one-bit input
 * deltas or 64*63/2 = 2016 two-bit input deltas, and finding the
 * probability of that delta causing a change to each of the 128 output
 * bits, using a sample of random initial states.
 *
 * The Shannon entropy of the computed probabilities is then summed
 * to produce a score.  Ideally, any input change has a 50% chance of
 * toggling any given output bit.
 *
 * Mixing scores (in bits) for (12,45):
 * Input delta: 1-bit      2-bit
 * 1 round:     713.3    42542.6
 * 2 rounds:   2753.7   140389.8
 * 3 rounds:   5954.1   233458.2
 * 4 rounds:   7862.6   256672.2
 * Perfect:    8192     258048
 *            (64*128) (64*63/2 * 128)
 */
#define HASH_MIX(x, y, a)	\
	(	x ^= (a),	\
	y ^= x,	x = rol64(x,12),\
	x += y,	y = rol64(y,45),\
	y *= 9			)

/*
 * Fold two longs into one 32-bit hash value.  This must be fast, but
 * latency isn't quite as critical, as there is a fair bit of additional
 * work done before the hash value is used.
 */
static inline unsigned int fold_hash(unsigned long x, unsigned long y)
{
	y ^= x * GOLDEN_RATIO_64;
	y *= GOLDEN_RATIO_64;
	return y >> 32;
}

#else	/* 32-bit case */

/*
 * Mixing scores (in bits) for (7,20):
 * Input delta: 1-bit      2-bit
 * 1 round:     330.3     9201.6
 * 2 rounds:   1246.4    25475.4
 * 3 rounds:   1907.1    31295.1
 * 4 rounds:   2042.3    31718.6
 * Perfect:    2048      31744
 *            (32*64)   (32*31/2 * 64)
 */
#define HASH_MIX(x, y, a)	\
	(	x ^= (a),	\
	y ^= x,	x = rol32(x, 7),\
	x += y,	y = rol32(y,20),\
	y *= 9			)

static inline unsigned int fold_hash(unsigned long x, unsigned long y)
{
	/* Use arch-optimized multiply if one exists */
	return __hash_32(y ^ __hash_32(x));
}

#endif

/*
 * Return the hash of a string of known length.  This is carfully
 * designed to match hash_name(), which is the more critical function.
 * In particular, we must end by hashing a final word containing 0..7
 * payload bytes, to match the way that hash_name() iterates until it
 * finds the delimiter after the name.
 */
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len)
{
	unsigned long a, x = 0, y = (unsigned long)salt;

	for (;;) {
		if (!len)
			goto done;
		a = load_unaligned_zeropad(name);
		if (len < sizeof(unsigned long))
			break;
		HASH_MIX(x, y, a);
		name += sizeof(unsigned long);
		len -= sizeof(unsigned long);
	}
	x ^= a & bytemask_from_count(len);
done:
	return fold_hash(x, y);
}
EXPORT_SYMBOL(full_name_hash);

/* Return the "hash_len" (hash and length) of a null-terminated string */
u64 hashlen_string(const void *salt, const char *name)
{
	unsigned long a = 0, x = 0, y = (unsigned long)salt;
	unsigned long adata, mask, len;
	const struct word_at_a_time constants = WORD_AT_A_TIME_CONSTANTS;

	len = 0;
	goto inside;

	do {
		HASH_MIX(x, y, a);
		len += sizeof(unsigned long);
inside:
		a = load_unaligned_zeropad(name+len);
	} while (!has_zero(a, &adata, &constants));

	adata = prep_zero_mask(a, adata, &constants);
	mask = create_zero_mask(adata);
	x ^= a & zero_bytemask(mask);

	return hashlen_create(fold_hash(x, y), len + find_zero(mask));
}
EXPORT_SYMBOL(hashlen_string);

/*
 * hash_name - Calculate the length and hash of the path component
 * @nd: the path resolution state
 * @name: the pathname to read the component from
 * @lastword: if the component fits in a single word, LAST_WORD_IS_DOT,
 * LAST_WORD_IS_DOTDOT, or some other value depending on whether the
 * component is '.', '..', or something else. Otherwise, @lastword is 0.
 *
 * Returns: a pointer to the terminating '/' or NUL character in @name.
 */
static inline const char *hash_name(struct nameidata *nd,
				    const char *name,
				    unsigned long *lastword)
{
	unsigned long a, b, x, y = (unsigned long)nd->path.dentry;
	unsigned long adata, bdata, mask, len;
	const struct word_at_a_time constants = WORD_AT_A_TIME_CONSTANTS;

	/*
	 * The first iteration is special, because it can result in
	 * '.' and '..' and has no mixing other than the final fold.
	 */
	a = load_unaligned_zeropad(name);
	b = a ^ REPEAT_BYTE('/');
	if (has_zero(a, &adata, &constants) | has_zero(b, &bdata, &constants)) {
		adata = prep_zero_mask(a, adata, &constants);
		bdata = prep_zero_mask(b, bdata, &constants);
		mask = create_zero_mask(adata | bdata);
		a &= zero_bytemask(mask);
		*lastword = a;
		len = find_zero(mask);
		nd->last.hash = fold_hash(a, y);
		nd->last.len = len;
		return name + len;
	}

	len = 0;
	x = 0;
	do {
		HASH_MIX(x, y, a);
		len += sizeof(unsigned long);
		a = load_unaligned_zeropad(name+len);
		b = a ^ REPEAT_BYTE('/');
	} while (!(has_zero(a, &adata, &constants) | has_zero(b, &bdata, &constants)));

	adata = prep_zero_mask(a, adata, &constants);
	bdata = prep_zero_mask(b, bdata, &constants);
	mask = create_zero_mask(adata | bdata);
	a &= zero_bytemask(mask);
	x ^= a;
	len += find_zero(mask);
	*lastword = 0;		// Multi-word components cannot be DOT or DOTDOT

	nd->last.hash = fold_hash(x, y);
	nd->last.len = len;
	return name + len;
}

/*
 * Note that the 'last' word is always zero-masked, but
 * was loaded as a possibly big-endian word.
 */
#ifdef __BIG_ENDIAN
  #define LAST_WORD_IS_DOT	(0x2eul << (BITS_PER_LONG-8))
  #define LAST_WORD_IS_DOTDOT	(0x2e2eul << (BITS_PER_LONG-16))
#endif

#else	/* !CONFIG_DCACHE_WORD_ACCESS: Slow, byte-at-a-time version */

/* Return the hash of a string of known length */
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len)
{
	unsigned long hash = init_name_hash(salt);
	while (len--)
		hash = partial_name_hash((unsigned char)*name++, hash);
	return end_name_hash(hash);
}
EXPORT_SYMBOL(full_name_hash);

/* Return the "hash_len" (hash and length) of a null-terminated string */
u64 hashlen_string(const void *salt, const char *name)
{
	unsigned long hash = init_name_hash(salt);
	unsigned long len = 0, c;

	c = (unsigned char)*name;
	while (c) {
		len++;
		hash = partial_name_hash(c, hash);
		c = (unsigned char)name[len];
	}
	return hashlen_create(end_name_hash(hash), len);
}
EXPORT_SYMBOL(hashlen_string);

/*
 * We know there's a real path component here of at least
 * one character.
 */
static inline const char *hash_name(struct nameidata *nd, const char *name, unsigned long *lastword)
{
	unsigned long hash = init_name_hash(nd->path.dentry);
	unsigned long len = 0, c, last = 0;

	c = (unsigned char)*name;
	do {
		last = (last << 8) + c;
		len++;
		hash = partial_name_hash(c, hash);
		c = (unsigned char)name[len];
	} while (c && c != '/');

	// This is reliable for DOT or DOTDOT, since the component
	// cannot contain NUL characters - top bits being zero means
	// we cannot have had any other pathnames.
	*lastword = last;
	nd->last.hash = end_name_hash(hash);
	nd->last.len = len;
	return name + len;
}

#endif

#ifndef LAST_WORD_IS_DOT
  #define LAST_WORD_IS_DOT	0x2e
  #define LAST_WORD_IS_DOTDOT	0x2e2e
#endif

/*
 * Name resolution.
 * This is the basic name resolution function, turning a pathname into
 * the final dentry. We expect 'base' to be positive and a directory.
 *
 * Returns 0 and nd will have valid dentry and mnt on success.
 * Returns error and drops reference to input namei data on failure.
 */
/**
 * link_path_walk - 解析并遍历文件路径名
 * @name: 待解析的路径名字符串
 * @nd: 指向 nameidata 结构体的指针，保存路径查找的状态和上下文信息
 *
 * 该函数用于逐步解析路径名中的各个分量（目录/文件名），处理特殊路径（如 '.' 和 '..'），
 * 并在遇到符号链接时进行递归或栈式跟踪。它会在遍历过程中更新 nd 结构体的状态。
 *
 * Return: 成功返回 0，失败返回相应的负错误码
 */
static int link_path_walk(const char *name, struct nameidata *nd)
{
	int depth = 0; // depth <= nd->depth，用于记录当前符号链接的嵌套深度
	int err;

	nd->last_type = LAST_ROOT; // 初始化最后分量类型为根目录
	nd->flags |= LOOKUP_PARENT; // 设置查找标志，表示正在查找父目录

	// 检查路径名指针是否包含错误信息
	if (IS_ERR(name))
		return PTR_ERR(name);

	// 跳过重复的 '/'
	if (*name == '/') {
		do {
			name++;
		} while (unlikely(*name == '/'));
	}

	// 如果路径名为空（例如只包含 '/' 的情况）
	if (unlikely(!*name)) {
		nd->dir_mode = 0; // short-circuit the 'hardening' idiocy，短路处理，避免硬ening相关的无谓操作
		nd_dbg(nd, "return 0, name = %p\n", name);
		return 0;
	}
	nd_dbg(nd, "name = %s\n", name);

	// 循环遍历路径中的每一个分量
	/* At this point we know we have a real path component. */
	for(;;) {
		struct mnt_idmap *idmap;
		const char *link;
		unsigned long lastword;

		// 获取当前挂载点的 idmap，用于权限检查
		idmap = mnt_idmap(nd->path.mnt);
		
		// 检查当前目录是否允许查找（权限验证）
		err = may_lookup(idmap, nd);
		if (unlikely(err))
			return err;

		nd->last.name = name; // 记录当前分量名的起始位置
		nd_dbg(nd, "before hash name = %s\n", name);
		
		// 对当前路径分量进行哈希计算，并提取最后一个字用于后续判断
		name = hash_name(nd, name, &lastword);
		nd_dbg(nd, "after hash name = %s\n", name);

		// 判断目录是 '..' 还是 '.'
		switch(lastword) {
		case LAST_WORD_IS_DOTDOT:
			nd->last_type = LAST_DOTDOT; // 标记为上级目录 '..'
			nd->state |= ND_JUMPED; // 标记发生了目录跳跃
			break;

		case LAST_WORD_IS_DOT:
			nd->last_type = LAST_DOT; // 标记为当前目录 '.'
			break;

		default:
			nd->last_type = LAST_NORM; // 标记为普通目录/文件名
			nd->state &= ~ND_JUMPED; // 清除目录跳跃标志

			struct dentry *parent = nd->path.dentry;
			// 如果父目录定义了自定义的哈希操作，则调用其哈希函数
			if (unlikely(parent->d_flags & DCACHE_OP_HASH)) {
				err = parent->d_op->d_hash(parent, &nd->last);
				if (err < 0)
					return err;
			}
		}

		// 如果当前分量已经是路径的最后一个分量
		if (!*name)
			goto OK;
		/*
		 * If it wasn't NUL, we know it was '/'. Skip that
		 * slash, and continue until no more slashes.
		 */
		// 跳过当前分量后面的斜杠 '/'
		do {
			name++;
		} while (unlikely(*name == '/'));

		// 如果跳过斜杠后到达字符串末尾，说明这也是最后一个分量
		if (unlikely(!*name)) {
			// 最后一个分量
OK:
			/* pathname or trailing symlink, done */
			// 如果没有嵌套的符号链接（深度为0），则路径解析完成
			if (likely(!depth)) {
				nd->dir_vfsuid = i_uid_into_vfsuid(idmap, nd->inode); // 记录目录的 VFS UID
				nd->dir_mode = nd->inode->i_mode; // 记录目录的模式/权限
				nd->flags &= ~LOOKUP_PARENT; // 清除 LOOKUP_PARENT 标志，查找结束
				nd_dbg(nd, "return 0 && depth == 0, name = %s\n", name);
				return 0;
			}
			/* last component of nested symlink */
			// 如果是嵌套符号链接的最后一个分量，退栈获取上一层符号链接的名字
			name = nd->stack[--depth].name;
			// 解析当前分量，不标记 WALK_MORE
			link = walk_component(nd, 0);
		} else {
			/* not the last component */
			// 如果不是最后一个分量，继续解析，标记 WALK_MORE 表示后面还有分量
			link = walk_component(nd, WALK_MORE);
		}

		// 如果 walk_component 返回了一个符号链接
		if (unlikely(link)) {
			if (IS_ERR(link))
				return PTR_ERR(link); // 如果返回的是错误指针，返回错误码
			/* a symlink to follow */
			// 将当前名字压栈，保存现场以便符号链接解析完后恢复
			nd->stack[depth++].name = name;
			name = link; // 指向符号链接的目标路径，继续遍历
			continue;
		}

		// 检查当前路径分量是否是一个有效的目录（不能继续向下查找非目录文件）
		if (unlikely(!d_can_lookup(nd->path.dentry))) {
			// 如果在 RCU 模式下，尝试退出 RCU 模式转为普通慢速路径
			if (nd->flags & LOOKUP_RCU) {
				if (!try_to_unlazy(nd))
					return -ECHILD; // 退出 RCU 失败，返回错误要求用户空间重试
			}
			return -ENOTDIR; // 当前路径分量不是目录，返回 -ENOTDIR 错误
		}
	}
}


/* must be paired with terminate_walk() */
/**
 * path_init - 初始化路径查找的起始状态
 * @nd: 指向 nameidata 结构体的指针，保存路径查找的状态和上下文信息
 * @flags: 查找标志位，控制查找行为（如是否使用 RCU、查找范围等）
 *
 * 该函数根据传入的路径名和标志位，确定路径查找的起始点（根目录、当前工作目录
 * 或指定的文件描述符对应的目录），并初始化 nameidata 结构体中的相关字段。
 * 如果遇到错误或需要重试，将返回相应的错误指针。
 *
 * 返回值: 成功时返回指向待解析路径的指针，失败时返回相应的 ERR_PTR 错误码。
 */
static const char *path_init(struct nameidata *nd, unsigned flags)
{
	/* 定义用于存储错误码的变量 */
	int error;
	/* 获取待解析的路径名字符串指针 */
	const char *s = nd->pathname;

	/* LOOKUP_CACHED 要求必须同时开启 RCU，若仅指定 CACHED 则要求调用者使用 RCU 重试 */
	if (unlikely((flags & (LOOKUP_RCU | LOOKUP_CACHED)) == LOOKUP_CACHED))
		return ERR_PTR(-EAGAIN);

	/* 如果路径名为空字符串，则不能使用 RCU 模式，清除 LOOKUP_RCU 标志 */
	if (unlikely(!*s))
		flags &= ~LOOKUP_RCU;
	
	/* 根据是否启用 RCU 模式进行相应的初始化 */
	if (flags & LOOKUP_RCU)
		rcu_read_lock(); /* 进入 RCU 读端临界区 */
	else
		nd->seq = nd->next_seq = 0; /* 非 RCU 模式下，序列号初始化为 0 */

	/* 保存标志位到 nameidata 结构体 */
	nd->flags = flags;
	/* 设置状态标志，表示可能发生了跳跃式查找（如挂载点跨越） */
	nd->state |= ND_JUMPED;

	/* 读取挂载锁的序列计数初始值，用于后续一致性检查 */
	nd->m_seq = __read_seqcount_begin(&mount_lock.seqcount);
	/* 读取重命名锁的序列计数初始值，用于后续一致性检查 */
	nd->r_seq = __read_seqcount_begin(&rename_lock.seqcount);
	/* 内存读屏障，确保上述锁序列号的读取在后续操作之前完成 */
	smp_rmb();

	/* 检查是否预设了根目录（通常在 openat2 等操作中设置） */
	if (unlikely(nd->state & ND_ROOT_PRESET)) {
		/* 获取预设的根 dentry 和对应的 inode */
		struct dentry *root = nd->root.dentry;
		struct inode *inode = root->d_inode;
		
		/* 如果路径名非空，但根 dentry 不支持目录查找（不是目录），则返回错误 */
		if (*s && unlikely(!d_can_lookup(root)))
			return ERR_PTR(-ENOTDIR);
		
		/* 设置当前查找路径和 inode 为预设的根 */
		nd->path = nd->root;
		nd->inode = inode;
		
		/* 在 RCU 模式下读取 dentry 的序列号以保证一致性 */
		if (flags & LOOKUP_RCU) {
			nd->seq = read_seqcount_begin(&nd->path.dentry->d_seq);
			nd->root_seq = nd->seq;
		} else {
			/* 非 RCU 模式下，增加路径的引用计数 */
			path_get(&nd->path);
		}
		return s;
	}

	/* 根目录的挂载点初始化为 NULL */
	nd->root.mnt = NULL;

	// 绝对路径，根目录
	/* 绝对路径名：获取根目录 (如果指定了 LOOKUP_IN_ROOT，则使用 nd->dfd 作为根) */
	if (*s == '/' && likely(!(flags & LOOKUP_IN_ROOT))) {
		/* 跳转到根目录 */
		error = nd_jump_root(nd);
		if (unlikely(error))
			return ERR_PTR(error);
		return s;
	}

	/* 相对路径名：获取相对的起始点 */
	if (nd->dfd == AT_FDCWD) {
		// 进程工作目录
		/* 相对路径基于调用进程的当前工作目录 */
		if (flags & LOOKUP_RCU) {
			/* RCU 模式下，安全地读取当前进程的文件系统结构体中的 pwd */
			struct fs_struct *fs = current->fs;
			unsigned seq;

			do {
				seq = read_seqbegin(&fs->seq);
				nd->path = fs->pwd;
				nd->inode = nd->path.dentry->d_inode;
				nd->seq = __read_seqcount_begin(&nd->path.dentry->d_seq);
			} while (read_seqretry(&fs->seq, seq)); /* 如果读取期间发生更改，则重试 */
		} else {
			/* 非 RCU 模式下，获取当前工作目录并增加引用计数 */
			get_fs_pwd(current->fs, &nd->path);
			nd->inode = nd->path.dentry->d_inode;
		}
	} else {
		// 其他相对路径
		/* 相对路径基于指定的文件描述符 (dirfd) */
		/* 调用者必须检查起始路径组件的执行权限 */
		CLASS(fd_raw, f)(nd->dfd);
		struct dentry *dentry;

		/* 检查文件描述符是否有效 */
		if (fd_empty(f))
			return ERR_PTR(-EBADF);

		/* 处理 LOOKUP_LINKAT_EMPTY 标志：允许通过空路径名引用文件描述符 */
		if (flags & LOOKUP_LINKAT_EMPTY) {
			/* 如果文件描述符的凭证与当前进程凭证不匹配，且没有相应能力，则拒绝访问 */
			if (fd_file(f)->f_cred != current_cred() &&
			    !ns_capable(fd_file(f)->f_cred->user_ns, CAP_DAC_READ_SEARCH))
				return ERR_PTR(-ENOENT);
		}

		/* 获取文件描述符对应的 dentry */
		dentry = fd_file(f)->f_path.dentry;

		/* 如果路径名非空，但 dentry 不支持目录查找，则返回 -ENOTDIR */
		if (*s && unlikely(!d_can_lookup(dentry)))
			return ERR_PTR(-ENOTDIR);

		/* 设置当前查找路径为文件描述符对应的路径 */
		nd->path = fd_file(f)->f_path;
		if (flags & LOOKUP_RCU) {
			/* RCU 模式下读取 inode 和序列号 */
			nd->inode = nd->path.dentry->d_inode;
			nd->seq = read_seqcount_begin(&nd->path.dentry->d_seq);
		} else {
			/* 非 RCU 模式下，增加路径引用计数并读取 inode */
			path_get(&nd->path);
			nd->inode = nd->path.dentry->d_inode;
		}
	}

	/* 对于作用域查找（如 LOOKUP_BENEATH、LOOKUP_IN_ROOT），需要将根目录也设置为起始 dirfd 对应的目录 */
	if (unlikely(flags & LOOKUP_IS_SCOPED)) {
		nd->root = nd->path;
		if (flags & LOOKUP_RCU) {
			/* RCU 模式下记录根目录序列号 */
			nd->root_seq = nd->seq;
		} else {
			/* 非 RCU 模式下，增加根路径引用计数并标记已抓取根目录 */
			path_get(&nd->root);
			nd->state |= ND_ROOT_GRABBED;
		}
	}
	return s;
}


static inline const char *lookup_last(struct nameidata *nd)
{
	if (nd->last_type == LAST_NORM && nd->last.name[nd->last.len])
		nd->flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;

	return walk_component(nd, WALK_TRAILING);
}

static int handle_lookup_down(struct nameidata *nd)
{
	if (!(nd->flags & LOOKUP_RCU))
		dget(nd->path.dentry);
	nd->next_seq = nd->seq;
	return PTR_ERR(step_into(nd, WALK_NOFOLLOW, nd->path.dentry));
}

/* Returns 0 and nd will be valid on success; Returns error, otherwise. */
/**
 * path_lookupat - 根据给定的名称数据查找路径
 * @nd: 指向名称数据结构体的指针，包含查找状态和路径信息
 * @flags: 查找标志位，用于控制查找行为（如 LOOKUP_DOWN, LOOKUP_MOUNTPOINT 等）
 * @path: 输出参数，用于存储查找到的路径结果
 *
 * 此函数执行文件系统路径查找的核心逻辑。它从初始化路径开始，
 * 逐级遍历路径组件，处理特殊的挂载点和目录检查，最终返回
 * 找到的路径或错误码。
 *
 * Return: 成功返回 0，失败返回相应的负错误码
 */
static int path_lookupat(struct nameidata *nd, unsigned flags, struct path *path)
{
	/* 初始化路径查找，获取待查找的路径字符串起始指针 */
	const char *s = path_init(nd, flags);
	int err;

	/* 调试输出：打印当前正在查找的路径 */
	dbg("path: %s\n", nd->pathname);

	/* 如果标志位要求向下查找（LOOKUP_DOWN）且路径起始指针有效 */
	if (unlikely(flags & LOOKUP_DOWN) && !IS_ERR(s)) {
		/* 处理向下查找的逻辑（通常用于处理起始点为符号链接等情况） */
		err = handle_lookup_down(nd);
		/* 如果处理失败，将路径指针置为错误指针，以便后续进入错误处理流程 */
		if (unlikely(err < 0))
			s = ERR_PTR(err);
	}

	/**
	 * 核心查找循环：
	 * 1. link_path_walk() 逐级遍历路径字符串中的各个组件
	 * 2. lookup_last() 处理路径的最后一个组件
	 * 当 link_path_walk 成功完成且 lookup_last 返回非 NULL（表示还有需追踪的链接）时继续循环
	 */
	while (!(err = link_path_walk(s, nd)) &&
	       (s = lookup_last(nd)) != NULL)
		;

	/* 如果路径遍历无错误，且标志位要求查找挂载点 */
	if (!err && unlikely(nd->flags & LOOKUP_MOUNTPOINT)) {
		/* 处理挂载点查找逻辑 */
		err = handle_lookup_down(nd);
		/* 清除 ND_JUMPED 状态标志，防止后续调用 d_weak_revalidate() */
		nd->state &= ~ND_JUMPED; // no d_weak_revalidate(), please...
	}

	/* 如果无错误，完成整个路径的遍历（进行一些内核内部的完整性确认及锁释放等） */
	if (!err)
		err = complete_walk(nd);

	/* 如果无错误，且标志位要求目标必须是一个目录 */
	if (!err && nd->flags & LOOKUP_DIRECTORY)
		/* 检查最终的 dentry 是否允许作为目录被查找，若不允许则返回 -ENOTDIR 错误 */
		if (!d_can_lookup(nd->path.dentry))
			err = -ENOTDIR;

	/* 如果一切顺利，没有发生错误 */
	if (!err) {
		/* 将查找到的路径信息拷贝到输出参数 path 中 */
		*path = nd->path;
		/* 将 nd 中的路径指针清空，转移路径的所有权，防止被后续操作释放 */
		nd->path.mnt = NULL;
		nd->path.dentry = NULL;
	}

	/* 终止本次查找，清理 nd 结构体中的临时状态和引用计数 */
	terminate_walk(nd);

	/* 返回错误码，0 表示成功 */
	return err;
}


int filename_lookup(int dfd, struct filename *name, unsigned flags,
		    struct path *path, const struct path *root)
{
	int retval;
	struct nameidata nd;
	if (IS_ERR(name))
		return PTR_ERR(name);
	set_nameidata(&nd, dfd, name, root);
	dbg("filename: %s\n", name->name);
	retval = path_lookupat(&nd, flags | LOOKUP_RCU, path);
	if (unlikely(retval == -ECHILD))
		retval = path_lookupat(&nd, flags, path);
	if (unlikely(retval == -ESTALE))
		retval = path_lookupat(&nd, flags | LOOKUP_REVAL, path);

	if (likely(!retval))
		audit_inode(name, path->dentry,
			    flags & LOOKUP_MOUNTPOINT ? AUDIT_INODE_NOEVAL : 0);
	restore_nameidata();
	return retval;
}

/* Returns 0 and nd will be valid on success; Returns error, otherwise. */
static int path_parentat(struct nameidata *nd, unsigned flags,
				struct path *parent)
{
	const char *s = path_init(nd, flags);
	int err = link_path_walk(s, nd);
	if (!err)
		err = complete_walk(nd);
	if (!err) {
		*parent = nd->path;
		nd->path.mnt = NULL;
		nd->path.dentry = NULL;
	}
	terminate_walk(nd);
	return err;
}

/* Note: this does not consume "name" */
static int __filename_parentat(int dfd, struct filename *name,
			       unsigned int flags, struct path *parent,
			       struct qstr *last, enum last_type *type,
			       const struct path *root)
{
	int retval;
	struct nameidata nd;

	if (IS_ERR(name))
		return PTR_ERR(name);
	set_nameidata(&nd, dfd, name, root);
	retval = path_parentat(&nd, flags | LOOKUP_RCU, parent);
	if (unlikely(retval == -ECHILD))
		retval = path_parentat(&nd, flags, parent);
	if (unlikely(retval == -ESTALE))
		retval = path_parentat(&nd, flags | LOOKUP_REVAL, parent);
	if (likely(!retval)) {
		*last = nd.last;
		*type = nd.last_type;
		audit_inode(name, parent->dentry, AUDIT_INODE_PARENT);
	}
	restore_nameidata();
	return retval;
}

static int filename_parentat(int dfd, struct filename *name,
			     unsigned int flags, struct path *parent,
			     struct qstr *last, enum last_type *type)
{
	return __filename_parentat(dfd, name, flags, parent, last, type, NULL);
}

static struct dentry *__start_dirop(struct dentry *parent, struct qstr *name,
				    unsigned int lookup_flags,
				    unsigned int state)
{
	struct dentry *dentry;
	struct inode *dir = d_inode(parent);

	if (state == TASK_KILLABLE) {
		int ret = down_write_killable_nested(&dir->i_rwsem,
						     I_MUTEX_PARENT);
		if (ret)
			return ERR_PTR(ret);
	} else {
		inode_lock_nested(dir, I_MUTEX_PARENT);
	}
	dentry = lookup_one_qstr_excl(name, parent, lookup_flags);
	if (IS_ERR(dentry))
		inode_unlock(dir);
	return dentry;
}

/**
 * start_dirop - begin a create or remove dirop, performing locking and lookup
 * @parent:       the dentry of the parent in which the operation will occur
 * @name:         a qstr holding the name within that parent
 * @lookup_flags: intent and other lookup flags.
 *
 * The lookup is performed and necessary locks are taken so that, on success,
 * the returned dentry can be operated on safely.
 * The qstr must already have the hash value calculated.
 *
 * Returns: a locked dentry, or an error.
 *
 */
struct dentry *start_dirop(struct dentry *parent, struct qstr *name,
			   unsigned int lookup_flags)
{
	return __start_dirop(parent, name, lookup_flags, TASK_NORMAL);
}

/**
 * end_dirop - signal completion of a dirop
 * @de: the dentry which was returned by start_dirop or similar.
 *
 * If the de is an error, nothing happens. Otherwise any lock taken to
 * protect the dentry is dropped and the dentry itself is release (dput()).
 */
void end_dirop(struct dentry *de)
{
	if (!IS_ERR(de)) {
		inode_unlock(de->d_parent->d_inode);
		dput(de);
	}
}
EXPORT_SYMBOL(end_dirop);

/* does lookup, returns the object with parent locked */
struct dentry *start_removing_path(const char *name, struct path *path)
{
	CLASS(filename_kernel, filename)(name);
	struct path parent_path __free(path_put) = {};
	struct dentry *d;
	struct qstr last;
	enum last_type type;
	int error;

	error = filename_parentat(AT_FDCWD, filename, 0, &parent_path, &last,
			&type);
	if (error)
		return ERR_PTR(error);
	if (unlikely(type != LAST_NORM))
		return ERR_PTR(-EINVAL);
	/* don't fail immediately if it's r/o, at least try to report other errors */
	error = mnt_want_write(parent_path.mnt);
	d = start_dirop(parent_path.dentry, &last, 0);
	if (IS_ERR(d))
		goto drop;
	if (error)
		goto fail;
	path->dentry = no_free_ptr(parent_path.dentry);
	path->mnt = no_free_ptr(parent_path.mnt);
	return d;

fail:
	end_dirop(d);
	d = ERR_PTR(error);
drop:
	if (!error)
		mnt_drop_write(parent_path.mnt);
	return d;
}

/**
 * kern_path_parent: lookup path returning parent and target
 * @name: path name
 * @path: path to store parent in
 *
 * The path @name should end with a normal component, not "." or ".." or "/".
 * A lookup is performed and if successful the parent information
 * is store in @parent and the dentry is returned.
 *
 * The dentry maybe negative, the parent will be positive.
 *
 * Returns:  dentry or error.
 */
struct dentry *kern_path_parent(const char *name, struct path *path)
{
	struct path parent_path __free(path_put) = {};
	CLASS(filename_kernel, filename)(name);
	struct dentry *d;
	struct qstr last;
	enum last_type type;
	int error;

	error = filename_parentat(AT_FDCWD, filename, 0, &parent_path, &last, &type);
	if (error)
		return ERR_PTR(error);
	if (unlikely(type != LAST_NORM))
		return ERR_PTR(-EINVAL);

	d = lookup_noperm_unlocked(&last, parent_path.dentry);
	if (IS_ERR(d))
		return d;
	path->dentry = no_free_ptr(parent_path.dentry);
	path->mnt = no_free_ptr(parent_path.mnt);
	return d;
}

int kern_path(const char *name, unsigned int flags, struct path *path)
{
	CLASS(filename_kernel, filename)(name);
	return filename_lookup(AT_FDCWD, filename, flags, path, NULL);
}
EXPORT_SYMBOL(kern_path);

/**
 * vfs_path_parent_lookup - lookup a parent path relative to a dentry-vfsmount pair
 * @filename: filename structure
 * @flags: lookup flags
 * @parent: pointer to struct path to fill
 * @last: last component
 * @root: pointer to struct path of the base directory
 */
int vfs_path_parent_lookup(struct filename *filename, unsigned int flags,
			   struct path *parent, struct qstr *last,
			   const struct path *root)
{
	enum last_type type;
	int err =  __filename_parentat(AT_FDCWD, filename, flags, parent, last,
				       &type, root);
	if (err)
		return err;
	if (unlikely(type != LAST_NORM)) {
		path_put(parent);
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL(vfs_path_parent_lookup);

/**
 * vfs_path_lookup - lookup a file path relative to a dentry-vfsmount pair
 * @dentry:  pointer to dentry of the base directory
 * @mnt: pointer to vfs mount of the base directory
 * @name: pointer to file name
 * @flags: lookup flags
 * @path: pointer to struct path to fill
 */
int vfs_path_lookup(struct dentry *dentry, struct vfsmount *mnt,
		    const char *name, unsigned int flags,
		    struct path *path)
{
	CLASS(filename_kernel, filename)(name);
	struct path root = {.mnt = mnt, .dentry = dentry};

	/* the first argument of filename_lookup() is ignored with root */
	return filename_lookup(AT_FDCWD, filename, flags, path, &root);
}
EXPORT_SYMBOL(vfs_path_lookup);

int lookup_noperm_common(struct qstr *qname, struct dentry *base)
{
	const char *name = qname->name;
	u32 len = qname->len;

	qname->hash = full_name_hash(base, name, len);
	if (!len)
		return -EACCES;

	if (name_is_dot_dotdot(name, len))
		return -EACCES;

	while (len--) {
		unsigned int c = *(const unsigned char *)name++;
		if (c == '/' || c == '\0')
			return -EACCES;
	}
	/*
	 * See if the low-level filesystem might want
	 * to use its own hash..
	 */
	if (base->d_flags & DCACHE_OP_HASH) {
		int err = base->d_op->d_hash(base, qname);
		if (err < 0)
			return err;
	}
	return 0;
}

static int lookup_one_common(struct mnt_idmap *idmap,
			     struct qstr *qname, struct dentry *base)
{
	int err;
	err = lookup_noperm_common(qname, base);
	if (err < 0)
		return err;
	return inode_permission(idmap, base->d_inode, MAY_EXEC);
}

/**
 * try_lookup_noperm - filesystem helper to lookup single pathname component
 * @name:	qstr storing pathname component to lookup
 * @base:	base directory to lookup from
 *
 * Look up a dentry by name in the dcache, returning NULL if it does not
 * currently exist or an error if there is a problem with the name.
 * The function does not try to create a dentry and if one
 * is found it doesn't try to revalidate it.
 *
 * Note that this routine is purely a helper for filesystem usage and should
 * not be called by generic code.  It does no permission checking.
 *
 * No locks need be held - only a counted reference to @base is needed.
 *
 * Returns:
 *   - ref-counted dentry on success, or
 *   - %NULL if name could not be found, or
 *   - ERR_PTR(-EACCES) if name is dot or dotdot or contains a slash or nul, or
 *   - ERR_PTR() if fs provide ->d_hash, and this returned an error.
 */
struct dentry *try_lookup_noperm(struct qstr *name, struct dentry *base)
{
	int err;

	err = lookup_noperm_common(name, base);
	if (err)
		return ERR_PTR(err);

	return d_lookup(base, name);
}
EXPORT_SYMBOL(try_lookup_noperm);

/**
 * lookup_noperm - filesystem helper to lookup single pathname component
 * @name:	qstr storing pathname component to lookup
 * @base:	base directory to lookup from
 *
 * Note that this routine is purely a helper for filesystem usage and should
 * not be called by generic code.  It does no permission checking.
 *
 * The caller must hold base->i_rwsem.
 */
struct dentry *lookup_noperm(struct qstr *name, struct dentry *base)
{
	struct dentry *dentry;
	int err;

	WARN_ON_ONCE(!inode_is_locked(base->d_inode));

	err = lookup_noperm_common(name, base);
	if (err)
		return ERR_PTR(err);

	dentry = lookup_dcache(name, base, 0);
	return dentry ? dentry : __lookup_slow(name, base, 0);
}
EXPORT_SYMBOL(lookup_noperm);

/**
 * lookup_one - lookup single pathname component
 * @idmap:	idmap of the mount the lookup is performed from
 * @name:	qstr holding pathname component to lookup
 * @base:	base directory to lookup from
 *
 * This can be used for in-kernel filesystem clients such as file servers.
 *
 * The caller must hold base->i_rwsem.
 */
struct dentry *lookup_one(struct mnt_idmap *idmap, struct qstr *name,
			  struct dentry *base)
{
	struct dentry *dentry;
	int err;

	WARN_ON_ONCE(!inode_is_locked(base->d_inode));

	err = lookup_one_common(idmap, name, base);
	if (err)
		return ERR_PTR(err);

	dentry = lookup_dcache(name, base, 0);
	return dentry ? dentry : __lookup_slow(name, base, 0);
}
EXPORT_SYMBOL(lookup_one);

/**
 * lookup_one_unlocked - lookup single pathname component
 * @idmap:	idmap of the mount the lookup is performed from
 * @name:	qstr olding pathname component to lookup
 * @base:	base directory to lookup from
 *
 * This can be used for in-kernel filesystem clients such as file servers.
 *
 * Unlike lookup_one, it should be called without the parent
 * i_rwsem held, and will take the i_rwsem itself if necessary.
 *
 * Returns: - A dentry, possibly negative, or
 *	    - same errors as try_lookup_noperm() or
 *	    - ERR_PTR(-ENOENT) if parent has been removed, or
 *	    - ERR_PTR(-EACCES) if parent directory is not searchable.
 */
struct dentry *lookup_one_unlocked(struct mnt_idmap *idmap, struct qstr *name,
				   struct dentry *base)
{
	int err;
	struct dentry *ret;

	err = lookup_one_common(idmap, name, base);
	if (err)
		return ERR_PTR(err);

	ret = lookup_dcache(name, base, 0);
	if (!ret)
		ret = lookup_slow(name, base, 0);
	return ret;
}
EXPORT_SYMBOL(lookup_one_unlocked);

/**
 * lookup_one_positive_killable - lookup single pathname component
 * @idmap:	idmap of the mount the lookup is performed from
 * @name:	qstr olding pathname component to lookup
 * @base:	base directory to lookup from
 *
 * This helper will yield ERR_PTR(-ENOENT) on negatives. The helper returns
 * known positive or ERR_PTR(). This is what most of the users want.
 *
 * Note that pinned negative with unlocked parent _can_ become positive at any
 * time, so callers of lookup_one_unlocked() need to be very careful; pinned
 * positives have >d_inode stable, so this one avoids such problems.
 *
 * This can be used for in-kernel filesystem clients such as file servers.
 *
 * It should be called without the parent i_rwsem held, and will take
 * the i_rwsem itself if necessary.  If a fatal signal is pending or
 * delivered, it will return %-EINTR if the lock is needed.
 *
 * Returns: A dentry, possibly negative, or
 *	   - same errors as lookup_one_unlocked() or
 *	   - ERR_PTR(-EINTR) if a fatal signal is pending.
 */
struct dentry *lookup_one_positive_killable(struct mnt_idmap *idmap,
					    struct qstr *name,
					    struct dentry *base)
{
	int err;
	struct dentry *ret;

	err = lookup_one_common(idmap, name, base);
	if (err)
		return ERR_PTR(err);

	ret = lookup_dcache(name, base, 0);
	if (!ret)
		ret = lookup_slow_killable(name, base, 0);
	if (!IS_ERR(ret) && d_flags_negative(smp_load_acquire(&ret->d_flags))) {
		dput(ret);
		ret = ERR_PTR(-ENOENT);
	}
	return ret;
}
EXPORT_SYMBOL(lookup_one_positive_killable);

/**
 * lookup_one_positive_unlocked - lookup single pathname component
 * @idmap:	idmap of the mount the lookup is performed from
 * @name:	qstr holding pathname component to lookup
 * @base:	base directory to lookup from
 *
 * This helper will yield ERR_PTR(-ENOENT) on negatives. The helper returns
 * known positive or ERR_PTR(). This is what most of the users want.
 *
 * Note that pinned negative with unlocked parent _can_ become positive at any
 * time, so callers of lookup_one_unlocked() need to be very careful; pinned
 * positives have >d_inode stable, so this one avoids such problems.
 *
 * This can be used for in-kernel filesystem clients such as file servers.
 *
 * The helper should be called without i_rwsem held.
 *
 * Returns: A positive dentry, or
 *	   - ERR_PTR(-ENOENT) if the name could not be found, or
 *	   - same errors as lookup_one_unlocked().
 */
struct dentry *lookup_one_positive_unlocked(struct mnt_idmap *idmap,
					    struct qstr *name,
					    struct dentry *base)
{
	struct dentry *ret = lookup_one_unlocked(idmap, name, base);

	if (!IS_ERR(ret) && d_flags_negative(smp_load_acquire(&ret->d_flags))) {
		dput(ret);
		ret = ERR_PTR(-ENOENT);
	}
	return ret;
}
EXPORT_SYMBOL(lookup_one_positive_unlocked);

/**
 * lookup_noperm_unlocked - filesystem helper to lookup single pathname component
 * @name:	pathname component to lookup
 * @base:	base directory to lookup from
 *
 * Note that this routine is purely a helper for filesystem usage and should
 * not be called by generic code. It does no permission checking.
 *
 * Unlike lookup_noperm(), it should be called without the parent
 * i_rwsem held, and will take the i_rwsem itself if necessary.
 *
 * Unlike try_lookup_noperm() it *does* revalidate the dentry if it already
 * existed.
 *
 * Returns: A dentry, possibly negative, or
 *	   - ERR_PTR(-ENOENT) if parent has been removed, or
 *	   - same errors as try_lookup_noperm()
 */
struct dentry *lookup_noperm_unlocked(struct qstr *name, struct dentry *base)
{
	struct dentry *ret;
	int err;

	err = lookup_noperm_common(name, base);
	if (err)
		return ERR_PTR(err);

	ret = lookup_dcache(name, base, 0);
	if (!ret)
		ret = lookup_slow(name, base, 0);
	return ret;
}
EXPORT_SYMBOL(lookup_noperm_unlocked);

/*
 * Like lookup_noperm_unlocked(), except that it yields ERR_PTR(-ENOENT)
 * on negatives.  Returns known positive or ERR_PTR(); that's what
 * most of the users want.  Note that pinned negative with unlocked parent
 * _can_ become positive at any time, so callers of lookup_noperm_unlocked()
 * need to be very careful; pinned positives have ->d_inode stable, so
 * this one avoids such problems.
 *
 * Returns: A positive dentry, or
 *	   - ERR_PTR(-ENOENT) if name cannot be found or parent has been removed, or
 *	   - same errors as try_lookup_noperm()
 */
struct dentry *lookup_noperm_positive_unlocked(struct qstr *name,
					       struct dentry *base)
{
	struct dentry *ret;

	ret = lookup_noperm_unlocked(name, base);
	if (!IS_ERR(ret) && d_flags_negative(smp_load_acquire(&ret->d_flags))) {
		dput(ret);
		ret = ERR_PTR(-ENOENT);
	}
	return ret;
}
EXPORT_SYMBOL(lookup_noperm_positive_unlocked);

/**
 * start_creating - prepare to create a given name with permission checking
 * @idmap:  idmap of the mount
 * @parent: directory in which to prepare to create the name
 * @name:   the name to be created
 *
 * Locks are taken and a lookup is performed prior to creating
 * an object in a directory.  Permission checking (MAY_EXEC) is performed
 * against @idmap.
 *
 * If the name already exists, a positive dentry is returned, so
 * behaviour is similar to O_CREAT without O_EXCL, which doesn't fail
 * with -EEXIST.
 *
 * Returns: a negative or positive dentry, or an error.
 */
struct dentry *start_creating(struct mnt_idmap *idmap, struct dentry *parent,
			      struct qstr *name)
{
	int err = lookup_one_common(idmap, name, parent);

	if (err)
		return ERR_PTR(err);
	return start_dirop(parent, name, LOOKUP_CREATE);
}
EXPORT_SYMBOL(start_creating);

/**
 * start_removing - prepare to remove a given name with permission checking
 * @idmap:  idmap of the mount
 * @parent: directory in which to find the name
 * @name:   the name to be removed
 *
 * Locks are taken and a lookup in performed prior to removing
 * an object from a directory.  Permission checking (MAY_EXEC) is performed
 * against @idmap.
 *
 * If the name doesn't exist, an error is returned.
 *
 * end_removing() should be called when removal is complete, or aborted.
 *
 * Returns: a positive dentry, or an error.
 */
struct dentry *start_removing(struct mnt_idmap *idmap, struct dentry *parent,
			      struct qstr *name)
{
	int err = lookup_one_common(idmap, name, parent);

	if (err)
		return ERR_PTR(err);
	return start_dirop(parent, name, 0);
}
EXPORT_SYMBOL(start_removing);

/**
 * start_creating_killable - prepare to create a given name with permission checking
 * @idmap:  idmap of the mount
 * @parent: directory in which to prepare to create the name
 * @name:   the name to be created
 *
 * Locks are taken and a lookup in performed prior to creating
 * an object in a directory.  Permission checking (MAY_EXEC) is performed
 * against @idmap.
 *
 * If the name already exists, a positive dentry is returned.
 *
 * If a signal is received or was already pending, the function aborts
 * with -EINTR;
 *
 * Returns: a negative or positive dentry, or an error.
 */
struct dentry *start_creating_killable(struct mnt_idmap *idmap,
				       struct dentry *parent,
				       struct qstr *name)
{
	int err = lookup_one_common(idmap, name, parent);

	if (err)
		return ERR_PTR(err);
	return __start_dirop(parent, name, LOOKUP_CREATE, TASK_KILLABLE);
}
EXPORT_SYMBOL(start_creating_killable);

/**
 * start_removing_killable - prepare to remove a given name with permission checking
 * @idmap:  idmap of the mount
 * @parent: directory in which to find the name
 * @name:   the name to be removed
 *
 * Locks are taken and a lookup in performed prior to removing
 * an object from a directory.  Permission checking (MAY_EXEC) is performed
 * against @idmap.
 *
 * If the name doesn't exist, an error is returned.
 *
 * end_removing() should be called when removal is complete, or aborted.
 *
 * If a signal is received or was already pending, the function aborts
 * with -EINTR;
 *
 * Returns: a positive dentry, or an error.
 */
struct dentry *start_removing_killable(struct mnt_idmap *idmap,
				       struct dentry *parent,
				       struct qstr *name)
{
	int err = lookup_one_common(idmap, name, parent);

	if (err)
		return ERR_PTR(err);
	return __start_dirop(parent, name, 0, TASK_KILLABLE);
}
EXPORT_SYMBOL(start_removing_killable);

/**
 * start_creating_noperm - prepare to create a given name without permission checking
 * @parent: directory in which to prepare to create the name
 * @name:   the name to be created
 *
 * Locks are taken and a lookup in performed prior to creating
 * an object in a directory.
 *
 * If the name already exists, a positive dentry is returned.
 *
 * Returns: a negative or positive dentry, or an error.
 */
struct dentry *start_creating_noperm(struct dentry *parent,
				     struct qstr *name)
{
	int err = lookup_noperm_common(name, parent);

	if (err)
		return ERR_PTR(err);
	return start_dirop(parent, name, LOOKUP_CREATE);
}
EXPORT_SYMBOL(start_creating_noperm);

/**
 * start_removing_noperm - prepare to remove a given name without permission checking
 * @parent: directory in which to find the name
 * @name:   the name to be removed
 *
 * Locks are taken and a lookup in performed prior to removing
 * an object from a directory.
 *
 * If the name doesn't exist, an error is returned.
 *
 * end_removing() should be called when removal is complete, or aborted.
 *
 * Returns: a positive dentry, or an error.
 */
struct dentry *start_removing_noperm(struct dentry *parent,
				     struct qstr *name)
{
	int err = lookup_noperm_common(name, parent);

	if (err)
		return ERR_PTR(err);
	return start_dirop(parent, name, 0);
}
EXPORT_SYMBOL(start_removing_noperm);

/**
 * start_creating_dentry - prepare to create a given dentry
 * @parent: directory from which dentry should be removed
 * @child:  the dentry to be removed
 *
 * A lock is taken to protect the dentry again other dirops and
 * the validity of the dentry is checked: correct parent and still hashed.
 *
 * If the dentry is valid and negative a reference is taken and
 * returned.  If not an error is returned.
 *
 * end_creating() should be called when creation is complete, or aborted.
 *
 * Returns: the valid dentry, or an error.
 */
struct dentry *start_creating_dentry(struct dentry *parent,
				     struct dentry *child)
{
	inode_lock_nested(parent->d_inode, I_MUTEX_PARENT);
	if (unlikely(IS_DEADDIR(parent->d_inode) ||
		     child->d_parent != parent ||
		     d_unhashed(child))) {
		inode_unlock(parent->d_inode);
		return ERR_PTR(-EINVAL);
	}
	if (d_is_positive(child)) {
		inode_unlock(parent->d_inode);
		return ERR_PTR(-EEXIST);
	}
	return dget(child);
}
EXPORT_SYMBOL(start_creating_dentry);

/**
 * start_removing_dentry - prepare to remove a given dentry
 * @parent: directory from which dentry should be removed
 * @child:  the dentry to be removed
 *
 * A lock is taken to protect the dentry again other dirops and
 * the validity of the dentry is checked: correct parent and still hashed.
 *
 * If the dentry is valid and positive, a reference is taken and
 * returned.  If not an error is returned.
 *
 * end_removing() should be called when removal is complete, or aborted.
 *
 * Returns: the valid dentry, or an error.
 */
struct dentry *start_removing_dentry(struct dentry *parent,
				     struct dentry *child)
{
	inode_lock_nested(parent->d_inode, I_MUTEX_PARENT);
	if (unlikely(IS_DEADDIR(parent->d_inode) ||
		     child->d_parent != parent ||
		     d_unhashed(child))) {
		inode_unlock(parent->d_inode);
		return ERR_PTR(-EINVAL);
	}
	if (d_is_negative(child)) {
		inode_unlock(parent->d_inode);
		return ERR_PTR(-ENOENT);
	}
	return dget(child);
}
EXPORT_SYMBOL(start_removing_dentry);

#ifdef CONFIG_UNIX98_PTYS
int path_pts(struct path *path)
{
	/* Find something mounted on "pts" in the same directory as
	 * the input path.
	 */
	struct dentry *parent = dget_parent(path->dentry);
	struct dentry *child;

	if (unlikely(!path_connected(path->mnt, parent))) {
		dput(parent);
		return -ENOENT;
	}
	dput(path->dentry);
	path->dentry = parent;
	child = d_hash_and_lookup(parent, &QSTR("pts"));
	if (IS_ERR_OR_NULL(child))
		return -ENOENT;

	path->dentry = child;
	dput(parent);
	follow_down(path, 0);
	return 0;
}
#endif

int user_path_at(int dfd, const char __user *name, unsigned flags,
		 struct path *path)
{
	CLASS(filename_flags, filename)(name, flags);
	dbg("mount dir: %s\n", filename->name);
	// 根据文件名查找
	// path->dentry: 指向挂载点目录（如 /mnt/usb）在父文件系统中的 dentry
	// path->mnt: 指向这个挂载点所属的父文件系统的 vfsmount 实例
	return filename_lookup(dfd, filename, flags, path, NULL);
}
EXPORT_SYMBOL(user_path_at);

int __check_sticky(struct mnt_idmap *idmap, struct inode *dir,
		   struct inode *inode)
{
	kuid_t fsuid = current_fsuid();

	if (vfsuid_eq_kuid(i_uid_into_vfsuid(idmap, inode), fsuid))
		return 0;
	if (vfsuid_eq_kuid(i_uid_into_vfsuid(idmap, dir), fsuid))
		return 0;
	return !capable_wrt_inode_uidgid(idmap, inode, CAP_FOWNER);
}
EXPORT_SYMBOL(__check_sticky);

/*
 *	Check whether we can remove a link victim from directory dir, check
 *  whether the type of victim is right.
 *  1. We can't do it if dir is read-only (done in permission())
 *  2. We should have write and exec permissions on dir
 *  3. We can't remove anything from append-only dir
 *  4. We can't do anything with immutable dir (done in permission())
 *  5. If the sticky bit on dir is set we should either
 *	a. be owner of dir, or
 *	b. be owner of victim, or
 *	c. have CAP_FOWNER capability
 *  6. If the victim is append-only or immutable we can't do antyhing with
 *     links pointing to it.
 *  7. If the victim has an unknown uid or gid we can't change the inode.
 *  8. If we were asked to remove a directory and victim isn't one - ENOTDIR.
 *  9. If we were asked to remove a non-directory and victim isn't one - EISDIR.
 * 10. We can't remove a root or mountpoint.
 * 11. We don't allow removal of NFS sillyrenamed files; it's handled by
 *     nfs_async_unlink().
 */
int may_delete_dentry(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *victim, bool isdir)
{
	struct inode *inode = d_backing_inode(victim);
	int error;

	if (d_is_negative(victim))
		return -ENOENT;
	BUG_ON(!inode);

	BUG_ON(victim->d_parent->d_inode != dir);

	/* Inode writeback is not safe when the uid or gid are invalid. */
	if (!vfsuid_valid(i_uid_into_vfsuid(idmap, inode)) ||
	    !vfsgid_valid(i_gid_into_vfsgid(idmap, inode)))
		return -EOVERFLOW;

	audit_inode_child(dir, victim, AUDIT_TYPE_CHILD_DELETE);

	error = inode_permission(idmap, dir, MAY_WRITE | MAY_EXEC);
	if (error)
		return error;
	if (IS_APPEND(dir))
		return -EPERM;

	if (check_sticky(idmap, dir, inode) || IS_APPEND(inode) ||
	    IS_IMMUTABLE(inode) || IS_SWAPFILE(inode) ||
	    HAS_UNMAPPED_ID(idmap, inode))
		return -EPERM;
	if (isdir) {
		if (!d_is_dir(victim))
			return -ENOTDIR;
		if (IS_ROOT(victim))
			return -EBUSY;
	} else if (d_is_dir(victim))
		return -EISDIR;
	if (IS_DEADDIR(dir))
		return -ENOENT;
	if (victim->d_flags & DCACHE_NFSFS_RENAMED)
		return -EBUSY;
	return 0;
}
EXPORT_SYMBOL(may_delete_dentry);

/*	Check whether we can create an object with dentry child in directory
 *  dir.
 *  1. We can't do it if child already exists (open has special treatment for
 *     this case, but since we are inlined it's OK)
 *  2. We can't do it if dir is read-only (done in permission())
 *  3. We can't do it if the fs can't represent the fsuid or fsgid.
 *  4. We should have write and exec permissions on dir
 *  5. We can't do it if dir is immutable (done in permission())
 */
int may_create_dentry(struct mnt_idmap *idmap,
		      struct inode *dir, struct dentry *child)
{
	audit_inode_child(dir, child, AUDIT_TYPE_CHILD_CREATE);
	if (child->d_inode)
		return -EEXIST;
	if (IS_DEADDIR(dir))
		return -ENOENT;
	if (!fsuidgid_has_mapping(dir->i_sb, idmap))
		return -EOVERFLOW;

	return inode_permission(idmap, dir, MAY_WRITE | MAY_EXEC);
}
EXPORT_SYMBOL(may_create_dentry);

// p1 != p2, both are on the same filesystem, ->s_vfs_rename_mutex is held
static struct dentry *lock_two_directories(struct dentry *p1, struct dentry *p2)
{
	struct dentry *p = p1, *q = p2, *r;

	while ((r = p->d_parent) != p2 && r != p)
		p = r;
	if (r == p2) {
		// p is a child of p2 and an ancestor of p1 or p1 itself
		inode_lock_nested(p2->d_inode, I_MUTEX_PARENT);
		inode_lock_nested(p1->d_inode, I_MUTEX_PARENT2);
		return p;
	}
	// p is the root of connected component that contains p1
	// p2 does not occur on the path from p to p1
	while ((r = q->d_parent) != p1 && r != p && r != q)
		q = r;
	if (r == p1) {
		// q is a child of p1 and an ancestor of p2 or p2 itself
		inode_lock_nested(p1->d_inode, I_MUTEX_PARENT);
		inode_lock_nested(p2->d_inode, I_MUTEX_PARENT2);
		return q;
	} else if (likely(r == p)) {
		// both p2 and p1 are descendents of p
		inode_lock_nested(p1->d_inode, I_MUTEX_PARENT);
		inode_lock_nested(p2->d_inode, I_MUTEX_PARENT2);
		return NULL;
	} else { // no common ancestor at the time we'd been called
		mutex_unlock(&p1->d_sb->s_vfs_rename_mutex);
		return ERR_PTR(-EXDEV);
	}
}

/*
 * p1 and p2 should be directories on the same fs.
 */
static struct dentry *lock_rename(struct dentry *p1, struct dentry *p2)
{
	if (p1 == p2) {
		inode_lock_nested(p1->d_inode, I_MUTEX_PARENT);
		return NULL;
	}

	mutex_lock(&p1->d_sb->s_vfs_rename_mutex);
	return lock_two_directories(p1, p2);
}

/*
 * c1 and p2 should be on the same fs.
 */
static struct dentry *lock_rename_child(struct dentry *c1, struct dentry *p2)
{
	if (READ_ONCE(c1->d_parent) == p2) {
		/*
		 * hopefully won't need to touch ->s_vfs_rename_mutex at all.
		 */
		inode_lock_nested(p2->d_inode, I_MUTEX_PARENT);
		/*
		 * now that p2 is locked, nobody can move in or out of it,
		 * so the test below is safe.
		 */
		if (likely(c1->d_parent == p2))
			return NULL;

		/*
		 * c1 got moved out of p2 while we'd been taking locks;
		 * unlock and fall back to slow case.
		 */
		inode_unlock(p2->d_inode);
	}

	mutex_lock(&c1->d_sb->s_vfs_rename_mutex);
	/*
	 * nobody can move out of any directories on this fs.
	 */
	if (likely(c1->d_parent != p2))
		return lock_two_directories(c1->d_parent, p2);

	/*
	 * c1 got moved into p2 while we were taking locks;
	 * we need p2 locked and ->s_vfs_rename_mutex unlocked,
	 * for consistency with lock_rename().
	 */
	inode_lock_nested(p2->d_inode, I_MUTEX_PARENT);
	mutex_unlock(&c1->d_sb->s_vfs_rename_mutex);
	return NULL;
}

static void unlock_rename(struct dentry *p1, struct dentry *p2)
{
	inode_unlock(p1->d_inode);
	if (p1 != p2) {
		inode_unlock(p2->d_inode);
		mutex_unlock(&p1->d_sb->s_vfs_rename_mutex);
	}
}

/**
 * __start_renaming - lookup and lock names for rename
 * @rd:           rename data containing parents and flags, and
 *                for receiving found dentries
 * @lookup_flags: extra flags to pass to ->lookup (e.g. LOOKUP_REVAL,
 *                LOOKUP_NO_SYMLINKS etc).
 * @old_last:     name of object in @rd.old_parent
 * @new_last:     name of object in @rd.new_parent
 *
 * Look up two names and ensure locks are in place for
 * rename.
 *
 * On success the found dentries are stored in @rd.old_dentry,
 * @rd.new_dentry and an extra ref is taken on @rd.old_parent.
 * These references and the lock are dropped by end_renaming().
 *
 * The passed in qstrs must have the hash calculated, and no permission
 * checking is performed.
 *
 * Returns: zero or an error.
 */
static int
__start_renaming(struct renamedata *rd, int lookup_flags,
		 struct qstr *old_last, struct qstr *new_last)
{
	struct dentry *trap;
	struct dentry *d1, *d2;
	int target_flags = LOOKUP_RENAME_TARGET | LOOKUP_CREATE;
	int err;

	if (rd->flags & RENAME_EXCHANGE)
		target_flags = 0;
	if (rd->flags & RENAME_NOREPLACE)
		target_flags |= LOOKUP_EXCL;

	trap = lock_rename(rd->old_parent, rd->new_parent);
	if (IS_ERR(trap))
		return PTR_ERR(trap);

	d1 = lookup_one_qstr_excl(old_last, rd->old_parent,
				  lookup_flags);
	err = PTR_ERR(d1);
	if (IS_ERR(d1))
		goto out_unlock;

	d2 = lookup_one_qstr_excl(new_last, rd->new_parent,
				  lookup_flags | target_flags);
	err = PTR_ERR(d2);
	if (IS_ERR(d2))
		goto out_dput_d1;

	if (d1 == trap) {
		/* source is an ancestor of target */
		err = -EINVAL;
		goto out_dput_d2;
	}

	if (d2 == trap) {
		/* target is an ancestor of source */
		if (rd->flags & RENAME_EXCHANGE)
			err = -EINVAL;
		else
			err = -ENOTEMPTY;
		goto out_dput_d2;
	}

	rd->old_dentry = d1;
	rd->new_dentry = d2;
	dget(rd->old_parent);
	return 0;

out_dput_d2:
	dput(d2);
out_dput_d1:
	dput(d1);
out_unlock:
	unlock_rename(rd->old_parent, rd->new_parent);
	return err;
}

/**
 * start_renaming - lookup and lock names for rename with permission checking
 * @rd:           rename data containing parents and flags, and
 *                for receiving found dentries
 * @lookup_flags: extra flags to pass to ->lookup (e.g. LOOKUP_REVAL,
 *                LOOKUP_NO_SYMLINKS etc).
 * @old_last:     name of object in @rd.old_parent
 * @new_last:     name of object in @rd.new_parent
 *
 * Look up two names and ensure locks are in place for
 * rename.
 *
 * On success the found dentries are stored in @rd.old_dentry,
 * @rd.new_dentry.  Also the refcount on @rd->old_parent is increased.
 * These references and the lock are dropped by end_renaming().
 *
 * The passed in qstrs need not have the hash calculated, and basic
 * eXecute permission checking is performed against @rd.mnt_idmap.
 *
 * Returns: zero or an error.
 */
int start_renaming(struct renamedata *rd, int lookup_flags,
		   struct qstr *old_last, struct qstr *new_last)
{
	int err;

	err = lookup_one_common(rd->mnt_idmap, old_last, rd->old_parent);
	if (err)
		return err;
	err = lookup_one_common(rd->mnt_idmap, new_last, rd->new_parent);
	if (err)
		return err;
	return __start_renaming(rd, lookup_flags, old_last, new_last);
}
EXPORT_SYMBOL(start_renaming);

static int
__start_renaming_dentry(struct renamedata *rd, int lookup_flags,
			struct dentry *old_dentry, struct qstr *new_last)
{
	struct dentry *trap;
	struct dentry *d2;
	int target_flags = LOOKUP_RENAME_TARGET | LOOKUP_CREATE;
	int err;

	if (rd->flags & RENAME_EXCHANGE)
		target_flags = 0;
	if (rd->flags & RENAME_NOREPLACE)
		target_flags |= LOOKUP_EXCL;

	/* Already have the dentry - need to be sure to lock the correct parent */
	trap = lock_rename_child(old_dentry, rd->new_parent);
	if (IS_ERR(trap))
		return PTR_ERR(trap);
	if (d_unhashed(old_dentry) ||
	    (rd->old_parent && rd->old_parent != old_dentry->d_parent)) {
		/* dentry was removed, or moved and explicit parent requested */
		err = -EINVAL;
		goto out_unlock;
	}

	d2 = lookup_one_qstr_excl(new_last, rd->new_parent,
				  lookup_flags | target_flags);
	err = PTR_ERR(d2);
	if (IS_ERR(d2))
		goto out_unlock;

	if (old_dentry == trap) {
		/* source is an ancestor of target */
		err = -EINVAL;
		goto out_dput_d2;
	}

	if (d2 == trap) {
		/* target is an ancestor of source */
		if (rd->flags & RENAME_EXCHANGE)
			err = -EINVAL;
		else
			err = -ENOTEMPTY;
		goto out_dput_d2;
	}

	rd->old_dentry = dget(old_dentry);
	rd->new_dentry = d2;
	rd->old_parent = dget(old_dentry->d_parent);
	return 0;

out_dput_d2:
	dput(d2);
out_unlock:
	unlock_rename(old_dentry->d_parent, rd->new_parent);
	return err;
}

/**
 * start_renaming_dentry - lookup and lock name for rename with permission checking
 * @rd:           rename data containing parents and flags, and
 *                for receiving found dentries
 * @lookup_flags: extra flags to pass to ->lookup (e.g. LOOKUP_REVAL,
 *                LOOKUP_NO_SYMLINKS etc).
 * @old_dentry:   dentry of name to move
 * @new_last:     name of target in @rd.new_parent
 *
 * Look up target name and ensure locks are in place for
 * rename.
 *
 * On success the found dentry is stored in @rd.new_dentry and
 * @rd.old_parent is confirmed to be the parent of @old_dentry.  If it
 * was originally %NULL, it is set.  In either case a reference is taken
 * so that end_renaming() can have a stable reference to unlock.
 *
 * References and the lock can be dropped with end_renaming()
 *
 * The passed in qstr need not have the hash calculated, and basic
 * eXecute permission checking is performed against @rd.mnt_idmap.
 *
 * Returns: zero or an error.
 */
int start_renaming_dentry(struct renamedata *rd, int lookup_flags,
			  struct dentry *old_dentry, struct qstr *new_last)
{
	int err;

	err = lookup_one_common(rd->mnt_idmap, new_last, rd->new_parent);
	if (err)
		return err;
	return __start_renaming_dentry(rd, lookup_flags, old_dentry, new_last);
}
EXPORT_SYMBOL(start_renaming_dentry);

/**
 * start_renaming_two_dentries - Lock to dentries in given parents for rename
 * @rd:           rename data containing parent
 * @old_dentry:   dentry of name to move
 * @new_dentry:   dentry to move to
 *
 * Ensure locks are in place for rename and check parentage is still correct.
 *
 * On success the two dentries are stored in @rd.old_dentry and
 * @rd.new_dentry and @rd.old_parent and @rd.new_parent are confirmed to
 * be the parents of the dentries.
 *
 * References and the lock can be dropped with end_renaming()
 *
 * Returns: zero or an error.
 */
int
start_renaming_two_dentries(struct renamedata *rd,
			    struct dentry *old_dentry, struct dentry *new_dentry)
{
	struct dentry *trap;
	int err;

	/* Already have the dentry - need to be sure to lock the correct parent */
	trap = lock_rename_child(old_dentry, rd->new_parent);
	if (IS_ERR(trap))
		return PTR_ERR(trap);
	err = -EINVAL;
	if (d_unhashed(old_dentry) ||
	    (rd->old_parent && rd->old_parent != old_dentry->d_parent))
		/* old_dentry was removed, or moved and explicit parent requested */
		goto out_unlock;
	if (d_unhashed(new_dentry) ||
	    rd->new_parent != new_dentry->d_parent)
		/* new_dentry was removed or moved */
		goto out_unlock;

	if (old_dentry == trap)
		/* source is an ancestor of target */
		goto out_unlock;

	if (new_dentry == trap) {
		/* target is an ancestor of source */
		if (rd->flags & RENAME_EXCHANGE)
			err = -EINVAL;
		else
			err = -ENOTEMPTY;
		goto out_unlock;
	}

	err = -EEXIST;
	if (d_is_positive(new_dentry) && (rd->flags & RENAME_NOREPLACE))
		goto out_unlock;

	rd->old_dentry = dget(old_dentry);
	rd->new_dentry = dget(new_dentry);
	rd->old_parent = dget(old_dentry->d_parent);
	return 0;

out_unlock:
	unlock_rename(old_dentry->d_parent, rd->new_parent);
	return err;
}
EXPORT_SYMBOL(start_renaming_two_dentries);

void end_renaming(struct renamedata *rd)
{
	unlock_rename(rd->old_parent, rd->new_parent);
	dput(rd->old_dentry);
	dput(rd->new_dentry);
	dput(rd->old_parent);
}
EXPORT_SYMBOL(end_renaming);

/**
 * vfs_prepare_mode - prepare the mode to be used for a new inode
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	parent directory of the new inode
 * @mode:	mode of the new inode
 * @mask_perms:	allowed permission by the vfs
 * @type:	type of file to be created
 *
 * This helper consolidates and enforces vfs restrictions on the @mode of a new
 * object to be created.
 *
 * Umask stripping depends on whether the filesystem supports POSIX ACLs (see
 * the kernel documentation for mode_strip_umask()). Moving umask stripping
 * after setgid stripping allows the same ordering for both non-POSIX ACL and
 * POSIX ACL supporting filesystems.
 *
 * Note that it's currently valid for @type to be 0 if a directory is created.
 * Filesystems raise that flag individually and we need to check whether each
 * filesystem can deal with receiving S_IFDIR from the vfs before we enforce a
 * non-zero type.
 *
 * Returns: mode to be passed to the filesystem
 */
static inline umode_t vfs_prepare_mode(struct mnt_idmap *idmap,
				       const struct inode *dir, umode_t mode,
				       umode_t mask_perms, umode_t type)
{
	mode = mode_strip_sgid(idmap, dir, mode);
	mode = mode_strip_umask(dir, mode);

	/*
	 * Apply the vfs mandated allowed permission mask and set the type of
	 * file to be created before we call into the filesystem.
	 */
	mode &= (mask_perms & ~S_IFMT);
	mode |= (type & S_IFMT);

	return mode;
}

/**
 * vfs_create - create new file
 * @idmap:	idmap of the mount the inode was found from
 * @dentry:	dentry of the child file
 * @mode:	mode of the child file
 * @di:		returns parent inode, if the inode is delegated.
 *
 * Create a new file.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_create(struct mnt_idmap *idmap, struct dentry *dentry, umode_t mode,
	       struct delegated_inode *di)
{
	struct inode *dir = d_inode(dentry->d_parent);
	int error;

	error = may_create_dentry(idmap, dir, dentry);
	if (error)
		return error;

	if (!dir->i_op->create)
		return -EACCES;	/* shouldn't it be ENOSYS? */

	mode = vfs_prepare_mode(idmap, dir, mode, S_IALLUGO, S_IFREG);
	error = security_inode_create(dir, dentry, mode);
	if (error)
		return error;
	error = try_break_deleg(dir, LEASE_BREAK_DIR_CREATE, di);
	if (error)
		return error;
	error = dir->i_op->create(idmap, dir, dentry, mode, true);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_create);

int vfs_mkobj(struct dentry *dentry, umode_t mode,
		int (*f)(struct dentry *, umode_t, void *),
		void *arg)
{
	struct inode *dir = dentry->d_parent->d_inode;
	int error = may_create_dentry(&nop_mnt_idmap, dir, dentry);
	if (error)
		return error;

	mode &= S_IALLUGO;
	mode |= S_IFREG;
	error = security_inode_create(dir, dentry, mode);
	if (error)
		return error;
	error = f(dentry, mode, arg);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_mkobj);

bool may_open_dev(const struct path *path)
{
	return !(path->mnt->mnt_flags & MNT_NODEV) &&
		!(path->mnt->mnt_sb->s_iflags & SB_I_NODEV);
}

static int may_open(struct mnt_idmap *idmap, const struct path *path,
		    int acc_mode, int flag)
{
	struct dentry *dentry = path->dentry;
	struct inode *inode = dentry->d_inode;
	int error;

	if (!inode)
		return -ENOENT;

	switch (inode->i_mode & S_IFMT) {
	case S_IFLNK:
		return -ELOOP;
	case S_IFDIR:
		if (acc_mode & MAY_WRITE)
			return -EISDIR;
		if (acc_mode & MAY_EXEC)
			return -EACCES;
		break;
	case S_IFBLK:
	case S_IFCHR:
		if (!may_open_dev(path))
			return -EACCES;
		fallthrough;
	case S_IFIFO:
	case S_IFSOCK:
		if (acc_mode & MAY_EXEC)
			return -EACCES;
		flag &= ~O_TRUNC;
		break;
	case S_IFREG:
		if ((acc_mode & MAY_EXEC) && path_noexec(path))
			return -EACCES;
		break;
	default:
		VFS_BUG_ON_INODE(!IS_ANON_FILE(inode), inode);
	}

	error = inode_permission(idmap, inode, MAY_OPEN | acc_mode);
	if (error)
		return error;

	/*
	 * An append-only file must be opened in append mode for writing.
	 */
	if (IS_APPEND(inode)) {
		if  ((flag & O_ACCMODE) != O_RDONLY && !(flag & O_APPEND))
			return -EPERM;
		if (flag & O_TRUNC)
			return -EPERM;
	}

	/* O_NOATIME can only be set by the owner or superuser */
	if (flag & O_NOATIME && !inode_owner_or_capable(idmap, inode))
		return -EPERM;

	return 0;
}

static int handle_truncate(struct mnt_idmap *idmap, struct file *filp)
{
	const struct path *path = &filp->f_path;
	struct inode *inode = path->dentry->d_inode;
	int error = get_write_access(inode);
	if (error)
		return error;

	error = security_file_truncate(filp);
	if (!error) {
		error = do_truncate(idmap, path->dentry, 0,
				    ATTR_MTIME|ATTR_CTIME|ATTR_OPEN,
				    filp);
	}
	put_write_access(inode);
	return error;
}

static inline int open_to_namei_flags(int flag)
{
	if ((flag & O_ACCMODE) == 3)
		flag--;
	return flag;
}

static int may_o_create(struct mnt_idmap *idmap,
			const struct path *dir, struct dentry *dentry,
			umode_t mode)
{
	int error = security_path_mknod(dir, dentry, mode, 0);
	if (error)
		return error;

	if (!fsuidgid_has_mapping(dir->dentry->d_sb, idmap))
		return -EOVERFLOW;

	error = inode_permission(idmap, dir->dentry->d_inode,
				 MAY_WRITE | MAY_EXEC);
	if (error)
		return error;

	return security_inode_create(dir->dentry->d_inode, dentry, mode);
}

/*
 * Attempt to atomically look up, create and open a file from a negative
 * dentry.
 *
 * Returns 0 if successful.  The file will have been created and attached to
 * @file by the filesystem calling finish_open().
 *
 * If the file was looked up only or didn't need creating, FMODE_OPENED won't
 * be set.  The caller will need to perform the open themselves.  @path will
 * have been updated to point to the new dentry.  This may be negative.
 *
 * Returns an error code otherwise.
 */
static struct dentry *atomic_open(const struct path *path, struct dentry *dentry,
				  struct file *file,
				  int open_flag, umode_t mode)
{
	struct dentry *const DENTRY_NOT_SET = (void *) -1UL;
	struct inode *dir =  path->dentry->d_inode;
	int error;

	file->__f_path.dentry = DENTRY_NOT_SET;
	file->__f_path.mnt = path->mnt;
	error = dir->i_op->atomic_open(dir, dentry, file,
				       open_to_namei_flags(open_flag), mode);
	d_lookup_done(dentry);
	if (!error) {
		if (file->f_mode & FMODE_OPENED) {
			if (unlikely(dentry != file->f_path.dentry)) {
				dput(dentry);
				dentry = dget(file->f_path.dentry);
			}
		} else if (WARN_ON(file->f_path.dentry == DENTRY_NOT_SET)) {
			error = -EIO;
		} else {
			if (file->f_path.dentry) {
				dput(dentry);
				dentry = file->f_path.dentry;
			}
			if (unlikely(d_is_negative(dentry)))
				error = -ENOENT;
		}
	}
	if (error) {
		dput(dentry);
		dentry = ERR_PTR(error);
	}
	return dentry;
}

/*
 * Look up and maybe create and open the last component.
 *
 * Must be called with parent locked (exclusive in O_CREAT case).
 *
 * Returns 0 on success, that is, if
 *  the file was successfully atomically created (if necessary) and opened, or
 *  the file was not completely opened at this time, though lookups and
 *  creations were performed.
 * These case are distinguished by presence of FMODE_OPENED on file->f_mode.
 * In the latter case dentry returned in @path might be negative if O_CREAT
 * hadn't been specified.
 *
 * An error code is returned on failure.
 */
static struct dentry *lookup_open(struct nameidata *nd, struct file *file,
				  const struct open_flags *op,
				  bool got_write, struct delegated_inode *delegated_inode)
{
	struct mnt_idmap *idmap;
	struct dentry *dir = nd->path.dentry;
	struct inode *dir_inode = dir->d_inode;
	int open_flag = op->open_flag;
	struct dentry *dentry;
	int error, create_error = 0;
	umode_t mode = op->mode;

	dentry_dbg(nd->path.dentry, "nd->last.name = %s, nd->pathname = %s\n", nd->last.name, nd->pathname);

	if (unlikely(IS_DEADDIR(dir_inode)))
		return ERR_PTR(-ENOENT);

	file->f_mode &= ~FMODE_CREATED;
	dentry = d_lookup(dir, &nd->last);
	for (;;) {
		if (!dentry) {
			dentry = d_alloc_parallel(dir, &nd->last);
			if (IS_ERR(dentry))
				return dentry;
		}
		if (d_in_lookup(dentry))
			break;

		error = d_revalidate(dir_inode, &nd->last, dentry, nd->flags);
		if (likely(error > 0))
			break;
		if (error)
			goto out_dput;
		d_invalidate(dentry);
		dput(dentry);
		dentry = NULL;
	}
	if (dentry->d_inode) {
		/* Cached positive dentry: will open in f_op->open */
		dentry_dbg(dentry, "dentry->d_name.name = %s, dentry->d_inode->i_ino = %lu\n", dentry->d_name.name, dentry->d_inode->i_ino);
		return dentry;
	}

	if (open_flag & O_CREAT)
		audit_inode(nd->name, dir, AUDIT_INODE_PARENT);

	/*
	 * Checking write permission is tricky, bacuse we don't know if we are
	 * going to actually need it: O_CREAT opens should work as long as the
	 * file exists.  But checking existence breaks atomicity.  The trick is
	 * to check access and if not granted clear O_CREAT from the flags.
	 *
	 * Another problem is returing the "right" error value (e.g. for an
	 * O_EXCL open we want to return EEXIST not EROFS).
	 */
	if (unlikely(!got_write))
		open_flag &= ~O_TRUNC;
	idmap = mnt_idmap(nd->path.mnt);
	if (open_flag & O_CREAT) {
		if (open_flag & O_EXCL)
			open_flag &= ~O_TRUNC;
		mode = vfs_prepare_mode(idmap, dir->d_inode, mode, mode, mode);
		if (likely(got_write))
			create_error = may_o_create(idmap, &nd->path,
						    dentry, mode);
		else
			create_error = -EROFS;
	}
	if (create_error)
		open_flag &= ~O_CREAT;
	if (dir_inode->i_op->atomic_open) {
		if (nd->flags & LOOKUP_DIRECTORY)
			open_flag |= O_DIRECTORY;
		dentry = atomic_open(&nd->path, dentry, file, open_flag, mode);
		if (unlikely(create_error) && dentry == ERR_PTR(-ENOENT))
			dentry = ERR_PTR(create_error);
		dentry_dbg(dentry, "after atomic_open dentry->d_inode->i_ino = %lu\n", dentry->d_inode->i_ino);
		return dentry;
	}

	if (d_in_lookup(dentry)) {
		struct dentry *res = dir_inode->i_op->lookup(dir_inode, dentry,
							     nd->flags);
		d_lookup_done(dentry);
		if (unlikely(res)) {
			if (IS_ERR(res)) {
				error = PTR_ERR(res);
				goto out_dput;
			}
			dput(dentry);
			dentry = res;
		}
	}

	/* Negative dentry, just create the file */
	if (!dentry->d_inode && (open_flag & O_CREAT)) {
		/* but break the directory lease first! */
		error = try_break_deleg(dir_inode, LEASE_BREAK_DIR_CREATE, delegated_inode);
		if (error)
			goto out_dput;

		file->f_mode |= FMODE_CREATED;
		audit_inode_child(dir_inode, dentry, AUDIT_TYPE_CHILD_CREATE);
		if (!dir_inode->i_op->create) {
			error = -EACCES;
			goto out_dput;
		}

		error = dir_inode->i_op->create(idmap, dir_inode, dentry,
						mode, open_flag & O_EXCL);
		if (error)
			goto out_dput;
	}
	if (unlikely(create_error) && !dentry->d_inode) {
		error = create_error;
		goto out_dput;
	}
	dentry_dbg(dentry, "finish => dentry->d_inode = %p\n", dentry->d_inode);
	return dentry;

out_dput:
	dput(dentry);
	return ERR_PTR(error);
}

static inline bool trailing_slashes(struct nameidata *nd)
{
	return (bool)nd->last.name[nd->last.len];
}

static struct dentry *lookup_fast_for_open(struct nameidata *nd, int open_flag)
{
	struct dentry *dentry;

	dentry_dbg(nd->path.dentry, "nd->last.name = %s, nd->pathname = %s\n", nd->last.name, nd->pathname);

	if (open_flag & O_CREAT) {
		if (trailing_slashes(nd))
			return ERR_PTR(-EISDIR);

		/* Don't bother on an O_EXCL create */
		if (open_flag & O_EXCL)
			return NULL;
	}

	if (trailing_slashes(nd))
		nd->flags |= LOOKUP_FOLLOW | LOOKUP_DIRECTORY;

	dentry = lookup_fast(nd);
	if (IS_ERR_OR_NULL(dentry))
		return dentry;
	dentry_dbg(dentry, "lookup_fast found name: %s, inode: %p\n", dentry->d_name.name, dentry->d_inode);
	inode_dbg(dentry->d_inode, "inode: %p\n", dentry->d_inode);

	if (open_flag & O_CREAT) {
		/* Discard negative dentries. Need inode_lock to do the create */
		if (!dentry->d_inode) {
			if (!(nd->flags & LOOKUP_RCU))
				dput(dentry);
			dentry = NULL;
		}
	}
	return dentry;
}

static const char *open_last_lookups(struct nameidata *nd,
		   struct file *file, const struct open_flags *op)
{
	struct delegated_inode delegated_inode = { };
	struct dentry *dir = nd->path.dentry;
	int open_flag = op->open_flag;
	bool got_write = false;
	struct dentry *dentry;
	const char *res;

	nd->flags |= op->intent;

	if (nd->last_type != LAST_NORM) {
		if (nd->depth)
			put_link(nd);
		return handle_dots(nd, nd->last_type);
	}

	dentry_dbg(nd->path.dentry, "nd->last.name = %s, nd->pathname = %s\n", nd->last.name, nd->pathname);

	/* We _can_ be in RCU mode here */
	dentry = lookup_fast_for_open(nd, open_flag);
	if (IS_ERR(dentry))
		return ERR_CAST(dentry);

	if (likely(dentry))
		goto finish_lookup;

	if (!(open_flag & O_CREAT)) {
		if (WARN_ON_ONCE(nd->flags & LOOKUP_RCU))
			return ERR_PTR(-ECHILD);
	} else {
		if (nd->flags & LOOKUP_RCU) {
			if (!try_to_unlazy(nd))
				return ERR_PTR(-ECHILD);
		}
	}
retry:
	if (open_flag & (O_CREAT | O_TRUNC | O_WRONLY | O_RDWR)) {
		got_write = !mnt_want_write(nd->path.mnt);
		/*
		 * do _not_ fail yet - we might not need that or fail with
		 * a different error; let lookup_open() decide; we'll be
		 * dropping this one anyway.
		 */
	}
	if (open_flag & O_CREAT)
		inode_lock(dir->d_inode);
	else
		inode_lock_shared(dir->d_inode);
	dentry = lookup_open(nd, file, op, got_write, &delegated_inode);
	if (!IS_ERR(dentry)) {
		if (file->f_mode & FMODE_CREATED)
			fsnotify_create(dir->d_inode, dentry);
		if (file->f_mode & FMODE_OPENED)
			fsnotify_open(file);
	}
	if (open_flag & O_CREAT)
		inode_unlock(dir->d_inode);
	else
		inode_unlock_shared(dir->d_inode);

	if (got_write)
		mnt_drop_write(nd->path.mnt);

	if (IS_ERR(dentry)) {
		if (is_delegated(&delegated_inode)) {
			int error = break_deleg_wait(&delegated_inode);

			if (!error)
				goto retry;
			return ERR_PTR(error);
		}
		return ERR_CAST(dentry);
	}

	if (file->f_mode & (FMODE_OPENED | FMODE_CREATED)) {
		dput(nd->path.dentry);
		nd->path.dentry = dentry;
		return NULL;
	}

finish_lookup:
	if (nd->depth)
		put_link(nd);
	dentry_dbg(dentry, "finish_lookup dentry->d_name.name = %s\n", dentry->d_name.name);
	res = step_into(nd, WALK_TRAILING, dentry);
	if (unlikely(res))
		nd->flags &= ~(LOOKUP_OPEN|LOOKUP_CREATE|LOOKUP_EXCL);
	return res;
}

/*
 * Handle the last step of open()
 */
static int do_open(struct nameidata *nd,
		   struct file *file, const struct open_flags *op)
{
	struct mnt_idmap *idmap;
	int open_flag = op->open_flag;
	bool do_truncate;
	int acc_mode;
	int error;

	dentry_dbg(nd->path.dentry, "file %p\n", file);
	if (!(file->f_mode & (FMODE_OPENED | FMODE_CREATED))) {
		error = complete_walk(nd);
		if (error)
			return error;
	}
	if (!(file->f_mode & FMODE_CREATED))
		audit_inode(nd->name, nd->path.dentry, 0);
	// user namespace用于资源划分和权限管理， 该inode的namespace
	idmap = mnt_idmap(nd->path.mnt);
	if (open_flag & O_CREAT) {
		if ((open_flag & O_EXCL) && !(file->f_mode & FMODE_CREATED))
			return -EEXIST;
		if (d_is_dir(nd->path.dentry))
			return -EISDIR;
		error = may_create_in_sticky(idmap, nd,
					     d_backing_inode(nd->path.dentry));
		if (unlikely(error))
			return error;
	}

	if ((open_flag & __O_REGULAR) && !d_is_reg(nd->path.dentry))
		return -EFTYPE;

	if ((nd->flags & LOOKUP_DIRECTORY) && !d_can_lookup(nd->path.dentry))
		return -ENOTDIR;

	do_truncate = false;
	acc_mode = op->acc_mode;
	if (file->f_mode & FMODE_CREATED) {
		/* Don't check for write permission, don't truncate */
		open_flag &= ~O_TRUNC;
		acc_mode = 0;
	} else if (d_is_reg(nd->path.dentry) && open_flag & O_TRUNC) {
		error = mnt_want_write(nd->path.mnt);
		if (error)
			return error;
		do_truncate = true;
	}
	error = may_open(idmap, &nd->path, acc_mode, open_flag);
	// 打开文件
	if (!error && !(file->f_mode & FMODE_OPENED))
		error = vfs_open(&nd->path, file);
	if (!error)
		error = security_file_post_open(file, op->acc_mode);
	if (!error && do_truncate)
		error = handle_truncate(idmap, file);
	if (unlikely(error > 0)) {
		WARN_ON(1);
		error = -EINVAL;
	}
	if (do_truncate)
		mnt_drop_write(nd->path.mnt);
	return error;
}

/**
 * vfs_tmpfile - create tmpfile
 * @idmap:	idmap of the mount the inode was found from
 * @parentpath:	pointer to the path of the base directory
 * @file:	file descriptor of the new tmpfile
 * @mode:	mode of the new tmpfile
 *
 * Create a temporary file.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_tmpfile(struct mnt_idmap *idmap,
		const struct path *parentpath,
		struct file *file, umode_t mode)
{
	struct dentry *child;
	struct inode *dir = d_inode(parentpath->dentry);
	struct inode *inode;
	int error;
	int open_flag = file->f_flags;

	/* A tmpfile is I_LINKABLE, so guard its owner like may_o_create(). */
	if (!fsuidgid_has_mapping(dir->i_sb, idmap))
		return -EOVERFLOW;

	/* we want directory to be writable */
	error = inode_permission(idmap, dir, MAY_WRITE | MAY_EXEC);
	if (error)
		return error;
	if (!dir->i_op->tmpfile)
		return -EOPNOTSUPP;
	child = d_alloc(parentpath->dentry, &slash_name);
	if (unlikely(!child))
		return -ENOMEM;
	file->__f_path.mnt = parentpath->mnt;
	file->__f_path.dentry = child;
	mode = vfs_prepare_mode(idmap, dir, mode, mode, mode);
	error = dir->i_op->tmpfile(idmap, dir, file, mode);
	dput(child);
	if (file->f_mode & FMODE_OPENED)
		fsnotify_open(file);
	if (error)
		return error;
	/* Don't check for other permissions, the inode was just created */
	error = may_open(idmap, &file->f_path, 0, file->f_flags);
	if (error)
		return error;
	inode = file_inode(file);
	if (!(open_flag & O_EXCL)) {
		spin_lock(&inode->i_lock);
		inode_state_set(inode, I_LINKABLE);
		spin_unlock(&inode->i_lock);
	}
	security_inode_post_create_tmpfile(idmap, inode);
	return 0;
}

/**
 * kernel_tmpfile_open - open a tmpfile for kernel internal use
 * @idmap:	idmap of the mount the inode was found from
 * @parentpath:	path of the base directory
 * @mode:	mode of the new tmpfile
 * @open_flag:	flags
 * @cred:	credentials for open
 *
 * Create and open a temporary file.  The file is not accounted in nr_files,
 * hence this is only for kernel internal use, and must not be installed into
 * file tables or such.
 */
struct file *kernel_tmpfile_open(struct mnt_idmap *idmap,
				 const struct path *parentpath,
				 umode_t mode, int open_flag,
				 const struct cred *cred)
{
	struct file *file;
	int error;

	file = alloc_empty_file_noaccount(open_flag, cred);
	if (IS_ERR(file))
		return file;

	error = vfs_tmpfile(idmap, parentpath, file, mode);
	if (error) {
		fput(file);
		file = ERR_PTR(error);
	}
	return file;
}
EXPORT_SYMBOL(kernel_tmpfile_open);

/**
 * do_tmpfile - 创建并处理临时文件
 * @nd: 名称数据结构指针，包含路径查找的上下文信息
 * @flags: 查找标志位
 * @op: 打开操作参数结构体指针，包含文件打开模式和相关信息
 * @file: 目标文件结构体指针，用于接收创建的临时文件
 *
 * 此函数用于在指定目录下创建临时文件，主要包含路径查找、
 * 获取写权限、调用底层创建临时文件接口以及审计记录等步骤。
 *
 * 返回值: 成功返回0，失败返回相应的负错误码。
 */
static int do_tmpfile(struct nameidata *nd, unsigned flags,
		const struct open_flags *op,
		struct file *file)
{
	/* 声明路径结构体，用于保存查找到的目录路径信息 */
	struct path path;
	
	/* 在给定的路径上下文中查找目标目录，并获取其路径信息 */
	int error = path_lookupat(nd, flags | LOOKUP_DIRECTORY, &path);

	/* 如果路径查找失败，使用unlikely提示编译器该分支发生概率较低，直接返回错误码 */
	if (unlikely(error))
		return error;
		
	/* 获取该挂载点的写权限，防止在只读文件系统上创建文件 */
	error = mnt_want_write(path.mnt);
	
	/* 如果获取写权限失败，跳转到out标签释放路径资源并返回错误 */
	if (unlikely(error))
		goto out;
		
	/* 调用虚拟文件系统层的vfs_tmpfile接口，真正创建临时文件 */
	error = vfs_tmpfile(mnt_idmap(path.mnt), &path, file, op->mode);
	
	/* 如果创建临时文件失败，跳转到out2标签释放写权限 */
	if (error)
		goto out2;
		
	/* 创建成功，将该临时文件的inode信息记录到审计系统中 */
	audit_inode(nd->name, file->f_path.dentry, 0);
	
out2:
	/* 释放之前获取的挂载点写权限 */
	mnt_drop_write(path.mnt);
out:
	/* 释放路径查找时引用的目录dentry和vfsmount，防止内存泄漏 */
	path_put(&path);
	
	/* 返回错误码（成功时为0） */
	return error;
}


static int do_o_path(struct nameidata *nd, unsigned flags, struct file *file)
{
	struct path path;
	int error = path_lookupat(nd, flags, &path);
	if (!error) {
		audit_inode(nd->name, path.dentry, 0);
		error = vfs_open(&path, file);
		path_put(&path);
	}
	return error;
}

/**
 * path_openat - 在给定的路径上下文中打开文件
 * @nd:   nameidata结构体指针，保存路径查找的状态和上下文信息
 * @op:   open_flags结构体指针，包含打开文件的标志和模式等信息
 * @flags: 查找标志位，控制路径查找的行为（如是否使用RCU模式等）
 *
 * 该函数是文件打开的核心处理函数。它会根据不同的打开标志（如临时文件、
 * O_PATH标志或普通文件）执行相应的处理逻辑，并返回一个初始化好的
 * file结构体指针，或者在失败时返回错误指针。
 *
 * Return: 成功时返回指向struct file的指针，失败时返回IS_ERR()为真的错误指针
 */
static struct file *path_openat(struct nameidata *nd,
			const struct open_flags *op, unsigned flags)
{
	struct file *file;
	int error;

	// 分配一个空的file结构体
	file = alloc_empty_file(op->open_flag, current_cred());
	if (IS_ERR(file))
		return file;

	nd_dbg(nd, "will open\n");

	/* 根据文件标志位分派不同的打开处理逻辑 */
	if (unlikely(file->f_flags & __O_TMPFILE)) {
		error = do_tmpfile(nd, flags, op, file); // 处理临时文件的创建与打开
	} else if (unlikely(file->f_flags & O_PATH)) {
		error = do_o_path(nd, flags, file); // 仅分配文件描述符，但不实际打开文件
	} else {
		/**
		* 普通文件的路径查找与打开流程
		* 
		* 1. path_init: 初始化路径查找，注意该函数内部会调用rcu_read_lock()
		* 2. link_path_walk: 逐级遍历路径名中的各个目录组件
		* 3. open_last_lookups: 处理路径的最后一级组件查找及可能的创建操作
		* 4. do_open: 最终执行文件的打开操作
		*/
		const char *s = path_init(nd, flags);
		while (!(error = link_path_walk(s, nd)) &&
		       (s = open_last_lookups(nd, file, op)) != NULL)
			;
		if (!error)
			error = do_open(nd, file, op); // 执行打开文件操作

		// 结束路径遍历，并调用rcu_read_unlock()释放RCU读锁
		terminate_walk(nd);
	}

	/* 检查打开操作是否成功 */
	if (likely(!error)) {
		if (likely(file->f_mode & FMODE_OPENED))
			return file; // 文件成功打开，返回file指针
		WARN_ON(1);      // 触发内核警告：无错误但文件未标记为已打开
		error = -EINVAL; // 赋值无效参数错误码
	}

	// 打开失败，释放file结构体引用并关闭
	fput_close(file);

	/* 
	 * 处理"陈旧"（stale）错误码：
	 * 如果在RCU模式下遇到陈旧文件句柄，返回-ECHILD以触发回退到非RCU模式重试；
	 * 如果在普通模式下遇到，则返回标准的-ESTALE错误码。
	 */
	if (error == -EOPENSTALE) {
		if (flags & LOOKUP_RCU)
			error = -ECHILD;
		else
			error = -ESTALE;
	}

	return ERR_PTR(error);
}


/**
 * do_file_open - 执行文件打开操作的核心函数
 * @dfd: 目录文件描述符，指示相对路径的基准目录
 * @pathname: 指向文件路径名结构的指针
 * @op: 指向打开标志结构体的指针，包含访问模式和打开标志等信息
 *
 * 此函数用于根据给定的路径和标志打开文件。它会先初始化文件查找所需的上下文，
 * 然后尝试以RCU（读-拷贝-更新）快速路径查找并打开文件。如果快速路径失败
 * （例如遇到-ECHILD或-ESTALE错误），则会回退到常规的查找模式或重新验证
 * 模式进行重试，最后恢复进程原有的上下文并返回文件指针。
 *
 * Return: 成功时返回打开的文件指针(struct file *)，失败时返回相应的错误指针(ERR_PTR)
 */
struct file *do_file_open(int dfd, struct filename *pathname,
		const struct open_flags *op)
{
	/* 定义nameidata结构体变量，用于保存文件路径查找过程中的上下文状态 */
	struct nameidata nd;
	/* 从打开标志结构体中提取路径查找标志 */
	int flags = op->lookup_flags;
	/* 定义文件指针，用于存放打开成功后的文件结构体 */
	struct file *filp;

	/* 检查路径名指针是否为无效错误指针 */
	if (IS_ERR(pathname))
		return ERR_CAST(pathname);

	// 设置进程文件查找结构,保存旧的
	set_nameidata(&nd, dfd, pathname, NULL);
	nd_dbg((&nd), "nameidata initlized\n");

	// 查找path并打开文件
	/* 首先尝试使用RCU快速路径进行查找和打开，以提高效率 */
	filp = path_openat(&nd, op, flags | LOOKUP_RCU);
	/* 如果RCU路径失败并返回-ECHILD错误（通常表示在RCU模式下发生了需要慢速路径的情况），
	   则回退到不带RCU的常规查找模式重试 */
	if (unlikely(filp == ERR_PTR(-ECHILD)))
		filp = path_openat(&nd, op, flags);
	/* 如果查找返回-ESTALE错误（通常表示缓存过时或文件句柄失效），
	   则增加LOOKUP_REVAL标志重新验证路径并重试 */
	if (unlikely(filp == ERR_PTR(-ESTALE)))
		filp = path_openat(&nd, op, flags | LOOKUP_REVAL);

	// 恢复进程文件查找结构
	restore_nameidata();
	
	/* 返回打开的文件指针（可能包含错误指针） */
	return filp;
}


struct file *do_file_open_root(const struct path *root,
		const char *name, const struct open_flags *op)
{
	struct nameidata nd;
	struct file *file;
	int flags = op->lookup_flags;

	if (d_is_symlink(root->dentry) && op->intent & LOOKUP_OPEN)
		return ERR_PTR(-ELOOP);

	CLASS(filename_kernel, filename)(name);
	if (IS_ERR(filename))
		return ERR_CAST(filename);

	set_nameidata(&nd, -1, filename, root);
	file = path_openat(&nd, op, flags | LOOKUP_RCU);
	if (unlikely(file == ERR_PTR(-ECHILD)))
		file = path_openat(&nd, op, flags);
	if (unlikely(file == ERR_PTR(-ESTALE)))
		file = path_openat(&nd, op, flags | LOOKUP_REVAL);
	restore_nameidata();
	return file;
}

static struct dentry *filename_create(int dfd, struct filename *name,
				      struct path *path, unsigned int lookup_flags)
{
	struct dentry *dentry = ERR_PTR(-EEXIST);
	struct qstr last;
	bool want_dir = lookup_flags & LOOKUP_DIRECTORY;
	unsigned int reval_flag = lookup_flags & LOOKUP_REVAL;
	unsigned int create_flags = LOOKUP_CREATE | LOOKUP_EXCL;
	enum last_type type;
	int error;

	error = filename_parentat(dfd, name, reval_flag, path, &last, &type);
	if (error)
		return ERR_PTR(error);

	/*
	 * Yucky last component or no last component at all?
	 * (foo/., foo/.., /////)
	 */
	if (unlikely(type != LAST_NORM))
		goto out;

	/* don't fail immediately if it's r/o, at least try to report other errors */
	error = mnt_want_write(path->mnt);
	/*
	 * Do the final lookup.  Suppress 'create' if there is a trailing
	 * '/', and a directory wasn't requested.
	 */
	if (last.name[last.len] && !want_dir)
		create_flags &= ~LOOKUP_CREATE;
	dentry = start_dirop(path->dentry, &last, reval_flag | create_flags);
	if (IS_ERR(dentry))
		goto out_drop_write;

	if (unlikely(error))
		goto fail;

	return dentry;
fail:
	end_dirop(dentry);
	dentry = ERR_PTR(error);
out_drop_write:
	if (!error)
		mnt_drop_write(path->mnt);
out:
	path_put(path);
	return dentry;
}

struct dentry *start_creating_path(int dfd, const char *pathname,
				   struct path *path, unsigned int lookup_flags)
{
	CLASS(filename_kernel, filename)(pathname);
	return filename_create(dfd, filename, path, lookup_flags);
}
EXPORT_SYMBOL(start_creating_path);

/**
 * end_creating_path - finish a code section started by start_creating_path()
 * @path: the path instantiated by start_creating_path()
 * @dentry: the dentry returned by start_creating_path()
 *
 * end_creating_path() will unlock and locks taken by start_creating_path()
 * and drop an references that were taken.  It should only be called
 * if start_creating_path() returned a non-error.
 * If vfs_mkdir() was called and it returned an error, that error *should*
 * be passed to end_creating_path() together with the path.
 */
void end_creating_path(const struct path *path, struct dentry *dentry)
{
	end_creating(dentry);
	mnt_drop_write(path->mnt);
	path_put(path);
}
EXPORT_SYMBOL(end_creating_path);

inline struct dentry *start_creating_user_path(
	int dfd, const char __user *pathname,
	struct path *path, unsigned int lookup_flags)
{
	CLASS(filename, filename)(pathname);
	return filename_create(dfd, filename, path, lookup_flags);
}
EXPORT_SYMBOL(start_creating_user_path);

/**
 * dentry_create - Create and open a file
 * @path: path to create
 * @flags: O\_ flags
 * @mode: mode bits for new file
 * @cred: credentials to use
 *
 * Caller must hold the parent directory's lock, and have prepared
 * a negative dentry, placed in @path->dentry, for the new file.
 *
 * Caller sets @path->mnt to the vfsmount of the filesystem where
 * the new file is to be created. The parent directory and the
 * negative dentry must reside on the same filesystem instance.
 *
 * On success, returns a ``struct file *``. Otherwise an ERR_PTR
 * is returned.
 */
struct file *dentry_create(struct path *path, int flags, umode_t mode,
			   const struct cred *cred)
{
	struct file *file __free(fput) = NULL;
	struct dentry *dentry = path->dentry;
	struct dentry *orig_dentry = dentry;
	struct dentry *dir = dentry->d_parent;
	struct inode *dir_inode = d_inode(dir);
	struct mnt_idmap *idmap;
	int error, create_error;

	file = alloc_empty_file(flags, cred);
	if (IS_ERR(file))
		return file;

	idmap = mnt_idmap(path->mnt);

	if (dir_inode->i_op->atomic_open) {
		path->dentry = dir;
		mode = vfs_prepare_mode(idmap, dir_inode, mode, S_IALLUGO, S_IFREG);

		create_error = may_o_create(idmap, path, dentry, mode);
		if (create_error)
			flags &= ~O_CREAT;

		/* atomic_open will dput(dentry) on error */
		dget(orig_dentry);
		dentry = atomic_open(path, dentry, file, flags, mode);
		error = PTR_ERR_OR_ZERO(dentry);

		if (IS_ERR(dentry))
			/* keep the original */
			dentry = orig_dentry;
		else
			/* Drop the extra reference */
			dput(orig_dentry);

		if (unlikely(create_error) && error == -ENOENT)
			error = create_error;

		if (!error) {
			if (file->f_mode & FMODE_CREATED)
				fsnotify_create(dir->d_inode, dentry);
			if (file->f_mode & FMODE_OPENED)
				fsnotify_open(file);
		}

		path->dentry = dentry;

	} else {
		error = vfs_create(mnt_idmap(path->mnt), path->dentry, mode, NULL);
		if (!error)
			error = vfs_open(path, file);
	}
	if (unlikely(error))
		return ERR_PTR(error);

	return no_free_ptr(file);
}
EXPORT_SYMBOL(dentry_create);

/**
 * vfs_mknod - create device node or file
 * @idmap:		idmap of the mount the inode was found from
 * @dir:		inode of the parent directory
 * @dentry:		dentry of the child device node
 * @mode:		mode of the child device node
 * @dev:		device number of device to create
 * @delegated_inode:	returns parent inode, if the inode is delegated.
 *
 * Create a device node or file.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
	      struct dentry *dentry, umode_t mode, dev_t dev,
	      struct delegated_inode *delegated_inode)
{
	bool is_whiteout = S_ISCHR(mode) && dev == WHITEOUT_DEV;
	int error = may_create_dentry(idmap, dir, dentry);

	if (error)
		return error;

	if ((S_ISCHR(mode) || S_ISBLK(mode)) && !is_whiteout &&
	    !capable(CAP_MKNOD))
		return -EPERM;

	if (!dir->i_op->mknod)
		return -EPERM;

	mode = vfs_prepare_mode(idmap, dir, mode, mode, mode);
	error = devcgroup_inode_mknod(mode, dev);
	if (error)
		return error;

	error = security_inode_mknod(dir, dentry, mode, dev);
	if (error)
		return error;

	error = try_break_deleg(dir, LEASE_BREAK_DIR_CREATE, delegated_inode);
	if (error)
		return error;

	error = dir->i_op->mknod(idmap, dir, dentry, mode, dev);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_mknod);

static int may_mknod(umode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
	case S_IFCHR:
	case S_IFBLK:
	case S_IFIFO:
	case S_IFSOCK:
	case 0: /* zero mode translates to S_IFREG */
		return 0;
	case S_IFDIR:
		return -EPERM;
	default:
		return -EINVAL;
	}
}

int filename_mknodat(int dfd, struct filename *name, umode_t mode,
		     unsigned int dev)
{
	struct delegated_inode di = { };
	struct mnt_idmap *idmap;
	struct dentry *dentry;
	struct path path;
	int error;
	unsigned int lookup_flags = 0;

	error = may_mknod(mode);
	if (error)
		return error;
retry:
	dentry = filename_create(dfd, name, &path, lookup_flags);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	error = security_path_mknod(&path, dentry,
			mode_strip_umask(path.dentry->d_inode, mode), dev);
	if (error)
		goto out2;

	idmap = mnt_idmap(path.mnt);
	switch (mode & S_IFMT) {
		case 0: case S_IFREG:
			error = vfs_create(idmap, dentry, mode, &di);
			if (!error)
				security_path_post_mknod(idmap, dentry);
			break;
		case S_IFCHR: case S_IFBLK:
			error = vfs_mknod(idmap, path.dentry->d_inode,
					  dentry, mode, new_decode_dev(dev), &di);
			break;
		case S_IFIFO: case S_IFSOCK:
			error = vfs_mknod(idmap, path.dentry->d_inode,
					  dentry, mode, 0, &di);
			break;
	}
out2:
	end_creating_path(&path, dentry);
	if (is_delegated(&di)) {
		error = break_deleg_wait(&di);
		if (!error)
			goto retry;
	}
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE4(mknodat, int, dfd, const char __user *, filename, umode_t, mode,
		unsigned int, dev)
{
	CLASS(filename, name)(filename);
	return filename_mknodat(dfd, name, mode, dev);
}

SYSCALL_DEFINE3(mknod, const char __user *, filename, umode_t, mode, unsigned, dev)
{
	CLASS(filename, name)(filename);
	return filename_mknodat(AT_FDCWD, name, mode, dev);
}

/**
 * vfs_mkdir - create directory returning correct dentry if possible
 * @idmap:		idmap of the mount the inode was found from
 * @dir:		inode of the parent directory
 * @dentry:		dentry of the child directory
 * @mode:		mode of the child directory
 * @delegated_inode:	returns parent inode, if the inode is delegated.
 *
 * Create a directory.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 *
 * In the event that the filesystem does not use the *@dentry but leaves it
 * negative or unhashes it and possibly splices a different one returning it,
 * the original dentry is dput() and the alternate is returned.
 *
 * In case of an error the dentry is dput() and an ERR_PTR() is returned.
 */
struct dentry *vfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode,
			 struct delegated_inode *delegated_inode)
{
	int error;
	unsigned max_links = dir->i_sb->s_max_links;
	struct dentry *de;

	error = may_create_dentry(idmap, dir, dentry);
	if (error)
		goto err;

	error = -EPERM;
	if (!dir->i_op->mkdir)
		goto err;

	mode = vfs_prepare_mode(idmap, dir, mode, S_IRWXUGO | S_ISVTX, 0);
	error = security_inode_mkdir(dir, dentry, mode);
	if (error)
		goto err;

	error = -EMLINK;
	if (max_links && dir->i_nlink >= max_links)
		goto err;

	error = try_break_deleg(dir, LEASE_BREAK_DIR_CREATE, delegated_inode);
	if (error)
		goto err;

	de = dir->i_op->mkdir(idmap, dir, dentry, mode);
	error = PTR_ERR(de);
	if (IS_ERR(de))
		goto err;
	if (de) {
		dput(dentry);
		dentry = de;
	}
	fsnotify_mkdir(dir, dentry);
	return dentry;

err:
	end_creating(dentry);
	return ERR_PTR(error);
}
EXPORT_SYMBOL(vfs_mkdir);

int filename_mkdirat(int dfd, struct filename *name, umode_t mode)
{
	struct dentry *dentry;
	struct path path;
	int error;
	unsigned int lookup_flags = LOOKUP_DIRECTORY;
	struct delegated_inode delegated_inode = { };

retry:
	dentry = filename_create(dfd, name, &path, lookup_flags);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	error = security_path_mkdir(&path, dentry,
			mode_strip_umask(path.dentry->d_inode, mode));
	if (!error) {
		dentry = vfs_mkdir(mnt_idmap(path.mnt), path.dentry->d_inode,
				   dentry, mode, &delegated_inode);
		if (IS_ERR(dentry))
			error = PTR_ERR(dentry);
	}
	end_creating_path(&path, dentry);
	if (is_delegated(&delegated_inode)) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry;
	}
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE3(mkdirat, int, dfd, const char __user *, pathname, umode_t, mode)
{
	CLASS(filename, name)(pathname);
	return filename_mkdirat(dfd, name, mode);
}

SYSCALL_DEFINE2(mkdir, const char __user *, pathname, umode_t, mode)
{
	CLASS(filename, name)(pathname);
	return filename_mkdirat(AT_FDCWD, name, mode);
}

/**
 * vfs_rmdir - remove directory
 * @idmap:		idmap of the mount the inode was found from
 * @dir:		inode of the parent directory
 * @dentry:		dentry of the child directory
 * @delegated_inode:	returns parent inode, if it's delegated.
 *
 * Remove a directory.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_rmdir(struct mnt_idmap *idmap, struct inode *dir,
	      struct dentry *dentry, struct delegated_inode *delegated_inode)
{
	int error = may_delete_dentry(idmap, dir, dentry, true);

	if (error)
		return error;

	if (!dir->i_op->rmdir)
		return -EPERM;

	dget(dentry);
	inode_lock(dentry->d_inode);

	error = -EBUSY;
	if (is_local_mountpoint(dentry) ||
	    (dentry->d_inode->i_flags & S_KERNEL_FILE))
		goto out;

	error = security_inode_rmdir(dir, dentry);
	if (error)
		goto out;

	error = try_break_deleg(dir, LEASE_BREAK_DIR_DELETE, delegated_inode);
	if (error)
		goto out;

	error = dir->i_op->rmdir(dir, dentry);
	if (error)
		goto out;

	shrink_dcache_parent(dentry);
	dentry->d_inode->i_flags |= S_DEAD;
	dont_mount(dentry);
	detach_mounts(dentry);

out:
	inode_unlock(dentry->d_inode);
	dput(dentry);
	if (!error)
		d_delete_notify(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_rmdir);

int filename_rmdir(int dfd, struct filename *name)
{
	int error;
	struct dentry *dentry;
	struct path path;
	struct qstr last;
	enum last_type type;
	unsigned int lookup_flags = 0;
	struct delegated_inode delegated_inode = { };
retry:
	error = filename_parentat(dfd, name, lookup_flags, &path, &last, &type);
	if (error)
		return error;

	switch (type) {
	case LAST_NORM:
		break;
	case LAST_DOTDOT:
		error = -ENOTEMPTY;
		goto exit2;
	case LAST_DOT:
		error = -EINVAL;
		goto exit2;
	case LAST_ROOT:
		error = -EBUSY;
		goto exit2;
	}

	error = mnt_want_write(path.mnt);
	if (error)
		goto exit2;

	dentry = start_dirop(path.dentry, &last, lookup_flags);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto exit3;
	error = security_path_rmdir(&path, dentry);
	if (error)
		goto exit4;
	error = vfs_rmdir(mnt_idmap(path.mnt), path.dentry->d_inode,
			  dentry, &delegated_inode);
exit4:
	end_dirop(dentry);
exit3:
	mnt_drop_write(path.mnt);
exit2:
	path_put(&path);
	if (is_delegated(&delegated_inode)) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry;
	}
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE1(rmdir, const char __user *, pathname)
{
	CLASS(filename, name)(pathname);
	return filename_rmdir(AT_FDCWD, name);
}

/**
 * vfs_unlink - unlink a filesystem object
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	parent directory
 * @dentry:	victim
 * @delegated_inode: returns victim inode, if the inode is delegated.
 *
 * The caller must hold dir->i_rwsem exclusively.
 *
 * If vfs_unlink discovers a delegation, it will return -EWOULDBLOCK and
 * return a reference to the inode in delegated_inode.  The caller
 * should then break the delegation on that inode and retry.  Because
 * breaking a delegation may take a long time, the caller should drop
 * dir->i_rwsem before doing so.
 *
 * Alternatively, a caller may pass NULL for delegated_inode.  This may
 * be appropriate for callers that expect the underlying filesystem not
 * to be NFS exported.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_unlink(struct mnt_idmap *idmap, struct inode *dir,
	       struct dentry *dentry, struct delegated_inode *delegated_inode)
{
	struct inode *target = dentry->d_inode;
	int error = may_delete_dentry(idmap, dir, dentry, false);

	if (error)
		return error;

	if (!dir->i_op->unlink)
		return -EPERM;

	inode_lock(target);
	if (IS_SWAPFILE(target))
		error = -EPERM;
	else if (is_local_mountpoint(dentry))
		error = -EBUSY;
	else {
		error = security_inode_unlink(dir, dentry);
		if (!error) {
			error = try_break_deleg(dir, LEASE_BREAK_DIR_DELETE, delegated_inode);
			if (error)
				goto out;
			error = try_break_deleg(target, 0, delegated_inode);
			if (error)
				goto out;
			error = dir->i_op->unlink(dir, dentry);
			if (!error) {
				dont_mount(dentry);
				detach_mounts(dentry);
			}
		}
	}
out:
	inode_unlock(target);

	/* We don't d_delete() NFS sillyrenamed files--they still exist. */
	if (!error && dentry->d_flags & DCACHE_NFSFS_RENAMED) {
		fsnotify_unlink(dir, dentry);
	} else if (!error) {
		fsnotify_link_count(target);
		d_delete_notify(dir, dentry);
	}

	return error;
}
EXPORT_SYMBOL(vfs_unlink);

/*
 * Make sure that the actual truncation of the file will occur outside its
 * directory's i_rwsem.  Truncate can take a long time if there is a lot of
 * writeout happening, and we don't want to prevent access to the directory
 * while waiting on the I/O.
 */
int filename_unlinkat(int dfd, struct filename *name)
{
	int error;
	struct dentry *dentry;
	struct path path;
	struct qstr last;
	enum last_type type;
	struct inode *inode;
	struct delegated_inode delegated_inode = { };
	unsigned int lookup_flags = 0;
retry:
	error = filename_parentat(dfd, name, lookup_flags, &path, &last, &type);
	if (error)
		return error;

	error = -EISDIR;
	if (type != LAST_NORM)
		goto exit_path_put;

	error = mnt_want_write(path.mnt);
	if (error)
		goto exit_path_put;
retry_deleg:
	dentry = start_dirop(path.dentry, &last, lookup_flags);
	error = PTR_ERR(dentry);
	if (IS_ERR(dentry))
		goto exit_drop_write;

	/* Why not before? Because we want correct error value */
	if (unlikely(last.name[last.len])) {
		if (d_is_dir(dentry))
			error = -EISDIR;
		else
			error = -ENOTDIR;
		end_dirop(dentry);
		goto exit_drop_write;
	}
	inode = dentry->d_inode;
	ihold(inode);
	error = security_path_unlink(&path, dentry);
	if (error)
		goto exit_end_dirop;
	error = vfs_unlink(mnt_idmap(path.mnt), path.dentry->d_inode,
			   dentry, &delegated_inode);
exit_end_dirop:
	end_dirop(dentry);
	iput(inode);	/* truncate the inode here */
	if (is_delegated(&delegated_inode)) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry_deleg;
	}
exit_drop_write:
	mnt_drop_write(path.mnt);
exit_path_put:
	path_put(&path);
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE3(unlinkat, int, dfd, const char __user *, pathname, int, flag)
{
	if ((flag & ~AT_REMOVEDIR) != 0)
		return -EINVAL;

	CLASS(filename, name)(pathname);
	if (flag & AT_REMOVEDIR)
		return filename_rmdir(dfd, name);
	return filename_unlinkat(dfd, name);
}

SYSCALL_DEFINE1(unlink, const char __user *, pathname)
{
	CLASS(filename, name)(pathname);
	return filename_unlinkat(AT_FDCWD, name);
}

/**
 * vfs_symlink - create symlink
 * @idmap:	idmap of the mount the inode was found from
 * @dir:	inode of the parent directory
 * @dentry:	dentry of the child symlink file
 * @oldname:	name of the file to link to
 * @delegated_inode: returns victim inode, if the inode is delegated.
 *
 * Create a symlink.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
		struct dentry *dentry, const char *oldname,
		struct delegated_inode *delegated_inode)
{
	int error;

	error = may_create_dentry(idmap, dir, dentry);
	if (error)
		return error;

	if (!dir->i_op->symlink)
		return -EPERM;

	error = security_inode_symlink(dir, dentry, oldname);
	if (error)
		return error;

	error = try_break_deleg(dir, LEASE_BREAK_DIR_CREATE, delegated_inode);
	if (error)
		return error;

	error = dir->i_op->symlink(idmap, dir, dentry, oldname);
	if (!error)
		fsnotify_create(dir, dentry);
	return error;
}
EXPORT_SYMBOL(vfs_symlink);

int filename_symlinkat(struct filename *from, int newdfd, struct filename *to)
{
	int error;
	struct dentry *dentry;
	struct path path;
	unsigned int lookup_flags = 0;
	struct delegated_inode delegated_inode = { };

	if (IS_ERR(from))
		return PTR_ERR(from);

retry:
	dentry = filename_create(newdfd, to, &path, lookup_flags);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	error = security_path_symlink(&path, dentry, from->name);
	if (!error)
		error = vfs_symlink(mnt_idmap(path.mnt), path.dentry->d_inode,
				    dentry, from->name, &delegated_inode);
	end_creating_path(&path, dentry);
	if (is_delegated(&delegated_inode)) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry;
	}
	if (retry_estale(error, lookup_flags)) {
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE3(symlinkat, const char __user *, oldname,
		int, newdfd, const char __user *, newname)
{
	CLASS(filename, old)(oldname);
	CLASS(filename, new)(newname);
	return filename_symlinkat(old, newdfd, new);
}

SYSCALL_DEFINE2(symlink, const char __user *, oldname, const char __user *, newname)
{
	CLASS(filename, old)(oldname);
	CLASS(filename, new)(newname);
	return filename_symlinkat(old, AT_FDCWD, new);
}

/**
 * vfs_link - create a new link
 * @old_dentry:	object to be linked
 * @idmap:	idmap of the mount
 * @dir:	new parent
 * @new_dentry:	where to create the new link
 * @delegated_inode: returns inode needing a delegation break
 *
 * The caller must hold dir->i_rwsem exclusively.
 *
 * If vfs_link discovers a delegation on the to-be-linked file in need
 * of breaking, it will return -EWOULDBLOCK and return a reference to the
 * inode in delegated_inode.  The caller should then break the delegation
 * and retry.  Because breaking a delegation may take a long time, the
 * caller should drop the i_rwsem before doing so.
 *
 * Alternatively, a caller may pass NULL for delegated_inode.  This may
 * be appropriate for callers that expect the underlying filesystem not
 * to be NFS exported.
 *
 * If the inode has been found through an idmapped mount the idmap of
 * the vfsmount must be passed through @idmap. This function will then take
 * care to map the inode according to @idmap before checking permissions.
 * On non-idmapped mounts or if permission checking is to be performed on the
 * raw inode simply pass @nop_mnt_idmap.
 */
int vfs_link(struct dentry *old_dentry, struct mnt_idmap *idmap,
	     struct inode *dir, struct dentry *new_dentry,
	     struct delegated_inode *delegated_inode)
{
	struct inode *inode = old_dentry->d_inode;
	unsigned max_links = dir->i_sb->s_max_links;
	int error;

	if (!inode)
		return -ENOENT;

	error = may_create_dentry(idmap, dir, new_dentry);
	if (error)
		return error;

	if (dir->i_sb != inode->i_sb)
		return -EXDEV;

	/*
	 * A link to an append-only or immutable file cannot be created.
	 */
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return -EPERM;
	/*
	 * Updating the link count will likely cause i_uid and i_gid to
	 * be written back improperly if their true value is unknown to
	 * the vfs.
	 */
	if (HAS_UNMAPPED_ID(idmap, inode))
		return -EPERM;
	if (!dir->i_op->link)
		return -EPERM;
	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	error = security_inode_link(old_dentry, dir, new_dentry);
	if (error)
		return error;

	inode_lock(inode);
	/* Make sure we don't allow creating hardlink to an unlinked file */
	if (inode->i_nlink == 0 && !(inode_state_read_once(inode) & I_LINKABLE))
		error =  -ENOENT;
	else if (max_links && inode->i_nlink >= max_links)
		error = -EMLINK;
	else {
		error = try_break_deleg(dir, LEASE_BREAK_DIR_CREATE, delegated_inode);
		if (!error)
			error = try_break_deleg(inode, 0, delegated_inode);
		if (!error)
			error = dir->i_op->link(old_dentry, dir, new_dentry);
	}

	if (!error && (inode_state_read_once(inode) & I_LINKABLE)) {
		spin_lock(&inode->i_lock);
		inode_state_clear(inode, I_LINKABLE);
		spin_unlock(&inode->i_lock);
	}
	inode_unlock(inode);
	if (!error)
		fsnotify_link(dir, inode, new_dentry);
	return error;
}
EXPORT_SYMBOL(vfs_link);

/*
 * Hardlinks are often used in delicate situations.  We avoid
 * security-related surprises by not following symlinks on the
 * newname.  --KAB
 *
 * We don't follow them on the oldname either to be compatible
 * with linux 2.0, and to avoid hard-linking to directories
 * and other special files.  --ADM
*/
int filename_linkat(int olddfd, struct filename *old,
		    int newdfd, struct filename *new, int flags)
{
	struct mnt_idmap *idmap;
	struct dentry *new_dentry;
	struct path old_path, new_path;
	struct delegated_inode delegated_inode = { };
	int how = 0;
	int error;

	if ((flags & ~(AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) != 0)
		return -EINVAL;
	/*
	 * To use null names we require CAP_DAC_READ_SEARCH or
	 * that the open-time creds of the dfd matches current.
	 * This ensures that not everyone will be able to create
	 * a hardlink using the passed file descriptor.
	 */
	if (flags & AT_EMPTY_PATH)
		how |= LOOKUP_LINKAT_EMPTY;

	if (flags & AT_SYMLINK_FOLLOW)
		how |= LOOKUP_FOLLOW;
retry:
	error = filename_lookup(olddfd, old, how, &old_path, NULL);
	if (error)
		return error;

	new_dentry = filename_create(newdfd, new, &new_path,
					(how & LOOKUP_REVAL));
	error = PTR_ERR(new_dentry);
	if (IS_ERR(new_dentry))
		goto out_putpath;

	error = -EXDEV;
	if (old_path.mnt != new_path.mnt)
		goto out_dput;
	idmap = mnt_idmap(new_path.mnt);
	error = may_linkat(idmap, &old_path);
	if (unlikely(error))
		goto out_dput;
	error = security_path_link(old_path.dentry, &new_path, new_dentry);
	if (error)
		goto out_dput;
	error = vfs_link(old_path.dentry, idmap, new_path.dentry->d_inode,
			 new_dentry, &delegated_inode);
out_dput:
	end_creating_path(&new_path, new_dentry);
	if (is_delegated(&delegated_inode)) {
		error = break_deleg_wait(&delegated_inode);
		if (!error) {
			path_put(&old_path);
			goto retry;
		}
	}
	if (retry_estale(error, how)) {
		path_put(&old_path);
		how |= LOOKUP_REVAL;
		goto retry;
	}
out_putpath:
	path_put(&old_path);
	return error;
}

SYSCALL_DEFINE5(linkat, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname, int, flags)
{
	CLASS(filename_uflags, old)(oldname, flags);
	CLASS(filename, new)(newname);
	return filename_linkat(olddfd, old, newdfd, new, flags);
}

SYSCALL_DEFINE2(link, const char __user *, oldname, const char __user *, newname)
{
	CLASS(filename, old)(oldname);
	CLASS(filename, new)(newname);
	return filename_linkat(AT_FDCWD, old, AT_FDCWD, new, 0);
}

/**
 * vfs_rename - rename a filesystem object
 * @rd:		pointer to &struct renamedata info
 *
 * The caller must hold multiple mutexes--see lock_rename()).
 *
 * If vfs_rename discovers a delegation in need of breaking at either
 * the source or destination, it will return -EWOULDBLOCK and return a
 * reference to the inode in delegated_inode.  The caller should then
 * break the delegation and retry.  Because breaking a delegation may
 * take a long time, the caller should drop all locks before doing
 * so.
 *
 * Alternatively, a caller may pass NULL for delegated_inode.  This may
 * be appropriate for callers that expect the underlying filesystem not
 * to be NFS exported.
 *
 * The worst of all namespace operations - renaming directory. "Perverted"
 * doesn't even start to describe it. Somebody in UCB had a heck of a trip...
 * Problems:
 *
 *	a) we can get into loop creation.
 *	b) race potential - two innocent renames can create a loop together.
 *	   That's where 4.4BSD screws up. Current fix: serialization on
 *	   sb->s_vfs_rename_mutex. We might be more accurate, but that's another
 *	   story.
 *	c) we may have to lock up to _four_ objects - parents and victim (if it exists),
 *	   and source (if it's a non-directory or a subdirectory that moves to
 *	   different parent).
 *	   And that - after we got ->i_rwsem on parents (until then we don't know
 *	   whether the target exists).  Solution: try to be smart with locking
 *	   order for inodes.  We rely on the fact that tree topology may change
 *	   only under ->s_vfs_rename_mutex _and_ that parent of the object we
 *	   move will be locked.  Thus we can rank directories by the tree
 *	   (ancestors first) and rank all non-directories after them.
 *	   That works since everybody except rename does "lock parent, lookup,
 *	   lock child" and rename is under ->s_vfs_rename_mutex.
 *	   HOWEVER, it relies on the assumption that any object with ->lookup()
 *	   has no more than 1 dentry.  If "hybrid" objects will ever appear,
 *	   we'd better make sure that there's no link(2) for them.
 *	d) conversion from fhandle to dentry may come in the wrong moment - when
 *	   we are removing the target. Solution: we will have to grab ->i_rwsem
 *	   in the fhandle_to_dentry code. [FIXME - current nfsfh.c relies on
 *	   ->i_rwsem on parents, which works but leads to some truly excessive
 *	   locking].
 */
int vfs_rename(struct renamedata *rd)
{
	int error;
	struct inode *old_dir = d_inode(rd->old_parent);
	struct inode *new_dir = d_inode(rd->new_parent);
	struct dentry *old_dentry = rd->old_dentry;
	struct dentry *new_dentry = rd->new_dentry;
	struct delegated_inode *delegated_inode = rd->delegated_inode;
	unsigned int flags = rd->flags;
	bool is_dir = d_is_dir(old_dentry);
	struct inode *source = old_dentry->d_inode;
	struct inode *target = new_dentry->d_inode;
	bool new_is_dir = false;
	unsigned max_links = new_dir->i_sb->s_max_links;
	struct name_snapshot old_name;
	bool lock_old_subdir, lock_new_subdir;

	if (source == target)
		return 0;

	error = may_delete_dentry(rd->mnt_idmap, old_dir, old_dentry, is_dir);
	if (error)
		return error;

	if (!target) {
		error = may_create_dentry(rd->mnt_idmap, new_dir, new_dentry);
	} else {
		new_is_dir = d_is_dir(new_dentry);

		if (!(flags & RENAME_EXCHANGE))
			error = may_delete_dentry(rd->mnt_idmap, new_dir,
						  new_dentry, is_dir);
		else
			error = may_delete_dentry(rd->mnt_idmap, new_dir,
						  new_dentry, new_is_dir);
	}
	if (error)
		return error;

	if (!old_dir->i_op->rename)
		return -EPERM;

	/*
	 * If we are going to change the parent - check write permissions,
	 * we'll need to flip '..'.
	 */
	if (new_dir != old_dir) {
		if (is_dir) {
			error = inode_permission(rd->mnt_idmap, source,
						 MAY_WRITE);
			if (error)
				return error;
		}
		if ((flags & RENAME_EXCHANGE) && new_is_dir) {
			error = inode_permission(rd->mnt_idmap, target,
						 MAY_WRITE);
			if (error)
				return error;
		}
	}

	error = security_inode_rename(old_dir, old_dentry, new_dir, new_dentry,
				      flags);
	if (error)
		return error;

	take_dentry_name_snapshot(&old_name, old_dentry);
	dget(new_dentry);
	/*
	 * Lock children.
	 * The source subdirectory needs to be locked on cross-directory
	 * rename or cross-directory exchange since its parent changes.
	 * The target subdirectory needs to be locked on cross-directory
	 * exchange due to parent change and on any rename due to becoming
	 * a victim.
	 * Non-directories need locking in all cases (for NFS reasons);
	 * they get locked after any subdirectories (in inode address order).
	 *
	 * NOTE: WE ONLY LOCK UNRELATED DIRECTORIES IN CROSS-DIRECTORY CASE.
	 * NEVER, EVER DO THAT WITHOUT ->s_vfs_rename_mutex.
	 */
	lock_old_subdir = new_dir != old_dir;
	lock_new_subdir = new_dir != old_dir || !(flags & RENAME_EXCHANGE);
	if (is_dir) {
		if (lock_old_subdir)
			inode_lock_nested(source, I_MUTEX_CHILD);
		if (target && (!new_is_dir || lock_new_subdir))
			inode_lock(target);
	} else if (new_is_dir) {
		if (lock_new_subdir)
			inode_lock_nested(target, I_MUTEX_CHILD);
		inode_lock(source);
	} else {
		lock_two_nondirectories(source, target);
	}

	error = -EPERM;
	if (IS_SWAPFILE(source) || (target && IS_SWAPFILE(target)))
		goto out;

	error = -EBUSY;
	if (is_local_mountpoint(old_dentry) || is_local_mountpoint(new_dentry))
		goto out;

	if (max_links && new_dir != old_dir) {
		error = -EMLINK;
		if (is_dir && !new_is_dir && new_dir->i_nlink >= max_links)
			goto out;
		if ((flags & RENAME_EXCHANGE) && !is_dir && new_is_dir &&
		    old_dir->i_nlink >= max_links)
			goto out;
	}
	error = try_break_deleg(old_dir,
				old_dir == new_dir ? LEASE_BREAK_DIR_RENAME :
						     LEASE_BREAK_DIR_DELETE,
				delegated_inode);
	if (error)
		goto out;
	if (new_dir != old_dir) {
		error = try_break_deleg(new_dir, LEASE_BREAK_DIR_CREATE, delegated_inode);
		if (error)
			goto out;
	}
	if (!is_dir) {
		error = try_break_deleg(source, 0, delegated_inode);
		if (error)
			goto out;
	}
	if (target && !new_is_dir) {
		error = try_break_deleg(target, 0, delegated_inode);
		if (error)
			goto out;
	}
	error = old_dir->i_op->rename(rd->mnt_idmap, old_dir, old_dentry,
				      new_dir, new_dentry, flags);
	if (error)
		goto out;

	if (!(flags & RENAME_EXCHANGE) && target) {
		if (is_dir) {
			shrink_dcache_parent(new_dentry);
			target->i_flags |= S_DEAD;
		}
		dont_mount(new_dentry);
		detach_mounts(new_dentry);
	}
	if (!(old_dir->i_sb->s_type->fs_flags & FS_RENAME_DOES_D_MOVE)) {
		if (!(flags & RENAME_EXCHANGE))
			d_move(old_dentry, new_dentry);
		else
			d_exchange(old_dentry, new_dentry);
	}
out:
	if (!is_dir || lock_old_subdir)
		inode_unlock(source);
	if (target && (!new_is_dir || lock_new_subdir))
		inode_unlock(target);
	dput(new_dentry);
	if (!error) {
		fsnotify_move(old_dir, new_dir, &old_name.name, is_dir,
			      !(flags & RENAME_EXCHANGE) ? target : NULL, old_dentry);
		if (flags & RENAME_EXCHANGE) {
			fsnotify_move(new_dir, old_dir, &old_dentry->d_name,
				      new_is_dir, NULL, new_dentry);
		}
	}
	release_dentry_name_snapshot(&old_name);

	return error;
}
EXPORT_SYMBOL(vfs_rename);

int filename_renameat2(int olddfd, struct filename *from,
		       int newdfd, struct filename *to, unsigned int flags)
{
	struct renamedata rd;
	struct path old_path, new_path;
	struct qstr old_last, new_last;
	enum last_type old_type, new_type;
	struct delegated_inode delegated_inode = { };
	unsigned int lookup_flags = 0;
	bool should_retry = false;
	int error;

	if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
		return -EINVAL;

	if ((flags & (RENAME_NOREPLACE | RENAME_WHITEOUT)) &&
	    (flags & RENAME_EXCHANGE))
		return -EINVAL;

retry:
	error = filename_parentat(olddfd, from, lookup_flags, &old_path,
				  &old_last, &old_type);
	if (error)
		return error;

	error = filename_parentat(newdfd, to, lookup_flags, &new_path, &new_last,
				  &new_type);
	if (error)
		goto exit1;

	error = -EXDEV;
	if (old_path.mnt != new_path.mnt)
		goto exit2;

	error = -EBUSY;
	if (old_type != LAST_NORM)
		goto exit2;

	if (flags & RENAME_NOREPLACE)
		error = -EEXIST;
	if (new_type != LAST_NORM)
		goto exit2;

	error = mnt_want_write(old_path.mnt);
	if (error)
		goto exit2;

retry_deleg:
	rd.old_parent	   = old_path.dentry;
	rd.mnt_idmap	   = mnt_idmap(old_path.mnt);
	rd.new_parent	   = new_path.dentry;
	rd.delegated_inode = &delegated_inode;
	rd.flags	   = flags;

	error = __start_renaming(&rd, lookup_flags, &old_last, &new_last);
	if (error)
		goto exit_lock_rename;

	if (flags & RENAME_EXCHANGE) {
		if (!d_is_dir(rd.new_dentry)) {
			error = -ENOTDIR;
			if (new_last.name[new_last.len])
				goto exit_unlock;
		}
	}
	/* unless the source is a directory trailing slashes give -ENOTDIR */
	if (!d_is_dir(rd.old_dentry)) {
		error = -ENOTDIR;
		if (old_last.name[old_last.len])
			goto exit_unlock;
		if (!(flags & RENAME_EXCHANGE) && new_last.name[new_last.len])
			goto exit_unlock;
	}

	error = security_path_rename(&old_path, rd.old_dentry,
				     &new_path, rd.new_dentry, flags);
	if (error)
		goto exit_unlock;

	error = vfs_rename(&rd);
exit_unlock:
	end_renaming(&rd);
exit_lock_rename:
	if (is_delegated(&delegated_inode)) {
		error = break_deleg_wait(&delegated_inode);
		if (!error)
			goto retry_deleg;
	}
	mnt_drop_write(old_path.mnt);
exit2:
	if (retry_estale(error, lookup_flags))
		should_retry = true;
	path_put(&new_path);
exit1:
	path_put(&old_path);
	if (should_retry) {
		should_retry = false;
		lookup_flags |= LOOKUP_REVAL;
		goto retry;
	}
	return error;
}

SYSCALL_DEFINE5(renameat2, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname, unsigned int, flags)
{
	CLASS(filename, old)(oldname);
	CLASS(filename, new)(newname);
	return filename_renameat2(olddfd, old, newdfd, new, flags);
}

SYSCALL_DEFINE4(renameat, int, olddfd, const char __user *, oldname,
		int, newdfd, const char __user *, newname)
{
	CLASS(filename, old)(oldname);
	CLASS(filename, new)(newname);
	return filename_renameat2(olddfd, old, newdfd, new, 0);
}

SYSCALL_DEFINE2(rename, const char __user *, oldname, const char __user *, newname)
{
	CLASS(filename, old)(oldname);
	CLASS(filename, new)(newname);
	return filename_renameat2(AT_FDCWD, old, AT_FDCWD, new, 0);
}

int readlink_copy(char __user *buffer, int buflen, const char *link, int linklen)
{
	int copylen;

	copylen = linklen;
	if (unlikely(copylen > (unsigned) buflen))
		copylen = buflen;
	if (copy_to_user(buffer, link, copylen))
		copylen = -EFAULT;
	return copylen;
}

/**
 * vfs_readlink - copy symlink body into userspace buffer
 * @dentry: dentry on which to get symbolic link
 * @buffer: user memory pointer
 * @buflen: size of buffer
 *
 * Does not touch atime.  That's up to the caller if necessary
 *
 * Does not call security hook.
 */
int vfs_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct inode *inode = d_inode(dentry);
	DEFINE_DELAYED_CALL(done);
	const char *link;
	int res;

	if (inode->i_opflags & IOP_CACHED_LINK)
		return readlink_copy(buffer, buflen, inode->i_link, inode->i_linklen);

	if (unlikely(!(inode->i_opflags & IOP_DEFAULT_READLINK))) {
		if (unlikely(inode->i_op->readlink))
			return inode->i_op->readlink(dentry, buffer, buflen);

		if (!d_is_symlink(dentry))
			return -EINVAL;

		spin_lock(&inode->i_lock);
		inode->i_opflags |= IOP_DEFAULT_READLINK;
		spin_unlock(&inode->i_lock);
	}

	link = READ_ONCE(inode->i_link);
	if (!link) {
		link = inode->i_op->get_link(dentry, inode, &done);
		if (IS_ERR(link))
			return PTR_ERR(link);
	}
	res = readlink_copy(buffer, buflen, link, strlen(link));
	do_delayed_call(&done);
	return res;
}
EXPORT_SYMBOL(vfs_readlink);

/**
 * vfs_get_link - get symlink body
 * @dentry: dentry on which to get symbolic link
 * @done: caller needs to free returned data with this
 *
 * Calls security hook and i_op->get_link() on the supplied inode.
 *
 * It does not touch atime.  That's up to the caller if necessary.
 *
 * Does not work on "special" symlinks like /proc/$$/fd/N
 */
const char *vfs_get_link(struct dentry *dentry, struct delayed_call *done)
{
	const char *res = ERR_PTR(-EINVAL);
	struct inode *inode = d_inode(dentry);

	if (d_is_symlink(dentry)) {
		res = ERR_PTR(security_inode_readlink(dentry));
		if (!res)
			res = inode->i_op->get_link(dentry, inode, done);
	}
	return res;
}
EXPORT_SYMBOL(vfs_get_link);

/* get the link contents into pagecache */
static char *__page_get_link(struct dentry *dentry, struct inode *inode,
			     struct delayed_call *callback)
{
	struct folio *folio;
	struct address_space *mapping = inode->i_mapping;

	if (!dentry) {
		folio = filemap_get_folio(mapping, 0);
		if (IS_ERR(folio))
			return ERR_PTR(-ECHILD);
		if (!folio_test_uptodate(folio)) {
			folio_put(folio);
			return ERR_PTR(-ECHILD);
		}
	} else {
		folio = read_mapping_folio(mapping, 0, NULL);
		if (IS_ERR(folio))
			return ERR_CAST(folio);
	}
	set_delayed_call(callback, page_put_link, folio);
	BUG_ON(mapping_gfp_mask(mapping) & __GFP_HIGHMEM);
	return folio_address(folio);
}

const char *page_get_link_raw(struct dentry *dentry, struct inode *inode,
			      struct delayed_call *callback)
{
	return __page_get_link(dentry, inode, callback);
}
EXPORT_SYMBOL_GPL(page_get_link_raw);

/**
 * page_get_link() - An implementation of the get_link inode_operation.
 * @dentry: The directory entry which is the symlink.
 * @inode: The inode for the symlink.
 * @callback: Used to drop the reference to the symlink.
 *
 * Filesystems which store their symlinks in the page cache should use
 * this to implement the get_link() member of their inode_operations.
 *
 * Return: A pointer to the NUL-terminated symlink.
 */
const char *page_get_link(struct dentry *dentry, struct inode *inode,
					struct delayed_call *callback)
{
	char *kaddr = __page_get_link(dentry, inode, callback);

	if (!IS_ERR(kaddr))
		nd_terminate_link(kaddr, inode->i_size, PAGE_SIZE - 1);
	return kaddr;
}
EXPORT_SYMBOL(page_get_link);

/**
 * page_put_link() - Drop the reference to the symlink.
 * @arg: The folio which contains the symlink.
 *
 * This is used internally by page_get_link().  It is exported for use
 * by filesystems which need to implement a variant of page_get_link()
 * themselves.  Despite the apparent symmetry, filesystems which use
 * page_get_link() do not need to call page_put_link().
 *
 * The argument, while it has a void pointer type, must be a pointer to
 * the folio which was retrieved from the page cache.  The delayed_call
 * infrastructure is used to drop the reference count once the caller
 * is done with the symlink.
 */
void page_put_link(void *arg)
{
	folio_put(arg);
}
EXPORT_SYMBOL(page_put_link);

int page_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	const char *link;
	int res;

	DEFINE_DELAYED_CALL(done);
	link = page_get_link(dentry, d_inode(dentry), &done);
	res = PTR_ERR(link);
	if (!IS_ERR(link))
		res = readlink_copy(buffer, buflen, link, strlen(link));
	do_delayed_call(&done);
	return res;
}
EXPORT_SYMBOL(page_readlink);

int page_symlink(struct inode *inode, const char *symname, int len)
{
	struct address_space *mapping = inode->i_mapping;
	const struct address_space_operations *aops = mapping->a_ops;
	bool nofs = !mapping_gfp_constraint(mapping, __GFP_FS);
	struct folio *folio;
	void *fsdata = NULL;
	int err;
	unsigned int flags;

retry:
	if (nofs)
		flags = memalloc_nofs_save();
	err = aops->write_begin(NULL, mapping, 0, len-1, &folio, &fsdata);
	if (nofs)
		memalloc_nofs_restore(flags);
	if (err)
		goto fail;

	memcpy(folio_address(folio), symname, len - 1);

	err = aops->write_end(NULL, mapping, 0, len - 1, len - 1,
						folio, fsdata);
	if (err < 0)
		goto fail;
	if (err < len-1)
		goto retry;

	mark_inode_dirty(inode);
	return 0;
fail:
	return err;
}
EXPORT_SYMBOL(page_symlink);

const struct inode_operations page_symlink_inode_operations = {
	.get_link	= page_get_link,
};
EXPORT_SYMBOL(page_symlink_inode_operations);
