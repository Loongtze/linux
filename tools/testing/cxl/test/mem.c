// SPDX-License-Identifier: GPL-2.0-only
// Copyright(c) 2021 Intel Corporation. All rights reserved.

#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/sizes.h>
#include <linux/bits.h>
#include <cxl/mailbox.h>
#include <linux/unaligned.h>
#include <crypto/sha2.h>
#include <cxlmem.h>

#include "trace.h"

#define LSA_SIZE SZ_128K
#define FW_SIZE SZ_64M
#define FW_SLOTS 3
#define DEV_SIZE SZ_2G
#define EFFECT(x) (1U << x)

#define MOCK_INJECT_DEV_MAX 8
#define MOCK_INJECT_TEST_MAX 128

static unsigned int poison_inject_dev_max = MOCK_INJECT_DEV_MAX;

enum cxl_command_effects {
	CONF_CHANGE_COLD_RESET = 0,
	CONF_CHANGE_IMMEDIATE,
	DATA_CHANGE_IMMEDIATE,
	POLICY_CHANGE_IMMEDIATE,
	LOG_CHANGE_IMMEDIATE,
	SECURITY_CHANGE_IMMEDIATE,
	BACKGROUND_OP,
	SECONDARY_MBOX_SUPPORTED,
};

#define CXL_CMD_EFFECT_NONE cpu_to_le16(0)

static struct cxl_cel_entry mock_cel[] = {
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_GET_SUPPORTED_LOGS),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_GET_SUPPORTED_FEATURES),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_GET_FEATURE),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_SET_FEATURE),
		.effect = cpu_to_le16(EFFECT(CONF_CHANGE_IMMEDIATE)),
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_IDENTIFY),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_GET_LSA),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_GET_PARTITION_INFO),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_SET_LSA),
		.effect = cpu_to_le16(EFFECT(CONF_CHANGE_IMMEDIATE) |
				      EFFECT(DATA_CHANGE_IMMEDIATE)),
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_GET_HEALTH_INFO),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_SET_SHUTDOWN_STATE),
		.effect = POLICY_CHANGE_IMMEDIATE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_GET_POISON),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_INJECT_POISON),
		.effect = cpu_to_le16(EFFECT(DATA_CHANGE_IMMEDIATE)),
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_CLEAR_POISON),
		.effect = cpu_to_le16(EFFECT(DATA_CHANGE_IMMEDIATE)),
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_GET_FW_INFO),
		.effect = CXL_CMD_EFFECT_NONE,
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_TRANSFER_FW),
		.effect = cpu_to_le16(EFFECT(CONF_CHANGE_COLD_RESET) |
				      EFFECT(BACKGROUND_OP)),
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_ACTIVATE_FW),
		.effect = cpu_to_le16(EFFECT(CONF_CHANGE_COLD_RESET) |
				      EFFECT(CONF_CHANGE_IMMEDIATE)),
	},
	{
		.opcode = cpu_to_le16(CXL_MBOX_OP_SANITIZE),
		.effect = cpu_to_le16(EFFECT(DATA_CHANGE_IMMEDIATE) |
				      EFFECT(SECURITY_CHANGE_IMMEDIATE) |
				      EFFECT(BACKGROUND_OP)),
	},
};

/* See CXL 2.0 Table 181 Get Health Info Output Payload */
struct cxl_mbox_health_info {
	u8 health_status;
	u8 media_status;
	u8 ext_status;
	u8 life_used;
	__le16 temperature;
	__le32 dirty_shutdowns;
	__le32 volatile_errors;
	__le32 pmem_errors;
} __packed;

static struct {
	struct cxl_mbox_get_supported_logs gsl;
	struct cxl_gsl_entry entry;
} mock_gsl_payload = {
	.gsl = {
		.entries = cpu_to_le16(1),
	},
	.entry = {
		.uuid = DEFINE_CXL_CEL_UUID,
		.size = cpu_to_le32(sizeof(mock_cel)),
	},
};

#define PASS_TRY_LIMIT 3

#define CXL_TEST_EVENT_CNT_MAX 15

/* Set a number of events to return at a time for simulation.  */
#define CXL_TEST_EVENT_RET_MAX 4

struct mock_event_log {
	u16 clear_idx;
	u16 cur_idx;
	u16 nr_events;
	u16 nr_overflow;
	u16 overflow_reset;
	struct cxl_event_record_raw *events[CXL_TEST_EVENT_CNT_MAX];
};

struct mock_event_store {
	struct mock_event_log mock_logs[CXL_EVENT_TYPE_MAX];
	u32 ev_status;
};

struct vendor_test_feat {
	__le32 data;
} __packed;

struct cxl_mockmem_data {
	void *lsa;
	void *fw;
	int fw_slot;
	int fw_staged;
	size_t fw_size;
	u32 security_state;
	u8 user_pass[NVDIMM_PASSPHRASE_LEN];
	u8 master_pass[NVDIMM_PASSPHRASE_LEN];
	int user_limit;
	int master_limit;
	struct mock_event_store mes;
	struct cxl_memdev_state *mds;
	u8 event_buf[SZ_4K];
	u64 timestamp;
	unsigned long sanitize_timeout;
	struct vendor_test_feat test_feat;
	u8 shutdown_state;
};

static struct mock_event_log *event_find_log(struct device *dev, int log_type)
{
	struct cxl_mockmem_data *mdata = dev_get_drvdata(dev);

	if (log_type >= CXL_EVENT_TYPE_MAX)
		return NULL;
	return &mdata->mes.mock_logs[log_type];
}

static struct cxl_event_record_raw *event_get_current(struct mock_event_log *log)
{
	return log->events[log->cur_idx];
}

static void event_reset_log(struct mock_event_log *log)
{
	log->cur_idx = 0;
	log->clear_idx = 0;
	log->nr_overflow = log->overflow_reset;
}

/* Handle can never be 0 use 1 based indexing for handle */
static u16 event_get_clear_handle(struct mock_event_log *log)
{
	return log->clear_idx + 1;
}

/* Handle can never be 0 use 1 based indexing for handle */
static __le16 event_get_cur_event_handle(struct mock_event_log *log)
{
	u16 cur_handle = log->cur_idx + 1;

	return cpu_to_le16(cur_handle);
}

static bool event_log_empty(struct mock_event_log *log)
{
	return log->cur_idx == log->nr_events;
}

static void mes_add_event(struct mock_event_store *mes,
			  enum cxl_event_log_type log_type,
			  struct cxl_event_record_raw *event)
{
	struct mock_event_log *log;

	if (WARN_ON(log_type >= CXL_EVENT_TYPE_MAX))
		return;

	log = &mes->mock_logs[log_type];

	if ((log->nr_events + 1) > CXL_TEST_EVENT_CNT_MAX) {
		log->nr_overflow++;
		log->overflow_reset = log->nr_overflow;
		return;
	}

	log->events[log->nr_events] = event;
	log->nr_events++;
}

/*
 * Vary the number of events returned to simulate events occuring while the
 * logs are being read.
 */
static int ret_limit = 0;

