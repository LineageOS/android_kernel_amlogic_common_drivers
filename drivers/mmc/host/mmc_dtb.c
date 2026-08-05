// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

//#define DEBUG
#include <linux/mmc/core.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/slab.h>

#include <linux/scatterlist.h>
#include <linux/swap.h>		/* For nr_free_buffer_pages() */
#include <linux/list.h>

#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/module.h>

#include "core.h"
#include "card.h"
#include "host.h"
#include "bus.h"
#include "mmc_ops.h"
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/scatterlist.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/amlogic/aml_sd.h>
#include "mmc_common.h"
#ifdef CONFIG_AMLOGIC_DTS_PARTITION
#include "mmc_dtb.h"
#include "mmc_key.h"
/*
 * Kernel-internal block headers, reached via -I$(srctree)/block (see Makefile).
 * Sabrina spells these "../../../../block/blk.h", which only works when
 * common_drivers is a real subdirectory of the kernel tree; here it is a
 * symlink, so ".." walks up into vendor/amlogic/ and misses.
 */
#include "blk.h"
#include "partitions/efi.h"
#endif

static dev_t amlmmc_dtb_no;
struct cdev amlmmc_dtb;
struct device *dtb_dev;
struct class *amlmmc_dtb_class;
struct mmc_card *card_dtb;
struct aml_dtb_info {
	unsigned int stamp[2];
	u8 valid[2];
};

static struct aml_dtb_info dtb_infos = {{0, 0}, {0, 0} };
struct mmc_partitions_fmt *pt_fmt;
struct mmc_dtb {
	struct mmc_card		*card;		/* the host this device belongs to */
	struct device		dev;		/* the device */
};

#define stamp_after(a, b)	((int)(b) - (int)(a)  < 0)
#define CONFIG_DTB_SIZE  (256 * 1024U)
#define DTB_CELL_SIZE   (16 * 1024U)

#define DTB_NAME		"dtb"
#define	SZ_1M			0x00100000
#define	MMC_DTB_PART_OFFSET	(40 * SZ_1M)
#define	EMMC_BLOCK_SIZE		(0x100)
#define	MAX_EMMC_BLOCK_SIZE	(128 * 1024)

#define DTB_RESERVE_OFFSET	(4 * SZ_1M)
#define	DTB_BLK_SIZE		(0x200)
#define	DTB_BLK_CNT		(512)
#define	DTB_SIZE		(DTB_BLK_CNT * DTB_BLK_SIZE)
#define DTB_COPIES		(2)
#define DTB_AREA_BLK_CNT	(DTB_BLK_CNT * DTB_COPIES)
/* pertransfer for internal operations. */
#define MAX_TRANS_BLK		(256)
#define	MAX_TRANS_SIZE		(MAX_TRANS_BLK * DTB_BLK_SIZE)

struct aml_dtb_rsv {
	u8 data[DTB_BLK_SIZE * DTB_BLK_CNT - 4 * sizeof(unsigned int)];
	unsigned int magic;
	unsigned int version;
	unsigned int timestamp;
	unsigned int checksum;
};

static CLASS_ATTR_STRING(emmcdtb, 0644, NULL);

int mmc_dtb_open(struct inode *node, struct file *file)
{
	return 0;
}

static unsigned int _calc_dtb_checksum(struct aml_dtb_rsv *dtb)
{
	int i = 0;
	int size = sizeof(struct aml_dtb_rsv) - sizeof(unsigned int);
	unsigned int *buffer;
	unsigned int checksum = 0;

	size = size >> 2;
	buffer = (unsigned int *)dtb;
	while (i < size)
		checksum += buffer[i++];

	return checksum;
}

static int _verify_dtb_checksum(struct aml_dtb_rsv *dtb)
{
	unsigned int checksum;

	checksum = _calc_dtb_checksum(dtb);
	pr_debug("calc %x, store %x\n", checksum, dtb->checksum);

	return !(checksum == dtb->checksum);
}

