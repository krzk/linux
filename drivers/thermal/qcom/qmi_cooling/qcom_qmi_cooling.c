// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022, Linaro Limited
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/thermal.h>

#include "qcom_tmd_services.h"

#define QMI_TMD_RESP_TIMEOUT		msecs_to_jiffies(100)
#define QMI_CLIENT_NAME_LENGTH		40
#define QMI_MAX_ALLOWED_INSTANCE_ID	0x80

/**
 * struct qmi_plat_data - qmi compile-time platform data
 * @ninstances: Number of instances supported by platform
 */
struct qmi_plat_data {
	const u32		ninstances;
};

struct qmi_cooling_device {
	struct device_node		*np;
	char				cdev_name[THERMAL_NAME_LENGTH];
	char				qmi_name[QMI_CLIENT_NAME_LENGTH];
	bool                            connection_active;
	struct list_head		qmi_node;
	struct thermal_cooling_device	*cdev;
	unsigned int			mtgn_state;
	unsigned int			max_level;
	struct qmi_tmd_instance		*instance;
};

struct qmi_tmd_instance {
	struct device			*dev;
	struct qmi_handle		handle;
	struct mutex			mutex;
	u32				instance_id;
	struct list_head		tmd_cdev_list;
	struct work_struct		svc_arrive_work;
};

/**
 * struct qmi_tmd_priv
 * @dev: device.
 * @instances: array of QMI TMD instances.
 * @ninstances: number of QMI TMD instances.
 */
struct qmi_tmd_priv {
	struct device			*dev;
	struct qmi_tmd_instance		*instances;
	u32				ninstances;
};

static char device_clients[][QMI_CLIENT_NAME_LENGTH] = {
	{"pa"},
	{"pa_fr1"},
	{"cx_vdd_limit"},
	{"modem"},
	{"modem_current"},
	{"modem_skin"},
	{"modem_bw"},
	{"modem_bw_backoff"},
	{"vbatt_low"},
	{"charge_state"},
	{"mmw0"},
	{"mmw1"},
	{"mmw2"},
	{"mmw3"},
	{"mmw_skin0"},
	{"mmw_skin1"},
	{"mmw_skin2"},
	{"mmw_skin3"},
	{"wlan"},
	{"wlan_bw"},
	{"mmw_skin0_dsc"},
	{"mmw_skin1_dsc"},
	{"mmw_skin2_dsc"},
	{"mmw_skin3_dsc"},
	{"modem_skin_lte_dsc"},
	{"modem_skin_nr_dsc"},
	{"pa_dsc"},
	{"pa_fr1_dsc"},
	{"cdsp_sw"},
	{"cdsp_hw"},
	{"cpuv_restriction_cold"},
	{"cpr_cold"},
	{"modem_lte_dsc"},
	{"modem_nr_dsc"},
	{"modem_nr_scg_dsc"},
	{"sdr0_lte_dsc"},
	{"sdr1_lte_dsc"},
	{"sdr0_nr_dsc"},
	{"sdr1_nr_dsc"},
	{"pa_lte_sdr0_dsc"},
	{"pa_lte_sdr1_dsc"},
	{"pa_nr_sdr0_dsc"},
	{"pa_nr_sdr1_dsc"},
	{"pa_nr_sdr0_scg_dsc"},
	{"pa_nr_sdr1_scg_dsc"},
	{"mmw0_dsc"},
	{"mmw1_dsc"},
	{"mmw2_dsc"},
	{"mmw3_dsc"},
	{"mmw_ific_dsc"},
};

static int qmi_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct qmi_cooling_device *qmi_cdev = cdev->devdata;

	if (!qmi_cdev)
		return -EINVAL;

	*state = qmi_cdev->max_level;

	return 0;
}

static int qmi_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct qmi_cooling_device *qmi_cdev = cdev->devdata;

	if (!qmi_cdev)
		return -EINVAL;

	*state = qmi_cdev->mtgn_state;

	return 0;
}

