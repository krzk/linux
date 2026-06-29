// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "xe_device.h"
#include "xe_drm_ras.h"
#include "xe_pm.h"
#include "xe_printk.h"
#include "xe_ras.h"
#include "xe_ras_types.h"
#include "xe_sysctrl.h"
#include "xe_sysctrl_event_types.h"
#include "xe_sysctrl_mailbox.h"
#include "xe_sysctrl_mailbox_types.h"

/* Severity of detected errors  */
enum xe_ras_severity {
	XE_RAS_SEV_NOT_SUPPORTED = 0,
	XE_RAS_SEV_CORRECTABLE,
	XE_RAS_SEV_UNCORRECTABLE,
	XE_RAS_SEV_INFORMATIONAL,
	XE_RAS_SEV_MAX
};

/* Major IP blocks/components where errors can originate */
enum xe_ras_component {
	XE_RAS_COMP_NOT_SUPPORTED = 0,
	XE_RAS_COMP_DEVICE_MEMORY,
	XE_RAS_COMP_CORE_COMPUTE,
	XE_RAS_COMP_RESERVED,
	XE_RAS_COMP_PCIE,
	XE_RAS_COMP_FABRIC,
	XE_RAS_COMP_SOC_INTERNAL,
	XE_RAS_COMP_MAX
};

/* RAS response status codes */
enum xe_ras_response_status {
	XE_RAS_STATUS_SUCCESS = 0,
	XE_RAS_STATUS_INVALID_PARAM,
	XE_RAS_STATUS_OP_NOT_SUPPORTED,
	XE_RAS_STATUS_TIMEOUT,
	XE_RAS_STATUS_HARDWARE_FAILURE,
	XE_RAS_STATUS_INSUFFICIENT_RESOURCES,
	XE_RAS_STATUS_MAX
};

static const char *const xe_ras_severities[] = {
	[XE_RAS_SEV_NOT_SUPPORTED]		= "Not Supported",
	[XE_RAS_SEV_CORRECTABLE]		= "Correctable Error",
	[XE_RAS_SEV_UNCORRECTABLE]		= "Uncorrectable Error",
	[XE_RAS_SEV_INFORMATIONAL]		= "Informational Error",
};
static_assert(ARRAY_SIZE(xe_ras_severities) == XE_RAS_SEV_MAX);

static const char *const xe_ras_components[] = {
	[XE_RAS_COMP_NOT_SUPPORTED]		= "Not Supported",
	[XE_RAS_COMP_DEVICE_MEMORY]		= "Device Memory",
	[XE_RAS_COMP_CORE_COMPUTE]		= "Core Compute",
	[XE_RAS_COMP_RESERVED]			= "Reserved",
	[XE_RAS_COMP_PCIE]			= "PCIe",
	[XE_RAS_COMP_FABRIC]			= "Fabric",
	[XE_RAS_COMP_SOC_INTERNAL]		= "SoC Internal",
};
static_assert(ARRAY_SIZE(xe_ras_components) == XE_RAS_COMP_MAX);

static u8 drm_to_xe_ras_severity(u8 severity)
{
	switch (severity) {
	case DRM_XE_RAS_ERR_SEV_CORRECTABLE:
		return XE_RAS_SEV_CORRECTABLE;
	case DRM_XE_RAS_ERR_SEV_UNCORRECTABLE:
		return XE_RAS_SEV_UNCORRECTABLE;
	default:
		return XE_RAS_SEV_NOT_SUPPORTED;
	}
}

static u8 drm_to_xe_ras_component(u8 component)
{
	switch (component) {
	case DRM_XE_RAS_ERR_COMP_CORE_COMPUTE:
		return XE_RAS_COMP_CORE_COMPUTE;
	case DRM_XE_RAS_ERR_COMP_SOC_INTERNAL:
		return XE_RAS_COMP_SOC_INTERNAL;
	case DRM_XE_RAS_ERR_COMP_DEVICE_MEMORY:
		return XE_RAS_COMP_DEVICE_MEMORY;
	case DRM_XE_RAS_ERR_COMP_PCIE:
		return XE_RAS_COMP_PCIE;
	case DRM_XE_RAS_ERR_COMP_FABRIC:
		return XE_RAS_COMP_FABRIC;
	default:
		return XE_RAS_COMP_NOT_SUPPORTED;
	}
}

static int ras_status_to_errno(u32 status)
{
	switch (status) {
	case XE_RAS_STATUS_SUCCESS:
		return 0;
	case XE_RAS_STATUS_INVALID_PARAM:
		return -EINVAL;
	case XE_RAS_STATUS_OP_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case XE_RAS_STATUS_TIMEOUT:
		return -ETIMEDOUT;
	case XE_RAS_STATUS_HARDWARE_FAILURE:
		return -EIO;
	case XE_RAS_STATUS_INSUFFICIENT_RESOURCES:
		return -ENOSPC;
	default:
		return -EPROTO;
	}
}

static inline const char *sev_to_str(u8 severity)
{
	if (severity >= XE_RAS_SEV_MAX)
		severity = XE_RAS_SEV_NOT_SUPPORTED;

	return xe_ras_severities[severity];
}

static inline const char *comp_to_str(u8 component)
{
	if (component >= XE_RAS_COMP_MAX)
		component = XE_RAS_COMP_NOT_SUPPORTED;

	return xe_ras_components[component];
}