static int _amlmmc_read(struct mmc_card *mmc, int blk, unsigned char *buf, int cnt)
{
	int ret = 0;
	unsigned char *dst = NULL;
	int bit = card_dtb->csd.read_blkbits;
	void *read_buf;
	int read_cnt;

	read_buf = kmalloc(DTB_CELL_SIZE, GFP_KERNEL | __GFP_RECLAIM);
	if (!read_buf)
		return -ENOMEM;

	dst = (unsigned char *)buf;
	read_cnt = DTB_CELL_SIZE >> bit;

	mmc_claim_host(mmc->host);
	aml_disable_mmc_cqe(mmc);
	do {
		if (cnt < read_cnt)
			read_cnt = cnt;
		ret = mmc_read_internal(mmc, blk, read_cnt, read_buf);
		if (ret) {
			ret = -EFAULT;
			break;
		}
		memcpy(dst, read_buf, read_cnt << bit);
		dst += read_cnt << bit;
		cnt -= read_cnt;
		blk += read_cnt;
	} while (cnt != 0);
	aml_enable_mmc_cqe(mmc);
	mmc_release_host(mmc->host);

	kfree(read_buf);
	return ret;
}

static int _amlmmc_write(struct mmc_card *mmc, int blk, unsigned char *buf, int cnt)
{
	int ret = 0;
	unsigned char *src = NULL;
	int bit = mmc->csd.read_blkbits;
	void *write_buf;
	int write_cnt;

	write_buf = kmalloc(DTB_CELL_SIZE, GFP_KERNEL | __GFP_RECLAIM);
	if (!write_buf)
		return -ENOMEM;

	src = (unsigned char *)buf;
	write_cnt = DTB_CELL_SIZE >> bit;

	mmc_claim_host(mmc->host);
	aml_disable_mmc_cqe(mmc);
	do {
		if (cnt < write_cnt)
			write_cnt = cnt;
		memcpy(write_buf, src, write_cnt << bit);
		ret = mmc_write_internal(mmc, blk, write_cnt, write_buf);
		if (ret) {
			ret = -EFAULT;
			break;
		}
		src += write_cnt << bit;
		cnt -= write_cnt;
		blk += write_cnt;
	} while (cnt != 0);
	aml_enable_mmc_cqe(mmc);
	mmc_release_host(mmc->host);

	kfree(write_buf);
	return ret;
}

static int _dtb_init(struct mmc_card *mmc)
{
	int ret = 0;
	struct aml_dtb_rsv *dtb;
	struct aml_dtb_info *info = &dtb_infos;
	int cpy = 1, valid = 0;
	int bit = mmc->csd.read_blkbits;
	int blk;
	int cnt = CONFIG_DTB_SIZE >> bit;

	dtb = vmalloc(CONFIG_DTB_SIZE);
	if (!dtb)
		return -ENOMEM;

	/* read dtb2 1st, for compatibility without checksum. */
	while (cpy >= 0) {
		blk = ((get_reserve_partition_off_from_tbl()
		       + DTB_RESERVE_OFFSET) >> bit)
		       + cpy * DTB_BLK_CNT;
		if (_amlmmc_read(mmc, blk, (unsigned char *)dtb, cnt)) {
			pr_err("%s: block # %#x ERROR!\n", __func__, blk);
		} else {
			ret = _verify_dtb_checksum(dtb);
			if (!ret) {
				info->stamp[cpy] = dtb->timestamp;
				info->valid[cpy] = 1;
			} else {
				pr_debug("cpy %d is not valid\n", cpy);
			}
		}
		valid += info->valid[cpy];
		cpy--;
	}
	pr_debug("total valid %d\n", valid);
	vfree(dtb);

	return ret;
}

int amlmmc_dtb_write(struct mmc_card *mmc, unsigned char *buf, int len)
{
	int ret = 0, blk;
	int bit = mmc->csd.read_blkbits;
	int cpy, valid;
	struct aml_dtb_rsv *dtb = (struct aml_dtb_rsv *)buf;
	struct aml_dtb_info *info = &dtb_infos;
	int cnt = CONFIG_DTB_SIZE >> bit;

	if (len > CONFIG_DTB_SIZE) {
		pr_err("%s dtb data len too much", __func__);
		return -EFAULT;
	}
	/* set info */
	valid = info->valid[0] + info->valid[1];
	if (valid == 0) {
		dtb->timestamp = 0;
	} else if (valid == 1) {
		dtb->timestamp = 1 + info->stamp[info->valid[0] ? 0 : 1];
	} else {
		/* both are valid */
		if (info->stamp[0] != info->stamp[1]) {
			pr_info("timestamp are not same %d:%d\n",
				info->stamp[0], info->stamp[1]);
			dtb->timestamp = 1 +
				(stamp_after(info->stamp[1], info->stamp[0]) ?
				info->stamp[1] : info->stamp[0]);
		} else {
			dtb->timestamp = 1 + info->stamp[0];
		}
	}
	/*setting version and magic*/
	dtb->version = 1; /* base version */
	dtb->magic = 0x00447e41; /*A~D\0*/
	dtb->checksum = _calc_dtb_checksum(dtb);
	pr_info("stamp %d, checksum 0x%x, version %d, magic %s\n",
		dtb->timestamp, dtb->checksum,
		dtb->version, (char *)&dtb->magic);
	/* write down... */
	for (cpy = 0; cpy < DTB_COPIES; cpy++) {
		blk = ((get_reserve_partition_off_from_tbl()
					+ DTB_RESERVE_OFFSET) >> bit)
			+ cpy * DTB_BLK_CNT;
		ret |= _amlmmc_write(mmc, blk, buf, cnt);
	}

	return ret;
}