static int qmi_tmd_send_state_request(struct qmi_cooling_device *qmi_cdev,
				uint8_t state)
{
	int ret = 0;
	struct tmd_set_mitigation_level_req_msg_v01 req;
	struct tmd_set_mitigation_level_resp_msg_v01 tmd_resp;
	struct qmi_tmd_instance *tmd_instance = qmi_cdev->instance;
	struct qmi_txn txn;

	memset(&req, 0, sizeof(req));
	memset(&tmd_resp, 0, sizeof(tmd_resp));

	strscpy(req.mitigation_dev_id.mitigation_dev_id, qmi_cdev->qmi_name,
		QMI_TMD_MITIGATION_DEV_ID_LENGTH_MAX_V01);
	req.mitigation_level = state;

	mutex_lock(&tmd_instance->mutex);

	ret = qmi_txn_init(&tmd_instance->handle, &txn,
		tmd_set_mitigation_level_resp_msg_v01_ei, &tmd_resp);
	if (ret < 0) {
		pr_err("qmi set state:%d txn init failed for %s ret:%d\n",
			state, qmi_cdev->cdev_name, ret);
		goto qmi_send_exit;
	}

	ret = qmi_send_request(&tmd_instance->handle, NULL, &txn,
			QMI_TMD_SET_MITIGATION_LEVEL_REQ_V01,
			TMD_SET_MITIGATION_LEVEL_REQ_MSG_V01_MAX_MSG_LEN,
			tmd_set_mitigation_level_req_msg_v01_ei, &req);
	if (ret < 0) {
		pr_err("qmi set state:%d txn send failed for %s ret:%d\n",
			state, qmi_cdev->cdev_name, ret);
		qmi_txn_cancel(&txn);
		goto qmi_send_exit;
	}

	ret = qmi_txn_wait(&txn, QMI_TMD_RESP_TIMEOUT);
	if (ret < 0) {
		pr_err("qmi set state:%d txn wait failed for %s ret:%d\n",
			state, qmi_cdev->cdev_name, ret);
		goto qmi_send_exit;
	}
	if (tmd_resp.resp.result != QMI_RESULT_SUCCESS_V01) {
		ret = tmd_resp.resp.result;
		pr_err("qmi set state:%d NOT success for %s ret:%d\n",
			state, qmi_cdev->cdev_name, ret);
		goto qmi_send_exit;
	}
	ret = 0;
	pr_debug("Requested qmi state:%d for %s\n", state, qmi_cdev->cdev_name);

qmi_send_exit:
	mutex_unlock(&tmd_instance->mutex);
	return ret;
}

static int qmi_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct qmi_cooling_device *qmi_cdev = cdev->devdata;
	int ret = 0;

	if (!qmi_cdev)
		return -EINVAL;

	if (state > qmi_cdev->max_level)
		return -EINVAL;

	if (qmi_cdev->mtgn_state == state)
		return 0;

	/* save it and return if server exit */
	if (!qmi_cdev->connection_active) {
		qmi_cdev->mtgn_state = state;
		pr_debug("Pending request:%ld for %s\n", state,
				qmi_cdev->cdev_name);
		return 0;
	}

	/* It is best effort to save state even if QMI fail */
	ret = qmi_tmd_send_state_request(qmi_cdev, (uint8_t)state);

	qmi_cdev->mtgn_state = state;

	return ret;
}

static struct thermal_cooling_device_ops qmi_device_ops = {
	.get_max_state = qmi_get_max_state,
	.get_cur_state = qmi_get_cur_state,
	.set_cur_state = qmi_set_cur_state,
};

static int qmi_register_cooling_device(struct qmi_cooling_device *qmi_cdev)
{
	qmi_cdev->cdev = thermal_of_cooling_device_register(
					qmi_cdev->np,
					qmi_cdev->cdev_name,
					qmi_cdev,
					&qmi_device_ops);
	if (IS_ERR(qmi_cdev->cdev)) {
		pr_err("Cooling register failed for %s, ret:%ld\n",
			qmi_cdev->cdev_name, PTR_ERR(qmi_cdev->cdev));
		return PTR_ERR(qmi_cdev->cdev);
	}
	pr_debug("Cooling register success for %s\n", qmi_cdev->cdev_name);

	return 0;
}

