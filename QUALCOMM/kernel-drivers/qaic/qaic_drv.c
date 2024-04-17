// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2019-2021, The Linux Foundation. All rights reserved. */
/* Copyright (c) 2021-2024 Qualcomm Innovation Center, Inc. All rights reserved. */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/kobject.h>
#include <linux/kref.h>
#include <linux/mhi.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#ifndef _QBP_NEED_DRM_ACCEL_FRAMEWORK
#include <drm/drm_accel.h>
#endif
#ifdef _QBP_INCLUDE_HAS_DRM_DRV
#include <drm/drm_drv.h>
#endif
#ifdef _QBP_INCLUDE_HAS_DRM_FILE
#ifdef _QBP_INCLUDE_FIX_DRM_FILE
/* needed before drm_file.h since it doesn't include idr yet uses them */
#include <linux/idr.h>
#endif
#include <drm/drm_file.h>
#endif
#ifdef _QBP_INCLUDE_FIX_DRM_GEM
/* needed before drm_gem.h since it doesn't include drm_vma_offset_node def */
#include <drm/drm_vma_manager.h>
#endif
#include <drm/drm_gem.h>
#ifdef _QBP_INCLUDE_HAS_DRM_IOCTL
#include <drm/drm_ioctl.h>
#endif
#ifndef _QBP_ALT_DRM_MANAGED
#include <drm/drm_managed.h>
#endif
#include <uapi/drm/qaic_accel.h>

#include "mhi_controller.h"
#include "mhi_qaic_ctrl.h"
#include "qaic.h"
#include "qaic_debugfs.h"
#include "qaic_ras.h"
#include "qaic_ssr.h"
#include "qaic_telemetry.h"
#include "qaic_timesync.h"
#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
#include "accel_helpers.h"
#endif

#ifdef _QBP_HAS_MOD_IMPORT_DMA_BUF
MODULE_IMPORT_NS(DMA_BUF);
#endif

#define PCI_DEV_AIC100			0xa100
#define QAIC_NAME			"qaic"
#define QAIC_DESC			"Qualcomm Cloud AI Accelerators"
#define CNTL_MAJOR			5
#define CNTL_MINOR			0

bool datapath_polling;
module_param(datapath_polling, bool, 0400);
MODULE_PARM_DESC(datapath_polling, "Operate the datapath in polling mode");
static bool link_up;
static DEFINE_IDA(qaic_usrs);

static void qaic_destroy_drm_device(struct qaic_drm_device *qddev);
static int qaic_destroy_part_drm_dev(struct qaic_device *qdev, s32 partition_id,
				     struct qaic_user *usr);

static void qaicm_wq_release(struct drm_device *dev, void *res)
{
	struct workqueue_struct *wq = res;

	destroy_workqueue(wq);
}

static struct workqueue_struct *qaicm_wq_init(struct drm_device *dev, const char *fmt)
{
	struct workqueue_struct *wq;
	int ret;

	wq = alloc_workqueue(fmt, WQ_UNBOUND, 0);
	if (!wq)
		return ERR_PTR(-ENOMEM);
	ret = drmm_add_action_or_reset(dev, qaicm_wq_release, wq);
	if (ret)
		return ERR_PTR(ret);

	return wq;
}

static void qaicm_srcu_release(struct drm_device *dev, void *res)
{
	struct srcu_struct *lock = res;

	cleanup_srcu_struct(lock);
}

static int qaicm_srcu_init(struct drm_device *dev, struct srcu_struct *lock)
{
	int ret;

	ret = init_srcu_struct(lock);
	if (ret)
		return ret;

	return drmm_add_action_or_reset(dev, qaicm_srcu_release, lock);
}

static void qaicm_pci_release(struct drm_device *dev, void *res)
{
	struct qaic_device *qdev = to_qaic_device(dev);

	pci_set_drvdata(qdev->pdev, NULL);
}

static void free_usr(struct kref *kref)
{
	struct qaic_user *usr = container_of(kref, struct qaic_user, ref_count);

	cleanup_srcu_struct(&usr->qddev_lock);
	ida_free(&qaic_usrs, usr->handle);
	kfree(usr);
}