static int mock_get_event(struct device *dev, struct cxl_mbox_cmd *cmd)
{
	struct cxl_get_event_payload *pl;
	struct mock_event_log *log;
	u16 nr_overflow;
	u8 log_type;
	int i;

	if (cmd->size_in != sizeof(log_type))
		return -EINVAL;

	ret_limit = (ret_limit + 1) % CXL_TEST_EVENT_RET_MAX;
	if (!ret_limit)
		ret_limit = 1;

	if (cmd->size_out < struct_size(pl, records, ret_limit))
		return -EINVAL;

	log_type = *((u8 *)cmd->payload_in);
	if (log_type >= CXL_EVENT_TYPE_MAX)
		return -EINVAL;

	memset(cmd->payload_out, 0, struct_size(pl, records, 0));

	log = event_find_log(dev, log_type);
	if (!log || event_log_empty(log))
		return 0;

	pl = cmd->payload_out;

	for (i = 0; i < ret_limit && !event_log_empty(log); i++) {
		memcpy(&pl->records[i], event_get_current(log),
		       sizeof(pl->records[i]));
		pl->records[i].event.generic.hdr.handle =
				event_get_cur_event_handle(log);
		log->cur_idx++;
	}

	cmd->size_out = struct_size(pl, records, i);
	pl->record_count = cpu_to_le16(i);
	if (!event_log_empty(log))
		pl->flags |= CXL_GET_EVENT_FLAG_MORE_RECORDS;

	if (log->nr_overflow) {
		u64 ns;

		pl->flags |= CXL_GET_EVENT_FLAG_OVERFLOW;
		pl->overflow_err_count = cpu_to_le16(nr_overflow);
		ns = ktime_get_real_ns();
		ns -= 5000000000; /* 5s ago */
		pl->first_overflow_timestamp = cpu_to_le64(ns);
		ns = ktime_get_real_ns();
		ns -= 1000000000; /* 1s ago */
		pl->last_overflow_timestamp = cpu_to_le64(ns);
	}

	return 0;
}

static int mock_clear_event(struct device *dev, struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_clear_event_payload *pl = cmd->payload_in;
	struct mock_event_log *log;
	u8 log_type = pl->event_log;
	u16 handle;
	int nr;

	if (log_type >= CXL_EVENT_TYPE_MAX)
		return -EINVAL;

	log = event_find_log(dev, log_type);
	if (!log)
		return 0; /* No mock data in this log */

	/*
	 * This check is technically not invalid per the specification AFAICS.
	 * (The host could 'guess' handles and clear them in order).
	 * However, this is not good behavior for the host so test it.
	 */
	if (log->clear_idx + pl->nr_recs > log->cur_idx) {
		dev_err(dev,
			"Attempting to clear more events than returned!\n");
		return -EINVAL;
	}

	/* Check handle order prior to clearing events */
	for (nr = 0, handle = event_get_clear_handle(log);
	     nr < pl->nr_recs;
	     nr++, handle++) {
		if (handle != le16_to_cpu(pl->handles[nr])) {
			dev_err(dev, "Clearing events out of order\n");
			return -EINVAL;
		}
	}

	if (log->nr_overflow)
		log->nr_overflow = 0;

	/* Clear events */
	log->clear_idx += pl->nr_recs;
	return 0;
}

static void cxl_mock_event_trigger(struct device *dev)
{
	struct cxl_mockmem_data *mdata = dev_get_drvdata(dev);
	struct mock_event_store *mes = &mdata->mes;
	int i;

	for (i = CXL_EVENT_TYPE_INFO; i < CXL_EVENT_TYPE_MAX; i++) {
		struct mock_event_log *log;

		log = event_find_log(dev, i);
		if (log)
			event_reset_log(log);
	}

	cxl_mem_get_event_records(mdata->mds, mes->ev_status);
}

struct cxl_event_record_raw maint_needed = {
	.id = UUID_INIT(0xBA5EBA11, 0xABCD, 0xEFEB,
			0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0xa5, 0x5a, 0xa5),
	.event.generic = {
		.hdr = {
			.length = sizeof(struct cxl_event_record_raw),
			.flags[0] = CXL_EVENT_RECORD_FLAG_MAINT_NEEDED,
			/* .handle = Set dynamically */
			.related_handle = cpu_to_le16(0xa5b6),
		},
		.data = { 0xDE, 0xAD, 0xBE, 0xEF },
	},
};

struct cxl_event_record_raw hardware_replace = {
	.id = UUID_INIT(0xABCDEFEB, 0xBA11, 0xBA5E,
			0xa5, 0x5a, 0xa5, 0x5a, 0xa5, 0xa5, 0x5a, 0xa5),
	.event.generic = {
		.hdr = {
			.length = sizeof(struct cxl_event_record_raw),
			.flags[0] = CXL_EVENT_RECORD_FLAG_HW_REPLACE,
			/* .handle = Set dynamically */
			.related_handle = cpu_to_le16(0xb6a5),
		},
		.data = { 0xDE, 0xAD, 0xBE, 0xEF },
	},
};

struct cxl_test_gen_media {
	uuid_t id;
	struct cxl_event_gen_media rec;
} __packed;

struct cxl_test_gen_media gen_media = {
	.id = CXL_EVENT_GEN_MEDIA_UUID,
	.rec = {
		.media_hdr = {
			.hdr = {
				.length = sizeof(struct cxl_test_gen_media),
				.flags[0] = CXL_EVENT_RECORD_FLAG_PERMANENT,
				/* .handle = Set dynamically */
				.related_handle = cpu_to_le16(0),
			},
			.phys_addr = cpu_to_le64(0x2000),
			.descriptor = CXL_GMER_EVT_DESC_UNCORECTABLE_EVENT,
			.type = CXL_GMER_MEM_EVT_TYPE_DATA_PATH_ERROR,
			.transaction_type = CXL_GMER_TRANS_HOST_WRITE,
			/* .validity_flags = <set below> */
			.channel = 1,
			.rank = 30,
		},
		.component_id = { 0x3, 0x74, 0xc5, 0x8, 0x9a, 0x1a, 0xb, 0xfc, 0xd2, 0x7e, 0x2f, 0x31, 0x9b, 0x3c, 0x81, 0x4d },
		.cme_threshold_ev_flags = 3,
		.cme_count = { 33, 0, 0 },
		.sub_type = 0x2,
	},
};

struct cxl_test_dram {
	uuid_t id;
	struct cxl_event_dram rec;
} __packed;

struct cxl_test_dram dram = {
	.id = CXL_EVENT_DRAM_UUID,
	.rec = {
		.media_hdr = {
			.hdr = {
				.length = sizeof(struct cxl_test_dram),
				.flags[0] = CXL_EVENT_RECORD_FLAG_PERF_DEGRADED,
				/* .handle = Set dynamically */
				.related_handle = cpu_to_le16(0),
			},
			.phys_addr = cpu_to_le64(0x8000),
			.descriptor = CXL_GMER_EVT_DESC_THRESHOLD_EVENT,
			.type = CXL_GMER_MEM_EVT_TYPE_INV_ADDR,
			.transaction_type = CXL_GMER_TRANS_INTERNAL_MEDIA_SCRUB,
			/* .validity_flags = <set below> */
			.channel = 1,
		},
		.bank_group = 5,
		.bank = 2,
		.column = {0xDE, 0xAD},
		.component_id = { 0x1, 0x74, 0xc5, 0x8, 0x9a, 0x1a, 0xb, 0xfc, 0xd2, 0x7e, 0x2f, 0x31, 0x9b, 0x3c, 0x81, 0x4d },
		.sub_channel = 8,
		.cme_threshold_ev_flags = 2,
		.cvme_count = { 14, 0, 0 },
		.sub_type = 0x5,
	},
};

struct cxl_test_mem_module {
	uuid_t id;
	struct cxl_event_mem_module rec;
} __packed;

