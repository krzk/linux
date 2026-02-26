// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Powercap Protocol
 *
 * Copyright (C) 2022-2026 ARM Ltd.
 */

#define pr_fmt(fmt) "SCMI Notifications POWERCAP - " fmt

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/scmi_protocol.h>
#include <linux/stddef.h>

#include <trace/events/scmi.h>

#include "protocols.h"
#include "notify.h"

/* Updated only after ALL the mandatory features for that version are merged */
#define SCMI_PROTOCOL_SUPPORTED_VERSION		0x30000

#define CPL0	0

enum scmi_powercap_protocol_cmd {
	POWERCAP_DOMAIN_ATTRIBUTES = 0x3,
	POWERCAP_CAP_GET = 0x4,
	POWERCAP_CAP_SET = 0x5,
	POWERCAP_PAI_GET = 0x6,
	POWERCAP_MAI_GET = POWERCAP_PAI_GET,
	POWERCAP_PAI_SET = 0x7,
	POWERCAP_MAI_SET = POWERCAP_PAI_SET,
	POWERCAP_DOMAIN_NAME_GET = 0x8,
	POWERCAP_MEASUREMENTS_GET = 0x9,
	POWERCAP_CAP_NOTIFY = 0xa,
	POWERCAP_MEASUREMENTS_NOTIFY = 0xb,
	POWERCAP_DESCRIBE_FASTCHANNEL = 0xc,
	POWERCAP_CPC_ATTRIBUTES = 0xd,
	POWERCAP_CAI_GET = 0xe,
	POWERCAP_CAI_SET = 0xf,
};

enum {
	POWERCAP_FC_CAP,
	POWERCAP_FC_XAI,
	POWERCAP_FC_MAI,
	POWERCAP_FC_MEASUREMENT,
	POWERCAP_FC_MAX
};

struct scmi_msg_resp_powercap_domain_attributes {
	__le32 attributes;
#define SUPPORTS_POWERCAP_CAP_CHANGE_NOTIFY(x)		((x) & BIT(31))
#define SUPPORTS_POWERCAP_MEASUREMENTS_CHANGE_NOTIFY(x)	((x) & BIT(30))
#define SUPPORTS_ASYNC_POWERCAP_CAP_SET(x)		((x) & BIT(29))
#define SUPPORTS_EXTENDED_NAMES(x)			((x) & BIT(28))
#define SUPPORTS_POWERCAP_CAP_CONFIGURATION(x)		((x) & BIT(27))
#define SUPPORTS_POWERCAP_MONITORING(x)			((x) & BIT(26))
#define SUPPORTS_POWERCAP_PAI_CONFIGURATION(x)		((x) & BIT(25))
#define SUPPORTS_POWERCAP_FASTCHANNELS(x)		((x) & BIT(22))
#define POWERCAP_POWER_UNIT(x)				\
		(FIELD_GET(GENMASK(24, 23), (x)))
#define	SUPPORTS_POWER_UNITS_MW(x)			\
		(POWERCAP_POWER_UNIT(x) == 0x2)
#define	SUPPORTS_POWER_UNITS_UW(x)			\
		(POWERCAP_POWER_UNIT(x) == 0x1)
	u8 name[SCMI_SHORT_NAME_MAX_SIZE];
	__le32 min_pai;
	__le32 max_pai;
	__le32 pai_step;
	__le32 min_power_cap;
	__le32 max_power_cap;
	__le32 power_cap_step;
	__le32 sustainable_power;
	__le32 accuracy;
	__le32 parent_id;
};

struct scmi_msg_resp_powercap_domain_attributes_v3 {
	__le32 attributes;
#define SUPPORTS_POWERCAP_MAI_CONFIGURATION(x)		((x) & BIT(25))
#define SUPPORTS_POWERCAP_FASTCHANNELS(x)		((x) & BIT(22))
#define SUPPORTS_POWERCAP_CAP_CHANGE_NOTIFY_V3(x)	((x) & BIT(21))
#define SUPPORTS_POWERCAP_CAI_CONFIGURATION(x)		((x) & BIT(20))
	u8 name[SCMI_SHORT_NAME_MAX_SIZE];
	__le32 min_mai;
	__le32 max_mai;
	__le32 mai_step;
	__le32 min_power_cap;
	__le32 max_power_cap;
	__le32 power_cap_step;
	__le32 sustainable_power;
	__le32 accuracy;
	__le32 parent_id;
	__le32 min_cai;
	__le32 max_cai;
	__le32 cai_step;
};

struct scmi_msg_powercap_cap_or_cai_get_v3 {
	__le32 domain_id;
	__le32 cpli;
};

struct scmi_msg_powercap_cap_or_pai_set {
	__le32 domain_id;
	__le32 flags;
#define CAP_SET_ASYNC		BIT(1)
#define CAP_SET_IGNORE_DRESP	BIT(0)
	__le32 value;
};

struct scmi_msg_powercap_cap_set_v3 {
	__le32 domain_id;
	__le32 cpli;
	__le32 flags;
	__le32 power_cap;
};

struct scmi_msg_powercap_cai_set {
	__le32 domain_id;
	__le32 flags;
	__le32 cai;
	__le32 cpli;
};

struct scmi_msg_resp_powercap_cap_set_complete {
	__le32 domain_id;
	__le32 power_cap;
};

struct scmi_msg_resp_powercap_cap_set_complete_v3 {
	__le32 domain_id;
	__le32 power_cap;
	__le32 cpli;
};

struct scmi_msg_resp_powercap_meas_get {
	__le32 power;
	__le32 pai;
};

struct scmi_msg_powercap_notify_cap {
	__le32 domain;
	__le32 notify_enable;
};

struct scmi_msg_powercap_notify_thresh {
	__le32 domain;
	__le32 notify_enable;
	__le32 power_thresh_low;
	__le32 power_thresh_high;
};

struct scmi_powercap_cap_changed_notify_payld {
	__le32 agent_id;
	__le32 domain_id;
	__le32 power_cap;
	__le32 avg_ivl;
	__le32 cpli;
};

struct scmi_powercap_meas_changed_notify_payld {
	__le32 agent_id;
	__le32 domain_id;
	__le32 power;
	__le32 mai;
};

struct scmi_msg_powercap_cpc {
	__le32 domain_id;
	__le32 desc_index;
};

struct scmi_msg_resp_powercap_cpc {
	__le32 num_cpl;
#define NUM_RETURNED(n)		(le32_get_bits((n), GENMASK(15, 0)))
#define NUM_REMAINING(n)	(le32_get_bits((n), GENMASK(31, 16)))
	struct {
		__le32 cpli;
		__le32 flags;
		__le32 min_power_cap;
		__le32 max_power_cap;
		__le32 power_cap_step;
		__le32 min_cai;
		__le32 max_cai;
		__le32 cai_step;
		u8 name[SCMI_SHORT_NAME_MAX_SIZE];
	} desc[];
};

