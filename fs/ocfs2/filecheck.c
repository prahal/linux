/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * filecheck.c
 *
 * Code which implements online file check.
 *
 * Copyright (C) 2007, 2009 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/sysctl.h>
#include <cluster/masklog.h>

#include "ocfs2.h"
#include "ocfs2_fs.h"
#include "stackglue.h"
#include "inode.h"

#include "filecheck.h"


/* File check error strings,
 * must correspond with error number in header file.
 */
static const char * const ocfs2_filecheck_errs[] = {
	"SUCCESS",
	"FAILED",
	"INPROGRESS",
	"READONLY",
	"INVALIDINO",
	"BLOCKECC",
	"BLOCKNO",
	"VALIDFLAG",
	"GENERATION",
	"UNSUPPORTED"
};

static DEFINE_SPINLOCK(ocfs2_filecheck_sysfs_lock);
static LIST_HEAD(ocfs2_filecheck_sysfs_list);

struct ocfs2_filecheck {
	struct list_head fc_head;	/* File check entry list head */
	spinlock_t fc_lock;
	unsigned int fc_max;	/* Maximum number of entry in list */
	unsigned int fc_size;	/* Current entry count in list */
	unsigned int fc_done;	/* File check entries are done in list */
};

struct ocfs2_filecheck_sysfs_entry {
	struct list_head fs_list;
	atomic_t fs_count;
	struct super_block *fs_sb;
	struct kset *fs_kset;
	struct ocfs2_filecheck *fs_fcheck;
};

#define OCFS2_FILECHECK_MAXSIZE		100
#define OCFS2_FILECHECK_MINSIZE		10

/* File check operation type */
enum {
	OCFS2_FILECHECK_TYPE_CHK = 0,	/* Check a file */
	OCFS2_FILECHECK_TYPE_FIX,	/* Fix a file */
	OCFS2_FILECHECK_TYPE_SET = 100	/* Set file check options */
};

struct ocfs2_filecheck_entry {
	struct list_head fe_list;
	unsigned long fe_ino;
	unsigned int fe_type;
	unsigned short fe_done:1;
	unsigned short fe_status:15;
};

struct ocfs2_filecheck_args {
	unsigned int fa_type;
	union {
		unsigned long fa_ino;
		unsigned int fa_len;
	};
};

static const char *
ocfs2_filecheck_error(int errno)
{
	if (!errno)
		return ocfs2_filecheck_errs[errno];

	BUG_ON(errno < OCFS2_FILECHECK_ERR_START ||
			errno > OCFS2_FILECHECK_ERR_END);
	return ocfs2_filecheck_errs[errno - OCFS2_FILECHECK_ERR_START + 1];
}

static ssize_t ocfs2_filecheck_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf);
static ssize_t ocfs2_filecheck_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count);
static struct kobj_attribute ocfs2_attr_filecheck =
					__ATTR(filecheck, S_IRUSR | S_IWUSR,
					ocfs2_filecheck_show,
					ocfs2_filecheck_store);

static int ocfs2_filecheck_sysfs_wait(atomic_t *p)
{
	schedule();
	return 0;
}

static void
ocfs2_filecheck_sysfs_free(struct ocfs2_filecheck_sysfs_entry *entry)
{
	struct ocfs2_filecheck_entry *p;

	if (!atomic_dec_and_test(&entry->fs_count))
		wait_on_atomic_t(&entry->fs_count, ocfs2_filecheck_sysfs_wait,
						TASK_UNINTERRUPTIBLE);

	spin_lock(&entry->fs_fcheck->fc_lock);
	while (!list_empty(&entry->fs_fcheck->fc_head)) {
		p = list_first_entry(&entry->fs_fcheck->fc_head,
				struct ocfs2_filecheck_entry, fe_list);
		list_del(&p->fe_list);
		BUG_ON(!p->fe_done); /* To free a undone file check entry */
		kfree(p);
	}
	spin_unlock(&entry->fs_fcheck->fc_lock);

	kset_unregister(entry->fs_kset);
	kfree(entry->fs_fcheck);
	kfree(entry);
}

static void
ocfs2_filecheck_sysfs_add(struct ocfs2_filecheck_sysfs_entry *entry)
{
	spin_lock(&ocfs2_filecheck_sysfs_lock);
	list_add_tail(&entry->fs_list, &ocfs2_filecheck_sysfs_list);
	spin_unlock(&ocfs2_filecheck_sysfs_lock);
}

