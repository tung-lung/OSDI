/*
 * Resizable simple ram filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *               2000 Transmeta Corp.
 *
 * Usage limits added by David Gibson, Linuxcare Australia.
 * This file is released under the GPL.
 */

/*
 * NOTE! This filesystem is probably most useful
 * not as a real filesystem, but as an example of
 * how virtual filesystems can be written.
 *
 * It doesn't get much simpler than this. Consider
 * that this file implements the full semantics of
 * a POSIX-compliant read-write filesystem.
 *
 * Note in particular how the filesystem does not
 * need to implement any data structures of its own
 * to keep track of the virtual data: using the VFS
 * caches is sufficient.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/ramfs.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include "internal.h"

#define RAMFS_DEFAULT_MODE  0755
#define MAX_BUF_SIZE    32

static const struct super_operations ramfs_ops;
static const struct inode_operations ramfs_dir_inode_operations;
static struct proc_dir_entry *proc_entry;
static char ram_buf[MAX_BUF_SIZE];
bool enable_encryption = false;

static struct backing_dev_info ramfs_backing_dev_info = {
    .name       = "ramfs",
    .ra_pages   = 0,    /* No readahead */
    .capabilities   = BDI_CAP_NO_ACCT_AND_WRITEBACK |
              BDI_CAP_MAP_DIRECT | BDI_CAP_MAP_COPY |
              BDI_CAP_READ_MAP | BDI_CAP_WRITE_MAP | BDI_CAP_EXEC_MAP,
};

struct inode *ramfs_get_inode(struct super_block *sb, int mode, dev_t dev)
{
    struct inode * inode = new_inode(sb);

    if (inode) {
        inode->i_mode = mode;
        inode->i_uid = current_fsuid();
        inode->i_gid = current_fsgid();
        inode->i_mapping->a_ops = &ramfs_aops;
        inode->i_mapping->backing_dev_info = &ramfs_backing_dev_info;
        mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
        mapping_set_unevictable(inode->i_mapping);
        inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
        switch (mode & S_IFMT) {
        default:
            init_special_inode(inode, mode, dev);
            break;
        case S_IFREG:
            inode->i_op = &ramfs_file_inode_operations;
            inode->i_fop = &ramfs_file_operations;
            break;
        case S_IFDIR:
            inode->i_op = &ramfs_dir_inode_operations;
            inode->i_fop = &simple_dir_operations;

            /* directory inodes start off with i_nlink == 2 (for "." entry) */
            inc_nlink(inode);
            break;
        case S_IFLNK:
            inode->i_op = &page_symlink_inode_operations;
            break;
        }
    }
    return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */
static int
ramfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
    struct inode * inode = ramfs_get_inode(dir->i_sb, mode, dev);
    int error = -ENOSPC;

    if (inode) {
        if (dir->i_mode & S_ISGID) {
            inode->i_gid = dir->i_gid;
            if (S_ISDIR(mode))
                inode->i_mode |= S_ISGID;
        }
        d_instantiate(dentry, inode);
        dget(dentry);   /* Extra count - pin the dentry in core */
        error = 0;
        dir->i_mtime = dir->i_ctime = CURRENT_TIME;
    }
    return error;
}

static int ramfs_mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
    int retval = ramfs_mknod(dir, dentry, mode | S_IFDIR, 0);
    if (!retval)
        inc_nlink(dir);
    return retval;
}

static int ramfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
    return ramfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int ramfs_symlink(struct inode * dir, struct dentry *dentry, const char * symname)
{
    struct inode *inode;
    int error = -ENOSPC;

    inode = ramfs_get_inode(dir->i_sb, S_IFLNK|S_IRWXUGO, 0);
    if (inode) {
        int l = strlen(symname)+1;
        error = page_symlink(inode, symname, l);
        if (!error) {
            if (dir->i_mode & S_ISGID)
                inode->i_gid = dir->i_gid;
            d_instantiate(dentry, inode);
            dget(dentry);
            dir->i_mtime = dir->i_ctime = CURRENT_TIME;
        } else
            iput(inode);
    }
    return error;
}

static const struct inode_operations ramfs_dir_inode_operations = {
    .create     = ramfs_create,
    .lookup     = simple_lookup,
    .link       = simple_link,
    .unlink     = simple_unlink,
    .symlink    = ramfs_symlink,
    .mkdir      = ramfs_mkdir,
    .rmdir      = simple_rmdir,
    .mknod      = ramfs_mknod,
    .rename     = simple_rename,
};

static const struct super_operations ramfs_ops = {
    .statfs     = simple_statfs,
    .drop_inode = generic_delete_inode,
    .show_options   = generic_show_options,
};

struct ramfs_mount_opts {
    umode_t mode;
};

enum {
    Opt_mode,
    Opt_err
};

static const match_table_t tokens = {
    {Opt_mode, "mode=%o"},
    {Opt_err, NULL}
};

struct ramfs_fs_info {
    struct ramfs_mount_opts mount_opts;
};