int amlmmc_dtb_read(struct mmc_card *card, unsigned char *buf, int len)
{
	int ret = 0, start_blk, blk_cnt;
	int bit = card->csd.read_blkbits;
	unsigned char *dst = NULL;
	unsigned char *buffer = NULL;

	if (len > CONFIG_DTB_SIZE) {
		pr_err("%s dtb data len too much", __func__);
		return -EFAULT;
	}
	memset(buf, 0x0, len);

	buffer = kmalloc(DTB_CELL_SIZE, GFP_KERNEL | __GFP_RECLAIM);
	if (!buffer)
		return -ENOMEM;

	start_blk = MMC_DTB_PART_OFFSET >> bit;
	blk_cnt = CONFIG_DTB_SIZE >> bit;
	dst = (unsigned char *)buffer;

	mmc_claim_host(card->host);
	aml_disable_mmc_cqe(card);
	while (blk_cnt != 0) {
		memset(buffer, 0x0, DTB_CELL_SIZE);
		ret = mmc_read_internal(card, start_blk, (DTB_CELL_SIZE >> bit), dst);
		if (ret) {
			pr_err("%s read dtb error", __func__);
			ret = -EFAULT;
			break;
		}
		start_blk += (DTB_CELL_SIZE >> bit);
		blk_cnt -= (DTB_CELL_SIZE >> bit);
		memcpy(buf, dst, DTB_CELL_SIZE);
		buf += DTB_CELL_SIZE;
	}
	aml_enable_mmc_cqe(card);
	mmc_release_host(card->host);
	kfree(buffer);
	return ret;
}

ssize_t mmc_dtb_read(struct file *file, char __user *buf,
		     size_t count, loff_t *ppos)
{
	unsigned char *dtb_ptr = NULL;
	ssize_t read_size = 0;
	int bit = card_dtb->csd.read_blkbits;
	int blk = (MMC_DTB_PART_OFFSET + *ppos) >> bit;
	int ret = 0, cnt = 0;

	if (*ppos == CONFIG_DTB_SIZE)
		return 0;

	if (*ppos >= CONFIG_DTB_SIZE) {
		pr_err("%s: out of space!", __func__);
		return -EFAULT;
	}

	if ((*ppos + count) > CONFIG_DTB_SIZE)
		read_size = CONFIG_DTB_SIZE - *ppos;
	else
		read_size = count;

	cnt = read_size >> bit;

	dtb_ptr = vmalloc(read_size);
	if (!dtb_ptr)
		return -ENOMEM;

	ret = _amlmmc_read(card_dtb, blk, dtb_ptr, cnt);
	if (ret) {
		pr_err("%s: read dtb failed", __func__);
		read_size = 0;
		goto exit;
	}

	ret = copy_to_user(buf, dtb_ptr, read_size);
	if (ret) {
		pr_err("%s: copy to user space failed", __func__);
		read_size -= ret;
		goto exit;
	}
	*ppos += read_size;
exit:
	vfree(dtb_ptr);
	return read_size;
}