static int ocfs2_filecheck_sysfs_del(const char *devname)
{
	struct ocfs2_filecheck_sysfs_entry *p;

	spin_lock(&ocfs2_filecheck_sysfs_lock);
	list_for_each_entry(p, &ocfs2_filecheck_sysfs_list, fs_list) {
		if (!strcmp(p->fs_sb->s_id, devname)) {
			list_del(&p->fs_list);
			spin_unlock(&ocfs2_filecheck_sysfs_lock);
			ocfs2_filecheck_sysfs_free(p);
			return 0;
		}
	}
	spin_unlock(&ocfs2_filecheck_sysfs_lock);
	return 1;
}

static void
ocfs2_filecheck_sysfs_put(struct ocfs2_filecheck_sysfs_entry *entry)
{
	if (atomic_dec_and_test(&entry->fs_count))
		wake_up_atomic_t(&entry->fs_count);
}

static struct ocfs2_filecheck_sysfs_entry *
ocfs2_filecheck_sysfs_get(const char *devname)
{
	struct ocfs2_filecheck_sysfs_entry *p = NULL;

	spin_lock(&ocfs2_filecheck_sysfs_lock);
	list_for_each_entry(p, &ocfs2_filecheck_sysfs_list, fs_list) {
		if (!strcmp(p->fs_sb->s_id, devname)) {
			atomic_inc(&p->fs_count);
			spin_unlock(&ocfs2_filecheck_sysfs_lock);
			return p;
		}
	}
	spin_unlock(&ocfs2_filecheck_sysfs_lock);
	return NULL;
}

int ocfs2_filecheck_create_sysfs(struct super_block *sb)
{
	int ret = 0;
	struct kset *ocfs2_filecheck_kset = NULL;
	struct ocfs2_filecheck *fcheck = NULL;
	struct ocfs2_filecheck_sysfs_entry *entry = NULL;
	struct attribute **attrs = NULL;
	struct attribute_group attrgp;

	if (!ocfs2_kset)
		return -ENOMEM;

	attrs = kmalloc(sizeof(struct attribute *) * 2, GFP_NOFS);
	if (!attrs) {
		ret = -ENOMEM;
		goto error;
	} else {
		attrs[0] = &ocfs2_attr_filecheck.attr;
		attrs[1] = NULL;
		memset(&attrgp, 0, sizeof(attrgp));
		attrgp.attrs = attrs;
	}

	fcheck = kmalloc(sizeof(struct ocfs2_filecheck), GFP_NOFS);
	if (!fcheck) {
		ret = -ENOMEM;
		goto error;
	} else {
		INIT_LIST_HEAD(&fcheck->fc_head);
		spin_lock_init(&fcheck->fc_lock);
		fcheck->fc_max = OCFS2_FILECHECK_MINSIZE;
		fcheck->fc_size = 0;
		fcheck->fc_done = 0;
	}

	if (strlen(sb->s_id) <= 0) {
		mlog(ML_ERROR,
		"Cannot get device basename when create filecheck sysfs\n");
		ret = -ENODEV;
		goto error;
	}

	ocfs2_filecheck_kset = kset_create_and_add(sb->s_id, NULL,
						&ocfs2_kset->kobj);
	if (!ocfs2_filecheck_kset) {
		ret = -ENOMEM;
		goto error;
	}

	ret = sysfs_create_group(&ocfs2_filecheck_kset->kobj, &attrgp);
	if (ret)
		goto error;

	entry = kmalloc(sizeof(struct ocfs2_filecheck_sysfs_entry), GFP_NOFS);
	if (!entry) {
		ret = -ENOMEM;
		goto error;
	} else {
		atomic_set(&entry->fs_count, 1);
		entry->fs_sb = sb;
		entry->fs_kset = ocfs2_filecheck_kset;
		entry->fs_fcheck = fcheck;
		ocfs2_filecheck_sysfs_add(entry);
	}

	kfree(attrs);
	return 0;

error:
	kfree(attrs);
	kfree(entry);
	kfree(fcheck);
	kset_unregister(ocfs2_filecheck_kset);
	return ret;
}

int ocfs2_filecheck_remove_sysfs(struct super_block *sb)
{
	return ocfs2_filecheck_sysfs_del(sb->s_id);
}

static int
ocfs2_filecheck_erase_entries(struct ocfs2_filecheck_sysfs_entry *ent,
				unsigned int count);
