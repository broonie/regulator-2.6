/*
 * linux/fs/nfsd/nfsfh.c
 *
 * NFS server file handle treatment.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 * Portions Copyright (C) 1999 G. Allen Morris III <gam3@acm.org>
 * Extensive rewrite by Neil Brown <neilb@cse.unsw.edu.au> Southern-Spring 1999
 * ... and again Southern-Winter 2001 to support export_operations
 */

#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/unistd.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/dcache.h>
#include <linux/exportfs.h>
#include <linux/mount.h>

#include <linux/sunrpc/clnt.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svcauth_gss.h>
#include <linux/nfsd/nfsd.h>
#include "auth.h"

#define NFSDDBG_FACILITY		NFSDDBG_FH


static int nfsd_nr_verified;
static int nfsd_nr_put;

/*
 * our acceptability function.
 * if NOSUBTREECHECK, accept anything
 * if not, require that we can walk up to exp->ex_dentry
 * doing some checks on the 'x' bits
 */
static int nfsd_acceptable(void *expv, struct dentry *dentry)
{
	struct svc_export *exp = expv;
	int rv;
	struct dentry *tdentry;
	struct dentry *parent;

	if (exp->ex_flags & NFSEXP_NOSUBTREECHECK)
		return 1;

	tdentry = dget(dentry);
	while (tdentry != exp->ex_path.dentry && !IS_ROOT(tdentry)) {
		/* make sure parents give x permission to user */
		int err;
		parent = dget_parent(tdentry);
		err = inode_permission(parent->d_inode, MAY_EXEC);
		if (err < 0) {
			dput(parent);
			break;
		}
		dput(tdentry);
		tdentry = parent;
	}
	if (tdentry != exp->ex_path.dentry)
		dprintk("nfsd_acceptable failed at %p %s\n", tdentry, tdentry->d_name.name);
	rv = (tdentry == exp->ex_path.dentry);
	dput(tdentry);
	return rv;
}

/* Type check. The correct error return for type mismatches does not seem to be
 * generally agreed upon. SunOS seems to use EISDIR if file isn't S_IFREG; a
 * comment in the NFSv3 spec says this is incorrect (implementation notes for
 * the write call).
 */
static inline __be32
nfsd_mode_check(struct svc_rqst *rqstp, umode_t mode, int type)
{
	/* Type can be negative when creating hardlinks - not to a dir */
	if (type > 0 && (mode & S_IFMT) != type) {
		if (rqstp->rq_vers == 4 && (mode & S_IFMT) == S_IFLNK)
			return nfserr_symlink;
		else if (type == S_IFDIR)
			return nfserr_notdir;
		else if ((mode & S_IFMT) == S_IFDIR)
			return nfserr_isdir;
		else
			return nfserr_inval;
	}
	if (type < 0 && (mode & S_IFMT) == -type) {
		if (rqstp->rq_vers == 4 && (mode & S_IFMT) == S_IFLNK)
			return nfserr_symlink;
		else if (type == -S_IFDIR)
			return nfserr_isdir;
		else
			return nfserr_notdir;
	}
	return 0;
}

static __be32 nfsd_setuser_and_check_port(struct svc_rqst *rqstp,
					  struct svc_export *exp)
{
	/* Check if the request originated from a secure port. */
	if (!rqstp->rq_secure && EX_SECURE(exp)) {
		RPC_IFDEBUG(char buf[RPC_MAX_ADDRBUFLEN]);
		dprintk(KERN_WARNING
		       "nfsd: request from insecure port %s!\n",
		       svc_print_addr(rqstp, buf, sizeof(buf)));
		return nfserr_perm;
	}

	/* Set user creds for this exportpoint */
	return nfserrno(nfsd_setuser(rqstp, exp));
}

/*
 * Use the given filehandle to look up the corresponding export and
 * dentry.  On success, the results are used to set fh_export and
 * fh_dentry.
 */
