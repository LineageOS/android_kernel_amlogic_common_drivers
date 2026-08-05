/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#ifndef _MMC_DTB_H_
#define _MMC_DTB_H_

#include <linux/mmc/core.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>

#ifdef CONFIG_AMLOGIC_DTS_PARTITION
#define STORE_CODE                      1
#define STORE_CACHE			BIT(1)
#define STORE_DATA			BIT(2)

#define MAX_PART_NAME_LEN               16
#define MAX_MMC_PART_NUM                32

/* MMC Partition Table */
#define MMC_PARTITIONS_MAGIC            "MPT"
#define MMC_RESERVED_NAME               "reserved"

/* the size of bootloader partition */
#define MMC_BOOT_PARTITION_SIZE         (4 * SZ_1M)

/* the size of reserve space behind bootloader partition */
#define MMC_BOOT_PARTITION_RESERVED     (32 * SZ_1M)

struct partitions {
	/* identifier string */
	char name[MAX_PART_NAME_LEN];
	/* partition size, byte unit */
	u64 size;
	/* offset within the master space, byte unit */
	u64 offset;
	/* master flags to mask out for this partition */
	unsigned int mask_flags;
};

struct mmc_partitions_fmt {
	char magic[4];
	unsigned char version[12];
	int part_num;
	int checksum;
	struct partitions partitions[MAX_MMC_PART_NUM];
};

int aml_emmc_partition_ops(struct mmc_card *card, struct gendisk *disk);
#endif
void amlmmc_dtb_init(struct mmc_card *card_dtbkey, int *retp);

#endif