struct cxl_test_mem_module mem_module = {
	.id = CXL_EVENT_MEM_MODULE_UUID,
	.rec = {
		.hdr = {
			.length = sizeof(struct cxl_test_mem_module),
			/* .handle = Set dynamically */
			.related_handle = cpu_to_le16(0),
		},
		.event_type = CXL_MMER_TEMP_CHANGE,
		.info = {
			.health_status = CXL_DHI_HS_PERFORMANCE_DEGRADED,
			.media_status = CXL_DHI_MS_ALL_DATA_LOST,
			.add_status = (CXL_DHI_AS_CRITICAL << 2) |
				      (CXL_DHI_AS_WARNING << 4) |
				      (CXL_DHI_AS_WARNING << 5),
			.device_temp = { 0xDE, 0xAD},
			.dirty_shutdown_cnt = { 0xde, 0xad, 0xbe, 0xef },
			.cor_vol_err_cnt = { 0xde, 0xad, 0xbe, 0xef },
			.cor_per_err_cnt = { 0xde, 0xad, 0xbe, 0xef },
		},
		/* .validity_flags = <set below> */
		.component_id = { 0x2, 0x74, 0xc5, 0x8, 0x9a, 0x1a, 0xb, 0xfc, 0xd2, 0x7e, 0x2f, 0x31, 0x9b, 0x3c, 0x81, 0x4d },
		.event_sub_type = 0x3,
	},
};

static int mock_set_timestamp(struct cxl_dev_state *cxlds,
			      struct cxl_mbox_cmd *cmd)
{
	struct cxl_mockmem_data *mdata = dev_get_drvdata(cxlds->dev);
	struct cxl_mbox_set_timestamp_in *ts = cmd->payload_in;

	if (cmd->size_in != sizeof(*ts))
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	mdata->timestamp = le64_to_cpu(ts->timestamp);
	return 0;
}

static void cxl_mock_add_event_logs(struct mock_event_store *mes)
{
	put_unaligned_le16(CXL_GMER_VALID_CHANNEL | CXL_GMER_VALID_RANK |
			   CXL_GMER_VALID_COMPONENT | CXL_GMER_VALID_COMPONENT_ID_FORMAT,
			   &gen_media.rec.media_hdr.validity_flags);

	put_unaligned_le16(CXL_DER_VALID_CHANNEL | CXL_DER_VALID_BANK_GROUP |
			   CXL_DER_VALID_BANK | CXL_DER_VALID_COLUMN | CXL_DER_VALID_SUB_CHANNEL |
			   CXL_DER_VALID_COMPONENT | CXL_DER_VALID_COMPONENT_ID_FORMAT,
			   &dram.rec.media_hdr.validity_flags);

	put_unaligned_le16(CXL_MMER_VALID_COMPONENT | CXL_MMER_VALID_COMPONENT_ID_FORMAT,
			   &mem_module.rec.validity_flags);

	mes_add_event(mes, CXL_EVENT_TYPE_INFO, &maint_needed);
	mes_add_event(mes, CXL_EVENT_TYPE_INFO,
		      (struct cxl_event_record_raw *)&gen_media);
	mes_add_event(mes, CXL_EVENT_TYPE_INFO,
		      (struct cxl_event_record_raw *)&mem_module);
	mes->ev_status |= CXLDEV_EVENT_STATUS_INFO;

	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &maint_needed);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL,
		      (struct cxl_event_record_raw *)&dram);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL,
		      (struct cxl_event_record_raw *)&gen_media);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL,
		      (struct cxl_event_record_raw *)&mem_module);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL,
		      (struct cxl_event_record_raw *)&dram);
	/* Overflow this log */
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FAIL, &hardware_replace);
	mes->ev_status |= CXLDEV_EVENT_STATUS_FAIL;

	mes_add_event(mes, CXL_EVENT_TYPE_FATAL, &hardware_replace);
	mes_add_event(mes, CXL_EVENT_TYPE_FATAL,
		      (struct cxl_event_record_raw *)&dram);
	mes->ev_status |= CXLDEV_EVENT_STATUS_FATAL;
}

static int mock_gsl(struct cxl_mbox_cmd *cmd)
{
	if (cmd->size_out < sizeof(mock_gsl_payload))
		return -EINVAL;

	memcpy(cmd->payload_out, &mock_gsl_payload, sizeof(mock_gsl_payload));
	cmd->size_out = sizeof(mock_gsl_payload);

	return 0;
}

static int mock_get_log(struct cxl_memdev_state *mds, struct cxl_mbox_cmd *cmd)
{
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;
	struct cxl_mbox_get_log *gl = cmd->payload_in;
	u32 offset = le32_to_cpu(gl->offset);
	u32 length = le32_to_cpu(gl->length);
	uuid_t uuid = DEFINE_CXL_CEL_UUID;
	void *data = &mock_cel;

	if (cmd->size_in < sizeof(*gl))
		return -EINVAL;
	if (length > cxl_mbox->payload_size)
		return -EINVAL;
	if (offset + length > sizeof(mock_cel))
		return -EINVAL;
	if (!uuid_equal(&gl->uuid, &uuid))
		return -EINVAL;
	if (length > cmd->size_out)
		return -EINVAL;

	memcpy(cmd->payload_out, data + offset, length);

	return 0;
}

static int mock_rcd_id(struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_identify id = {
		.fw_revision = { "mock fw v1 " },
		.total_capacity =
			cpu_to_le64(DEV_SIZE / CXL_CAPACITY_MULTIPLIER),
		.volatile_capacity =
			cpu_to_le64(DEV_SIZE / CXL_CAPACITY_MULTIPLIER),
	};

	if (cmd->size_out < sizeof(id))
		return -EINVAL;

	memcpy(cmd->payload_out, &id, sizeof(id));

	return 0;
}

static int mock_id(struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_identify id = {
		.fw_revision = { "mock fw v1 " },
		.lsa_size = cpu_to_le32(LSA_SIZE),
		.partition_align =
			cpu_to_le64(SZ_256M / CXL_CAPACITY_MULTIPLIER),
		.total_capacity =
			cpu_to_le64(DEV_SIZE / CXL_CAPACITY_MULTIPLIER),
		.inject_poison_limit = cpu_to_le16(MOCK_INJECT_TEST_MAX),
	};

	put_unaligned_le24(CXL_POISON_LIST_MAX, id.poison_list_max_mer);

	if (cmd->size_out < sizeof(id))
		return -EINVAL;

	memcpy(cmd->payload_out, &id, sizeof(id));

	return 0;
}

static int mock_partition_info(struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_get_partition_info pi = {
		.active_volatile_cap =
			cpu_to_le64(DEV_SIZE / 2 / CXL_CAPACITY_MULTIPLIER),
		.active_persistent_cap =
			cpu_to_le64(DEV_SIZE / 2 / CXL_CAPACITY_MULTIPLIER),
	};

	if (cmd->size_out < sizeof(pi))
		return -EINVAL;

	memcpy(cmd->payload_out, &pi, sizeof(pi));

	return 0;
}

void cxl_mockmem_sanitize_work(struct work_struct *work)
{
	struct cxl_memdev_state *mds =
		container_of(work, typeof(*mds), security.poll_dwork.work);
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;

	mutex_lock(&cxl_mbox->mbox_mutex);
	if (mds->security.sanitize_node)
		sysfs_notify_dirent(mds->security.sanitize_node);
	mds->security.sanitize_active = false;
	mutex_unlock(&cxl_mbox->mbox_mutex);

	dev_dbg(mds->cxlds.dev, "sanitize complete\n");
}

static int mock_sanitize(struct cxl_mockmem_data *mdata,
			 struct cxl_mbox_cmd *cmd)
{
	struct cxl_memdev_state *mds = mdata->mds;
	struct cxl_mailbox *cxl_mbox = &mds->cxlds.cxl_mbox;
	int rc = 0;

	if (cmd->size_in != 0)
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	if (mdata->security_state & CXL_PMEM_SEC_STATE_USER_PASS_SET) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}
	if (mdata->security_state & CXL_PMEM_SEC_STATE_LOCKED) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	mutex_lock(&cxl_mbox->mbox_mutex);
	if (schedule_delayed_work(&mds->security.poll_dwork,
				  msecs_to_jiffies(mdata->sanitize_timeout))) {
		mds->security.sanitize_active = true;
		dev_dbg(mds->cxlds.dev, "sanitize issued\n");
	} else
		rc = -EBUSY;
	mutex_unlock(&cxl_mbox->mbox_mutex);

	return rc;
}