static __be32 nfsd_set_fh_dentry(struct svc_rqst *rqstp, struct svc_fh *fhp)
{
	struct knfsd_fh	*fh = &fhp->fh_handle;
	struct fid *fid = NULL, sfid;
	struct svc_export *exp;
	struct dentry *dentry;
	int fileid_type;
	int data_left = fh->fh_size/4;
	__be32 error;

	error = nfserr_stale;
	if (rqstp->rq_vers > 2)
		error = nfserr_badhandle;
	if (rqstp->rq_vers == 4 && fh->fh_size == 0)
		return nfserr_nofilehandle;

	if (fh->fh_version == 1) {
		int len;

		if (--data_left < 0)
			return error;
		if (fh->fh_auth_type != 0)
			return error;
		len = key_len(fh->fh_fsid_type) / 4;
		if (len == 0)
			return error;
		if  (fh->fh_fsid_type == FSID_MAJOR_MINOR) {
			/* deprecated, convert to type 3 */
			len = key_len(FSID_ENCODE_DEV)/4;
			fh->fh_fsid_type = FSID_ENCODE_DEV;
			fh->fh_fsid[0] = new_encode_dev(MKDEV(ntohl(fh->fh_fsid[0]), ntohl(fh->fh_fsid[1])));
			fh->fh_fsid[1] = fh->fh_fsid[2];
		}
		data_left -= len;
		if (data_left < 0)
			return error;
		exp = rqst_exp_find(rqstp, fh->fh_fsid_type, fh->fh_auth);
		fid = (struct fid *)(fh->fh_auth + len);
	} else {
		__u32 tfh[2];
		dev_t xdev;
		ino_t xino;

		if (fh->fh_size != NFS_FHSIZE)
			return error;
		/* assume old filehandle format */
		xdev = old_decode_dev(fh->ofh_xdev);
		xino = u32_to_ino_t(fh->ofh_xino);
		mk_fsid(FSID_DEV, tfh, xdev, xino, 0, NULL);
		exp = rqst_exp_find(rqstp, FSID_DEV, tfh);
	}

	error = nfserr_stale;
	if (PTR_ERR(exp) == -ENOENT)
		return error;

	if (IS_ERR(exp))
		return nfserrno(PTR_ERR(exp));

	if (exp->ex_flags & NFSEXP_NOSUBTREECHECK) {
		/* Elevate privileges so that the lack of 'r' or 'x'
		 * permission on some parent directory will
		 * not stop exportfs_decode_fh from being able
		 * to reconnect a directory into the dentry cache.
		 * The same problem can affect "SUBTREECHECK" exports,
		 * but as nfsd_acceptable depends on correct
		 * access control settings being in effect, we cannot
		 * fix that case easily.
		 */
		struct cred *new = prepare_creds();
		if (!new)
			return nfserrno(-ENOMEM);
		new->cap_effective =
			cap_raise_nfsd_set(new->cap_effective,
					   new->cap_permitted);
		put_cred(override_creds(new));
		put_cred(new);
	} else {
		error = nfsd_setuser_and_check_port(rqstp, exp);
		if (error)
			goto out;
	}

	/*
	 * Look up the dentry using the NFS file handle.
	 */
	error = nfserr_stale;
	if (rqstp->rq_vers > 2)
		error = nfserr_badhandle;

	if (fh->fh_version != 1) {
		sfid.i32.ino = fh->ofh_ino;
		sfid.i32.gen = fh->ofh_generation;
		sfid.i32.parent_ino = fh->ofh_dirino;
		fid = &sfid;
		data_left = 3;
		if (fh->ofh_dirino == 0)
			fileid_type = FILEID_INO32_GEN;
		else
			fileid_type = FILEID_INO32_GEN_PARENT;
	} else
		fileid_type = fh->fh_fileid_type;

	if (fileid_type == FILEID_ROOT)
		dentry = dget(exp->ex_path.dentry);
	else {
		dentry = exportfs_decode_fh(exp->ex_path.mnt, fid,
				data_left, fileid_type,
				nfsd_acceptable, exp);
	}
	if (dentry == NULL)
		goto out;
	if (IS_ERR(dentry)) {
		if (PTR_ERR(dentry) != -EINVAL)
			error = nfserrno(PTR_ERR(dentry));
		goto out;
	}

	if (exp->ex_flags & NFSEXP_NOSUBTREECHECK) {
		error = nfsd_setuser_and_check_port(rqstp, exp);
		if (error) {
			dput(dentry);
			goto out;
		}
	}

	if (S_ISDIR(dentry->d_inode->i_mode) &&
			(dentry->d_flags & DCACHE_DISCONNECTED)) {
		printk("nfsd: find_fh_dentry returned a DISCONNECTED directory: %s/%s\n",
				dentry->d_parent->d_name.name, dentry->d_name.name);
	}

	fhp->fh_dentry = dentry;
	fhp->fh_export = exp;
	nfsd_nr_verified++;
	return 0;
out:
	exp_put(exp);
	return error;
}

