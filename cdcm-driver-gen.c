/**
 * @file cdcm-driver-gen.c
 *
 * @brief
 *
 * <long description>
 *
 * @author Copyright (C) 2009 CERN CO/HT Yury GEORGIEVSKIY <ygeorgie@cern.ch>
 *
 * @date Created on 08/06/2009
 *
 * @section license_sec License
 *          Released under the GPL
 */
#include "list_extra.h" /* for extra handy list operations */
#include "cdcmDrvr.h"
#include "cdcmLynxAPI.h"

/* _GIOCTL_GET_DG_DEV_INFO ioctl argument
   Keep syncronized with one from user space! */
#define DDNMSZ    32   /* device/driver name size in bytes. Same as in Lynx */
struct dg_module_info {
	int major;		/* device major number */
	char name[DDNMSZ];	/* its name */
};


static DECLARE_MUTEX(mmutex);

/* external crap */
extern struct dldd entry_points; /* declared in the user driver part */
extern char* read_info_file(char*, int*);

/** @brief driverGen devices
 *
 * list  -- linked list
 * major -- device major number
 * addr  -- statics table address
 * name  -- device name. String of DEVICE_NAME_FMT layout.
 *          Examples: sharedcibc_sim03.info, tric_drvr20.info, etc...
 */
struct dgd {
	struct list_head list;
	unsigned int major;
	char *addr;
	char *name;
};

static LIST_HEAD(st_list); /* driverGen device linked list */

/**
 * @brief Add new statics table in the list
 *
 * @param major -- device major number
 * @param st    -- statics table to add
 * @param dname -- device name
 *
 * @return -ENOMEM - can't allocate new statics table descriptor
 * @return 0       - success
 */
static int dg_add_st(uint major, char *st, char *dname)
{
	struct dgd *new_st;

	new_st = kzalloc(sizeof(*new_st), GFP_KERNEL);
	if (!new_st)
		return -ENOMEM;

	new_st->name = kasprintf(GFP_KERNEL, "%s", dname);
	if (!new_st->name) {
		kfree(new_st);
		return -ENOMEM;
	}

	new_st->major = major;
	new_st->addr  = st;

	list_add_tail(&new_st->list, &st_list);

	return 0;
}

/**
 * @brief Removes deviced description from the list and frees resources.
 *
 * @param dgd -- what to find and remove.
 *
 * @return zero on success or a negative number on failure
 */
static int dg_rm_st(struct dgd *dgd)
{
	struct dgd *ptr, *tmp;
	list_for_each_entry_safe(ptr, tmp, &st_list, list) {
		if (ptr == dgd) {
			list_del(&ptr->list); /* exclude from linked list */
			kfree(ptr->name);
			kfree(ptr);	/* free mem */
			return 0;
		}
	}
	return -1;
}

/**
 * @brief Get dgd structure by statics table
 *
 * @param st -- statics table address
 *
 * @return NULL    - not found
 * @return pointer - found
 *
 */
static __attribute__((unused)) struct dgd* dg_get_dgd_by_st(char *st)
{
	struct dgd *ptr;
	list_for_each_entry(ptr, &st_list, list) {
		if (ptr->addr == st)
			return ptr;
	}
	return NULL;
}


/**
 * @brief Get dgd structure by major number
 *
 * @param major -- device major number to find
 *
 * @return NULL    - not found
 * @return pointer - found
 */
static struct dgd* dg_get_dgd_by_major(uint major)
{
	struct dgd *ptr;
	list_for_each_entry(ptr, &st_list, list) {
		if (ptr->major == major)
			return ptr;
	}
	return NULL;
}

/**
 * @brief Get statics table address by major number
 *
 * @param major -- device major number to find statics table
 *
 * <long-description>
 *
 * @return statics table address - if found
 * @return NULL                  - not found
 */
static char* dg_get_st_by_major(uint major)
{
	struct dgd *ptr;

	list_for_each_entry(ptr, &st_list, list) {
		if (ptr->major == major)
			return ptr->addr;
	}

	return NULL;
}

/**
 * @brief Information about all controlled devices
 *
 * @param arg -- user space buffer to put results.
 *
 * <long-description>
 *
 * @return zero on success or a negative number on failure
 */
int dg_get_dev_info(unsigned long arg)
{
	struct dgd *ptr;
	struct dg_module_info __user *buf = (struct dg_module_info __user *)arg;
	struct dg_module_info local;

	list_for_each_entry(ptr, &st_list, list) {
		local.major = ptr->major;
		strncpy(local.name, ptr->name, sizeof(local.name));
		if (copy_to_user(buf, &local, sizeof(local)))
			return -EFAULT;
		++buf;
	}

	return 0;
}

/**
 * @brief How many devices driver hosts.
 *
 * @param void - none
 *
 * Used by driver installation/deinstallation programs basically.
 *
 * @return device driver amount
 */