static int mock_secure_erase(struct cxl_mockmem_data *mdata,
			     struct cxl_mbox_cmd *cmd)
{
	if (cmd->size_in != 0)
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	if (mdata->security_state & CXL_PMEM_SEC_STATE_USER_PASS_SET) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	if (mdata->security_state & CXL_PMEM_SEC_STATE_LOCKED) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	return 0;
}

static int mock_get_security_state(struct cxl_mockmem_data *mdata,
				   struct cxl_mbox_cmd *cmd)
{
	if (cmd->size_in)
		return -EINVAL;

	if (cmd->size_out != sizeof(u32))
		return -EINVAL;

	memcpy(cmd->payload_out, &mdata->security_state, sizeof(u32));

	return 0;
}

static void master_plimit_check(struct cxl_mockmem_data *mdata)
{
	if (mdata->master_limit == PASS_TRY_LIMIT)
		return;
	mdata->master_limit++;
	if (mdata->master_limit == PASS_TRY_LIMIT)
		mdata->security_state |= CXL_PMEM_SEC_STATE_MASTER_PLIMIT;
}

static void user_plimit_check(struct cxl_mockmem_data *mdata)
{
	if (mdata->user_limit == PASS_TRY_LIMIT)
		return;
	mdata->user_limit++;
	if (mdata->user_limit == PASS_TRY_LIMIT)
		mdata->security_state |= CXL_PMEM_SEC_STATE_USER_PLIMIT;
}

static int mock_set_passphrase(struct cxl_mockmem_data *mdata,
			       struct cxl_mbox_cmd *cmd)
{
	struct cxl_set_pass *set_pass;

	if (cmd->size_in != sizeof(*set_pass))
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	if (mdata->security_state & CXL_PMEM_SEC_STATE_FROZEN) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	set_pass = cmd->payload_in;
	switch (set_pass->type) {
	case CXL_PMEM_SEC_PASS_MASTER:
		if (mdata->security_state & CXL_PMEM_SEC_STATE_MASTER_PLIMIT) {
			cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
			return -ENXIO;
		}
		/*
		 * CXL spec rev3.0 8.2.9.8.6.2, The master pasphrase shall only be set in
		 * the security disabled state when the user passphrase is not set.
		 */
		if (mdata->security_state & CXL_PMEM_SEC_STATE_USER_PASS_SET) {
			cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
			return -ENXIO;
		}
		if (memcmp(mdata->master_pass, set_pass->old_pass, NVDIMM_PASSPHRASE_LEN)) {
			master_plimit_check(mdata);
			cmd->return_code = CXL_MBOX_CMD_RC_PASSPHRASE;
			return -ENXIO;
		}
		memcpy(mdata->master_pass, set_pass->new_pass, NVDIMM_PASSPHRASE_LEN);
		mdata->security_state |= CXL_PMEM_SEC_STATE_MASTER_PASS_SET;
		return 0;

	case CXL_PMEM_SEC_PASS_USER:
		if (mdata->security_state & CXL_PMEM_SEC_STATE_USER_PLIMIT) {
			cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
			return -ENXIO;
		}
		if (memcmp(mdata->user_pass, set_pass->old_pass, NVDIMM_PASSPHRASE_LEN)) {
			user_plimit_check(mdata);
			cmd->return_code = CXL_MBOX_CMD_RC_PASSPHRASE;
			return -ENXIO;
		}
		memcpy(mdata->user_pass, set_pass->new_pass, NVDIMM_PASSPHRASE_LEN);
		mdata->security_state |= CXL_PMEM_SEC_STATE_USER_PASS_SET;
		return 0;

	default:
		cmd->return_code = CXL_MBOX_CMD_RC_INPUT;
	}
	return -EINVAL;
}

static int mock_disable_passphrase(struct cxl_mockmem_data *mdata,
				   struct cxl_mbox_cmd *cmd)
{
	struct cxl_disable_pass *dis_pass;

	if (cmd->size_in != sizeof(*dis_pass))
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	if (mdata->security_state & CXL_PMEM_SEC_STATE_FROZEN) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	dis_pass = cmd->payload_in;
	switch (dis_pass->type) {
	case CXL_PMEM_SEC_PASS_MASTER:
		if (mdata->security_state & CXL_PMEM_SEC_STATE_MASTER_PLIMIT) {
			cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
			return -ENXIO;
		}

		if (!(mdata->security_state & CXL_PMEM_SEC_STATE_MASTER_PASS_SET)) {
			cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
			return -ENXIO;
		}

		if (memcmp(dis_pass->pass, mdata->master_pass, NVDIMM_PASSPHRASE_LEN)) {
			master_plimit_check(mdata);
			cmd->return_code = CXL_MBOX_CMD_RC_PASSPHRASE;
			return -ENXIO;
		}

		mdata->master_limit = 0;
		memset(mdata->master_pass, 0, NVDIMM_PASSPHRASE_LEN);
		mdata->security_state &= ~CXL_PMEM_SEC_STATE_MASTER_PASS_SET;
		return 0;

	case CXL_PMEM_SEC_PASS_USER:
		if (mdata->security_state & CXL_PMEM_SEC_STATE_USER_PLIMIT) {
			cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
			return -ENXIO;
		}

		if (!(mdata->security_state & CXL_PMEM_SEC_STATE_USER_PASS_SET)) {
			cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
			return -ENXIO;
		}

		if (memcmp(dis_pass->pass, mdata->user_pass, NVDIMM_PASSPHRASE_LEN)) {
			user_plimit_check(mdata);
			cmd->return_code = CXL_MBOX_CMD_RC_PASSPHRASE;
			return -ENXIO;
		}

		mdata->user_limit = 0;
		memset(mdata->user_pass, 0, NVDIMM_PASSPHRASE_LEN);
		mdata->security_state &= ~(CXL_PMEM_SEC_STATE_USER_PASS_SET |
					   CXL_PMEM_SEC_STATE_LOCKED);
		return 0;

	default:
		cmd->return_code = CXL_MBOX_CMD_RC_INPUT;
		return -EINVAL;
	}

	return 0;
}

static int mock_freeze_security(struct cxl_mockmem_data *mdata,
				struct cxl_mbox_cmd *cmd)
{
	if (cmd->size_in != 0)
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	if (mdata->security_state & CXL_PMEM_SEC_STATE_FROZEN)
		return 0;

	mdata->security_state |= CXL_PMEM_SEC_STATE_FROZEN;
	return 0;
}

static int mock_unlock_security(struct cxl_mockmem_data *mdata,
				struct cxl_mbox_cmd *cmd)
{
	if (cmd->size_in != NVDIMM_PASSPHRASE_LEN)
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	if (mdata->security_state & CXL_PMEM_SEC_STATE_FROZEN) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	if (!(mdata->security_state & CXL_PMEM_SEC_STATE_USER_PASS_SET)) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	if (mdata->security_state & CXL_PMEM_SEC_STATE_USER_PLIMIT) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	if (!(mdata->security_state & CXL_PMEM_SEC_STATE_LOCKED)) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	if (memcmp(cmd->payload_in, mdata->user_pass, NVDIMM_PASSPHRASE_LEN)) {
		if (++mdata->user_limit == PASS_TRY_LIMIT)
			mdata->security_state |= CXL_PMEM_SEC_STATE_USER_PLIMIT;
		cmd->return_code = CXL_MBOX_CMD_RC_PASSPHRASE;
		return -ENXIO;
	}

	mdata->user_limit = 0;
	mdata->security_state &= ~CXL_PMEM_SEC_STATE_LOCKED;
	return 0;
}