/*
 * Perform sanity checks on the dentry in a client's file handle.
 *
 * Note that the file handle dentry may need to be freed even after
 * an error return.
 *
 * This is only called at the start of an nfsproc call, so fhp points to
 * a svc_fh which is all 0 except for the over-the-wire file handle.
 */
__be32
fh_verify(struct svc_rqst *rqstp, struct svc_fh *fhp, int type, int access)
{
	struct svc_export *exp;
	struct dentry	*dentry;
	__be32		error;

	dprintk("nfsd: fh_verify(%s)\n", SVCFH_fmt(fhp));

	if (!fhp->fh_dentry) {
		error = nfsd_set_fh_dentry(rqstp, fhp);
		if (error)
			goto out;
		dentry = fhp->fh_dentry;
		exp = fhp->fh_export;
	} else {
		/*
		 * just rechecking permissions
		 * (e.g. nfsproc_create calls fh_verify, then nfsd_create
		 * does as well)
		 */
		dprintk("nfsd: fh_verify - just checking\n");
		dentry = fhp->fh_dentry;
		exp = fhp->fh_export;
		/*
		 * Set user creds for this exportpoint; necessary even
		 * in the "just checking" case because this may be a
		 * filehandle that was created by fh_compose, and that
		 * is about to be used in another nfsv4 compound
		 * operation.
		 */
		error = nfsd_setuser_and_check_port(rqstp, exp);
		if (error)
			goto out;
	}

	error = nfsd_mode_check(rqstp, dentry->d_inode->i_mode, type);
	if (error)
		goto out;

	/*
	 * pseudoflavor restrictions are not enforced on NLM,
	 * which clients virtually always use auth_sys for,
	 * even while using RPCSEC_GSS for NFS.
	 */
	if (access & NFSD_MAY_LOCK)
		goto skip_pseudoflavor_check;
	/*
	 * Clients may expect to be able to use auth_sys during mount,
	 * even if they use gss for everything else; see section 2.3.2
	 * of rfc 2623.
	 */
	if (access & NFSD_MAY_BYPASS_GSS_ON_ROOT
			&& exp->ex_path.dentry == dentry)
		goto skip_pseudoflavor_check;

	error = check_nfsd_access(exp, rqstp);
	if (error)
		goto out;

skip_pseudoflavor_check:
	/* Finally, check access permissions. */
	error = nfsd_permission(rqstp, exp, dentry, access);

	if (error) {
		dprintk("fh_verify: %s/%s permission failure, "
			"acc=%x, error=%d\n",
			dentry->d_parent->d_name.name,
			dentry->d_name.name,
			access, ntohl(error));
	}
out:
	if (error == nfserr_stale)
		nfsdstats.fh_stale++;
	return error;
}


/*
 * Compose a file handle for an NFS reply.
 *
 * Note that when first composed, the dentry may not yet have
 * an inode.  In this case a call to fh_update should be made
 * before the fh goes out on the wire ...
 */
static void _fh_update(struct svc_fh *fhp, struct svc_export *exp,
		struct dentry *dentry)
{
	if (dentry != exp->ex_path.dentry) {
		struct fid *fid = (struct fid *)
			(fhp->fh_handle.fh_auth + fhp->fh_handle.fh_size/4 - 1);
		int maxsize = (fhp->fh_maxsize - fhp->fh_handle.fh_size)/4;
		int subtreecheck = !(exp->ex_flags & NFSEXP_NOSUBTREECHECK);

		fhp->fh_handle.fh_fileid_type =
			exportfs_encode_fh(dentry, fid, &maxsize, subtreecheck);
		fhp->fh_handle.fh_size += maxsize * 4;
	} else {
		fhp->fh_handle.fh_fileid_type = FILEID_ROOT;
	}
}

/*
 * for composing old style file handles
 */
static inline void _fh_update_old(struct dentry *dentry,
				  struct svc_export *exp,
				  struct knfsd_fh *fh)
{
	fh->ofh_ino = ino_t_to_u32(dentry->d_inode->i_ino);
	fh->ofh_generation = dentry->d_inode->i_generation;
	if (S_ISDIR(dentry->d_inode->i_mode) ||
	    (exp->ex_flags & NFSEXP_NOSUBTREECHECK))
		fh->ofh_dirino = 0;
}

__be32
fh_compose(struct svc_fh *fhp, struct svc_export *exp, struct dentry *dentry,
	   struct svc_fh *ref_fh)
{
	/* ref_fh is a reference file handle.
	 * if it is non-null and for the same filesystem, then we should compose
	 * a filehandle which is of the same version, where possible.
	 * Currently, that means that if ref_fh->fh_handle.fh_version == 0xca
	 * Then create a 32byte filehandle using nfs_fhbase_old
	 *
	 */