int dg_get_mod_am(void)
{
	return list_capacity(&st_list);
}

/**
 * @brief driverGen cdv_install
 *
 * @param name  -- info table file name
 * @param fops  -- file operations
 *
 * Registers new char device and insert it in driverGen device linked list.
 *
 * @return   major char device number - if success
 * @return < 0                        - failed
 */
int dg_cdv_install(char *name, struct file_operations *fops)
{
	unsigned int maj;     /* assigned major number */
	char *dname;	      /* device name */
	char *st;	      /* statics table */
	int cc;
	char *it;		/* info table */

	/* get info table */
	it = read_info_file(name, NULL);
	if (!it) {
		PRNT_ABS_ERR("Can't read info file.");
		return -EFAULT;
	}

	dname = strrchr(name, '/') + 1; /* device name is the info file name */

	maj = register_chrdev(0, dname, fops);
	if (maj < 0) {
		PRNT_ABS_ERR("Can't register %s character device", dname);
		cc = maj;
		goto out2;
	}

	st = entry_points.dldd_install((void*)it);
	if (st == (char*)SYSERR) {
		PRNT_ABS_ERR("Installation vector failed for %s device.",
			     dname);
		cc = -EAGAIN;
		goto out1;
	}

	/* save statics table */
	cc = dg_add_st(maj, st, dname);
	if (cc) {
		PRNT_ABS_ERR("Can't add %s device (maj %d) in the"
			     " device linked list.\n", dname, maj);
		goto out1;
	}

	/* all OK, return allocated major dev number */
	cc = maj;
	goto out2;

 out1:
	unregister_chrdev(maj, dname);
 out2:
	kfree(it);
	return cc;

}

/**
 * @brief driverGen cdv_uninstall
 *
 * @param filp -- file struct pointer
 * @param arg  -- user args
 *
 * @return zero on success or a negative number on failure
 */
int dg_cdv_uninstall(struct file *filp, unsigned long arg)
{
	uint major; /* device major number */
	struct dgd *dgd; /* device info */

	if (copy_from_user(&major, (void __user*)arg, sizeof(major)))
		return -EFAULT;

	dgd = dg_get_dgd_by_major(major);
	if (!dgd) /* should not happen */
		return -ENODEV;

	if (entry_points.dldd_uninstall(dgd->addr)) {
		PRNT_ABS_ERR("Uninstall vector failed.");
		return -EAGAIN;
	}

	unregister_chrdev(dgd->major, dgd->name);


	if (dg_rm_st(dgd))
		/* should not happen */
		return -ENODEV;

	return 0;
}

ssize_t dg_read(struct file *filp, char __user *buf, size_t size, loff_t *off)
{
	char *priv = filp->private_data; /* statics table */
	return entry_points.dldd_read(priv, (struct cdcm_file *)filp,
				      buf, size);
}


ssize_t dg_fop_write(struct file *filp, const char __user *buf,
		     size_t size, loff_t *off)
{
	char *priv = filp->private_data; /* statics table */
	return entry_points.dldd_write(priv, (struct cdcm_file *)filp,
				       (char *)buf, size);
}

unsigned int dg_fop_poll(struct file* filp, poll_table* wait)
{
	char *priv = filp->private_data; /* statics table */
	return entry_points.dldd_select(priv, (struct cdcm_file *)filp,
					0/*not vaild*/,
					(struct cdcm_sel *)wait);
}


long dg_fop_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	char *priv = filp->private_data; /* statics table */

	return entry_points.dldd_ioctl(priv, (struct cdcm_file *)filp,
				       cmd, (char *)arg);

}

static int get_device_memory_size(char *st)
{
	return 1024;		/* TODO */
}

int dg_fop_mmap(struct file *filp, struct vm_area_struct *vma)
{
	char *priv = filp->private_data; /* statics table */
	unsigned long size = vma->vm_end - vma->vm_start;

	if (size > get_device_memory_size(priv) || (size & (PAGE_SIZE-1)))
		return -EINVAL;

	return -ENOSYS;
}

/**
 * @brief Open file
 *
 * @param inode -- device inode
 * @param filp  -- file pointer
 *
 * @return zero on success or a negative number on failure
 */
int dg_fop_open(struct inode *inode, struct file *filp)
{
	char *st;  /* statics table */

	st = dg_get_st_by_major(imajor(filp->f_dentry->d_inode));
	if (!st)
		return -ENODEV;

	filp->private_data = st; /* save it */

	return 0;
}

int dg_fop_release(struct inode *inode, struct file *filp)
{
	int cc;
	char *priv = filp->private_data; /* statics table */

	cc = entry_points.dldd_close(priv, (struct cdcm_file *)filp);
	filp->private_data = NULL;
	return cc;
}