struct scmi_cpls_priv {
	u32 domain_id;
	struct scmi_powercap_cpl_info *cpli;
};

struct scmi_powercap_state {
	bool enabled;
	u32 last_pcap;
	bool meas_notif_enabled;
	u64 thresholds;
#define THRESH_LOW(p, id)				\
	(lower_32_bits((p)->states[(id)].thresholds))
#define THRESH_HIGH(p, id)				\
	(upper_32_bits((p)->states[(id)].thresholds))
};

struct powercap_info {
	int num_domains;
	bool notify_cap_cmd;
	bool notify_measurements_cmd;
	struct scmi_powercap_state *states;
	struct scmi_powercap_info *powercaps;
	int (*xfer_cap_get)(const struct scmi_protocol_handle *ph,
			    u32 domain_id, u32 cpl_id, u32 *power_cap);
	int (*xfer_cap_set)(const struct scmi_protocol_handle *ph,
			    const struct scmi_powercap_info *pc,
			    u32 cpl_id, u32 power_cap, bool ignore_dresp);
	int (*xfer_avg_ivl_get)(const struct scmi_protocol_handle *ph,
				u32 domain_id, u32 cpl_id, u32 *ivl);
	int (*xfer_avg_ivl_set)(const struct scmi_protocol_handle *ph,
				u32 domain_id, u32 cpl_id, u32 ivl);
};

static enum scmi_powercap_protocol_cmd evt_2_cmd[] = {
	POWERCAP_CAP_NOTIFY,
	POWERCAP_MEASUREMENTS_NOTIFY,
};

static int scmi_powercap_notify(const struct scmi_protocol_handle *ph,
				u32 domain, int message_id, bool enable);

static int
scmi_powercap_attributes_get(const struct scmi_protocol_handle *ph,
			     struct powercap_info *pi)
{
	int ret;
	struct scmi_xfer *t;

	ret = ph->xops->xfer_get_init(ph, PROTOCOL_ATTRIBUTES, 0,
				      sizeof(u32), &t);
	if (ret)
		return ret;

	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		u32 attributes;

		attributes = get_unaligned_le32(t->rx.buf);
		pi->num_domains = FIELD_GET(GENMASK(15, 0), attributes);
	}

	ph->xops->xfer_put(ph, t);

	if (!ret) {
		if (!ph->hops->protocol_msg_check(ph,
						  POWERCAP_CAP_NOTIFY, NULL))
			pi->notify_cap_cmd = true;

		if (!ph->hops->protocol_msg_check(ph,
						  POWERCAP_MEASUREMENTS_NOTIFY,
						  NULL))
			pi->notify_measurements_cmd = true;
	}

	return ret;
}

static inline int
scmi_powercap_validate(unsigned int min_val, unsigned int max_val,
		       unsigned int step_val, bool configurable)
{
	if (!min_val || !max_val)
		return -EPROTO;

	if ((configurable && min_val == max_val) ||
	    (!configurable && min_val != max_val))
		return -EPROTO;

	if (min_val != max_val && !step_val)
		return -EPROTO;

	return 0;
}

static void iter_powercap_cpls_prepare_message(void *message,
					       unsigned int desc_index,
					       const void *priv)
{
	struct scmi_msg_powercap_cpc *msg = message;
	const struct scmi_cpls_priv *p = priv;

	msg->domain_id = cpu_to_le32(p->domain_id);
	msg->desc_index = cpu_to_le32(desc_index);
}

static int iter_powercap_cpls_update_state(struct scmi_iterator_state *st,
					   const void *response, void *priv)
{
	const struct scmi_msg_resp_powercap_cpc *r = response;

	st->num_returned = NUM_RETURNED(r->num_cpl);
	st->num_remaining = NUM_REMAINING(r->num_cpl);

	return 0;
}

static int
iter_powercap_cpls_process_response(const struct scmi_protocol_handle *ph,
				    const void *response,
				    struct scmi_iterator_state *st, void *priv)
{
	const struct scmi_msg_resp_powercap_cpc *r = response;
	struct scmi_cpls_priv *p = priv;
	struct scmi_powercap_cpl_info *cpl;

	cpl = &p->cpli[st->desc_index + st->loop_idx];

	cpl->id = le32_to_cpu(r->desc[st->loop_idx].cpli);
	cpl->cap_config = le32_to_cpu(r->desc[st->loop_idx].flags) & BIT(0);

	cpl->min_power_cap = le32_to_cpu(r->desc[st->loop_idx].min_power_cap);
	cpl->max_power_cap = le32_to_cpu(r->desc[st->loop_idx].max_power_cap);
	cpl->power_cap_step = le32_to_cpu(r->desc[st->loop_idx].power_cap_step);
	if (!cpl->power_cap_step && cpl->min_power_cap != cpl->max_power_cap)
		return -EINVAL;

	cpl->min_avg_ivl = le32_to_cpu(r->desc[st->loop_idx].min_cai);
	cpl->max_avg_ivl = le32_to_cpu(r->desc[st->loop_idx].max_cai);
	cpl->avg_ivl_step = le32_to_cpu(r->desc[st->loop_idx].cai_step);
	if (!cpl->avg_ivl_step && cpl->min_avg_ivl != cpl->max_avg_ivl)
		return -EINVAL;

	cpl->avg_ivl_config = cpl->min_avg_ivl != cpl->max_avg_ivl;

	strscpy(cpl->name, r->desc[st->loop_idx].name, SCMI_SHORT_NAME_MAX_SIZE);

	return 0;
}

static int scmi_powercap_cpls_enumerate(const struct scmi_protocol_handle *ph,
					struct scmi_powercap_info *dom_info)
{
	void *iter;
	struct scmi_iterator_ops ops = {
		.prepare_message = iter_powercap_cpls_prepare_message,
		.update_state = iter_powercap_cpls_update_state,
		.process_response = iter_powercap_cpls_process_response,
	};
	struct scmi_cpls_priv cpriv = {
		.domain_id = dom_info->id,
		.cpli = dom_info->cpli,
	};

	iter = ph->hops->iter_response_init(ph, &ops, dom_info->num_cpli,
					    POWERCAP_CPC_ATTRIBUTES,
					    sizeof(struct scmi_msg_powercap_cpc),
					    &cpriv);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	return ph->hops->iter_response_run(iter);
}

static int
scmi_powercap_domain_attrs_process(const struct scmi_protocol_handle *ph,
				   struct powercap_info *pinfo,
				   struct scmi_powercap_info *dom_info, void *r)
{
	struct scmi_msg_resp_powercap_domain_attributes *resp = r;
	u32 flags = le32_to_cpu(resp->attributes);
	bool cap_config;
	int ret;

