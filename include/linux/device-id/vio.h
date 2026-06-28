#ifndef LINUX_MOD_DEVICE_ID_VIO_H
#define LINUX_MOD_DEVICE_ID_VIO_H

/* VIO */
struct vio_device_id {
	char type[32];
	char compat[32];
};

#endif /* ifndef LINUX_MOD_DEVICE_ID_VIO_H */
