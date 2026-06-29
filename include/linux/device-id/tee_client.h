#ifndef LINUX_MOD_DEVICE_ID_TEE_CLIENT_H
#define LINUX_MOD_DEVICE_ID_TEE_CLIENT_H

#ifdef __KERNEL__
#include <linux/uuid.h>
#endif

/**
 * struct tee_client_device_id - tee based device identifier
 * @uuid: For TEE based client devices we use the device uuid as
 *        the identifier.
 */
struct tee_client_device_id {
	uuid_t uuid;
};

#endif /* ifndef LINUX_MOD_DEVICE_ID_TEE_CLIENT_H */