static int mock_passphrase_secure_erase(struct cxl_mockmem_data *mdata,
					struct cxl_mbox_cmd *cmd)
{
	struct cxl_pass_erase *erase;

	if (cmd->size_in != sizeof(*erase))
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	erase = cmd->payload_in;
	if (mdata->security_state & CXL_PMEM_SEC_STATE_FROZEN) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	if (mdata->security_state & CXL_PMEM_SEC_STATE_USER_PLIMIT &&
	    erase->type == CXL_PMEM_SEC_PASS_USER) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	if (mdata->security_state & CXL_PMEM_SEC_STATE_MASTER_PLIMIT &&
	    erase->type == CXL_PMEM_SEC_PASS_MASTER) {
		cmd->return_code = CXL_MBOX_CMD_RC_SECURITY;
		return -ENXIO;
	}

	switch (erase->type) {
	case CXL_PMEM_SEC_PASS_MASTER:
		/*
		 * The spec does not clearly define the behavior of the scenario
		 * where a master passphrase is passed in while the master
		 * passphrase is not set and user passphrase is not set. The
		 * code will take the assumption that it will behave the same
		 * as a CXL secure erase command without passphrase (0x4401).
		 */
		if (mdata->security_state & CXL_PMEM_SEC_STATE_MASTER_PASS_SET) {
			if (memcmp(mdata->master_pass, erase->pass,
				   NVDIMM_PASSPHRASE_LEN)) {
				master_plimit_check(mdata);
				cmd->return_code = CXL_MBOX_CMD_RC_PASSPHRASE;
				return -ENXIO;
			}
			mdata->master_limit = 0;
			mdata->user_limit = 0;
			mdata->security_state &= ~CXL_PMEM_SEC_STATE_USER_PASS_SET;
			memset(mdata->user_pass, 0, NVDIMM_PASSPHRASE_LEN);
			mdata->security_state &= ~CXL_PMEM_SEC_STATE_LOCKED;
		} else {
			/*
			 * CXL rev3 8.2.9.8.6.3 Disable Passphrase
			 * When master passphrase is disabled, the device shall
			 * return Invalid Input for the Passphrase Secure Erase
			 * command with master passphrase.
			 */
			return -EINVAL;
		}
		/* Scramble encryption keys so that data is effectively erased */
		break;
	case CXL_PMEM_SEC_PASS_USER:
		/*
		 * The spec does not clearly define the behavior of the scenario
		 * where a user passphrase is passed in while the user
		 * passphrase is not set. The code will take the assumption that
		 * it will behave the same as a CXL secure erase command without
		 * passphrase (0x4401).
		 */
		if (mdata->security_state & CXL_PMEM_SEC_STATE_USER_PASS_SET) {
			if (memcmp(mdata->user_pass, erase->pass,
				   NVDIMM_PASSPHRASE_LEN)) {
				user_plimit_check(mdata);
				cmd->return_code = CXL_MBOX_CMD_RC_PASSPHRASE;
				return -ENXIO;
			}
			mdata->user_limit = 0;
			mdata->security_state &= ~CXL_PMEM_SEC_STATE_USER_PASS_SET;
			memset(mdata->user_pass, 0, NVDIMM_PASSPHRASE_LEN);
		}

		/*
		 * CXL rev3 Table 8-118
		 * If user passphrase is not set or supported by device, current
		 * passphrase value is ignored. Will make the assumption that
		 * the operation will proceed as secure erase w/o passphrase
		 * since spec is not explicit.
		 */

		/* Scramble encryption keys so that data is effectively erased */
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int mock_get_lsa(struct cxl_mockmem_data *mdata,
			struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_get_lsa *get_lsa = cmd->payload_in;
	void *lsa = mdata->lsa;
	u32 offset, length;

	if (sizeof(*get_lsa) > cmd->size_in)
		return -EINVAL;
	offset = le32_to_cpu(get_lsa->offset);
	length = le32_to_cpu(get_lsa->length);
	if (offset + length > LSA_SIZE)
		return -EINVAL;
	if (length > cmd->size_out)
		return -EINVAL;

	memcpy(cmd->payload_out, lsa + offset, length);
	return 0;
}

static int mock_set_lsa(struct cxl_mockmem_data *mdata,
			struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_set_lsa *set_lsa = cmd->payload_in;
	void *lsa = mdata->lsa;
	u32 offset, length;

	if (sizeof(*set_lsa) > cmd->size_in)
		return -EINVAL;
	offset = le32_to_cpu(set_lsa->offset);
	length = cmd->size_in - sizeof(*set_lsa);
	if (offset + length > LSA_SIZE)
		return -EINVAL;

	memcpy(lsa + offset, &set_lsa->data[0], length);
	return 0;
}

static int mock_health_info(struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_health_info health_info = {
		/* set flags for maint needed, perf degraded, hw replacement */
		.health_status = 0x7,
		/* set media status to "All Data Lost" */
		.media_status = 0x3,
		/*
		 * set ext_status flags for:
		 *  ext_life_used: normal,
		 *  ext_temperature: critical,
		 *  ext_corrected_volatile: warning,
		 *  ext_corrected_persistent: normal,
		 */
		.ext_status = 0x18,
		.life_used = 15,
		.temperature = cpu_to_le16(25),
		.dirty_shutdowns = cpu_to_le32(10),
		.volatile_errors = cpu_to_le32(20),
		.pmem_errors = cpu_to_le32(30),
	};

	if (cmd->size_out < sizeof(health_info))
		return -EINVAL;

	memcpy(cmd->payload_out, &health_info, sizeof(health_info));
	return 0;
}

static int mock_set_shutdown_state(struct cxl_mockmem_data *mdata,
				   struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_set_shutdown_state_in *ss = cmd->payload_in;

	if (cmd->size_in != sizeof(*ss))
		return -EINVAL;

	if (cmd->size_out != 0)
		return -EINVAL;

	mdata->shutdown_state = ss->state;
	return 0;
}

static struct mock_poison {
	struct cxl_dev_state *cxlds;
	u64 dpa;
} mock_poison_list[MOCK_INJECT_TEST_MAX];

static struct cxl_mbox_poison_out *
cxl_get_injected_po(struct cxl_dev_state *cxlds, u64 offset, u64 length)
{
	struct cxl_mbox_poison_out *po;
	int nr_records = 0;
	u64 dpa;

	po = kzalloc(struct_size(po, record, poison_inject_dev_max), GFP_KERNEL);
	if (!po)
		return NULL;

	for (int i = 0; i < MOCK_INJECT_TEST_MAX; i++) {
		if (mock_poison_list[i].cxlds != cxlds)
			continue;
		if (mock_poison_list[i].dpa < offset ||
		    mock_poison_list[i].dpa > offset + length - 1)
			continue;

		dpa = mock_poison_list[i].dpa + CXL_POISON_SOURCE_INJECTED;
		po->record[nr_records].address = cpu_to_le64(dpa);
		po->record[nr_records].length = cpu_to_le32(1);
		nr_records++;
		if (nr_records == poison_inject_dev_max)
			break;
	}

	/* Always return count, even when zero */
	po->count = cpu_to_le16(nr_records);

	return po;
}

static int mock_get_poison(struct cxl_dev_state *cxlds,
			   struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_poison_in *pi = cmd->payload_in;
	struct cxl_mbox_poison_out *po;
	u64 offset = le64_to_cpu(pi->offset);
	u64 length = le64_to_cpu(pi->length);
	int nr_records;

	po = cxl_get_injected_po(cxlds, offset, length);
	if (!po)
		return -ENOMEM;
	nr_records = le16_to_cpu(po->count);
	memcpy(cmd->payload_out, po, struct_size(po, record, nr_records));
	cmd->size_out = struct_size(po, record, nr_records);
	kfree(po);

	return 0;
}

static bool mock_poison_dev_max_injected(struct cxl_dev_state *cxlds)
{
	int count = 0;

	for (int i = 0; i < MOCK_INJECT_TEST_MAX; i++) {
		if (mock_poison_list[i].cxlds == cxlds)
			count++;
	}
	return (count >= poison_inject_dev_max);
}

static int mock_poison_add(struct cxl_dev_state *cxlds, u64 dpa)
{
	/* Return EBUSY to match the CXL driver handling */
	if (mock_poison_dev_max_injected(cxlds)) {
		dev_dbg(cxlds->dev,
			"Device poison injection limit has been reached: %d\n",
			poison_inject_dev_max);
		return -EBUSY;
	}

	for (int i = 0; i < MOCK_INJECT_TEST_MAX; i++) {
		if (!mock_poison_list[i].cxlds) {
			mock_poison_list[i].cxlds = cxlds;
			mock_poison_list[i].dpa = dpa;
			return 0;
		}
	}
	dev_dbg(cxlds->dev,
		"Mock test poison injection limit has been reached: %d\n",
		MOCK_INJECT_TEST_MAX);

	return -ENXIO;
}

static bool mock_poison_found(struct cxl_dev_state *cxlds, u64 dpa)
{
	for (int i = 0; i < MOCK_INJECT_TEST_MAX; i++) {
		if (mock_poison_list[i].cxlds == cxlds &&
		    mock_poison_list[i].dpa == dpa)
			return true;
	}
	return false;
}

static int mock_inject_poison(struct cxl_dev_state *cxlds,
			      struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_inject_poison *pi = cmd->payload_in;
	u64 dpa = le64_to_cpu(pi->address);

	if (mock_poison_found(cxlds, dpa)) {
		/* Not an error to inject poison if already poisoned */
		dev_dbg(cxlds->dev, "DPA: 0x%llx already poisoned\n", dpa);
		return 0;
	}

	return mock_poison_add(cxlds, dpa);
}

static bool mock_poison_del(struct cxl_dev_state *cxlds, u64 dpa)
{
	for (int i = 0; i < MOCK_INJECT_TEST_MAX; i++) {
		if (mock_poison_list[i].cxlds == cxlds &&
		    mock_poison_list[i].dpa == dpa) {
			mock_poison_list[i].cxlds = NULL;
			return true;
		}
	}
	return false;
}

static int mock_clear_poison(struct cxl_dev_state *cxlds,
			     struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_clear_poison *pi = cmd->payload_in;
	u64 dpa = le64_to_cpu(pi->address);

	/*
	 * A real CXL device will write pi->write_data to the address
	 * being cleared. In this mock, just delete this address from
	 * the mock poison list.
	 */
	if (!mock_poison_del(cxlds, dpa))
		dev_dbg(cxlds->dev, "DPA: 0x%llx not in poison list\n", dpa);

	return 0;
}

static bool mock_poison_list_empty(void)
{
	for (int i = 0; i < MOCK_INJECT_TEST_MAX; i++) {
		if (mock_poison_list[i].cxlds)
			return false;
	}
	return true;
}

static ssize_t poison_inject_max_show(struct device_driver *drv, char *buf)
{
	return sysfs_emit(buf, "%u\n", poison_inject_dev_max);
}

static ssize_t poison_inject_max_store(struct device_driver *drv,
				       const char *buf, size_t len)
{
	int val;

	if (kstrtoint(buf, 0, &val) < 0)
		return -EINVAL;

	if (!mock_poison_list_empty())
		return -EBUSY;

	if (val <= MOCK_INJECT_TEST_MAX)
		poison_inject_dev_max = val;
	else
		return -EINVAL;

	return len;
}

static DRIVER_ATTR_RW(poison_inject_max);

static struct attribute *cxl_mock_mem_core_attrs[] = {
	&driver_attr_poison_inject_max.attr,
	NULL
};
ATTRIBUTE_GROUPS(cxl_mock_mem_core);

static int mock_fw_info(struct cxl_mockmem_data *mdata,
			struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_get_fw_info fw_info = {
		.num_slots = FW_SLOTS,
		.slot_info = (mdata->fw_slot & 0x7) |
			     ((mdata->fw_staged & 0x7) << 3),
		.activation_cap = 0,
	};

	strcpy(fw_info.slot_1_revision, "cxl_test_fw_001");
	strcpy(fw_info.slot_2_revision, "cxl_test_fw_002");
	strcpy(fw_info.slot_3_revision, "cxl_test_fw_003");
	strcpy(fw_info.slot_4_revision, "");

	if (cmd->size_out < sizeof(fw_info))
		return -EINVAL;

	memcpy(cmd->payload_out, &fw_info, sizeof(fw_info));
	return 0;
}

static int mock_transfer_fw(struct cxl_mockmem_data *mdata,
			    struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_transfer_fw *transfer = cmd->payload_in;
	void *fw = mdata->fw;
	size_t offset, length;

	offset = le32_to_cpu(transfer->offset) * CXL_FW_TRANSFER_ALIGNMENT;
	length = cmd->size_in - sizeof(*transfer);
	if (offset + length > FW_SIZE)
		return -EINVAL;

	switch (transfer->action) {
	case CXL_FW_TRANSFER_ACTION_FULL:
		if (offset != 0)
			return -EINVAL;
		fallthrough;
	case CXL_FW_TRANSFER_ACTION_END:
		if (transfer->slot == 0 || transfer->slot > FW_SLOTS)
			return -EINVAL;
		mdata->fw_size = offset + length;
		break;
	case CXL_FW_TRANSFER_ACTION_INITIATE:
	case CXL_FW_TRANSFER_ACTION_CONTINUE:
		break;
	case CXL_FW_TRANSFER_ACTION_ABORT:
		return 0;
	default:
		return -EINVAL;
	}

	memcpy(fw + offset, transfer->data, length);
	usleep_range(1500, 2000);
	return 0;
}

static int mock_activate_fw(struct cxl_mockmem_data *mdata,
			    struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_activate_fw *activate = cmd->payload_in;

	if (activate->slot == 0 || activate->slot > FW_SLOTS)
		return -EINVAL;

	switch (activate->action) {
	case CXL_FW_ACTIVATE_ONLINE:
		mdata->fw_slot = activate->slot;
		mdata->fw_staged = 0;
		return 0;
	case CXL_FW_ACTIVATE_OFFLINE:
		mdata->fw_staged = activate->slot;
		return 0;
	}

	return -EINVAL;
}

#define CXL_VENDOR_FEATURE_TEST							\
	UUID_INIT(0xffffffff, 0xffff, 0xffff, 0xff, 0xff, 0xff, 0xff, 0xff,	\
		  0xff, 0xff, 0xff)

static void fill_feature_vendor_test(struct cxl_feat_entry *feat)
{
	feat->uuid = CXL_VENDOR_FEATURE_TEST;
	feat->id = 0;
	feat->get_feat_size = cpu_to_le16(0x4);
	feat->set_feat_size = cpu_to_le16(0x4);
	feat->flags = cpu_to_le32(CXL_FEATURE_F_CHANGEABLE |
				  CXL_FEATURE_F_DEFAULT_SEL |
				  CXL_FEATURE_F_SAVED_SEL);
	feat->get_feat_ver = 1;
	feat->set_feat_ver = 1;
	feat->effects = cpu_to_le16(CXL_CMD_CONFIG_CHANGE_COLD_RESET |
				    CXL_CMD_EFFECTS_VALID);
}

#define MAX_CXL_TEST_FEATS	1

static int mock_get_test_feature(struct cxl_mockmem_data *mdata,
				 struct cxl_mbox_cmd *cmd)
{
	struct vendor_test_feat *output = cmd->payload_out;
	struct cxl_mbox_get_feat_in *input = cmd->payload_in;
	u16 offset = le16_to_cpu(input->offset);
	u16 count = le16_to_cpu(input->count);
	u8 *ptr;

	if (offset > sizeof(*output)) {
		cmd->return_code = CXL_MBOX_CMD_RC_INPUT;
		return -EINVAL;
	}

	if (offset + count > sizeof(*output)) {
		cmd->return_code = CXL_MBOX_CMD_RC_INPUT;
		return -EINVAL;
	}

	ptr = (u8 *)&mdata->test_feat + offset;
	memcpy((u8 *)output + offset, ptr, count);

	return 0;
}

static int mock_get_feature(struct cxl_mockmem_data *mdata,
			    struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_get_feat_in *input = cmd->payload_in;

	if (uuid_equal(&input->uuid, &CXL_VENDOR_FEATURE_TEST))
		return mock_get_test_feature(mdata, cmd);

	cmd->return_code = CXL_MBOX_CMD_RC_UNSUPPORTED;

	return -EOPNOTSUPP;
}

static int mock_set_test_feature(struct cxl_mockmem_data *mdata,
				 struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_set_feat_in *input = cmd->payload_in;
	struct vendor_test_feat *test =
		(struct vendor_test_feat *)input->feat_data;
	u32 action;

	action = FIELD_GET(CXL_SET_FEAT_FLAG_DATA_TRANSFER_MASK,
			   le32_to_cpu(input->hdr.flags));
	/*
	 * While it is spec compliant to support other set actions, it is not
	 * necessary to add the complication in the emulation currently. Reject
	 * anything besides full xfer.
	 */
	if (action != CXL_SET_FEAT_FLAG_FULL_DATA_TRANSFER) {
		cmd->return_code = CXL_MBOX_CMD_RC_INPUT;
		return -EINVAL;
	}

	/* Offset should be reserved when doing full transfer */
	if (input->hdr.offset) {
		cmd->return_code = CXL_MBOX_CMD_RC_INPUT;
		return -EINVAL;
	}

	memcpy(&mdata->test_feat.data, &test->data, sizeof(u32));

	return 0;
}

static int mock_set_feature(struct cxl_mockmem_data *mdata,
			    struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_set_feat_in *input = cmd->payload_in;

	if (uuid_equal(&input->hdr.uuid, &CXL_VENDOR_FEATURE_TEST))
		return mock_set_test_feature(mdata, cmd);

	cmd->return_code = CXL_MBOX_CMD_RC_UNSUPPORTED;

	return -EOPNOTSUPP;
}

static int mock_get_supported_features(struct cxl_mockmem_data *mdata,
				       struct cxl_mbox_cmd *cmd)
{
	struct cxl_mbox_get_sup_feats_in *in = cmd->payload_in;
	struct cxl_mbox_get_sup_feats_out *out = cmd->payload_out;
	struct cxl_feat_entry *feat;
	u16 start_idx, count;

	if (cmd->size_out < sizeof(*out)) {
		cmd->return_code = CXL_MBOX_CMD_RC_PAYLOADLEN;
		return -EINVAL;
	}

	/*
	 * Current emulation only supports 1 feature
	 */
	start_idx = le16_to_cpu(in->start_idx);
	if (start_idx != 0) {
		cmd->return_code = CXL_MBOX_CMD_RC_INPUT;
		return -EINVAL;
	}

	count = le16_to_cpu(in->count);
	if (count < struct_size(out, ents, 0)) {
		cmd->return_code = CXL_MBOX_CMD_RC_PAYLOADLEN;
		return -EINVAL;
	}

	out->supported_feats = cpu_to_le16(MAX_CXL_TEST_FEATS);
	cmd->return_code = 0;
	if (count < struct_size(out, ents, MAX_CXL_TEST_FEATS)) {
		out->num_entries = 0;
		return 0;
	}

	out->num_entries = cpu_to_le16(MAX_CXL_TEST_FEATS);
	feat = out->ents;
	fill_feature_vendor_test(feat);

	return 0;
}

static int cxl_mock_mbox_send(struct cxl_mailbox *cxl_mbox,
			      struct cxl_mbox_cmd *cmd)
{
	struct device *dev = cxl_mbox->host;
	struct cxl_mockmem_data *mdata = dev_get_drvdata(dev);
	struct cxl_memdev_state *mds = mdata->mds;
	struct cxl_dev_state *cxlds = &mds->cxlds;
	int rc = -EIO;

	switch (cmd->opcode) {
	case CXL_MBOX_OP_SET_TIMESTAMP:
		rc = mock_set_timestamp(cxlds, cmd);
		break;
	case CXL_MBOX_OP_GET_SUPPORTED_LOGS:
		rc = mock_gsl(cmd);
		break;
	case CXL_MBOX_OP_GET_LOG:
		rc = mock_get_log(mds, cmd);
		break;
	case CXL_MBOX_OP_IDENTIFY:
		if (cxlds->rcd)
			rc = mock_rcd_id(cmd);
		else
			rc = mock_id(cmd);
		break;
	case CXL_MBOX_OP_GET_LSA:
		rc = mock_get_lsa(mdata, cmd);
		break;
	case CXL_MBOX_OP_GET_PARTITION_INFO:
		rc = mock_partition_info(cmd);
		break;
	case CXL_MBOX_OP_GET_EVENT_RECORD:
		rc = mock_get_event(dev, cmd);
		break;
	case CXL_MBOX_OP_CLEAR_EVENT_RECORD:
		rc = mock_clear_event(dev, cmd);
		break;
	case CXL_MBOX_OP_SET_LSA:
		rc = mock_set_lsa(mdata, cmd);
		break;
	case CXL_MBOX_OP_GET_HEALTH_INFO:
		rc = mock_health_info(cmd);
		break;
	case CXL_MBOX_OP_SANITIZE:
		rc = mock_sanitize(mdata, cmd);
		break;
	case CXL_MBOX_OP_SECURE_ERASE:
		rc = mock_secure_erase(mdata, cmd);
		break;
	case CXL_MBOX_OP_GET_SECURITY_STATE:
		rc = mock_get_security_state(mdata, cmd);
		break;
	case CXL_MBOX_OP_SET_PASSPHRASE:
		rc = mock_set_passphrase(mdata, cmd);
		break;
	case CXL_MBOX_OP_DISABLE_PASSPHRASE:
		rc = mock_disable_passphrase(mdata, cmd);
		break;
	case CXL_MBOX_OP_FREEZE_SECURITY:
		rc = mock_freeze_security(mdata, cmd);
		break;
	case CXL_MBOX_OP_UNLOCK:
		rc = mock_unlock_security(mdata, cmd);
		break;
	case CXL_MBOX_OP_PASSPHRASE_SECURE_ERASE:
		rc = mock_passphrase_secure_erase(mdata, cmd);
		break;
	case CXL_MBOX_OP_SET_SHUTDOWN_STATE:
		rc = mock_set_shutdown_state(mdata, cmd);
		break;
	case CXL_MBOX_OP_GET_POISON:
		rc = mock_get_poison(cxlds, cmd);
		break;
	case CXL_MBOX_OP_INJECT_POISON:
		rc = mock_inject_poison(cxlds, cmd);
		break;
	case CXL_MBOX_OP_CLEAR_POISON:
		rc = mock_clear_poison(cxlds, cmd);
		break;
	case CXL_MBOX_OP_GET_FW_INFO:
		rc = mock_fw_info(mdata, cmd);
		break;
	case CXL_MBOX_OP_TRANSFER_FW:
		rc = mock_transfer_fw(mdata, cmd);
		break;
	case CXL_MBOX_OP_ACTIVATE_FW:
		rc = mock_activate_fw(mdata, cmd);
		break;
	case CXL_MBOX_OP_GET_SUPPORTED_FEATURES:
		rc = mock_get_supported_features(mdata, cmd);
		break;
	case CXL_MBOX_OP_GET_FEATURE:
		rc = mock_get_feature(mdata, cmd);
		break;
	case CXL_MBOX_OP_SET_FEATURE:
		rc = mock_set_feature(mdata, cmd);
		break;
	default:
		break;
	}

	dev_dbg(dev, "opcode: %#x sz_in: %zd sz_out: %zd rc: %d\n", cmd->opcode,
		cmd->size_in, cmd->size_out, rc);

	return rc;
}

static void label_area_release(void *lsa)
{
	vfree(lsa);
}

static void fw_buf_release(void *buf)
{
	vfree(buf);
}

static bool is_rcd(struct platform_device *pdev)
{
	const struct platform_device_id *id = platform_get_device_id(pdev);

	return !!id->driver_data;
}

static ssize_t event_trigger_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	cxl_mock_event_trigger(dev);
	return count;
}
static DEVICE_ATTR_WO(event_trigger);

static int cxl_mock_mailbox_create(struct cxl_dev_state *cxlds)
{
	int rc;

	rc = cxl_mailbox_init(&cxlds->cxl_mbox, cxlds->dev);
	if (rc)
		return rc;

	return 0;
}

static void cxl_mock_test_feat_init(struct cxl_mockmem_data *mdata)
{
	mdata->test_feat.data = cpu_to_le32(0xdeadbeef);
}

static int cxl_mock_mem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cxl_memdev *cxlmd;
	struct cxl_memdev_state *mds;
	struct cxl_dev_state *cxlds;
	struct cxl_mockmem_data *mdata;
	struct cxl_mailbox *cxl_mbox;
	struct cxl_dpa_info range_info = { 0 };
	int rc;