static int verify_devices_and_register(struct qmi_tmd_instance *tmd_instance)
{
	struct tmd_get_mitigation_device_list_req_msg_v01 req;
	struct tmd_get_mitigation_device_list_resp_msg_v01 *tmd_resp;
	int ret = 0, i;
	struct qmi_txn txn;

	memset(&req, 0, sizeof(req));
	/* size of tmd_resp is very high, use heap memory rather than stack */
	tmd_resp = kzalloc(sizeof(*tmd_resp), GFP_KERNEL);
	if (!tmd_resp)
		return -ENOMEM;

	mutex_lock(&tmd_instance->mutex);
	ret = qmi_txn_init(&tmd_instance->handle, &txn,
		tmd_get_mitigation_device_list_resp_msg_v01_ei, tmd_resp);
	if (ret < 0) {
		pr_err("Transaction Init error for instance_id:0x%x ret:%d\n",
			tmd_instance->instance_id, ret);
		goto reg_exit;
	}

	ret = qmi_send_request(&tmd_instance->handle, NULL, &txn,
			QMI_TMD_GET_MITIGATION_DEVICE_LIST_REQ_V01,
			TMD_GET_MITIGATION_DEVICE_LIST_REQ_MSG_V01_MAX_MSG_LEN,
			tmd_get_mitigation_device_list_req_msg_v01_ei,
			&req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		goto reg_exit;
	}

	ret = qmi_txn_wait(&txn, QMI_TMD_RESP_TIMEOUT);
	if (ret < 0) {
		pr_err("Transaction wait error for instance_id:0x%x ret:%d\n",
			tmd_instance->instance_id, ret);
		goto reg_exit;
	}
	if (tmd_resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		ret = tmd_resp->resp.result;
		pr_err("Get device list NOT success for instance_id:0x%x ret:%d\n",
			tmd_instance->instance_id, ret);
		goto reg_exit;
	}
	mutex_unlock(&tmd_instance->mutex);

	for (i = 0; i < tmd_resp->mitigation_device_list_len; i++) {
		struct qmi_cooling_device *qmi_cdev = NULL;

		list_for_each_entry(qmi_cdev, &tmd_instance->tmd_cdev_list,
					qmi_node) {
			struct tmd_mitigation_dev_list_type_v01 *device =
				&tmd_resp->mitigation_device_list[i];

			if ((strncasecmp(qmi_cdev->qmi_name,
				device->mitigation_dev_id.mitigation_dev_id,
				QMI_TMD_MITIGATION_DEV_ID_LENGTH_MAX_V01)))
				continue;

			qmi_cdev->connection_active = true;
			qmi_cdev->max_level = device->max_mitigation_level;
			/*
			 * It is better to set current state
			 * initially or during restart
			 */
			qmi_tmd_send_state_request(qmi_cdev,
							qmi_cdev->mtgn_state);
			if (!qmi_cdev->cdev)
				ret = qmi_register_cooling_device(qmi_cdev);
			break;
		}
	}

	kfree(tmd_resp);
	return ret;

reg_exit:
	mutex_unlock(&tmd_instance->mutex);
	kfree(tmd_resp);

	return ret;
}

static void qmi_tmd_svc_arrive(struct work_struct *work)
{
	struct qmi_tmd_instance *tmd_instance = container_of(work,
						struct qmi_tmd_instance,
						svc_arrive_work);

	verify_devices_and_register(tmd_instance);
}

static void thermal_qmi_net_reset(struct qmi_handle *qmi)
{
	struct qmi_tmd_instance *tmd_instance = container_of(qmi,
						struct qmi_tmd_instance,
						handle);
	struct qmi_cooling_device *qmi_cdev = NULL;

	list_for_each_entry(qmi_cdev, &tmd_instance->tmd_cdev_list,
					qmi_node) {
		if (qmi_cdev->connection_active)
			qmi_tmd_send_state_request(qmi_cdev,
							qmi_cdev->mtgn_state);
	}
}

static void thermal_qmi_del_server(struct qmi_handle *qmi,
				    struct qmi_service *service)
{
	struct qmi_tmd_instance *tmd_instance = container_of(qmi,
						struct qmi_tmd_instance,
						handle);
	struct qmi_cooling_device *qmi_cdev = NULL;