	cap_config = SUPPORTS_POWERCAP_CAP_CONFIGURATION(flags);
	if (PROTOCOL_REV_MAJOR(ph->version) < 0x3) {
		dom_info->num_cpli = 1;
	} else {
		dom_info->num_cpli = le32_get_bits(resp->attributes,
						   GENMASK(18, 15));
		if (cap_config && !dom_info->num_cpli)
			return -EINVAL;
	}

	dom_info->cpli = devm_kcalloc(ph->dev, dom_info->num_cpli,
				      sizeof(*dom_info->cpli), GFP_KERNEL);
	if (!dom_info->cpli)
		return -ENOMEM;

	if (pinfo->notify_cap_cmd) {
		if (PROTOCOL_REV_MAJOR(ph->version) < 0x3)
			dom_info->notify_powercap_cap_change =
				SUPPORTS_POWERCAP_CAP_CHANGE_NOTIFY(flags);
		else
			dom_info->notify_powercap_cap_change =
				SUPPORTS_POWERCAP_CAP_CHANGE_NOTIFY_V3(flags);
	}

	if (pinfo->notify_measurements_cmd)
		dom_info->notify_powercap_measurement_change =
			SUPPORTS_POWERCAP_MEASUREMENTS_CHANGE_NOTIFY(flags);

	dom_info->extended_names = SUPPORTS_EXTENDED_NAMES(flags);

	dom_info->async_powercap_cap_set =
		SUPPORTS_ASYNC_POWERCAP_CAP_SET(flags);

	dom_info->powercap_monitoring =
		SUPPORTS_POWERCAP_MONITORING(flags);
	dom_info->powercap_scale_mw =
		SUPPORTS_POWER_UNITS_MW(flags);
	dom_info->powercap_scale_uw =
		SUPPORTS_POWER_UNITS_UW(flags);
	dom_info->fastchannels =
		SUPPORTS_POWERCAP_FASTCHANNELS(flags);

	strscpy(dom_info->name, resp->name, SCMI_SHORT_NAME_MAX_SIZE);

	dom_info->sustainable_power =
		le32_to_cpu(resp->sustainable_power);
	dom_info->accuracy = le32_to_cpu(resp->accuracy);

	dom_info->parent_id = le32_to_cpu(resp->parent_id);
	if (dom_info->parent_id != SCMI_POWERCAP_ROOT_ZONE_ID &&
	    (dom_info->parent_id >= pinfo->num_domains ||
	     dom_info->parent_id == dom_info->id)) {
		dev_err(ph->dev,
			"Platform reported inconsistent parent ID for domain %d - %s\n",
			dom_info->id, dom_info->name);
		return -ENODEV;
	}

	dom_info->cpli[0].id = CPL0;
	if (PROTOCOL_REV_MAJOR(ph->version) < 0x3)
		dom_info->cpli[0].avg_ivl_config =
			SUPPORTS_POWERCAP_PAI_CONFIGURATION(flags);
	else
		dom_info->cpli[0].avg_ivl_config =
			SUPPORTS_POWERCAP_CAI_CONFIGURATION(flags);

	if (PROTOCOL_REV_MAJOR(ph->version) < 0x3) {
		dom_info->cpli[0].min_avg_ivl = le32_to_cpu(resp->min_pai);
		dom_info->cpli[0].max_avg_ivl = le32_to_cpu(resp->max_pai);
		dom_info->cpli[0].avg_ivl_step = le32_to_cpu(resp->pai_step);
	} else {
		struct scmi_msg_resp_powercap_domain_attributes_v3 *resp = r;

		dom_info->cpli[0].min_avg_ivl = le32_to_cpu(resp->min_cai);
		dom_info->cpli[0].max_avg_ivl = le32_to_cpu(resp->max_cai);
		dom_info->cpli[0].avg_ivl_step = le32_to_cpu(resp->cai_step);
	}

	ret = scmi_powercap_validate(dom_info->cpli[0].min_avg_ivl,
				     dom_info->cpli[0].max_avg_ivl,
				     dom_info->cpli[0].avg_ivl_step,
				     dom_info->cpli[0].avg_ivl_config);
	if (ret) {
		dev_err(ph->dev,
			"Platform reported inconsistent PAI config for domain %d - %s\n",
			dom_info->id, dom_info->name);
		return ret;
	}

	dom_info->cpli[0].cap_config = cap_config;
	dom_info->cpli[0].min_power_cap = le32_to_cpu(resp->min_power_cap);
	dom_info->cpli[0].max_power_cap = le32_to_cpu(resp->max_power_cap);
	dom_info->cpli[0].power_cap_step = le32_to_cpu(resp->power_cap_step);
	ret = scmi_powercap_validate(dom_info->cpli[0].min_power_cap,
				     dom_info->cpli[0].max_power_cap,
				     dom_info->cpli[0].power_cap_step,
				     dom_info->cpli[0].cap_config);
	if (ret) {
		dev_err(ph->dev,
			"Platform reported inconsistent CAP config for domain %d - %s\n",
			dom_info->id, dom_info->name);
		return ret;
	}
	/* Just using same short name */
	strscpy(dom_info->cpli[0].name, dom_info->name, SCMI_SHORT_NAME_MAX_SIZE);

	return 0;
}

static int
scmi_powercap_domain_attributes_get(const struct scmi_protocol_handle *ph,
				    struct powercap_info *pinfo,
				    struct scmi_powercap_info *dom_info)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_powercap_domain_attributes *resp;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_DOMAIN_ATTRIBUTES,
				      sizeof(dom_info->id), 0, &t);
	if (ret)
		return ret;

	put_unaligned_le32(dom_info->id, t->tx.buf);
	resp = t->rx.buf;

	ret = ph->xops->do_xfer(ph, t);
	if (!ret)
		ret = scmi_powercap_domain_attrs_process(ph, pinfo, dom_info, resp);

	ph->xops->xfer_put(ph, t);

	/*
	 * If supported overwrite short name with the extended one;
	 * on error just carry on and use already provided short name.
	 */
	if (!ret && dom_info->extended_names)
		ph->hops->extended_name_get(ph, POWERCAP_DOMAIN_NAME_GET,
					    dom_info->id, NULL, dom_info->name,
					    SCMI_MAX_STR_SIZE);

	/* When protocol version > 0x3 there can possibly be more than 1 CPLs */
	if (!ret && dom_info->num_cpli > 1)
		ret = scmi_powercap_cpls_enumerate(ph, dom_info);

	return ret;
}