	mdata = devm_kzalloc(dev, sizeof(*mdata), GFP_KERNEL);
	if (!mdata)
		return -ENOMEM;
	dev_set_drvdata(dev, mdata);

	mdata->lsa = vmalloc(LSA_SIZE);
	if (!mdata->lsa)
		return -ENOMEM;
	mdata->fw = vmalloc(FW_SIZE);
	if (!mdata->fw)
		return -ENOMEM;
	mdata->fw_slot = 2;

	rc = devm_add_action_or_reset(dev, label_area_release, mdata->lsa);
	if (rc)
		return rc;

	rc = devm_add_action_or_reset(dev, fw_buf_release, mdata->fw);
	if (rc)
		return rc;

	mds = cxl_memdev_state_create(dev);
	if (IS_ERR(mds))
		return PTR_ERR(mds);

	cxlds = &mds->cxlds;
	rc = cxl_mock_mailbox_create(cxlds);
	if (rc)
		return rc;

	cxl_mbox = &mds->cxlds.cxl_mbox;
	mdata->mds = mds;
	cxl_mbox->mbox_send = cxl_mock_mbox_send;
	cxl_mbox->payload_size = SZ_4K;
	mds->event.buf = (struct cxl_get_event_payload *) mdata->event_buf;
	INIT_DELAYED_WORK(&mds->security.poll_dwork, cxl_mockmem_sanitize_work);