void xe_ras_counter_threshold_crossed(struct xe_device *xe,
				      struct xe_sysctrl_event_response *response)
{
	struct xe_ras_threshold_crossed *pending = (void *)&response->data;
	struct xe_ras_error_class *errors = pending->counters;
	u32 id, ncounters = pending->ncounters;

	BUILD_BUG_ON(sizeof(response->data) < sizeof(*pending));
	xe_device_assert_mem_access(xe);

	if (!ncounters || ncounters > XE_RAS_NUM_COUNTERS)
		xe_err(xe, "sysctrl: unexpected counter threshold crossed %u\n", ncounters);
	else
		xe_warn(xe, "[RAS]: counter threshold crossed, %u new errors\n", ncounters);

	for (id = 0; id < ncounters && id < XE_RAS_NUM_COUNTERS; id++) {
		u8 severity, component;

		severity = errors[id].common.severity;
		component = errors[id].common.component;

		xe_warn(xe, "[RAS]: %s %s detected\n",
			comp_to_str(component), sev_to_str(severity));
	}
}

static int get_counter(struct xe_device *xe, struct xe_ras_error_class *counter, u32 *value)
{
	struct xe_ras_get_counter_response response = {0};
	struct xe_ras_get_counter_request request = {0};
	struct xe_sysctrl_mailbox_command command = {0};
	struct xe_ras_error_common *common;
	size_t rlen;
	int ret;

	request.counter = *counter;

	xe_sysctrl_create_command(&command, XE_SYSCTRL_GROUP_GFSP, XE_SYSCTRL_CMD_GET_COUNTER,
				  &request, sizeof(request), &response, sizeof(response));

	ret = xe_sysctrl_send_command(&xe->sc, &command, &rlen);
	if (ret) {
		xe_err(xe, "sysctrl: failed to get counter %d\n", ret);
		return ret;
	}

	if (rlen != sizeof(response)) {
		xe_err(xe, "sysctrl: unexpected get counter response length %zu (expected %zu)\n",
		       rlen, sizeof(response));
		return -EIO;
	}

	common = &response.counter.common;
	*value = response.value;

	xe_dbg(xe, "[RAS]: get counter %u for %s %s\n", *value, comp_to_str(common->component),
	       sev_to_str(common->severity));

	return 0;
}

/**
 * xe_ras_get_counter() - Get error counter value
 * @xe: Xe device instance
 * @severity: Error severity to be queried (&enum drm_xe_ras_error_severity)
 * @component: Error component to be queried (&enum drm_xe_ras_error_component)
 * @value: Counter value
 *
 * This function retrieves the value of a specific error counter based on
 * the error severity and component.
 *
 * Return: 0 on success, negative error code on failure.
 */
int xe_ras_get_counter(struct xe_device *xe, u8 severity, u8 component, u32 *value)
{
	struct xe_ras_error_class counter = {0};

	counter.common.severity = drm_to_xe_ras_severity(severity);
	counter.common.component = drm_to_xe_ras_component(component);

	guard(xe_pm_runtime)(xe);
	return get_counter(xe, &counter, value);
}

/**
 * xe_ras_clear_counter() - Clear error counter value
 * @xe: Xe device instance
 * @severity: Error severity to be cleared (&enum drm_xe_ras_error_severity)
 * @component: Error component to be cleared (&enum drm_xe_ras_error_component)
 *
 * This function clears the value of a specific error counter based on
 * the error severity and component.
 *
 * Return: 0 on success, negative error code on failure.
 */
int xe_ras_clear_counter(struct xe_device *xe, u8 severity, u8 component)
{
	struct xe_ras_clear_counter_response response = {0};
	struct xe_ras_clear_counter_request request = {0};
	struct xe_sysctrl_mailbox_command command = {0};
	struct xe_ras_error_class *counter;
	size_t rlen;
	int ret;

	counter = &request.counter;
	counter->common.severity = drm_to_xe_ras_severity(severity);
	counter->common.component = drm_to_xe_ras_component(component);

	xe_sysctrl_create_command(&command, XE_SYSCTRL_GROUP_GFSP, XE_SYSCTRL_CMD_CLEAR_COUNTER,
				  &request, sizeof(request), &response, sizeof(response));

	guard(xe_pm_runtime)(xe);
	ret = xe_sysctrl_send_command(&xe->sc, &command, &rlen);
	if (ret) {
		xe_err(xe, "sysctrl: failed to clear counter %d\n", ret);
		return ret;
	}

	if (rlen != sizeof(response)) {
		xe_err(xe, "sysctrl: unexpected clear counter response length %zu (expected %zu)\n",
		       rlen, sizeof(response));
		return -EIO;
	}

	ret = ras_status_to_errno(response.status);
	if (ret) {
		xe_err(xe, "sysctrl: clear counter command failed with status %#x\n",
		       response.status);
		return ret;
	}

	counter = &response.counter;

	xe_dbg(xe, "[RAS]: clear counter for %s %s\n", comp_to_str(counter->common.component),
	       sev_to_str(counter->common.severity));

	return 0;
}

/**
 * xe_ras_init - Initialize Xe RAS
 * @xe: xe device instance
 *
 * Register drm_ras nodes
 */
void xe_ras_init(struct xe_device *xe)
{
	if (!xe->info.has_drm_ras)
		return;

	xe_drm_ras_init(xe);
}