static int scmi_powercap_num_domains_get(const struct scmi_protocol_handle *ph)
{
	struct powercap_info *pi = ph->get_priv(ph);

	return pi->num_domains;
}

static const struct scmi_powercap_info *
scmi_powercap_dom_info_get(const struct scmi_protocol_handle *ph, u32 domain_id)
{
	struct powercap_info *pi = ph->get_priv(ph);

	if (domain_id >= pi->num_domains)
		return NULL;

	return pi->powercaps + domain_id;
}

static int scmi_powercap_xfer_cap_get(const struct scmi_protocol_handle *ph,
				      u32 domain_id, u32 cpl_id, u32 *power_cap)
{
	int ret;
	struct scmi_xfer *t;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_CAP_GET, sizeof(u32),
				      sizeof(u32), &t);
	if (ret)
		return ret;

	put_unaligned_le32(domain_id, t->tx.buf);

	ret = ph->xops->do_xfer(ph, t);
	if (!ret)
		*power_cap = get_unaligned_le32(t->rx.buf);

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int scmi_powercap_xfer_cap_get_v3(const struct scmi_protocol_handle *ph,
					 u32 domain_id, u32 cpl_id,
					 u32 *power_cap)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_powercap_cap_or_cai_get_v3 *msg;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_CAP_GET, sizeof(*msg),
				      sizeof(u32), &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->domain_id = cpu_to_le32(domain_id);
	msg->cpli = cpu_to_le32(cpl_id);

	ret = ph->xops->do_xfer(ph, t);
	if (!ret)
		*power_cap = get_unaligned_le32(t->rx.buf);

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int __scmi_powercap_cap_get(const struct scmi_protocol_handle *ph,
				   const struct scmi_powercap_info *dom,
				   u32 cpl_id, u32 *power_cap)
{
	struct powercap_info *pi = ph->get_priv(ph);

	if (dom->cpli[cpl_id].fc_info &&
	    dom->cpli[cpl_id].fc_info[POWERCAP_FC_CAP].get_addr) {
		*power_cap = ioread32(dom->cpli[cpl_id].fc_info[POWERCAP_FC_CAP].get_addr);
		trace_scmi_fc_call(SCMI_PROTOCOL_POWERCAP, POWERCAP_CAP_GET,
				   dom->id, *power_cap, 0);
		return 0;
	}

	return pi->xfer_cap_get(ph, dom->id, cpl_id, power_cap);
}

static int scmi_powercap_cap_get(const struct scmi_protocol_handle *ph,
				 u32 domain_id, u32 cpl_id, u32 *power_cap)
{
	const struct scmi_powercap_info *dom;

	if (!power_cap)
		return -EINVAL;

	dom = scmi_powercap_dom_info_get(ph, domain_id);
	if (!dom)
		return -EINVAL;

	return __scmi_powercap_cap_get(ph, dom, cpl_id, power_cap);
}

static int scmi_powercap_xfer_cap_set(const struct scmi_protocol_handle *ph,
				      const struct scmi_powercap_info *pc,
				      u32 cpl_id, u32 power_cap,
				      bool ignore_dresp)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_powercap_cap_or_pai_set *msg;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_CAP_SET,
				      sizeof(*msg), 0, &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->domain_id = cpu_to_le32(pc->id);
	msg->flags =
		cpu_to_le32(FIELD_PREP(CAP_SET_ASYNC, pc->async_powercap_cap_set) |
			    FIELD_PREP(CAP_SET_IGNORE_DRESP, ignore_dresp));
	msg->value = cpu_to_le32(power_cap);

	if (!pc->async_powercap_cap_set || ignore_dresp) {
		ret = ph->xops->do_xfer(ph, t);
	} else {
		ret = ph->xops->do_xfer_with_response(ph, t);
		if (!ret) {
			struct scmi_msg_resp_powercap_cap_set_complete *resp;

			resp = t->rx.buf;
			if (le32_to_cpu(resp->domain_id) == pc->id)
				dev_dbg(ph->dev,
					"Powercap ID %d CAP set async to %u\n",
					pc->id,
					get_unaligned_le32(&resp->power_cap));
			else
				ret = -EPROTO;
		}
	}

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_powercap_xfer_cap_set_v3(const struct scmi_protocol_handle *ph,
					 const struct scmi_powercap_info *pc,
					 u32 cpl_id, u32 power_cap,
					 bool ignore_dresp)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_powercap_cap_set_v3 *msg;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_CAP_SET,
				      sizeof(*msg), 0, &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->domain_id = cpu_to_le32(pc->id);
	msg->cpli = cpu_to_le32(cpl_id);
	msg->flags =
		cpu_to_le32(FIELD_PREP(CAP_SET_ASYNC, pc->async_powercap_cap_set) |
			    FIELD_PREP(CAP_SET_IGNORE_DRESP, ignore_dresp));
	msg->power_cap = cpu_to_le32(power_cap);

	if (!pc->async_powercap_cap_set || ignore_dresp) {
		ret = ph->xops->do_xfer(ph, t);
	} else {
		ret = ph->xops->do_xfer_with_response(ph, t);
		if (!ret) {
			struct scmi_msg_resp_powercap_cap_set_complete_v3 *resp;

			resp = t->rx.buf;
			if (le32_to_cpu(resp->domain_id) == pc->id &&
			    le32_to_cpu(resp->cpli) == pc->cpli[cpl_id].id)
				dev_dbg(ph->dev,
					"Powercap ID:%d/CPLI:%d CAP set async to %u\n",
					pc->id, cpl_id,
					get_unaligned_le32(&resp->power_cap));
			else
				ret = -EPROTO;
		}
	}

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int __scmi_powercap_cap_set(const struct scmi_protocol_handle *ph,
				   struct powercap_info *pi, u32 domain_id,
				   u32 cpl_id, u32 power_cap, bool ignore_dresp)
{
	int ret = -EINVAL;
	const struct scmi_powercap_info *pc;

	pc = scmi_powercap_dom_info_get(ph, domain_id);
	if (!pc || !pc->cpli[cpl_id].cap_config)
		return ret;

	if (power_cap &&
	    (power_cap < pc->cpli[cpl_id].min_power_cap ||
	     power_cap > pc->cpli[cpl_id].max_power_cap))
		return ret;

	if (pc->cpli[cpl_id].fc_info &&
	    pc->cpli[cpl_id].fc_info[POWERCAP_FC_CAP].set_addr) {
		struct scmi_fc_info *fci = &pc->cpli[cpl_id].fc_info[POWERCAP_FC_CAP];

		iowrite32(power_cap, fci->set_addr);
		ph->hops->fastchannel_db_ring(fci->set_db);
		trace_scmi_fc_call(SCMI_PROTOCOL_POWERCAP, POWERCAP_CAP_SET,
				   domain_id, power_cap, 0);
		ret = 0;
	} else {
		ret = pi->xfer_cap_set(ph, pc, cpl_id, power_cap, ignore_dresp);
	}

	/* Save the last explicitly set non-zero powercap value for CPL0 */
	if (PROTOCOL_REV_MAJOR(ph->version) >= 0x2 && !ret &&
	    cpl_id == CPL0 && power_cap)
		pi->states[domain_id].last_pcap = power_cap;

	return ret;
}