	cxlds->serial = pdev->id + 1;
	if (is_rcd(pdev))
		cxlds->rcd = true;

	rc = cxl_enumerate_cmds(mds);
	if (rc)
		return rc;

	rc = cxl_poison_state_init(mds);
	if (rc)
		return rc;

	rc = cxl_set_timestamp(mds);
	if (rc)
		return rc;

	cxlds->media_ready = true;
	rc = cxl_dev_state_identify(mds);
	if (rc)
		return rc;

	rc = cxl_mem_dpa_fetch(mds, &range_info);
	if (rc)
		return rc;

	rc = cxl_dpa_setup(cxlds, &range_info);
	if (rc)
		return rc;

	rc = devm_cxl_setup_features(cxlds);
	if (rc)
		dev_dbg(dev, "No CXL Features discovered\n");

	cxl_mock_add_event_logs(&mdata->mes);

	cxlmd = devm_cxl_add_memdev(&pdev->dev, cxlds);
	if (IS_ERR(cxlmd))
		return PTR_ERR(cxlmd);

	rc = devm_cxl_setup_fw_upload(&pdev->dev, mds);
	if (rc)
		return rc;

	rc = devm_cxl_sanitize_setup_notifier(&pdev->dev, cxlmd);
	if (rc)
		return rc;