static int qaic_open(struct drm_device *dev, struct drm_file *file)
{
	struct qaic_drm_device *qddev = to_qaic_drm_device(dev);
	struct qaic_device *qdev = qddev->qdev;
	struct qaic_user *usr;
	int rcu_id;
	int ret;

	rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->reset_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto dev_unlock;
	}

	usr = kmalloc(sizeof(*usr), GFP_KERNEL);
	if (!usr) {
		ret = -ENOMEM;
		goto dev_unlock;
	}

	usr->handle = ida_alloc(&qaic_usrs, GFP_KERNEL);
	if (usr->handle < 0) {
		ret = usr->handle;
		goto free_usr;
	}
	usr->qddev = qddev;
	atomic_set(&usr->chunk_id, 0);
	init_srcu_struct(&usr->qddev_lock);
	kref_init(&usr->ref_count);

	ret = mutex_lock_interruptible(&qddev->users_mutex);
	if (ret)
		goto cleanup_usr;

	list_add(&usr->node, &qddev->users);
	mutex_unlock(&qddev->users_mutex);

	file->driver_priv = usr;

	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return 0;

cleanup_usr:
	cleanup_srcu_struct(&usr->qddev_lock);
	ida_free(&qaic_usrs, usr->handle);
free_usr:
	kfree(usr);
dev_unlock:
	srcu_read_unlock(&qdev->dev_lock, rcu_id);
	return ret;
}

static void qaic_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct qaic_user *usr = file->driver_priv;
	struct qaic_drm_device *qddev;
	struct qaic_device *qdev;
	int qdev_rcu_id;
	int usr_rcu_id;
	int i;

	qddev = usr->qddev;
	usr_rcu_id = srcu_read_lock(&usr->qddev_lock);
	if (qddev) {
		qdev = qddev->qdev;
		qdev_rcu_id = srcu_read_lock(&qdev->dev_lock);
		if (qdev->reset_state == QAIC_ONLINE) {
			/* Remove all partitioned device for this user */
			if (qddev->partition_id == QAIC_NO_PARTITION)
				qaic_destroy_part_drm_dev(qdev, QAIC_NO_PARTITION, usr);
			qaic_release_usr(qdev, usr);
			for (i = 0; i < qdev->num_dbc; ++i)
				if (qdev->dbc[i].usr && qdev->dbc[i].usr->handle == usr->handle)
					release_dbc(qdev, i);
		}
		srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);

		mutex_lock(&qddev->users_mutex);
		if (!list_empty(&usr->node))
			list_del_init(&usr->node);
		mutex_unlock(&qddev->users_mutex);
	}

	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);
	kref_put(&usr->ref_count, free_usr);

	file->driver_priv = NULL;
}

#if defined(_QBP_NEED_ACCEL_FOP_MMAP) && !defined(_QBP_NEED_DRM_ACCEL_FRAMEWORK)
static const struct file_operations qaic_accel_fops = {
	.owner = THIS_MODULE,
	.mmap = drm_gem_mmap,
	DRM_ACCEL_FOPS,
};
#else
DEFINE_DRM_ACCEL_FOPS(qaic_accel_fops);
#endif