ssize_t mmc_dtb_write(struct file *file,
			const char __user *buf, size_t count, loff_t *ppos)
{
	unsigned char *dtb_ptr = NULL;
	ssize_t write_size = 0;
	int ret = 0;

	if (*ppos == CONFIG_DTB_SIZE)
		return 0;
	if (*ppos >= CONFIG_DTB_SIZE) {
		pr_err("%s: out of space!", __func__);
		return -EFAULT;
	}

	dtb_ptr = vmalloc(CONFIG_DTB_SIZE);
	if (!dtb_ptr)
		return -ENOMEM;

	if ((*ppos + count) > CONFIG_DTB_SIZE)
		write_size = CONFIG_DTB_SIZE - *ppos;
	else
		write_size = count;

	ret = amlmmc_dtb_read(card_dtb, dtb_ptr, CONFIG_DTB_SIZE);
	if (ret) {
		pr_err("%s: read dtb failed", __func__);
		ret = -EFAULT;
		goto exit;
	}

	ret = copy_from_user((dtb_ptr + *ppos), buf, write_size);
	if (ret) {
		pr_err("%s: copy from user space failed", __func__);
		write_size = 0;
		goto exit;
	}

	ret = amlmmc_dtb_write(card_dtb, dtb_ptr, CONFIG_DTB_SIZE);
	if (ret) {
		pr_err("%s: write dtb failed", __func__);
		write_size = 0;
		goto exit;
	}
	*ppos += write_size;
exit:
	vfree(dtb_ptr);
	return write_size;
}

long mmc_dtb_ioctl(struct file *file, unsigned int cmd, unsigned long args)
{
	return 0;
}

static const struct file_operations dtb_ops = {
	.open = mmc_dtb_open,
	.read = mmc_dtb_read,
	.write = mmc_dtb_write,
	.unlocked_ioctl = mmc_dtb_ioctl,
};

int get_reserve_partition_off_from_tbl(void)
{
#ifdef CONFIG_AMLOGIC_DTS_PARTITION
	int i;

	/*
	 * add_dtbkey() (meson-g12a-mmc.c, meson-gx-mmc.c) polls for mmc->card
	 * every 50ms and then calls emmc_key_init()/amlmmc_dtb_init(), both of
	 * which land here. mmc->card is set just before mmc_blk_probe() runs
	 * aml_emmc_partition_ops(), so that work can fire before pt_fmt is
	 * allocated. -1 is the existing "no reserved partition" return.
	 */
	if (!pt_fmt)
		return -1;

	for (i = 0; i < pt_fmt->part_num; i++) {
		if (!strcmp(pt_fmt->partitions[i].name, MMC_RESERVED_NAME))
			return pt_fmt->partitions[i].offset;
	}
	return -1;
#else
	return 0x2400000;
#endif
}

void amlmmc_dtb_init(struct mmc_card *card, int *retp)
{
	*retp = 0;
	mmc_claim_host(card->host);

	card_dtb = card;
	pr_debug("%s: register dtb chardev", __func__);

	_dtb_init(card);

	*retp = alloc_chrdev_region(&amlmmc_dtb_no, 0, 1, DTB_NAME);
	if (*retp < 0) {
		pr_err("alloc dtb dev_t no failed");
		*retp = -1;
		goto exit;
	}

	cdev_init(&amlmmc_dtb, &dtb_ops);
	amlmmc_dtb.owner = THIS_MODULE;
	*retp = cdev_add(&amlmmc_dtb, amlmmc_dtb_no, 1);
	if (*retp) {
		pr_err("dtb dev add failed");
		*retp = -1;
		goto exit_err1;
	}

	amlmmc_dtb_class = class_create(THIS_MODULE, DTB_NAME);
	if (IS_ERR(amlmmc_dtb_class)) {
		pr_err("dtb dev add failed");
		*retp = -1;
		goto exit_err2;
	}

	*retp = class_create_file(amlmmc_dtb_class, &class_attr_emmcdtb.attr);
	if (*retp) {
		pr_err("dtb dev add failed");
		*retp = -1;
		goto exit_err2;
	}

	dtb_dev = device_create(amlmmc_dtb_class, NULL, amlmmc_dtb_no, NULL, DTB_NAME);
	if (IS_ERR(dtb_dev)) {
		pr_err("dtb dev add failed");
		*retp = -1;
		goto exit_err3;
	}

	pr_info("%s: register dtb chardev OK", __func__);
	goto exit;

exit_err3:
	class_remove_file(amlmmc_dtb_class, &class_attr_emmcdtb.attr);
	class_destroy(amlmmc_dtb_class);
exit_err2:
	cdev_del(&amlmmc_dtb);
exit_err1:
	unregister_chrdev_region(amlmmc_dtb_no, 1);
exit:
	mmc_release_host(card->host);
}


#ifdef CONFIG_AMLOGIC_DTS_PARTITION
static int mmc_partition_tbl_checksum_calc(struct partitions *part,
					   int part_num)
{
	int i, j;
	u32 checksum = 0, *p;

	for (i = 0; i < part_num; i++) {
		p = (u32 *)part;
		for (j = sizeof(struct partitions) / sizeof(checksum); j > 0; j--) {
			checksum += *p;
			p++;
		}
	}

	return checksum;
}