static int scmi_powercap_cap_set(const struct scmi_protocol_handle *ph,
				 u32 domain_id, u32 cpl_id, u32 power_cap,
				 bool ignore_dresp)
{
	struct powercap_info *pi = ph->get_priv(ph);

	/*
	 * Disallow zero as a possible explicitly requested powercap:
	 * there are enable/disable operations for this.
	 */
	if (!power_cap)
		return -EINVAL;

	/* Just log the last set request on CPL0 on a disabled domain */
	if (PROTOCOL_REV_MAJOR(ph->version) >= 0x2 && cpl_id == CPL0 &&
	    !pi->states[domain_id].enabled) {
		pi->states[domain_id].last_pcap = power_cap;
		return 0;
	}

	return __scmi_powercap_cap_set(ph, pi, domain_id, cpl_id,
				       power_cap, ignore_dresp);
}

static int
scmi_powercap_xfer_avg_interval_get(const struct scmi_protocol_handle *ph,
				    u32 domain_id, u32 cpl_id, u32 *ivl)
{
	int ret;
	struct scmi_xfer *t;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_PAI_GET, sizeof(u32),
				      sizeof(u32), &t);
	if (ret)
		return ret;

	put_unaligned_le32(domain_id, t->tx.buf);
	ret = ph->xops->do_xfer(ph, t);
	if (!ret)
		*ivl = get_unaligned_le32(t->rx.buf);

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int
scmi_powercap_xfer_avg_interval_get_v3(const struct scmi_protocol_handle *ph,
				       u32 domain_id, u32 cpl_id, u32 *ivl)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_powercap_cap_or_cai_get_v3 *msg;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_CAI_GET, sizeof(*msg),
				      sizeof(u32), &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->domain_id = cpu_to_le32(domain_id);
	msg->cpli = cpu_to_le32(cpl_id);

	ret = ph->xops->do_xfer(ph, t);
	if (!ret)
		*ivl = get_unaligned_le32(t->rx.buf);

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int scmi_powercap_avg_interval_get(const struct scmi_protocol_handle *ph,
					  u32 domain_id, u32 cpl_id, u32 *val)
{
	struct scmi_powercap_info *dom;
	struct powercap_info *pi = ph->get_priv(ph);

	if (!val || domain_id >= pi->num_domains)
		return -EINVAL;

	dom = pi->powercaps + domain_id;
	if (cpl_id >= dom->num_cpli)
		return -EINVAL;

	if (dom->cpli[cpl_id].fc_info &&
	    dom->cpli[cpl_id].fc_info[POWERCAP_FC_XAI].get_addr) {
		int trace_cmd = (PROTOCOL_REV_MAJOR(ph->version) < 0x3) ?
			POWERCAP_PAI_GET : POWERCAP_CAI_GET;

		*val = ioread32(dom->cpli[cpl_id].fc_info[POWERCAP_FC_XAI].get_addr);
		trace_scmi_fc_call(SCMI_PROTOCOL_POWERCAP, trace_cmd, domain_id, *val, 0);
		return 0;
	}

	return pi->xfer_avg_ivl_get(ph, domain_id, cpl_id, val);
}

static int
scmi_powercap_xfer_avg_interval_set(const struct scmi_protocol_handle *ph,
				    u32 domain_id, u32 cpl_id, u32 ivl)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_powercap_cap_or_pai_set *msg;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_PAI_SET, sizeof(*msg), 0, &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->domain_id = cpu_to_le32(domain_id);
	msg->flags = cpu_to_le32(0);
	msg->value = cpu_to_le32(ivl);

	ret = ph->xops->do_xfer(ph, t);

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int
scmi_powercap_xfer_avg_interval_set_v3(const struct scmi_protocol_handle *ph,
				       u32 domain_id, u32 cpl_id, u32 ivl)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_powercap_cai_set *msg;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_CAI_SET, sizeof(*msg), 0, &t);
	if (ret)
		return ret;

	msg = t->tx.buf;
	msg->domain_id = cpu_to_le32(domain_id);
	msg->flags = cpu_to_le32(0);
	msg->cai = cpu_to_le32(ivl);
	msg->cpli = cpu_to_le32(cpl_id);

	ret = ph->xops->do_xfer(ph, t);

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_powercap_avg_interval_set(const struct scmi_protocol_handle *ph,
					  u32 domain_id, u32 cpl_id, u32 ivl)
{
	const struct scmi_powercap_info *pc;
	struct powercap_info *pi = ph->get_priv(ph);

	pc = scmi_powercap_dom_info_get(ph, domain_id);
	if (!pc || cpl_id >= pc->num_cpli || !pc->cpli[cpl_id].avg_ivl_config ||
	    !ivl || ivl < pc->cpli[cpl_id].min_avg_ivl ||
	    ivl > pc->cpli[cpl_id].max_avg_ivl)
		return -EINVAL;

	/* Note that fc_info descriptors for any unsupported FC will be NULL */
	if (pc->cpli[cpl_id].fc_info &&
	    pc->cpli[cpl_id].fc_info[POWERCAP_FC_XAI].set_addr) {
		int trace_cmd = (PROTOCOL_REV_MAJOR(ph->version) < 0x3) ?
			POWERCAP_PAI_SET : POWERCAP_CAI_SET;
		struct scmi_fc_info *fci = &pc->cpli[cpl_id].fc_info[POWERCAP_FC_XAI];

		trace_scmi_fc_call(SCMI_PROTOCOL_POWERCAP, trace_cmd, domain_id, ivl, 0);
		iowrite32(ivl, fci->set_addr);
		ph->hops->fastchannel_db_ring(fci->set_db);
		return 0;
	}

	return pi->xfer_avg_ivl_set(ph, domain_id, cpl_id, ivl);
}