	u8 version;
	u8 fsid_type = 0;
	struct inode * inode = dentry->d_inode;
	struct dentry *parent = dentry->d_parent;
	__u32 *datap;
	dev_t ex_dev = exp->ex_path.dentry->d_inode->i_sb->s_dev;
	int root_export = (exp->ex_path.dentry == exp->ex_path.dentry->d_sb->s_root);

	dprintk("nfsd: fh_compose(exp %02x:%02x/%ld %s/%s, ino=%ld)\n",
		MAJOR(ex_dev), MINOR(ex_dev),
		(long) exp->ex_path.dentry->d_inode->i_ino,
		parent->d_name.name, dentry->d_name.name,
		(inode ? inode->i_ino : 0));

	/* Choose filehandle version and fsid type based on
	 * the reference filehandle (if it is in the same export)
	 * or the export options.
	 */
 retry:
	version = 1;
	if (ref_fh && ref_fh->fh_export == exp) {
		version = ref_fh->fh_handle.fh_version;
		fsid_type = ref_fh->fh_handle.fh_fsid_type;

		if (ref_fh == fhp)
			fh_put(ref_fh);
		ref_fh = NULL;

		switch (version) {
		case 0xca:
			fsid_type = FSID_DEV;
			break;
		case 1:
			break;
		default:
			goto retry;
		}

		/* Need to check that this type works for this
		 * export point.  As the fsid -> filesystem mapping
		 * was guided by user-space, there is no guarantee
		 * that the filesystem actually supports that fsid
		 * type. If it doesn't we loop around again without
		 * ref_fh set.
		 */
		switch(fsid_type) {
		case FSID_DEV:
			if (!old_valid_dev(ex_dev))
				goto retry;
			/* FALL THROUGH */
		case FSID_MAJOR_MINOR:
		case FSID_ENCODE_DEV:
			if (!(exp->ex_path.dentry->d_inode->i_sb->s_type->fs_flags
			      & FS_REQUIRES_DEV))
				goto retry;
			break;
		case FSID_NUM:
			if (! (exp->ex_flags & NFSEXP_FSID))
				goto retry;
			break;
		case FSID_UUID8:
		case FSID_UUID16:
			if (!root_export)
				goto retry;
			/* fall through */
		case FSID_UUID4_INUM:
		case FSID_UUID16_INUM:
			if (exp->ex_uuid == NULL)
				goto retry;
			break;
		}
	} else if (exp->ex_uuid) {
		if (fhp->fh_maxsize >= 64) {
			if (root_export)
				fsid_type = FSID_UUID16;
			else
				fsid_type = FSID_UUID16_INUM;
		} else {
			if (root_export)
				fsid_type = FSID_UUID8;
			else
				fsid_type = FSID_UUID4_INUM;
		}
	} else if (exp->ex_flags & NFSEXP_FSID)
		fsid_type = FSID_NUM;
	else if (!old_valid_dev(ex_dev))
		/* for newer device numbers, we must use a newer fsid format */
		fsid_type = FSID_ENCODE_DEV;
	else
		fsid_type = FSID_DEV;

	if (ref_fh == fhp)
		fh_put(ref_fh);

	if (fhp->fh_locked || fhp->fh_dentry) {
		printk(KERN_ERR "fh_compose: fh %s/%s not initialized!\n",
		       parent->d_name.name, dentry->d_name.name);
	}
	if (fhp->fh_maxsize < NFS_FHSIZE)
		printk(KERN_ERR "fh_compose: called with maxsize %d! %s/%s\n",
		       fhp->fh_maxsize,
		       parent->d_name.name, dentry->d_name.name);

	fhp->fh_dentry = dget(dentry); /* our internal copy */
	fhp->fh_export = exp;
	cache_get(&exp->h);