static int mmc_read_partition_tbl(struct mmc_card *card, struct mmc_partitions_fmt *pt_fmt)
{
	int ret = 0, start_blk, size, blk_cnt;
	int bit = card->csd.read_blkbits;
	int blk_size = 1 << bit; /* size of a block */
	char *buf, *dst;

	buf = kmalloc(blk_size, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto exit_err;
	}
	memset(pt_fmt, 0, sizeof(struct mmc_partitions_fmt));
	memset(buf, 0, blk_size);
	start_blk = MMC_BOOT_PARTITION_SIZE + MMC_BOOT_PARTITION_RESERVED;
	if (start_blk < 0) {
		ret = -EINVAL;
		goto exit_err;
	}
	start_blk >>= bit;
	size = sizeof(struct mmc_partitions_fmt);
	dst = (char *)pt_fmt;
	if (size >= blk_size) {
		blk_cnt = size >> bit;
		ret = mmc_read_internal(card, start_blk, blk_cnt, dst);
		if (ret) { /* error */
			goto exit_err;
		}
		start_blk += blk_cnt;
		dst += blk_cnt << bit;
		size -= blk_cnt << bit;
	}
	if (size > 0) { /* the last block */
		ret = mmc_read_internal(card, start_blk, 1, buf);
		if (ret)
			goto exit_err;
		memcpy(dst, buf, size);
	}
	/* pr_info("Partition table stored in eMMC/TSD:\n"); */
	/* pr_info("magic: %s, version: %s, checksum=%#x\n", */
	/* pt_fmt->magic, pt_fmt->version, pt_fmt->checksum); */
	/* show_mmc_partition(pt_fmt->partitions, pt_fmt->part_num); */

	if ((strncmp(pt_fmt->magic, MMC_PARTITIONS_MAGIC,
		     sizeof(pt_fmt->magic)) == 0) && pt_fmt->part_num > 0 &&
			pt_fmt->part_num <= MAX_MMC_PART_NUM &&
			pt_fmt->checksum == mmc_partition_tbl_checksum_calc
			(pt_fmt->partitions, pt_fmt->part_num)) {
		ret = 0; /* everything is OK now */

	} else {
		if (strncmp(pt_fmt->magic, MMC_PARTITIONS_MAGIC,
			    sizeof(pt_fmt->magic)) != 0) {
			pr_info("magic error: %s\n", pt_fmt->magic);
		} else if ((pt_fmt->part_num < 0) ||
			    (pt_fmt->part_num > MAX_MMC_PART_NUM)) {
			pr_info("partition number error: %d\n",
				pt_fmt->part_num);
		} else {
			pr_info("checksum error: pt_fmt->checksum=%d,calc_result=%d\n",
				pt_fmt->checksum,
				mmc_partition_tbl_checksum_calc
				(pt_fmt->partitions, pt_fmt->part_num));
		}

		pr_info("[%s]: partition verified error\n", __func__);
		ret = -1; /* the partition information is invalid */
	}

exit_err:
	kfree(buf);

	pr_info("[%s] mmc read partition %s!\n",
		__func__, (ret == 0) ? "OK" : "ERROR");

	return ret;
}

static inline struct partition_meta_info *alloc_part_info(struct gendisk *disk)
{
	if (disk)
		return kzalloc_node(sizeof(struct partition_meta_info),
				    GFP_KERNEL, disk->node_id);
	return kzalloc(sizeof(struct partition_meta_info), GFP_KERNEL);
}