	list_for_each_entry(qmi_cdev, &tmd_instance->tmd_cdev_list, qmi_node)
		qmi_cdev->connection_active = false;
}

static int thermal_qmi_new_server(struct qmi_handle *qmi,
				    struct qmi_service *service)
{
	struct qmi_tmd_instance *tmd_instance = container_of(qmi,
						struct qmi_tmd_instance,
						handle);
	struct sockaddr_qrtr sq = {AF_QIPCRTR, service->node, service->port};

	mutex_lock(&tmd_instance->mutex);
	kernel_connect(qmi->sock, (struct sockaddr *)&sq, sizeof(sq), 0);
	mutex_unlock(&tmd_instance->mutex);
	queue_work(system_highpri_wq, &tmd_instance->svc_arrive_work);

	return 0;
}

static struct qmi_ops thermal_qmi_event_ops = {
	.new_server = thermal_qmi_new_server,
	.del_server = thermal_qmi_del_server,
	.net_reset = thermal_qmi_net_reset,
};

static void qmi_tmd_cleanup(struct qmi_tmd_priv *priv)
{
	int i;
	struct qmi_tmd_instance *tmd_instance = priv->instances;
	struct qmi_cooling_device *qmi_cdev, *c_next;

	for (i = 0; i < priv->ninstances; i++) {
		mutex_lock(&tmd_instance[i].mutex);
		list_for_each_entry_safe(qmi_cdev, c_next,
				&tmd_instance[i].tmd_cdev_list, qmi_node) {
			qmi_cdev->connection_active = false;
			if (qmi_cdev->cdev)
				thermal_cooling_device_unregister(
					qmi_cdev->cdev);

			list_del(&qmi_cdev->qmi_node);
		}
		qmi_handle_release(&tmd_instance[i].handle);

		mutex_unlock(&tmd_instance[i].mutex);
	}
}

static int qmi_get_dt_instance_data(struct qmi_tmd_priv *priv,
				    struct qmi_tmd_instance *instance,
				    struct device_node *node)
{
	struct device *dev = priv->dev;
	struct qmi_cooling_device *qmi_cdev;
	struct device_node *subnode;
	int ret, i;
	u32 instance_id;

	ret = of_property_read_u32(node, "qcom,instance-id", &instance_id);
	if (ret) {
		dev_err(dev, "error reading qcom,instance-id (%d)\n",
				ret);
		return ret;
	}

	if (instance_id >= QMI_MAX_ALLOWED_INSTANCE_ID) {
		dev_err(dev, "Instance ID exceeds max allowed value (%d)\n", instance_id);
		return -EINVAL;
	}

	instance->instance_id = instance_id;

	instance->dev = dev;
	mutex_init(&instance->mutex);
	INIT_LIST_HEAD(&instance->tmd_cdev_list);
	INIT_WORK(&instance->svc_arrive_work, qmi_tmd_svc_arrive);

	for_each_available_child_of_node(node, subnode) {
		const char *qmi_name;

		qmi_cdev = devm_kzalloc(dev, sizeof(*qmi_cdev),
				GFP_KERNEL);
		if (!qmi_cdev) {
			ret = -ENOMEM;
			goto data_error;
		}

		strscpy(qmi_cdev->cdev_name, subnode->name,
				THERMAL_NAME_LENGTH);

		if (!of_property_read_string(subnode,
					"label",
					&qmi_name)) {
			strscpy(qmi_cdev->qmi_name, qmi_name,
					QMI_CLIENT_NAME_LENGTH);
		} else {
			dev_err(dev, "Fail to parse dev name for %s\n",
					subnode->name);
			of_node_put(subnode);
			break;
		}

		/* Check for supported qmi dev */
		for (i = 0; i < ARRAY_SIZE(device_clients); i++) {
			if (strcmp(device_clients[i],
						qmi_cdev->qmi_name) == 0)
				break;
		}

		if (i >= ARRAY_SIZE(device_clients)) {
			dev_err(dev, "Not supported dev name for %s\n",
					subnode->name);
			of_node_put(subnode);
			break;
		}
		qmi_cdev->instance = instance;
		qmi_cdev->np = subnode;
		qmi_cdev->mtgn_state = 0;
		list_add(&qmi_cdev->qmi_node, &instance->tmd_cdev_list);
	}