	if (version == 0xca) {
		/* old style filehandle please */
		memset(&fhp->fh_handle.fh_base, 0, NFS_FHSIZE);
		fhp->fh_handle.fh_size = NFS_FHSIZE;
		fhp->fh_handle.ofh_dcookie = 0xfeebbaca;
		fhp->fh_handle.ofh_dev =  old_encode_dev(ex_dev);
		fhp->fh_handle.ofh_xdev = fhp->fh_handle.ofh_dev;
		fhp->fh_handle.ofh_xino =
			ino_t_to_u32(exp->ex_path.dentry->d_inode->i_ino);
		fhp->fh_handle.ofh_dirino = ino_t_to_u32(parent_ino(dentry));
		if (inode)
			_fh_update_old(dentry, exp, &fhp->fh_handle);
	} else {
		int len;
		fhp->fh_handle.fh_version = 1;
		fhp->fh_handle.fh_auth_type = 0;
		datap = fhp->fh_handle.fh_auth+0;
		fhp->fh_handle.fh_fsid_type = fsid_type;
		mk_fsid(fsid_type, datap, ex_dev,
			exp->ex_path.dentry->d_inode->i_ino,
			exp->ex_fsid, exp->ex_uuid);

		len = key_len(fsid_type);
		datap += len/4;
		fhp->fh_handle.fh_size = 4 + len;

		if (inode)
			_fh_update(fhp, exp, dentry);
		if (fhp->fh_handle.fh_fileid_type == 255)
			return nfserr_opnotsupp;
	}

	nfsd_nr_verified++;
	return 0;
}

/*
 * Update file handle information after changing a dentry.
 * This is only called by nfsd_create, nfsd_create_v3 and nfsd_proc_create
 */
__be32
fh_update(struct svc_fh *fhp)
{
	struct dentry *dentry;

	if (!fhp->fh_dentry)
		goto out_bad;

	dentry = fhp->fh_dentry;
	if (!dentry->d_inode)
		goto out_negative;
	if (fhp->fh_handle.fh_version != 1) {
		_fh_update_old(dentry, fhp->fh_export, &fhp->fh_handle);
	} else {
		if (fhp->fh_handle.fh_fileid_type != FILEID_ROOT)
			goto out;

		_fh_update(fhp, fhp->fh_export, dentry);
		if (fhp->fh_handle.fh_fileid_type == 255)
			return nfserr_opnotsupp;
	}
out:
	return 0;

out_bad:
	printk(KERN_ERR "fh_update: fh not verified!\n");
	goto out;
out_negative:
	printk(KERN_ERR "fh_update: %s/%s still negative!\n",
		dentry->d_parent->d_name.name, dentry->d_name.name);
	goto out;
}

/*
 * Release a file handle.
 */
void
fh_put(struct svc_fh *fhp)
{
	struct dentry * dentry = fhp->fh_dentry;
	struct svc_export * exp = fhp->fh_export;
	if (dentry) {
		fh_unlock(fhp);
		fhp->fh_dentry = NULL;
		dput(dentry);
#ifdef CONFIG_NFSD_V3
		fhp->fh_pre_saved = 0;
		fhp->fh_post_saved = 0;
#endif
		nfsd_nr_put++;
	}
	if (exp) {
		cache_put(&exp->h, &svc_export_cache);
		fhp->fh_export = NULL;
	}
	return;
}

/*
 * Shorthand for dprintk()'s
 */
char * SVCFH_fmt(struct svc_fh *fhp)
{
	struct knfsd_fh *fh = &fhp->fh_handle;

	static char buf[80];
	sprintf(buf, "%d: %08x %08x %08x %08x %08x %08x",
		fh->fh_size,
		fh->fh_base.fh_pad[0],
		fh->fh_base.fh_pad[1],
		fh->fh_base.fh_pad[2],
		fh->fh_base.fh_pad[3],
		fh->fh_base.fh_pad[4],
		fh->fh_base.fh_pad[5]);
	return buf;
}

enum fsid_source fsid_source(struct svc_fh *fhp)
{
	if (fhp->fh_handle.fh_version != 1)
		return FSIDSOURCE_DEV;
	switch(fhp->fh_handle.fh_fsid_type) {
	case FSID_DEV:
	case FSID_ENCODE_DEV:
	case FSID_MAJOR_MINOR:
		if (fhp->fh_export->ex_path.dentry->d_inode->i_sb->s_type->fs_flags
		    & FS_REQUIRES_DEV)
			return FSIDSOURCE_DEV;
		break;
	case FSID_NUM:
		if (fhp->fh_export->ex_flags & NFSEXP_FSID)
			return FSIDSOURCE_FSID;
		break;
	default:
		break;
	}
	/* either a UUID type filehandle, or the filehandle doesn't
	 * match the export.
	 */
	if (fhp->fh_export->ex_flags & NFSEXP_FSID)
		return FSIDSOURCE_FSID;
	if (fhp->fh_export->ex_uuid)
		return FSIDSOURCE_UUID;
	return FSIDSOURCE_DEV;
}