static int ramfs_parse_options(char *data, struct ramfs_mount_opts *opts)
{
    substring_t args[MAX_OPT_ARGS];
    int option;
    int token;
    char *p;

    opts->mode = RAMFS_DEFAULT_MODE;

    while ((p = strsep(&data, ",")) != NULL) {
        if (!*p)
            continue;

        token = match_token(p, tokens, args);
        switch (token) {
        case Opt_mode:
            if (match_octal(&args[0], &option))
                return -EINVAL;
            opts->mode = option & S_IALLUGO;
            break;
        /*
         * We might like to report bad mount options here;
         * but traditionally ramfs has ignored all mount options,
         * and as it is used as a !CONFIG_SHMEM simple substitute
         * for tmpfs, better continue to ignore other mount options.
         */
        }
    }

    return 0;
}

static int ramfs_fill_super(struct super_block * sb, void * data, int silent)
{
    struct ramfs_fs_info *fsi;
    struct inode *inode = NULL;
    struct dentry *root;
    int err;

    save_mount_options(sb, data);

    fsi = kzalloc(sizeof(struct ramfs_fs_info), GFP_KERNEL);
    sb->s_fs_info = fsi;
    if (!fsi) {
        err = -ENOMEM;
        goto fail;
    }

    err = ramfs_parse_options(data, &fsi->mount_opts);
    if (err)
        goto fail;

    sb->s_maxbytes      = MAX_LFS_FILESIZE;
    sb->s_blocksize     = PAGE_CACHE_SIZE;
    sb->s_blocksize_bits    = PAGE_CACHE_SHIFT;
    sb->s_magic     = RAMFS_MAGIC;
    sb->s_op        = &ramfs_ops;
    sb->s_time_gran     = 1;

    inode = ramfs_get_inode(sb, S_IFDIR | fsi->mount_opts.mode, 0);
    if (!inode) {
        err = -ENOMEM;
        goto fail;
    }

    root = d_alloc_root(inode);
    sb->s_root = root;
    if (!root) {
        err = -ENOMEM;
        goto fail;
    }

    return 0;
fail:
    kfree(fsi);
    sb->s_fs_info = NULL;
    iput(inode);
    return err;
}

int ramfs_get_sb(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *data, struct vfsmount *mnt)
{
    return get_sb_nodev(fs_type, flags, data, ramfs_fill_super, mnt);
}

static int rootfs_get_sb(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *data, struct vfsmount *mnt)
{
    return get_sb_nodev(fs_type, flags|MS_NOUSER, data, ramfs_fill_super,
                mnt);
}

static void ramfs_kill_sb(struct super_block *sb)
{
    kfree(sb->s_fs_info);
    kill_litter_super(sb);
}

static struct file_system_type ramfs_fs_type = {
    .name       = "ramfs",
    .get_sb     = ramfs_get_sb,
    .kill_sb    = ramfs_kill_sb,
};
static struct file_system_type rootfs_fs_type = {
    .name       = "rootfs",
    .get_sb     = rootfs_get_sb,
    .kill_sb    = kill_litter_super,
};

static int my_write_proc(struct file *file, const char *buf,
    int count, void *data)
{
    if(count > MAX_BUF_SIZE)
        count = MAX_BUF_SIZE;

    if( copy_from_user(ram_buf, buf, count) )
        return -EFAULT;

    if( !strncmp(ram_buf, "1", 1) )
        enable_encryption = true;
    else
        enable_encryption = false;

    printk(KERN_INFO "rams_flag: %-5d\n", enable_encryption);

    return count;
}

static int my_read_proc(char *buf, char **start, off_t offset,
    int count, int *eof, void *data)
{
    int len = 0;
    len = sprintf(buf, "%s\n", ram_buf);

    return len;

}

static int create_new_proc_entry(void)
{
    proc_entry = create_proc_entry("flag", 0666, NULL);
    if(!proc_entry) {
        printk(KERN_INFO "Error creating proc entry\n");
        return -ENOMEM;
    }
    proc_entry->read_proc = my_read_proc;
    proc_entry->write_proc = my_write_proc;
    printk(KERN_INFO "flag proc initialize!!!\n");
    return 0;
}

static void proc_cleanup(void)
{
    printk(KERN_INFO "flag proc exit!!!\n");
    remove_proc_entry("flag", NULL);
}

static int __init init_ramfs_fs(void)
{
    int ret;
    ret = create_new_proc_entry();
    return register_filesystem(&ramfs_fs_type);
}

static void __exit exit_ramfs_fs(void)
{
    proc_cleanup();
    unregister_filesystem(&ramfs_fs_type);
}

module_init(init_ramfs_fs)
module_exit(exit_ramfs_fs)

int __init init_rootfs(void)
{
    int err;

    err = bdi_init(&ramfs_backing_dev_info);
    if (err)
        return err;

    err = register_filesystem(&rootfs_fs_type);
    if (err)
        bdi_destroy(&ramfs_backing_dev_info);

    return err;
}

MODULE_LICENSE("GPL");