static int
ocfs2_filecheck_adjust_max(struct ocfs2_filecheck_sysfs_entry *ent,
				unsigned int len)
{
	int ret;

	if ((len < OCFS2_FILECHECK_MINSIZE) || (len > OCFS2_FILECHECK_MAXSIZE))
		return -EINVAL;

	spin_lock(&ent->fs_fcheck->fc_lock);
	if (len < (ent->fs_fcheck->fc_size - ent->fs_fcheck->fc_done)) {
		mlog(ML_ERROR,
		"Cannot set online file check maximum entry number "
		"to %u due to too many pending entries(%u)\n",
		len, ent->fs_fcheck->fc_size - ent->fs_fcheck->fc_done);
		ret = -EBUSY;
	} else {
		if (len < ent->fs_fcheck->fc_size)
			BUG_ON(!ocfs2_filecheck_erase_entries(ent,
				ent->fs_fcheck->fc_size - len));

		ent->fs_fcheck->fc_max = len;
		ret = 0;
	}
	spin_unlock(&ent->fs_fcheck->fc_lock);

	return ret;
}

#define OCFS2_FILECHECK_ARGS_LEN	32
static int
ocfs2_filecheck_args_get_long(const char *buf, size_t count,
				unsigned long *val)
{
	char buffer[OCFS2_FILECHECK_ARGS_LEN];

	if (count < 1)
		return 1;

	memcpy(buffer, buf, count);
	buffer[count] = '\0';

	if (kstrtoul(buffer, 0, val))
		return 1;

	return 0;
}

static int
ocfs2_filecheck_args_parse(const char *buf, size_t count,
				struct ocfs2_filecheck_args *args)
{
	unsigned long val = 0;

	/* too short/long args length */
	if ((count < 5) || (count > OCFS2_FILECHECK_ARGS_LEN))
		return 1;

	if ((strncmp(buf, "FIX ", 4) == 0) ||
		(strncmp(buf, "fix ", 4) == 0)) {
		if (ocfs2_filecheck_args_get_long(buf + 4, count - 4, &val))
			return 1;

		args->fa_type = OCFS2_FILECHECK_TYPE_FIX;
		args->fa_ino = val;
		return 0;
	} else if ((strncmp(buf, "CHECK ", 6) == 0) ||
		(strncmp(buf, "check ", 6) == 0)) {
		if (ocfs2_filecheck_args_get_long(buf + 6, count - 6, &val))
			return 1;

		args->fa_type = OCFS2_FILECHECK_TYPE_CHK;
		args->fa_ino = val;
		return 0;
	} else if ((strncmp(buf, "SET ", 4) == 0) ||
		(strncmp(buf, "set ", 4) == 0)) {
		if (ocfs2_filecheck_args_get_long(buf + 4, count - 4, &val))
			return 1;

		args->fa_type = OCFS2_FILECHECK_TYPE_SET;
		args->fa_len = (unsigned int)val;
		return 0;
	} else { /* invalid args */
		return 1;
	}
}

static ssize_t ocfs2_filecheck_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{

	ssize_t ret = 0, total = 0, remain = PAGE_SIZE;
	struct ocfs2_filecheck_entry *p;
	struct ocfs2_filecheck_sysfs_entry *ent;

	ent = ocfs2_filecheck_sysfs_get(kobj->name);
	if (!ent) {
		mlog(ML_ERROR,
		"Cannot get the corresponding entry via device basename %s\n",
		kobj->name);
		return -ENODEV;
	}

	spin_lock(&ent->fs_fcheck->fc_lock);
	ret = snprintf(buf, remain, "INO\t\tTYPE\tDONE\tERROR\n");
	total += ret;
	remain -= ret;

	list_for_each_entry(p, &ent->fs_fcheck->fc_head, fe_list) {
		ret = snprintf(buf + total, remain, "%lu\t\t%u\t%u\t%s\n",
			p->fe_ino, p->fe_type, p->fe_done,
			ocfs2_filecheck_error(p->fe_status));
		if (ret < 0) {
			total = ret;
			break;
		}
		if (ret == remain) {
			/* snprintf() didn't fit */
			total = -E2BIG;
			break;
		}
		total += ret;
		remain -= ret;
	}
	spin_unlock(&ent->fs_fcheck->fc_lock);

	ocfs2_filecheck_sysfs_put(ent);
	return total;
}

static int
ocfs2_filecheck_erase_entry(struct ocfs2_filecheck_sysfs_entry *ent)
{
	struct ocfs2_filecheck_entry *p;

	list_for_each_entry(p, &ent->fs_fcheck->fc_head, fe_list) {
		if (p->fe_done) {
			list_del(&p->fe_list);
			kfree(p);
			ent->fs_fcheck->fc_size--;
			ent->fs_fcheck->fc_done--;
			return 1;
		}
	}

	return 0;
}