static int
scmi_powercap_xfer_measurements_get(const struct scmi_protocol_handle *ph,
				    const struct scmi_powercap_info *pc,
				    u32 *avg_power, u32 *avg_ivl)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_powercap_meas_get *resp;

	ret = ph->xops->xfer_get_init(ph, POWERCAP_MEASUREMENTS_GET,
				      sizeof(u32), sizeof(*resp), &t);
	if (ret)
		return ret;

	resp = t->rx.buf;
	put_unaligned_le32(pc->id, t->tx.buf);
	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		*avg_power = le32_to_cpu(resp->power);
		*avg_ivl = le32_to_cpu(resp->pai);
	}

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_powercap_measurements_get(const struct scmi_protocol_handle *ph,
					  u32 domain_id, u32 *avg_power,
					  u32 *avg_ivl)
{
	const struct scmi_powercap_info *pc;
	struct scmi_fc_info *fci;

	pc = scmi_powercap_dom_info_get(ph, domain_id);
	if (!pc || !pc->powercap_monitoring || !avg_ivl || !avg_power)
		return -EINVAL;

	/* Note that fc_info descriptors for any unsupported FC will be NULL */
	fci = pc->cpli[CPL0].fc_info;
	if (fci && fci[POWERCAP_FC_MEASUREMENT].get_addr) {
		*avg_power = ioread32(fci[POWERCAP_FC_MEASUREMENT].get_addr);
		/* See SCMIv4.0 3.10.2 - Payload is 32bit ONLY avg_power */
		*avg_ivl = 0;
		trace_scmi_fc_call(SCMI_PROTOCOL_POWERCAP, POWERCAP_MEASUREMENTS_GET,
				   pc->id, *avg_power, *avg_ivl);
		return 0;
	}

	return scmi_powercap_xfer_measurements_get(ph, pc, avg_power, avg_ivl);
}

static int
scmi_powercap_measurements_threshold_get(const struct scmi_protocol_handle *ph,
					 u32 domain_id, u32 *power_thresh_low,
					 u32 *power_thresh_high)
{
	struct powercap_info *pi = ph->get_priv(ph);

	if (!power_thresh_low || !power_thresh_high ||
	    domain_id >= pi->num_domains)
		return -EINVAL;

	*power_thresh_low =  THRESH_LOW(pi, domain_id);
	*power_thresh_high = THRESH_HIGH(pi, domain_id);

	return 0;
}

static int
scmi_powercap_measurements_threshold_set(const struct scmi_protocol_handle *ph,
					 u32 domain_id, u32 power_thresh_low,
					 u32 power_thresh_high)
{
	int ret = 0;
	struct powercap_info *pi = ph->get_priv(ph);

	if (domain_id >= pi->num_domains ||
	    power_thresh_low > power_thresh_high)
		return -EINVAL;

	/* Anything to do ? */
	if (THRESH_LOW(pi, domain_id) == power_thresh_low &&
	    THRESH_HIGH(pi, domain_id) == power_thresh_high)
		return ret;

	pi->states[domain_id].thresholds =
		(FIELD_PREP(GENMASK_ULL(31, 0), power_thresh_low) |
		 FIELD_PREP(GENMASK_ULL(63, 32), power_thresh_high));

	/* Update thresholds if notification already enabled */
	if (pi->states[domain_id].meas_notif_enabled)
		ret = scmi_powercap_notify(ph, domain_id,
					   POWERCAP_MEASUREMENTS_NOTIFY,
					   true);

	return ret;
}

static int scmi_powercap_cap_enable_set(const struct scmi_protocol_handle *ph,
					u32 domain_id, bool enable)
{
	int ret;
	u32 power_cap;
	struct powercap_info *pi = ph->get_priv(ph);

	if (PROTOCOL_REV_MAJOR(ph->version) < 0x2)
		return -EINVAL;

	if (enable == pi->states[domain_id].enabled)
		return 0;

	if (enable) {
		/* Cannot enable with a zero powercap. */
		if (!pi->states[domain_id].last_pcap)
			return -EINVAL;

		ret = __scmi_powercap_cap_set(ph, pi, domain_id, CPL0,
					      pi->states[domain_id].last_pcap,
					      true);
	} else {
		ret = __scmi_powercap_cap_set(ph, pi, domain_id, CPL0, 0, true);
	}

	if (ret)
		return ret;

	/*
	 * Update our internal state to reflect final platform state: the SCMI
	 * server could have ignored a disable request and kept enforcing some
	 * powercap limit requested by other agents.
	 */
	ret = scmi_powercap_cap_get(ph, domain_id, CPL0, &power_cap);
	if (!ret)
		pi->states[domain_id].enabled = !!power_cap;

	return ret;
}

static int scmi_powercap_cap_enable_get(const struct scmi_protocol_handle *ph,
					u32 domain_id, bool *enable)
{
	int ret;
	u32 power_cap;
	struct powercap_info *pi = ph->get_priv(ph);

	*enable = true;
	if (PROTOCOL_REV_MAJOR(ph->version) < 0x2)
		return 0;

	/*
	 * Report always real platform state; platform could have ignored
	 * a previous disable request. Default true on any error.
	 */
	ret = scmi_powercap_cap_get(ph, domain_id, CPL0, &power_cap);
	if (!ret)
		*enable = !!power_cap;

	/* Update internal state with current real platform state */
	pi->states[domain_id].enabled = *enable;

	return 0;
}

static const struct scmi_powercap_proto_ops powercap_proto_ops = {
	.num_domains_get = scmi_powercap_num_domains_get,
	.info_get = scmi_powercap_dom_info_get,
	.cap_get = scmi_powercap_cap_get,
	.cap_set = scmi_powercap_cap_set,
	.cap_enable_set = scmi_powercap_cap_enable_set,
	.cap_enable_get = scmi_powercap_cap_enable_get,
	.avg_interval_get = scmi_powercap_avg_interval_get,
	.avg_interval_set = scmi_powercap_avg_interval_set,
	.measurements_get = scmi_powercap_measurements_get,
	.measurements_threshold_set = scmi_powercap_measurements_threshold_set,
	.measurements_threshold_get = scmi_powercap_measurements_threshold_get,
};