/* This function is copy and modified from kernel function add_partition() */
static struct block_device *add_emmc_each_part(struct gendisk *disk, int partno,
					    sector_t start, sector_t len,
					    int flags, char *pname)
{
	dev_t devt = MKDEV(0, 0);
	struct device *ddev = disk_to_dev(disk);
	struct device *pdev;
	struct block_device *bdev;
	int err;

	lockdep_assert_held(&disk->open_mutex);

	if (partno >= disk_max_parts(disk))
		return ERR_PTR(-EINVAL);

	/*
	 * Partitions are not supported on zoned block devices that are used as
	 * such.
	 */
	switch (disk->queue->limits.zoned) {
	case BLK_ZONED_HM:
		pr_warn("%s: partitions not supported on host managed zoned block device\n",
			disk->disk_name);
		return ERR_PTR(-ENXIO);
	case BLK_ZONED_HA:
		pr_info("%s: disabling host aware zoned block device support due to partitions\n",
			disk->disk_name);
		blk_queue_set_zoned(disk, BLK_ZONED_NONE);
		break;
	case BLK_ZONED_NONE:
		break;
	}

	if (xa_load(&disk->part_tbl, partno))
		return ERR_PTR(-EBUSY);

	/* ensure we always have a reference to the whole disk */
	get_device(disk_to_dev(disk));

	err = -ENOMEM;
	bdev = bdev_alloc(disk, partno);
	if (!bdev)
		goto out_put_disk;

	bdev->bd_start_sect = start;
	spin_lock(&bdev->bd_size_lock);
	i_size_write(bdev->bd_inode, (loff_t)len << SECTOR_SHIFT);
	spin_unlock(&bdev->bd_size_lock);

	pdev = &bdev->bd_device;
	dev_set_name(pdev, "%s", pname);

	device_initialize(pdev);
	pdev->class = &block_class;
	pdev->type = &part_type;
	pdev->parent = ddev;

	/* in consecutive minor range? */
	if (bdev->bd_partno < disk->minors) {
		devt = MKDEV(disk->major, disk->first_minor + bdev->bd_partno);
	} else {
		err = blk_alloc_ext_minor();
		if (err < 0)
			goto out_put;
		devt = MKDEV(BLOCK_EXT_MAJOR, err);
	}
	pdev->devt = devt;

	err = -ENOMEM;
	bdev->bd_meta_info = alloc_part_info(disk);
	if (!bdev->bd_meta_info)
		goto out_put;
	sprintf(bdev->bd_meta_info->volname, "%s", pname);

	/* delay uevent until 'holders' subdir is created */
	dev_set_uevent_suppress(pdev, 1);
	err = device_add(pdev);
	if (err)
		goto out_put;

	err = -ENOMEM;
	bdev->bd_holder_dir = kobject_create_and_add("holders", &pdev->kobj);
	if (!bdev->bd_holder_dir)
		goto out_del;

	dev_set_uevent_suppress(pdev, 0);

	/* everything is up and running, commence */
	err = xa_insert(&disk->part_tbl, partno, bdev, GFP_KERNEL);
	if (err)
		goto out_del;
	bdev_add(bdev, devt);

	/* suppress uevent if the disk suppresses it */
	if (!dev_get_uevent_suppress(ddev))
		kobject_uevent(&pdev->kobj, KOBJ_ADD);
	return bdev;

out_del:
	kobject_put(bdev->bd_holder_dir);
	device_del(pdev);
out_put:
	put_device(pdev);
	return ERR_PTR(err);
out_put_disk:
	put_disk(disk);
	return ERR_PTR(err);
}

static inline int card_proc_info(struct seq_file *m, char *dev_name, int i)
{
	struct partitions *this = &pt_fmt->partitions[i];

	if (i >= pt_fmt->part_num)
		return 0;

	seq_printf(m, "%s%02d: %9llx %9x \"%s\"\n", dev_name,
		   i + 1, (unsigned long long)this->size,
		   512 * 1024, this->name);
	return 0;
}

static int card_proc_show(struct seq_file *m, void *v)
{
	int i;

	seq_puts(m, "dev:	size   erasesize  name\n");
	for (i = 0; i < 16; i++)
		card_proc_info(m, "inand", i);

	return 0;
}

static int card_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, card_proc_show, NULL);
}

