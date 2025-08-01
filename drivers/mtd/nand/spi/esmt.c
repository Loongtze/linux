// SPDX-License-Identifier: GPL-2.0
/*
 * Author:
 *	Chuanhong Guo <gch981213@gmail.com> - the main driver logic
 *	Martin Kurbanov <mmkurbanov@sberdevices.ru> - OOB layout
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mtd/spinand.h>
#include <linux/spi/spi-mem.h>

/* ESMT uses GigaDevice 0xc8 JECDEC ID on some SPI NANDs */
#define SPINAND_MFR_ESMT_C8			0xc8

#define ESMT_F50L1G41LB_CFG_OTP_PROTECT		BIT(7)
#define ESMT_F50L1G41LB_CFG_OTP_LOCK		\
	(CFG_OTP_ENABLE | ESMT_F50L1G41LB_CFG_OTP_PROTECT)

static SPINAND_OP_VARIANTS(read_cache_variants,
			   SPINAND_PAGE_READ_FROM_CACHE_1S_1S_4S_OP(0, 1, NULL, 0, 0),
			   SPINAND_PAGE_READ_FROM_CACHE_1S_1S_2S_OP(0, 1, NULL, 0, 0),
			   SPINAND_PAGE_READ_FROM_CACHE_FAST_1S_1S_1S_OP(0, 1, NULL, 0, 0),
			   SPINAND_PAGE_READ_FROM_CACHE_1S_1S_1S_OP(0, 1, NULL, 0, 0));

static SPINAND_OP_VARIANTS(write_cache_variants,
			   SPINAND_PROG_LOAD_1S_1S_4S_OP(true, 0, NULL, 0),
			   SPINAND_PROG_LOAD_1S_1S_1S_OP(true, 0, NULL, 0));

static SPINAND_OP_VARIANTS(update_cache_variants,
			   SPINAND_PROG_LOAD_1S_1S_4S_OP(false, 0, NULL, 0),
			   SPINAND_PROG_LOAD_1S_1S_1S_OP(false, 0, NULL, 0));

/*
 * OOB spare area map (64 bytes)
 *
 * Bad Block Markers
 * filled by HW and kernel                 Reserved
 *   |                 +-----------------------+-----------------------+
 *   |                 |                       |                       |
 *   |                 |    OOB free data Area |non ECC protected      |
 *   |   +-------------|-----+-----------------|-----+-----------------|-----+
 *   |   |             |     |                 |     |                 |     |
 * +-|---|----------+--|-----|--------------+--|-----|--------------+--|-----|--------------+
 * | |   | section0 |  |     |    section1  |  |     |    section2  |  |     |    section3  |
 * +-v-+-v-+---+----+--v--+--v--+-----+-----+--v--+--v--+-----+-----+--v--+--v--+-----+-----+
 * |   |   |   |    |     |     |     |     |     |     |     |     |     |     |     |     |
 * |0:1|2:3|4:7|8:15|16:17|18:19|20:23|24:31|32:33|34:35|36:39|40:47|48:49|50:51|52:55|56:63|
 * |   |   |   |    |     |     |     |     |     |     |     |     |     |     |     |     |
 * +---+---+-^-+--^-+-----+-----+--^--+--^--+-----+-----+--^--+--^--+-----+-----+--^--+--^--+
 *           |    |                |     |                 |     |                 |     |
 *           |    +----------------|-----+-----------------|-----+-----------------|-----+
 *           |             ECC Area|(Main + Spare) - filled|by ESMT NAND HW        |
 *           |                     |                       |                       |
 *           +---------------------+-----------------------+-----------------------+
 *                         OOB ECC protected Area - not used due to
 *                         partial programming from some filesystems
 *                             (like JFFS2 with cleanmarkers)
 */

#define ESMT_OOB_SECTION_COUNT			4
#define ESMT_OOB_SECTION_SIZE(nand) \
	(nanddev_per_page_oobsize(nand) / ESMT_OOB_SECTION_COUNT)
#define ESMT_OOB_FREE_SIZE(nand) \
	(ESMT_OOB_SECTION_SIZE(nand) / 2)
#define ESMT_OOB_ECC_SIZE(nand) \
	(ESMT_OOB_SECTION_SIZE(nand) - ESMT_OOB_FREE_SIZE(nand))
#define ESMT_OOB_BBM_SIZE			2

static int f50l1g41lb_ooblayout_ecc(struct mtd_info *mtd, int section,
				    struct mtd_oob_region *region)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);

	if (section >= ESMT_OOB_SECTION_COUNT)
		return -ERANGE;

	region->offset = section * ESMT_OOB_SECTION_SIZE(nand) +
			 ESMT_OOB_FREE_SIZE(nand);
	region->length = ESMT_OOB_ECC_SIZE(nand);

	return 0;
}

static int f50l1g41lb_ooblayout_free(struct mtd_info *mtd, int section,
				     struct mtd_oob_region *region)
{
	struct nand_device *nand = mtd_to_nanddev(mtd);

	if (section >= ESMT_OOB_SECTION_COUNT)
		return -ERANGE;

	/*
	 * Reserve space for bad blocks markers (section0) and
	 * reserved bytes (sections 1-3)
	 */
	region->offset = section * ESMT_OOB_SECTION_SIZE(nand) + 2;