	rc = devm_cxl_setup_fwctl(&pdev->dev, cxlmd);
	if (rc)
		dev_dbg(dev, "No CXL FWCTL setup\n");

	cxl_mem_get_event_records(mds, CXLDEV_EVENT_STATUS_ALL);
	cxl_mock_test_feat_init(mdata);

	return 0;
}

static ssize_t security_lock_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cxl_mockmem_data *mdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n",
			  !!(mdata->security_state & CXL_PMEM_SEC_STATE_LOCKED));
}

static ssize_t security_lock_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cxl_mockmem_data *mdata = dev_get_drvdata(dev);
	u32 mask = CXL_PMEM_SEC_STATE_FROZEN | CXL_PMEM_SEC_STATE_USER_PLIMIT |
		   CXL_PMEM_SEC_STATE_MASTER_PLIMIT;
	int val;

	if (kstrtoint(buf, 0, &val) < 0)
		return -EINVAL;

	if (val == 1) {
		if (!(mdata->security_state & CXL_PMEM_SEC_STATE_USER_PASS_SET))
			return -ENXIO;
		mdata->security_state |= CXL_PMEM_SEC_STATE_LOCKED;
		mdata->security_state &= ~mask;
	} else {
		return -EINVAL;
	}
	return count;
}

static DEVICE_ATTR_RW(security_lock);

static ssize_t fw_buf_checksum_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cxl_mockmem_data *mdata = dev_get_drvdata(dev);
	u8 hash[SHA256_DIGEST_SIZE];

	sha256(mdata->fw, mdata->fw_size, hash);

	return sysfs_emit(buf, "%*phN\n", SHA256_DIGEST_SIZE, hash);
}

static DEVICE_ATTR_RO(fw_buf_checksum);

static ssize_t sanitize_timeout_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cxl_mockmem_data *mdata = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%lu\n", mdata->sanitize_timeout);
}

static ssize_t sanitize_timeout_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct cxl_mockmem_data *mdata = dev_get_drvdata(dev);
	unsigned long val;
	int rc;

	rc = kstrtoul(buf, 0, &val);
	if (rc)
		return rc;

	mdata->sanitize_timeout = val;

	return count;
}

static DEVICE_ATTR_RW(sanitize_timeout);

static struct attribute *cxl_mock_mem_attrs[] = {
	&dev_attr_security_lock.attr,
	&dev_attr_event_trigger.attr,
	&dev_attr_fw_buf_checksum.attr,
	&dev_attr_sanitize_timeout.attr,
	NULL
};
ATTRIBUTE_GROUPS(cxl_mock_mem);

static const struct platform_device_id cxl_mock_mem_ids[] = {
	{ .name = "cxl_mem", 0 },
	{ .name = "cxl_rcd", 1 },
	{ },
};
MODULE_DEVICE_TABLE(platform, cxl_mock_mem_ids);

static struct platform_driver cxl_mock_mem_driver = {
	.probe = cxl_mock_mem_probe,
	.id_table = cxl_mock_mem_ids,
	.driver = {
		.name = KBUILD_MODNAME,
		.dev_groups = cxl_mock_mem_groups,
		.groups = cxl_mock_mem_core_groups,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_platform_driver(cxl_mock_mem_driver);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("cxl_test: mem device mock module");
MODULE_IMPORT_NS("CXL");