static const struct proc_ops card_proc_fops = {
	.proc_open = card_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static int add_emmc_partition(struct gendisk *disk, struct mmc_partitions_fmt *pt_fmt)
{
	unsigned int i;
	struct block_device *ret = NULL;
	u64 offset, size, cap;
	struct partitions *pp;
	struct proc_dir_entry *proc_card;

	cap = get_capacity(disk); /* unit:512 bytes */
	for (i = 0; i < pt_fmt->part_num; i++) {
		pp = &pt_fmt->partitions[i];
		offset = pp->offset >> 9; /* unit:512 bytes */
		size = pp->size >> 9; /* unit:512 bytes */
		if ((offset + size) <= cap) {
			ret = add_emmc_each_part(disk, 1 + i, offset,
						 size, 0, pp->name);

			pr_info("[%sp%02d] %20s  offset 0x%012llx, size 0x%012llx %s\n",
				disk->disk_name, 1 + i,
				pp->name, offset << 9,
				size << 9, IS_ERR(ret) ? "add fail" : "");
		} else {
			pr_info("[%s] %s: partition exceeds device capacity:\n",
				__func__, disk->disk_name);

			pr_info("%20s	offset 0x%012llx, size 0x%012llx\n",
				pp->name, offset << 9, size << 9);

			break;
		}
	}
	/* create /proc/inand */

	proc_card = proc_create("inand", 0444, NULL, &card_proc_fops);
	if (!proc_card)
		pr_info("[%s] create /proc/inand fail.\n", __func__);

	/* create /proc/ntd */
	if (!proc_create("ntd", 0444, NULL, &card_proc_fops))
		pr_info("[%s] create /proc/ntd fail.\n", __func__);

	return 0;
}

static bool is_card_emmc(struct mmc_card *card)
{
		unsigned int card_type;

		/* emmc port, so it must be an eMMC or TSD */
		if (device_property_read_u32(card->host->parent, "card_type",
									 &card_type) < 0)
			return false;

		return card_type == CARD_TYPE_MMC;
}

static ssize_t emmc_version_get(struct class *class,
				struct class_attribute *attr, char *buf)
{
	int num = 0;

	return sprintf(buf, "%d", num);
}

static void show_partition_table(struct partitions *table)
{
	int i = 0;
	struct partitions *par_table = NULL;

	pr_info("show partition table:\n");
	for (i = 0; i < MAX_MMC_PART_NUM; i++) {
		par_table = &table[i];
		if (par_table->size == -1)
			pr_info("part: %d, name : %10s, size : %-4s mask_flag %d\n",
				i, par_table->name, "end",
				par_table->mask_flags);
		else
			pr_info("part: %d, name : %10s, size : %-4llx  mask_flag %d\n",
				i, par_table->name, par_table->size,
				par_table->mask_flags);
	}
}

static ssize_t emmc_part_table_get(struct class *class,
				   struct class_attribute *attr, char *buf)
{
	struct partitions *part_table = NULL;
	struct partitions *tmp_table = NULL;
	int i = 0, part_num = 0;

	tmp_table = pt_fmt->partitions;
	part_table = kmalloc_array(MAX_MMC_PART_NUM,
				   sizeof(struct partitions), GFP_KERNEL);

	if (!part_table) {
		pr_info("[%s] malloc failed for  part_table!\n", __func__);
		return -ENOMEM;
	}

	for (i = 0; i < MAX_MMC_PART_NUM; i++) {
		if (tmp_table[i].mask_flags == STORE_CODE) {
			strncpy(part_table[part_num].name,
				tmp_table[i].name,
				MAX_MMC_PART_NAME_LEN);

			part_table[part_num].size = tmp_table[i].size;
			part_table[part_num].offset = tmp_table[i].offset;

			part_table[part_num].mask_flags =
				tmp_table[i].mask_flags;
			part_num++;
		}
	}
	for (i = 0; i < MAX_MMC_PART_NUM; i++) {
		if (tmp_table[i].mask_flags == STORE_CACHE) {
			strncpy(part_table[part_num].name,
				tmp_table[i].name,
				MAX_MMC_PART_NAME_LEN);

			part_table[part_num].size = tmp_table[i].size;
			part_table[part_num].offset = tmp_table[i].offset;

			part_table[part_num].mask_flags =
				tmp_table[i].mask_flags;

			part_num++;
		}
	}
	for (i = 0; i < MAX_MMC_PART_NUM; i++) {
		if (tmp_table[i].mask_flags == STORE_DATA) {
			strncpy(part_table[part_num].name,
				tmp_table[i].name,
				MAX_MMC_PART_NAME_LEN);

			part_table[part_num].size = tmp_table[i].size;
			part_table[part_num].offset = tmp_table[i].offset;
			part_table[part_num].mask_flags =
				tmp_table[i].mask_flags;

			if (!strncmp(part_table[part_num].name, "data",
				     MAX_MMC_PART_NAME_LEN))
				/* last part size is FULL */
				part_table[part_num].size = -1;
			part_num++;
		}
	}

	show_partition_table(part_table);
	memcpy(buf, part_table, MAX_MMC_PART_NUM * sizeof(struct partitions));

	kfree(part_table);
	part_table = NULL;

	return MAX_MMC_PART_NUM * sizeof(struct partitions);
}

static int store_device = -1;
static ssize_t store_device_flag_get(struct class *class,
				     struct class_attribute *attr, char *buf)
{
	if (store_device == -1) {
		pr_info("[%s]  get store device flag something wrong !\n",
			__func__);
	}

	return sprintf(buf, "%d", store_device);
}

static ssize_t get_bootloader_offset(struct class *class,
				     struct class_attribute *attr, char *buf)
{
	int offset = 0;

	offset = 512;
	return sprintf(buf, "%d", offset);
}

static struct class_attribute aml_version =
__ATTR(version, 0444, emmc_version_get, NULL);
static struct class_attribute aml_part_table =
__ATTR(part_table, 0444, emmc_part_table_get, NULL);
static struct class_attribute aml_store_device =
__ATTR(store_device, 0444, store_device_flag_get, NULL);
static struct class_attribute bootloader_offset =
__ATTR(bl_off_bytes, 0444, get_bootloader_offset, NULL);

int aml_emmc_partition_ops(struct mmc_card *card, struct gendisk *disk)
{
	int ret = 0;
	struct class *aml_store_class = NULL;
	struct _gpt_header *gpt_h = NULL;
	unsigned char *buffer = NULL;

	if (!is_card_emmc(card)) /* not emmc, nothing to do */
		return ret;

	buffer = kmalloc(512, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	mmc_claim_host(card->host);
	aml_disable_mmc_cqe(card);

	/*self adapting*/
	ret = mmc_read_internal(card, 1, 1, buffer);
	if (ret) {
		pr_err("%s: error", __func__);
		aml_enable_mmc_cqe(card);
		mmc_release_host(card->host);
		kfree(buffer);
		return ret;
	}

	pt_fmt = kmalloc(sizeof(*pt_fmt), GFP_KERNEL);
	if (!pt_fmt) {
		aml_enable_mmc_cqe(card);
		mmc_release_host(card->host);
		kfree(buffer);
		return -ENOMEM;
	}

	gpt_h = (struct _gpt_header *)buffer;

	if (le64_to_cpu(gpt_h->signature) == GPT_HEADER_SIGNATURE) {
		kfree(buffer);
		ret = mmc_read_partition_tbl(card, pt_fmt);
		aml_enable_mmc_cqe(card);
		mmc_release_host(card->host);
		if (ret == 0)
			emmc_key_init(card, &ret);
		if (ret)
			goto out;
		amlmmc_dtb_init(card, &ret);
		if (ret)
			goto out;
		return 0;
	}

	kfree(buffer);

	blk_drop_partitions(disk);

	ret = mmc_read_partition_tbl(card, pt_fmt);
	if (ret == 0) { /* ok */
		ret = add_emmc_partition(disk, pt_fmt);
	}
	aml_enable_mmc_cqe(card);
	mmc_release_host(card->host);

	if (ret == 0) /* ok */
		emmc_key_init(card, &ret);
	if (ret)
		goto out;

	amlmmc_dtb_init(card, &ret);
	if (ret)
		goto out;

	aml_store_class = class_create(THIS_MODULE, "aml_store");
	if (IS_ERR(aml_store_class)) {
		pr_info("[%s] create aml_store_class class fail.\n", __func__);
		ret = -1;
		goto out;
	}

	ret = class_create_file(aml_store_class, &aml_version);
	if (ret) {
		pr_info("[%s] can't create aml_store_class file .\n", __func__);
		goto out_class1;
	}
	ret = class_create_file(aml_store_class, &aml_part_table);
	if (ret) {
		pr_info("[%s] can't create aml_store_class file .\n", __func__);
		goto out_class2;
	}
	ret = class_create_file(aml_store_class, &aml_store_device);
	if (ret) {
		pr_info("[%s] can't create aml_store_class file .\n", __func__);
		goto out_class3;
	}

	ret = class_create_file(aml_store_class, &bootloader_offset);
	if (ret) {
		pr_info("[%s] can't create aml_store_class file .\n", __func__);
		goto out_class3;
	}

	/*ret = class_create_file(aml_store_class, &cd_irq_cnt_);
	 *if (ret) {
	 *	pr_info("[%s] can't create aml_store_class file .\n", __func__);
	 *	goto out_class3;
	 *}
	 */
	pr_info("Exit %s %s.\n", __func__, (ret == 0) ? "OK" : "ERROR");
	return ret;

out_class3:
	class_remove_file(aml_store_class, &aml_part_table);
out_class2:
	class_remove_file(aml_store_class, &aml_version);
out_class1:
	class_destroy(aml_store_class);
out:
	kfree(pt_fmt);
	return ret;
}
EXPORT_SYMBOL_GPL(aml_emmc_partition_ops);
#endif