static void scmi_powercap_domain_init_fc(const struct scmi_protocol_handle *ph,
					 struct scmi_powercap_info *dom_info)
{
	for (int id = 0; id < dom_info->num_cpli; id++) {
		struct scmi_fc_info *fc;
		u32 *cpl_id, zero_cpl_id = 0;

		fc = devm_kcalloc(ph->dev, POWERCAP_FC_MAX, sizeof(*fc), GFP_KERNEL);
		if (!fc)
			return;

		/* NOTE THAT when num_cpli == 1 the arg *cpl_id is 0 */
		cpl_id = (PROTOCOL_REV_MAJOR(ph->version) >= 0x3) ? &id : NULL;

		ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
					   POWERCAP_CAP_SET, 4, dom_info->id,
					   cpl_id,
					   &fc[POWERCAP_FC_CAP].set_addr,
					   &fc[POWERCAP_FC_CAP].set_db,
					   &fc[POWERCAP_FC_CAP].rate_limit);

		ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
					   POWERCAP_CAP_GET, 4, dom_info->id,
					   cpl_id,
					   &fc[POWERCAP_FC_CAP].get_addr, NULL,
					   &fc[POWERCAP_FC_CAP].rate_limit);

		if (PROTOCOL_REV_MAJOR(ph->version) < 0x3) {
			ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
						   POWERCAP_PAI_SET, 4,
						   dom_info->id, NULL,
						   &fc[POWERCAP_FC_XAI].set_addr,
						   &fc[POWERCAP_FC_XAI].set_db,
						   &fc[POWERCAP_FC_XAI].rate_limit);

			ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
						   POWERCAP_PAI_GET, 4,
						   dom_info->id, NULL,
						   &fc[POWERCAP_FC_XAI].get_addr, NULL,
						   &fc[POWERCAP_FC_XAI].rate_limit);
		} else {
			ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
						   POWERCAP_CAI_SET, 4,
						   dom_info->id, &id,
						   &fc[POWERCAP_FC_XAI].set_addr,
						   &fc[POWERCAP_FC_XAI].set_db,
						   &fc[POWERCAP_FC_XAI].rate_limit);
			ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
						   POWERCAP_CAI_GET, 4,
						   dom_info->id, &id,
						   &fc[POWERCAP_FC_XAI].get_addr, NULL,
						   &fc[POWERCAP_FC_XAI].rate_limit);
			ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
						   POWERCAP_MAI_SET, 4,
						   dom_info->id, &zero_cpl_id,
						   &fc[POWERCAP_FC_MAI].set_addr,
						   &fc[POWERCAP_FC_MAI].set_db,
						   &fc[POWERCAP_FC_MAI].rate_limit);
			ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
						   POWERCAP_MAI_GET, 4,
						   dom_info->id, &zero_cpl_id,
						   &fc[POWERCAP_FC_MAI].get_addr, NULL,
						   &fc[POWERCAP_FC_MAI].rate_limit);
			ph->hops->fastchannel_init(ph, POWERCAP_DESCRIBE_FASTCHANNEL,
						   POWERCAP_MEASUREMENTS_GET, 4,
						   dom_info->id, &zero_cpl_id,
						   &fc[POWERCAP_FC_MEASUREMENT].get_addr, NULL,
						   &fc[POWERCAP_FC_MEASUREMENT].rate_limit);
		}

		dom_info->cpli[id].fc_info = fc;
	}
}

static int scmi_powercap_notify(const struct scmi_protocol_handle *ph,
				u32 domain, int message_id, bool enable)
{
	int ret;
	struct scmi_xfer *t;

	switch (message_id) {
	case POWERCAP_CAP_NOTIFY:
	{
		struct scmi_msg_powercap_notify_cap *notify;

		ret = ph->xops->xfer_get_init(ph, message_id,
					      sizeof(*notify), 0, &t);
		if (ret)
			return ret;

		notify = t->tx.buf;
		notify->domain = cpu_to_le32(domain);
		notify->notify_enable = cpu_to_le32(enable ? BIT(0) : 0);
		break;
	}
	case POWERCAP_MEASUREMENTS_NOTIFY:
	{
		u32 low, high;
		struct scmi_msg_powercap_notify_thresh *notify;

		/*
		 * Note that we have to pick the most recently configured
		 * thresholds to build a proper POWERCAP_MEASUREMENTS_NOTIFY
		 * enable request and we fail, complaining, if no thresholds
		 * were ever set, since this is an indication the API has been
		 * used wrongly.
		 */
		ret = scmi_powercap_measurements_threshold_get(ph, domain,
							       &low, &high);
		if (ret)
			return ret;

		ret = ph->xops->xfer_get_init(ph, message_id,
					      sizeof(*notify), 0, &t);
		if (ret)
			return ret;

		notify = t->tx.buf;
		notify->domain = cpu_to_le32(domain);
		notify->notify_enable = cpu_to_le32(enable ? BIT(0) : 0);
		notify->power_thresh_low = cpu_to_le32(low);
		notify->power_thresh_high = cpu_to_le32(high);
		break;
	}
	default:
		return -EINVAL;
	}

	ret = ph->xops->do_xfer(ph, t);

	ph->xops->xfer_put(ph, t);
	return ret;
}

static bool
scmi_powercap_notify_supported(const struct scmi_protocol_handle *ph,
			       u8 evt_id, u32 src_id)
{
	bool supported = false;
	const struct scmi_powercap_info *dom_info;
	struct powercap_info *pi = ph->get_priv(ph);

	if (evt_id >= ARRAY_SIZE(evt_2_cmd) || src_id >= pi->num_domains)
		return false;

	dom_info = pi->powercaps + src_id;
	if (evt_id == SCMI_EVENT_POWERCAP_CAP_CHANGED)
		supported = dom_info->notify_powercap_cap_change;
	else if (evt_id == SCMI_EVENT_POWERCAP_MEASUREMENTS_CHANGED)
		supported = dom_info->notify_powercap_measurement_change;

	return supported;
}

static int
scmi_powercap_set_notify_enabled(const struct scmi_protocol_handle *ph,
				 u8 evt_id, u32 src_id, bool enable)
{
	int ret, cmd_id;
	struct powercap_info *pi = ph->get_priv(ph);

	if (evt_id >= ARRAY_SIZE(evt_2_cmd) || src_id >= pi->num_domains)
		return -EINVAL;

	cmd_id = evt_2_cmd[evt_id];
	ret = scmi_powercap_notify(ph, src_id, cmd_id, enable);
	if (ret)
		pr_debug("FAIL_ENABLED - evt[%X] dom[%d] - ret:%d\n",
			 evt_id, src_id, ret);
	else if (cmd_id == POWERCAP_MEASUREMENTS_NOTIFY)
		/*
		 * On success save the current notification enabled state, so
		 * as to be able to properly update the notification thresholds
		 * when they are modified on a domain for which measurement
		 * notifications were currently enabled.
		 *
		 * This is needed because the SCMI Notification core machinery
		 * and API does not support passing per-notification custom
		 * arguments at callback registration time.
		 *
		 * Note that this can be done here with a simple flag since the
		 * SCMI core Notifications code takes care of keeping proper
		 * per-domain enables refcounting, so that this helper function
		 * will be called only once (for enables) when the first user
		 * registers a callback on this domain and once more (disable)
		 * when the last user de-registers its callback.
		 */
		pi->states[src_id].meas_notif_enabled = enable;

	return ret;
}