	/* Use only 2 non-protected ECC bytes per each OOB section */
	region->length = 2;

	return 0;
}

static const struct mtd_ooblayout_ops f50l1g41lb_ooblayout = {
	.ecc = f50l1g41lb_ooblayout_ecc,
	.free = f50l1g41lb_ooblayout_free,
};

static int f50l1g41lb_otp_info(struct spinand_device *spinand, size_t len,
			       struct otp_info *buf, size_t *retlen, bool user)
{
	if (len < sizeof(*buf))
		return -EINVAL;

	buf->locked = 0;
	buf->start = 0;
	buf->length = user ? spinand_user_otp_size(spinand) :
			     spinand_fact_otp_size(spinand);

	*retlen = sizeof(*buf);
	return 0;
}

static int f50l1g41lb_fact_otp_info(struct spinand_device *spinand, size_t len,
				    struct otp_info *buf, size_t *retlen)
{
	return f50l1g41lb_otp_info(spinand, len, buf, retlen, false);
}

static int f50l1g41lb_user_otp_info(struct spinand_device *spinand, size_t len,
				    struct otp_info *buf, size_t *retlen)
{
	return f50l1g41lb_otp_info(spinand, len, buf, retlen, true);
}

static int f50l1g41lb_otp_lock(struct spinand_device *spinand, loff_t from,
			       size_t len)
{
	struct spi_mem_op write_op = SPINAND_WR_EN_DIS_1S_0_0_OP(true);
	struct spi_mem_op exec_op = SPINAND_PROG_EXEC_1S_1S_0_OP(0);
	u8 status;
	int ret;

	ret = spinand_upd_cfg(spinand, ESMT_F50L1G41LB_CFG_OTP_LOCK,
			      ESMT_F50L1G41LB_CFG_OTP_LOCK);
	if (!ret)
		return ret;

	ret = spi_mem_exec_op(spinand->spimem, &write_op);
	if (!ret)
		goto out;

	ret = spi_mem_exec_op(spinand->spimem, &exec_op);
	if (!ret)
		goto out;

	ret = spinand_wait(spinand,
			   SPINAND_WRITE_INITIAL_DELAY_US,
			   SPINAND_WRITE_POLL_DELAY_US,
			   &status);
	if (!ret && (status & STATUS_PROG_FAILED))
		ret = -EIO;

out:
	if (spinand_upd_cfg(spinand, ESMT_F50L1G41LB_CFG_OTP_LOCK, 0)) {
		dev_warn(&spinand_to_mtd(spinand)->dev,
			 "Can not disable OTP mode\n");
		ret = -EIO;
	}

	return ret;
}

static const struct spinand_user_otp_ops f50l1g41lb_user_otp_ops = {
	.info = f50l1g41lb_user_otp_info,
	.lock = f50l1g41lb_otp_lock,
	.read = spinand_user_otp_read,
	.write = spinand_user_otp_write,
};

static const struct spinand_fact_otp_ops f50l1g41lb_fact_otp_ops = {
	.info = f50l1g41lb_fact_otp_info,
	.read = spinand_fact_otp_read,
};

static const struct spinand_info esmt_c8_spinand_table[] = {
	SPINAND_INFO("F50L1G41LB",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_ADDR, 0x01, 0x7f,
				0x7f, 0x7f),
		     NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(1, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     0,
		     SPINAND_ECCINFO(&f50l1g41lb_ooblayout, NULL),
		     SPINAND_USER_OTP_INFO(28, 2, &f50l1g41lb_user_otp_ops),
		     SPINAND_FACT_OTP_INFO(2, 0, &f50l1g41lb_fact_otp_ops)),
	SPINAND_INFO("F50D1G41LB",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_ADDR, 0x11, 0x7f,
				0x7f),
		     NAND_MEMORG(1, 2048, 64, 64, 1024, 20, 1, 1, 1),
		     NAND_ECCREQ(1, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     0,
		     SPINAND_ECCINFO(&f50l1g41lb_ooblayout, NULL),
		     SPINAND_USER_OTP_INFO(28, 2, &f50l1g41lb_user_otp_ops),
		     SPINAND_FACT_OTP_INFO(2, 0, &f50l1g41lb_fact_otp_ops)),
	SPINAND_INFO("F50D2G41KA",
		     SPINAND_ID(SPINAND_READID_METHOD_OPCODE_ADDR, 0x51, 0x7f,
				0x7f, 0x7f),
		     NAND_MEMORG(1, 2048, 128, 64, 2048, 40, 1, 1, 1),
		     NAND_ECCREQ(8, 512),
		     SPINAND_INFO_OP_VARIANTS(&read_cache_variants,
					      &write_cache_variants,
					      &update_cache_variants),
		     0,
		     SPINAND_ECCINFO(&f50l1g41lb_ooblayout, NULL)),
};

static const struct spinand_manufacturer_ops esmt_spinand_manuf_ops = {
};

const struct spinand_manufacturer esmt_c8_spinand_manufacturer = {
	.id = SPINAND_MFR_ESMT_C8,
	.name = "ESMT",
	.chips = esmt_c8_spinand_table,
	.nchips = ARRAY_SIZE(esmt_c8_spinand_table),
	.ops = &esmt_spinand_manuf_ops,
};