	of_node_put(node);

	return 0;
data_error:
	of_node_put(subnode);

	return ret;
}

static int qmi_tmd_device_init(struct qmi_tmd_priv *priv)
{
	int i, ret;
	u32 ninstances = priv->ninstances;

	for (i = 0; i < ninstances; i++) {
		struct qmi_tmd_instance *tmd_instance = &priv->instances[i];

		if (list_empty(&tmd_instance->tmd_cdev_list))
			continue;

		ret = qmi_handle_init(&tmd_instance->handle,
			TMD_GET_MITIGATION_DEVICE_LIST_RESP_MSG_V01_MAX_MSG_LEN,
			&thermal_qmi_event_ops, NULL);
		if (ret < 0) {
			dev_err(priv->dev, "QMI[0x%x] handle init failed. err:%d\n",
					tmd_instance->instance_id, ret);
			priv->ninstances = i;
			return ret;
		}

		ret = qmi_add_lookup(&tmd_instance->handle, TMD_SERVICE_ID_V01,
					TMD_SERVICE_VERS_V01,
					tmd_instance->instance_id);
		if (ret < 0) {
			dev_err(priv->dev, "QMI register failed for 0x%x, ret:%d\n",
				tmd_instance->instance_id, ret);
			return ret;
		}
	}

	return 0;
}

static const struct of_device_id qmi_tmd_device_table[] = {
	{.compatible = "qcom,qmi-tmd-devices"},
	{}
};
MODULE_DEVICE_TABLE(of, qmi_tmd_device_table);

static int qmi_tmd_device_probe(struct platform_device *pdev)
{
	struct device *dev;
	struct device_node *np;
	struct device_node *child;
	struct qmi_tmd_instance *instances;
	const struct qmi_plat_data *data;
	const struct of_device_id *id;
	struct qmi_tmd_priv *priv;
	int ret;
	u32 ninstances;

	if (pdev->dev.of_node)
		dev = &pdev->dev;
	else
		dev = pdev->dev.parent;

	np = dev->of_node;

	id = of_match_node(qmi_tmd_device_table, np);
	if (!id)
		return -ENODEV;

	data = id->data;

	if (np)
		ninstances = of_get_available_child_count(np);

	if (ninstances <= 0) {
		dev_err(dev, "No instances to process\n");
		return -EINVAL;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->ninstances = ninstances;

	priv->instances = devm_kcalloc(dev, priv->ninstances,
					sizeof(*priv->instances), GFP_KERNEL);
	if (!priv->instances)
		return -ENOMEM;

	instances = priv->instances;

	for_each_available_child_of_node(np, child) {
		ret = qmi_get_dt_instance_data(priv, instances, child);
		if (ret) {
			of_node_put(child);
			return ret;
		}

		instances++;
	}

	platform_set_drvdata(pdev, priv);

	ret = qmi_tmd_device_init(priv);
	if (ret)
		goto probe_err;

	dev_dbg(dev, "QMI Thermal Mitigation Device driver probe success!\n");
	return 0;

probe_err:
	qmi_tmd_cleanup(priv);
	return ret;
}

static int qmi_tmd_device_remove(struct platform_device *pdev)
{
	struct qmi_tmd_priv *priv = platform_get_drvdata(pdev);

	qmi_tmd_cleanup(priv);

	return 0;
}

static struct platform_driver qmi_tmd_device_driver = {
	.probe          = qmi_tmd_device_probe,
	.remove         = qmi_tmd_device_remove,
	.driver         = {
		.name   = "qcom-qmi-tmd-devices",
		.of_match_table = qmi_tmd_device_table,
	},
};

module_platform_driver(qmi_tmd_device_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Qualcomm QMI Thermal Mitigation Device driver");
MODULE_ALIAS("platform:qcom-qmi-tmd-devices");