static int
ocfs2_filecheck_erase_entries(struct ocfs2_filecheck_sysfs_entry *ent,
				unsigned int count)
{
	unsigned int i = 0;
	unsigned int ret = 0;

	while (i++ < count) {
		if (ocfs2_filecheck_erase_entry(ent))
			ret++;
		else
			break;
	}

	return (ret == count ? 1 : 0);
}

static void
ocfs2_filecheck_done_entry(struct ocfs2_filecheck_sysfs_entry *ent,
				struct ocfs2_filecheck_entry *entry)
{
	entry->fe_done = 1;
	spin_lock(&ent->fs_fcheck->fc_lock);
	ent->fs_fcheck->fc_done++;
	spin_unlock(&ent->fs_fcheck->fc_lock);
}

static unsigned short
ocfs2_filecheck_handle(struct super_block *sb,
				unsigned long ino, unsigned int flags)
{
	unsigned short ret = OCFS2_FILECHECK_ERR_SUCCESS;
	struct inode *inode = NULL;
	int rc;

	inode = ocfs2_iget(OCFS2_SB(sb), ino, flags, 0);
	if (IS_ERR(inode)) {
		rc = (int)(-(long)inode);
		if (rc >= OCFS2_FILECHECK_ERR_START &&
			rc < OCFS2_FILECHECK_ERR_END)
			ret = rc;
		else
			ret = OCFS2_FILECHECK_ERR_FAILED;
	} else
		iput(inode);

	return ret;
}

static void
ocfs2_filecheck_handle_entry(struct ocfs2_filecheck_sysfs_entry *ent,
				struct ocfs2_filecheck_entry *entry)
{
	if (entry->fe_type == OCFS2_FILECHECK_TYPE_CHK)
		entry->fe_status = ocfs2_filecheck_handle(ent->fs_sb,
				entry->fe_ino, OCFS2_FI_FLAG_FILECHECK_CHK);
	else if (entry->fe_type == OCFS2_FILECHECK_TYPE_FIX)
		entry->fe_status = ocfs2_filecheck_handle(ent->fs_sb,
				entry->fe_ino, OCFS2_FI_FLAG_FILECHECK_FIX);
	else
		entry->fe_status = OCFS2_FILECHECK_ERR_UNSUPPORTED;

	ocfs2_filecheck_done_entry(ent, entry);
}

static ssize_t ocfs2_filecheck_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	struct ocfs2_filecheck_args args;
	struct ocfs2_filecheck_entry *entry;
	struct ocfs2_filecheck_sysfs_entry *ent;
	ssize_t ret = 0;

	if (count == 0)
		return count;

	if (ocfs2_filecheck_args_parse(buf, count, &args)) {
		mlog(ML_ERROR, "Invalid arguments for online file check\n");
		return -EINVAL;
	}

	ent = ocfs2_filecheck_sysfs_get(kobj->name);
	if (!ent) {
		mlog(ML_ERROR,
		"Cannot get the corresponding entry via device basename %s\n",
		kobj->name);
		return -ENODEV;
	}

	if (args.fa_type == OCFS2_FILECHECK_TYPE_SET) {
		ret = ocfs2_filecheck_adjust_max(ent, args.fa_len);
		ocfs2_filecheck_sysfs_put(ent);
		return (!ret ? count : ret);
	}

	entry = kmalloc(sizeof(*entry), GFP_NOFS);
	if (!entry) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock(&ent->fs_fcheck->fc_lock);
	if ((ent->fs_fcheck->fc_size >= ent->fs_fcheck->fc_max) &&
			(ent->fs_fcheck->fc_done == 0)) {
		mlog(ML_ERROR, "Online file check queue(%u) is full\n",
				ent->fs_fcheck->fc_max);
		kfree(entry);
		entry = NULL;
		ret = -EBUSY;
	} else {
		if ((ent->fs_fcheck->fc_size >= ent->fs_fcheck->fc_max) &&
			(ent->fs_fcheck->fc_done > 0)) {
			/* Delete the oldest entry which was done,
			 * make sure the entry size in list does
			 * not exceed maximum value
			 */
			BUG_ON(!ocfs2_filecheck_erase_entry(ent));
		}

		entry->fe_ino = args.fa_ino;
		entry->fe_type = args.fa_type;
		entry->fe_done = 0;
		entry->fe_status = OCFS2_FILECHECK_ERR_INPROGRESS;
		list_add_tail(&entry->fe_list, &ent->fs_fcheck->fc_head);

		ent->fs_fcheck->fc_size++;
		ret = count;
	}
	spin_unlock(&ent->fs_fcheck->fc_lock);

	if (entry)
		ocfs2_filecheck_handle_entry(ent, entry);

out:
	ocfs2_filecheck_sysfs_put(ent);
	return ret;
}