static void *
scmi_powercap_fill_custom_report(const struct scmi_protocol_handle *ph,
				 u8 evt_id, ktime_t timestamp,
				 const void *payld, size_t payld_sz,
				 void *report, u32 *src_id)
{
	void *rep = NULL;

	switch (evt_id) {
	case SCMI_EVENT_POWERCAP_CAP_CHANGED:
	{
		const struct scmi_powercap_cap_changed_notify_payld *p = payld;
		struct scmi_powercap_cap_changed_report *r = report;

		if (sizeof(*p) > payld_sz)
			break;

		r->timestamp = timestamp;
		r->agent_id = le32_to_cpu(p->agent_id);
		r->domain_id = le32_to_cpu(p->domain_id);
		r->power_cap = le32_to_cpu(p->power_cap);
		r->avg_ivl = le32_to_cpu(p->avg_ivl);
		if (sizeof(*p) == payld_sz)
			r->cpli = le32_to_cpu(p->cpli);
		else
			r->cpli = 0;
		*src_id = r->domain_id;
		rep = r;
		break;
	}
	case SCMI_EVENT_POWERCAP_MEASUREMENTS_CHANGED:
	{
		const struct scmi_powercap_meas_changed_notify_payld *p = payld;
		struct scmi_powercap_meas_changed_report *r = report;
		const size_t sz_v2 = offsetofend(struct scmi_powercap_meas_changed_notify_payld,
						 power);
		const size_t sz_v3 = sizeof(*p);

		if (payld_sz != sz_v2 && payld_sz != sz_v3)
			break;

		r->timestamp = timestamp;
		r->agent_id = le32_to_cpu(p->agent_id);
		r->domain_id = le32_to_cpu(p->domain_id);
		r->power = le32_to_cpu(p->power);

		if (payld_sz == sz_v3 && PROTOCOL_REV_MAJOR(ph->version) >= 0x3)
			r->mai = le32_to_cpu(p->mai);
		else
			r->mai = 0;

		*src_id = r->domain_id;
		rep = r;
		break;
	}
	default:
		break;
	}

	return rep;
}

static int
scmi_powercap_get_num_sources(const struct scmi_protocol_handle *ph)
{
	struct powercap_info *pi = ph->get_priv(ph);

	if (!pi)
		return -EINVAL;

	return pi->num_domains;
}

static const struct scmi_event powercap_events[] = {
	{
		.id = SCMI_EVENT_POWERCAP_CAP_CHANGED,
		.max_payld_sz =
			sizeof(struct scmi_powercap_cap_changed_notify_payld),
		.max_report_sz =
			sizeof(struct scmi_powercap_cap_changed_report),
	},
	{
		.id = SCMI_EVENT_POWERCAP_MEASUREMENTS_CHANGED,
		.max_payld_sz =
			sizeof(struct scmi_powercap_meas_changed_notify_payld),
		.max_report_sz =
			sizeof(struct scmi_powercap_meas_changed_report),
	},
};

static const struct scmi_event_ops powercap_event_ops = {
	.is_notify_supported = scmi_powercap_notify_supported,
	.get_num_sources = scmi_powercap_get_num_sources,
	.set_notify_enabled = scmi_powercap_set_notify_enabled,
	.fill_custom_report = scmi_powercap_fill_custom_report,
};

static const struct scmi_protocol_events powercap_protocol_events = {
	.queue_sz = SCMI_PROTO_QUEUE_SZ,
	.ops = &powercap_event_ops,
	.evts = powercap_events,
	.num_events = ARRAY_SIZE(powercap_events),
};

static int
scmi_powercap_protocol_init(const struct scmi_protocol_handle *ph)
{
	int domain, ret;
	struct powercap_info *pinfo;

	dev_dbg(ph->dev, "Powercap Version %d.%d\n",
		PROTOCOL_REV_MAJOR(ph->version), PROTOCOL_REV_MINOR(ph->version));

	pinfo = devm_kzalloc(ph->dev, sizeof(*pinfo), GFP_KERNEL);
	if (!pinfo)
		return -ENOMEM;

	ph->set_priv(ph, pinfo);

	if (PROTOCOL_REV_MAJOR(ph->version) < 0x3) {
		pinfo->xfer_cap_get = scmi_powercap_xfer_cap_get;
		pinfo->xfer_cap_set = scmi_powercap_xfer_cap_set;
		pinfo->xfer_avg_ivl_get = scmi_powercap_xfer_avg_interval_get;
		pinfo->xfer_avg_ivl_set = scmi_powercap_xfer_avg_interval_set;

	} else {
		pinfo->xfer_cap_get = scmi_powercap_xfer_cap_get_v3;
		pinfo->xfer_cap_set = scmi_powercap_xfer_cap_set_v3;
		pinfo->xfer_avg_ivl_get = scmi_powercap_xfer_avg_interval_get_v3;
		pinfo->xfer_avg_ivl_set = scmi_powercap_xfer_avg_interval_set_v3;
	}

	ret = scmi_powercap_attributes_get(ph, pinfo);
	if (ret)
		return ret;

	pinfo->powercaps = devm_kcalloc(ph->dev, pinfo->num_domains,
					sizeof(*pinfo->powercaps),
					GFP_KERNEL);
	if (!pinfo->powercaps)
		return -ENOMEM;

	pinfo->states = devm_kcalloc(ph->dev, pinfo->num_domains,
				     sizeof(*pinfo->states), GFP_KERNEL);
	if (!pinfo->states)
		return -ENOMEM;

	/*
	 * Note that any failure in retrieving any domain attribute leads to
	 * the whole Powercap protocol initialization failure: this way the
	 * reported Powercap domains are all assured, when accessed, to be well
	 * formed and correlated by sane parent-child relationship (if any).
	 */
	for (domain = 0; domain < pinfo->num_domains; domain++) {
		struct scmi_powercap_info *dom_info = pinfo->powercaps + domain;

		dom_info->id = domain;
		ret = scmi_powercap_domain_attributes_get(ph, pinfo, dom_info);
		if (ret)
			return ret;

		if (dom_info->fastchannels)
			scmi_powercap_domain_init_fc(ph, dom_info);

		/* Grab initial state when disable is supported. */
		if (PROTOCOL_REV_MAJOR(ph->version) >= 0x2) {
			ret = __scmi_powercap_cap_get(ph, dom_info, CPL0,
						      &pinfo->states[domain].last_pcap);
			if (ret)
				return ret;

			pinfo->states[domain].enabled =
				!!pinfo->states[domain].last_pcap;
		}
	}

	return ph->set_priv(ph, pinfo);
}

static const struct scmi_protocol scmi_powercap = {
	.id = SCMI_PROTOCOL_POWERCAP,
	.owner = THIS_MODULE,
	.instance_init = &scmi_powercap_protocol_init,
	.ops = &powercap_proto_ops,
	.events = &powercap_protocol_events,
	.supported_version = SCMI_PROTOCOL_SUPPORTED_VERSION,
};

DEFINE_SCMI_PROTOCOL_REGISTER_UNREGISTER(powercap, scmi_powercap)