static const struct drm_ioctl_desc qaic_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(QAIC_MANAGE, qaic_manage_ioctl, 0),
	DRM_IOCTL_DEF_DRV(QAIC_CREATE_BO, qaic_create_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(QAIC_MMAP_BO, qaic_mmap_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(QAIC_ATTACH_SLICE_BO, qaic_attach_slice_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(QAIC_EXECUTE_BO, qaic_execute_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(QAIC_PARTIAL_EXECUTE_BO, qaic_partial_execute_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(QAIC_WAIT_BO, qaic_wait_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(QAIC_PERF_STATS_BO, qaic_perf_stats_bo_ioctl, 0),
	DRM_IOCTL_DEF_DRV(QAIC_DETACH_SLICE_BO, qaic_detach_slice_bo_ioctl, 0),
};

#ifdef _QBP_HAS_CONST_DRM_DRIVER
static const struct drm_driver qaic_accel_driver = {
#else
static struct drm_driver qaic_accel_driver = {
#endif
	.driver_features	= DRIVER_GEM | DRIVER_COMPUTE_ACCEL,

	.name			= QAIC_NAME,
	.desc			= QAIC_DESC,
	.date			= "20190618",

	.fops			= &qaic_accel_fops,
	.open			= qaic_open,
	.postclose		= qaic_postclose,
	.debugfs_init		= qaic_debugfs_init,

	.ioctls			= qaic_drm_ioctls,
	.num_ioctls		= ARRAY_SIZE(qaic_drm_ioctls),
	.gem_prime_import	= qaic_gem_prime_import,
#ifdef _QBP_ALT_DRM_GEM_OBJ_FUNCS
	.gem_free_object_unlocked = qaic_accel_free_object,
#ifdef _QBP_HAS_DRM_DRV_GEM_PRINT_INFO
	
	.gem_print_info = qaic_accel_gem_print_info,
#endif
#endif
#ifdef _QBP_ALT_DRM_MANAGED
#ifndef _QBP_ALT_DRM_MANAGED_NO_RELEASE
	
	.release = qaicm_dev_release,
#endif
#endif
#ifdef _QBP_HAS_DRM_DRV_DEBUGFS_CLEANUP
	
	.debugfs_cleanup = qaic_debugfs_cleanup,
#endif
#ifdef _QBP_HAS_DRM_DRV_WITH_GEM_PRIME_MMAP
	
	.gem_prime_mmap = drm_gem_prime_mmap,
#endif
#ifdef _QBP_HAS_DRM_DRV_WITH_GEM_PRIME_GET_SG_TABLE
	
	.gem_prime_get_sg_table = qaic_get_sg_table,
#endif
#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0))
	
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
#endif

};

static void *qaic_drm_dev_alloc(struct qaic_device *qdev, struct device *parent)
{
	struct qaic_drm_device *qddev;
#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
	int ret;
#endif

	qddev = devm_drm_dev_alloc(parent, &qaic_accel_driver, struct qaic_drm_device, drm);
	if (IS_ERR(qddev))
		return qddev;

	drmm_mutex_init(to_drm(qddev), &qddev->users_mutex);
	INIT_LIST_HEAD(&qddev->users);
	qddev->qdev = qdev;
#ifdef CONFIG_DEBUG_FS
	qddev->dbc_debugfs_list = drmm_kcalloc(to_drm(qddev), DBC_DEBUGFS_ENTRIES * qdev->num_dbc,
					       sizeof(*qddev->dbc_debugfs_list), GFP_KERNEL);
	if (!qddev->dbc_debugfs_list)
		return ERR_PTR(-ENOMEM);
#endif

#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
	ret = accel_alloc(qddev);
	if (ret)
		return ERR_PTR(ret);
#endif
	
	return qddev;
}

static int __qaic_create_drm_device(struct qaic_drm_device *qddev)
{
	struct drm_device *drm = to_drm(qddev);
	struct qaic_device *qdev = qddev->qdev;
	int ret;
#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
	ret = qaic_accel_minor_register(qddev->accel);
	if (ret) {
		pci_dbg(qdev->pdev,
			"%s: qaic_accel_minor_register failed %d\n", __func__,
			ret);
		return ret;
	}
#endif

	ret = drm_dev_register(drm, 0);
	if (ret) {
		pci_dbg(qdev->pdev, "drm_dev_register failed %d\n", ret);
		return ret;
	}

	ret = qaic_sysfs_init(qddev);
	if (ret) {
		qaic_destroy_drm_device(qddev);
		pci_dbg(qdev->pdev, "qaic_sysfs_init failed %d\n", ret);
		return ret;
	}

	return ret;
}

static int qaic_create_drm_device(struct qaic_device *qdev)
{
	struct qaic_drm_device *qddev = qdev->qddev;
	int ret;

	ret = __qaic_create_drm_device(qddev);
	if (ret)
		return ret;

	qddev->partition_id = QAIC_NO_PARTITION;

	return ret;
}

static int qaic_create_part_drm_dev(struct qaic_device *qdev, s32 partition_id,
				    struct qaic_user *usr)
{
	struct qaic_drm_device *qddev = NULL;
	int ret;

	if (partition_id == QAIC_NO_PARTITION)
		return -EINVAL;

	qddev = qaic_drm_dev_alloc(qdev, to_accel_kdev(qdev->qddev));
	if (IS_ERR(qddev))
		return PTR_ERR(qddev);

	ret = __qaic_create_drm_device(qddev);
	if (ret)
		return ret;

	qddev->partition_id = partition_id;
	qddev->owner = usr;
	mutex_lock(&qdev->part_devs_mutex);
	list_add(&qddev->part_dev, &qdev->part_devs);
	mutex_unlock(&qdev->part_devs_mutex);

	return ret;
}

static void qaic_destroy_drm_device(struct qaic_drm_device *qddev)
{
	struct drm_device *drm = to_drm(qddev);
	struct qaic_user *usr;

	qaic_sysfs_remove(qddev);
	drm_dev_unregister(drm);
#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
	qaic_accel_minor_unregister(qddev->accel);
#endif
	qddev->partition_id = 0;
	qddev->owner = NULL;

	/*
	 * Existing users get unresolvable errors till they close FDs.
	 * Need to sync carefully with users calling close(). The
	 * list of users can be modified elsewhere when the lock isn't
	 * held here, but the sync'ing the srcu with the mutex held
	 * could deadlock. Grab the mutex so that the list will be
	 * unmodified. The user we get will exist as long as the
	 * lock is held. Signal that the qcdev is going away, and
	 * grab a reference to the user so they don't go away for
	 * synchronize_srcu(). Then release the mutex to avoid
	 * deadlock and make sure the user has observed the signal.
	 * With the lock released, we cannot maintain any state of the
	 * user list.
	 */
	mutex_lock(&qddev->users_mutex);
	while (!list_empty(&qddev->users)) {
		usr = list_first_entry(&qddev->users, struct qaic_user, node);
		list_del_init(&usr->node);
		kref_get(&usr->ref_count);
		usr->qddev = NULL;
		mutex_unlock(&qddev->users_mutex);
		synchronize_srcu(&usr->qddev_lock);
		kref_put(&usr->ref_count, free_usr);
		mutex_lock(&qddev->users_mutex);
	}
	mutex_unlock(&qddev->users_mutex);
}

/* Caller must ensure synchronization */
static void __qaic_destroy_part_drm_dev(struct qaic_drm_device *qddev)
{
		list_del(&qddev->part_dev);
		qaic_destroy_drm_device(qddev);
		/*
		 * Although we use devm_drm_dev_alloc() to allocate device partitions we have
		 * to separately call drm_dev_put() because the parent device (accel) of device
		 * partitions do not call drm_dev_put() when they are going away. This expectation
		 * is fulfilled by devices with parent device as PCI device.
		 */
		drm_dev_put(to_drm(qddev));
}

static void qaic_destroy_all_part_drm_dev(struct qaic_device *qdev)
{
	struct qaic_drm_device *qddev, *n;

	mutex_lock(&qdev->part_devs_mutex);
	list_for_each_entry_safe(qddev, n, &qdev->part_devs, part_dev)
		__qaic_destroy_part_drm_dev(qddev);
	mutex_unlock(&qdev->part_devs_mutex);
}

static int qaic_destroy_part_drm_dev(struct qaic_device *qdev, s32 partition_id,
				     struct qaic_user *usr)
{
	struct qaic_drm_device *qddev, *n;
	int ret = -ENODEV;

	mutex_lock(&qdev->part_devs_mutex);
	list_for_each_entry_safe(qddev, n, &qdev->part_devs, part_dev)
		if ((partition_id == QAIC_NO_PARTITION || qddev->partition_id == partition_id) &&
		    qddev->owner == usr) {
			__qaic_destroy_part_drm_dev(qddev);
			ret = 0;
		}
	mutex_unlock(&qdev->part_devs_mutex);

	return ret;
}

static int qaic_mhi_probe(struct mhi_device *mhi_dev, const struct mhi_device_id *id)
{
	u16 major = -1, minor = -1;
	struct qaic_device *qdev;
	int ret;

	/*
	 * Invoking this function indicates that the control channel to the
	 * device is available. We use that as a signal to indicate that
	 * the device side firmware has booted. The device side firmware
	 * manages the device resources, so we need to communicate with it
	 * via the control channel in order to utilize the device. Therefore
	 * we wait until this signal to create the drm dev that userspace will
	 * use to control the device, because without the device side firmware,
	 * userspace can't do anything useful.
	 */

	qdev = pci_get_drvdata(to_pci_dev(mhi_dev->mhi_cntrl->cntrl_dev));

	dev_set_drvdata(&mhi_dev->dev, qdev);
	qdev->cntl_ch = mhi_dev;

	ret = qaic_control_open(qdev);
	if (ret) {
		pci_dbg(qdev->pdev, "%s: control_open failed %d\n", __func__, ret);
		return ret;
	}

	qdev->reset_state = QAIC_BOOT;
	ret = get_cntl_version(qdev, NULL, &major, &minor);
	if (ret || major != CNTL_MAJOR || minor > CNTL_MINOR) {
		pci_err(qdev->pdev, "%s: Control protocol version (%d.%d) not supported. Supported version is (%d.%d). Ret: %d\n",
			__func__, major, minor, CNTL_MAJOR, CNTL_MINOR, ret);
		ret = -EINVAL;
		goto close_control;
	}
	qdev->reset_state = QAIC_ONLINE;
	kobject_uevent(&(to_accel_kdev(qdev->qddev))->kobj, KOBJ_ONLINE);

	return ret;

close_control:
	qaic_control_close(qdev);
	return ret;
}

static void qaic_mhi_remove(struct mhi_device *mhi_dev)
{
/* This is redundant since we have already observed the device crash */
}

static void qaic_notify_reset(struct qaic_device *qdev)
{
	int i;

	kobject_uevent(&(to_accel_kdev(qdev->qddev))->kobj, KOBJ_OFFLINE);
	qdev->reset_state = QAIC_OFFLINE;
	/* wake up any waiters to avoid waiting for timeouts at sync */
	wake_all_cntl(qdev);
	wake_all_telemetry(qdev);
	for (i = 0; i < qdev->num_dbc; ++i)
		wakeup_dbc(qdev, i);
	synchronize_srcu(&qdev->dev_lock);
}

void qaic_dev_reset_clean_local_state(struct qaic_device *qdev)
{
	int i;

	qaic_notify_reset(qdev);

	/* remove drmdev partitions since QSM won't recognize them */
	qaic_destroy_all_part_drm_dev(qdev);

	/* start tearing things down */
	clean_up_ssr(qdev);
	for (i = 0; i < qdev->num_dbc; ++i)
		release_dbc(qdev, i);
}

static struct qaic_device *create_qdev(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct qaic_drm_device *qddev;
	struct qaic_device *qdev;
	struct drm_device *drm;
	int i, ret;

	qdev = devm_kzalloc(dev, sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return NULL;

	qdev->reset_state = QAIC_OFFLINE;
	if (id->device == PCI_DEV_AIC100) {
		qdev->num_dbc = 16;
		qdev->dbc = devm_kcalloc(dev, qdev->num_dbc, sizeof(*qdev->dbc), GFP_KERNEL);
		if (!qdev->dbc)
			return NULL;
	}

	qddev = qaic_drm_dev_alloc(qdev, dev);
	if (IS_ERR(qddev))
		return NULL;

	drm = to_drm(qddev);
	pci_set_drvdata(pdev, qdev);

	ret = drmm_add_action_or_reset(drm, qaicm_pci_release, NULL);
	if (ret)
		return NULL;
	ret = drmm_mutex_init(drm, &qdev->cntl_mutex);
	if (ret)
		return NULL;
	ret = drmm_mutex_init(drm, &qdev->bootlog_mutex);
	if (ret)
		return NULL;
	ret = drmm_mutex_init(drm, &qdev->tele_mutex);
	if (ret)
		return NULL;
	ret = drmm_mutex_init(drm, &qdev->part_devs_mutex);
	if (ret)
		return NULL;

	qdev->cntl_wq = qaicm_wq_init(drm, "qaic_cntl");
	if (IS_ERR(qdev->cntl_wq))
		return NULL;
	qdev->ssr_wq = qaicm_wq_init(drm, "qaic_ssr");
	if (IS_ERR(qdev->ssr_wq))
		return NULL;
	qdev->tele_wq = qaicm_wq_init(drm, "qaic_tele");
	if (IS_ERR(qdev->tele_wq))
		return NULL;
	qdev->qts_wq = qaicm_wq_init(drm, "qaic_ts");
	if (IS_ERR(qdev->qts_wq))
		return NULL;

	ret = qaicm_srcu_init(drm, &qdev->dev_lock);
	if (ret)
		return NULL;

	ret = ssr_init(qdev, drm);
	if (ret)
		pci_info(pdev, "QAIC SSR crashdump collection not supported (No memory).\n");

	qdev->qddev = qddev;
	qdev->pdev = pdev;
	INIT_LIST_HEAD(&qdev->cntl_xfer_list);
	INIT_LIST_HEAD(&qdev->bootlog);
	INIT_LIST_HEAD(&qdev->tele_xfer_list);
	INIT_LIST_HEAD(&qdev->part_devs);

	for (i = 0; i < qdev->num_dbc; ++i) {
		spin_lock_init(&qdev->dbc[i].xfer_lock);
		qdev->dbc[i].qdev = qdev;
		qdev->dbc[i].id = i;
		INIT_LIST_HEAD(&qdev->dbc[i].xfer_list);
		ret = qaicm_srcu_init(drm, &qdev->dbc[i].ch_lock);
		if (ret)
			return NULL;
		init_waitqueue_head(&qdev->dbc[i].dbc_release);
		INIT_LIST_HEAD(&qdev->dbc[i].bo_lists);
	}

	/* Since this is base DRM device, it won't be part of ->part_devs list */
	INIT_LIST_HEAD(&qddev->part_dev);

	return qdev;
}

static int init_pci(struct qaic_device *qdev, struct pci_dev *pdev)
{
	int bars;
	int ret;

	bars = pci_select_bars(pdev, IORESOURCE_MEM);

	/* make sure the device has the expected BARs */
	if (bars != (BIT(0) | BIT(2) | BIT(4))) {
		pci_dbg(pdev, "%s: expected BARs 0, 2, and 4 not found in device. Found 0x%x\n",
			__func__, bars);
		return -EINVAL;
	}

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	ret = dma_set_max_seg_size(&pdev->dev, UINT_MAX);
	if (ret)
		return ret;

	qdev->bar_0 = devm_ioremap_resource(&pdev->dev, &pdev->resource[0]);
	if (IS_ERR(qdev->bar_0))
		return PTR_ERR(qdev->bar_0);

	qdev->bar_2 = devm_ioremap_resource(&pdev->dev, &pdev->resource[2]);
	if (IS_ERR(qdev->bar_2))
		return PTR_ERR(qdev->bar_2);

	/* Managed release since we use pcim_enable_device above */
	pci_set_master(pdev);

	return 0;
}

static int init_msi(struct qaic_device *qdev, struct pci_dev *pdev)
{
	int mhi_irq;
	int ret;
	int i;

	/* Managed release since we use pcim_enable_device */
	ret = pci_alloc_irq_vectors(pdev, 32, 32, PCI_IRQ_MSI);
	if (ret == -ENOSPC) {
		ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
		if (ret < 0)
			return ret;

		/*
		 * Operate in one MSI mode. All interrupts will be directed to
		 * MSI0; every interrupt will wake up all the interrupt handlers
		 * (MHI and DBC[0-15]). Since the interrupt is now shared, it is
		 * not disabled during DBC threaded handler, but only one thread
		 * will be allowed to run per DBC, so while it can be
		 * interrupted, it shouldn't race with itself.
		 */
		qdev->single_msi = true;
		pci_info(qdev->pdev, "Allocating 32 MSIs failed, operating in 1 MSI mode. Performance may be impacted.\n");
	} else if (ret < 0) {
		return ret;
	}

	mhi_irq = pci_irq_vector(pdev, 0);
	if (mhi_irq < 0)
		return mhi_irq;

	for (i = 0; i < qdev->num_dbc; ++i) {
		ret = devm_request_threaded_irq(&pdev->dev,
						pci_irq_vector(pdev, qdev->single_msi ? 0 : i + 1),
						dbc_irq_handler, dbc_irq_threaded_fn, IRQF_SHARED,
						"qaic_dbc", &qdev->dbc[i]);
		if (ret)
			return ret;

		if (datapath_polling) {
			qdev->dbc[i].irq = pci_irq_vector(pdev, qdev->single_msi ? 0 : i + 1);
			if (!qdev->single_msi)
				disable_irq_nosync(qdev->dbc[i].irq);
			INIT_WORK(&qdev->dbc[i].poll_work, irq_polling_work);
		}
	}

	return mhi_irq;
}

static int qaic_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct qaic_device *qdev;
	int mhi_irq;
	int ret;
	int i;

	qdev = create_qdev(pdev, id);
	if (!qdev)
		return -ENOMEM;

	ret = init_pci(qdev, pdev);
	if (ret)
		return ret;

	for (i = 0; i < qdev->num_dbc; ++i)
		qdev->dbc[i].dbc_base = qdev->bar_2 + QAIC_DBC_OFF(i);

	mhi_irq = init_msi(qdev, pdev);
	if (mhi_irq < 0)
		return mhi_irq;

	ret = qaic_create_drm_device(qdev);
	if (ret)
		return ret;

	qdev->mhi_cntrl = qaic_mhi_register_controller(pdev, qdev->bar_0, mhi_irq,
						       qdev->single_msi);
	if (IS_ERR(qdev->mhi_cntrl)) {
		ret = PTR_ERR(qdev->mhi_cntrl);
		qaic_destroy_drm_device(qdev->qddev);
		return ret;
	}

	return 0;
}

static void qaic_pci_remove(struct pci_dev *pdev)
{
	struct qaic_device *qdev = pci_get_drvdata(pdev);

	if (!qdev)
		return;

	qaic_dev_reset_clean_local_state(qdev);
	qaic_mhi_free_controller(qdev->mhi_cntrl, link_up);
	qaic_destroy_drm_device(qdev->qddev);
}

static void qaic_pci_shutdown(struct pci_dev *pdev)
{
	/* see qaic_exit for what link_up is doing */
	link_up = true;
	qaic_pci_remove(pdev);
}

static pci_ers_result_t qaic_pci_error_detected(struct pci_dev *pdev, pci_channel_state_t error)
{
	return PCI_ERS_RESULT_NEED_RESET;
}

#ifdef _QBP_HAS_PCI_ERROR_RESET_PREPDONE
static void qaic_pci_reset_prepare(struct pci_dev *pdev)
{
	struct qaic_device *qdev = pci_get_drvdata(pdev);

	qaic_notify_reset(qdev);
	qaic_mhi_start_reset(qdev->mhi_cntrl);
	qaic_dev_reset_clean_local_state(qdev);
}
#endif /* end _QBP_HAS_PCI_ERROR_RESET_PREPDONE */

#ifdef _QBP_HAS_PCI_ERROR_RESET_PREPDONE
static void qaic_pci_reset_done(struct pci_dev *pdev)
{
	struct qaic_device *qdev = pci_get_drvdata(pdev);

	qaic_mhi_reset_done(qdev->mhi_cntrl);
}
#elif defined(_QBP_HAS_PCI_ERROR_RESET_NOTIFY)
static void qaic_pci_reset_notify(struct pci_dev *pdev, bool prepare) {
	struct qaic_device *qdev = pci_get_drvdata(pdev);
	if (prepare) {
		qaic_notify_reset(qdev);
		qaic_mhi_start_reset(qdev->mhi_cntrl);
		qaic_dev_reset_clean_local_state(qdev);
	}
	else {
		qaic_mhi_reset_done(qdev->mhi_cntrl);
	}
}
#endif /* end _QBP_HAS_PCI_ERROR_RESET_PREPDONE or _QBP_HAS_PCI_ERROR_RESET_NOTIFY */

static const struct mhi_device_id qaic_mhi_match_table[] = {
	{ .chan = "QAIC_CONTROL", },
	{},
};

static struct mhi_driver qaic_mhi_driver = {
	.id_table = qaic_mhi_match_table,
	.remove = qaic_mhi_remove,
	.probe = qaic_mhi_probe,
	.ul_xfer_cb = qaic_mhi_ul_xfer_cb,
	.dl_xfer_cb = qaic_mhi_dl_xfer_cb,
	.driver = {
		.name = "qaic_mhi",
	},
};

static const struct pci_device_id qaic_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_QCOM, PCI_DEV_AIC100), },
	{ }
};
MODULE_DEVICE_TABLE(pci, qaic_ids);

static const struct pci_error_handlers qaic_pci_err_handler = {
	.error_detected = qaic_pci_error_detected,
#ifdef _QBP_HAS_PCI_ERROR_RESET_PREPDONE
	.reset_prepare = qaic_pci_reset_prepare,
	.reset_done = qaic_pci_reset_done,
#else
#ifdef _QBP_HAS_PCI_ERROR_RESET_NOTIFY
	.reset_notify = qaic_pci_reset_notify,
#endif /* end _QBP_HAS_PCI_ERROR_RESET_NOTIFY */
#endif /* end _QBP_HAS_PCI_ERROR_RESET_PREPDONE */

};

static struct pci_driver qaic_pci_driver = {
	.name = QAIC_NAME,
	.id_table = qaic_ids,
	.probe = qaic_pci_probe,
	.remove = qaic_pci_remove,
	.shutdown = qaic_pci_shutdown,
	.err_handler = &qaic_pci_err_handler,
};

static int __init qaic_init(void)
{
	int ret;
#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
	ret = accel_init();
	if (ret) {
		pr_debug("qaic: accel_init failed %d\n", ret);
		return ret;
	}
#endif

	ret = pci_register_driver(&qaic_pci_driver);
	if (ret) {
		pr_debug("qaic: pci_register_driver failed %d\n", ret);
#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
		goto free_accel;
#else
		return ret;
#endif
	}

	ret = mhi_driver_register(&qaic_mhi_driver);
	if (ret) {
		pr_debug("qaic: mhi_driver_register failed %d\n", ret);
		goto free_pci;
	}

	ret = qaic_bootlog_register();
	if (ret) {
		pr_debug("qaic: qaic_bootlog_register failed %d\n", ret);
		goto free_mhi;
	}

	ret = qaic_ssr_register();
	if (ret) {
		pr_debug("qaic: qaic_ssr_register failed %d\n", ret);
		goto free_bootlog;
	}

	ret = qaic_ras_register();
	if (ret) {
		pr_debug("qaic: qaic_ras_register failed %d\n", ret);
		goto free_ssr;
	}

	ret = qaic_telemetry_register();
	if (ret) {
		pr_debug("qaic: qaic_telemetry_register failed %d\n", ret);
		goto free_ras;
	}

	ret = qaic_timesync_init();
	if (ret) {
		pr_debug("qaic: qaic_timesync_init failed %d\n", ret);
		goto free_telemetry;
	}

	ret = mhi_qaic_ctrl_init();
	if (ret) {
		pr_debug("qaic: mhi_qaic_ctrl_init failed %d\n", ret);
		goto free_timesync;
	}

	return 0;

free_timesync:
	qaic_timesync_deinit();
free_telemetry:
	qaic_telemetry_unregister();
free_ras:
	qaic_ras_unregister();
free_ssr:
	qaic_ssr_unregister();
free_bootlog:
	qaic_bootlog_unregister();
free_mhi:
	mhi_driver_unregister(&qaic_mhi_driver);
free_pci:
	pci_unregister_driver(&qaic_pci_driver);
#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
free_accel:
	accel_exit();
#endif
	return ret;
}

static void __exit qaic_exit(void)
{
	/*
	 * We assume that qaic_pci_remove() is called due to a hotplug event
	 * which would mean that the link is down, and thus
	 * qaic_mhi_free_controller() should not try to access the device during
	 * cleanup.
	 * We call pci_unregister_driver() below, which also triggers
	 * qaic_pci_remove(), but since this is module exit, we expect the link
	 * to the device to be up, in which case qaic_mhi_free_controller()
	 * should try to access the device during cleanup to put the device in
	 * a sane state.
	 * For that reason, we set link_up here to let qaic_mhi_free_controller
	 * know the expected link state. Since the module is going to be
	 * removed at the end of this, we don't need to worry about
	 * reinitializing the link_up state after the cleanup is done.
	 */
	link_up = true;
	mhi_qaic_ctrl_deinit();
	qaic_timesync_deinit();
	qaic_telemetry_unregister();
	qaic_ras_unregister();
	qaic_ssr_unregister();
	qaic_bootlog_unregister();
	mhi_driver_unregister(&qaic_mhi_driver);
	pci_unregister_driver(&qaic_pci_driver);
#ifdef _QBP_NEED_DRM_ACCEL_FRAMEWORK
	accel_exit();
#endif
}

module_init(qaic_init);
module_exit(qaic_exit);

MODULE_AUTHOR(QAIC_DESC " Kernel Driver Team");
MODULE_DESCRIPTION(QAIC_DESC " Accel Driver");
MODULE_LICENSE("GPL");
