/* SPDX-License-Identifier: GPL-2.0-only */

/* Copyright (c) 2020, The Linux Foundation. All rights reserved. */

#ifndef __QAIC_DEBUGFS_H__
#define __QAIC_DEBUGFS_H__

#ifdef _QBP_INCLUDE_HAS_DRM_FILE
#ifdef _QBP_INCLUDE_FIX_DRM_FILE
/* needed before drm_file.h since it doesn't include idr yet uses them */
#include <linux/idr.h>
#endif
#include <drm/drm_file.h>
#endif

#define DBC_DEBUGFS_ENTRIES			2

#ifdef CONFIG_DEBUG_FS
int qaic_bootlog_register(void);
void qaic_bootlog_unregister(void);
#ifdef _QBP_HAS_INT_DEBUGFS_INIT_RETVAL
int qaic_debugfs_init(struct drm_minor *minor);
#else
void qaic_debugfs_init(struct drm_minor *minor);
#endif
#ifdef _QBP_HAS_DRM_DRV_DEBUGFS_CLEANUP
void qaic_debugfs_cleanup(struct drm_minor *minor);
#endif
#else
int qaic_bootlog_register(void) { return 0; }
void qaic_bootlog_unregister(void) {}
#ifdef _QBP_HAS_INT_DEBUGFS_INIT_RETVAL
int qaic_debugfs_init(struct drm_minor *minor){
	
	return 0;
}
#else
void qaic_debugfs_init(struct drm_minor *minor) {}
#endif
#ifdef _QBP_HAS_DRM_DRV_DEBUGFS_CLEANUP
void qaic_debugfs_cleanup(struct drm_minor *minor){
	struct qaic_device *qdev = to_qaic_drm_device(minor->dev)->qdev;
	uint16_t i;

	drm_debugfs_remove_files(qaic_debugfs_list, QAIC_DEBUGFS_ENTRIES,
				 minor);
	for(i = 0;i < qdev->num_dbc;++i)
		debugfs_remove_recursive((&qdev->dbc[i])->debugfs_root);
}
#endif
#ifdef _QBP_HAS_DRM_DRV_DEBUGFS_CLEANUP
void qaic_debugfs_cleanup(struct drm_minor *minor) {
}
#endif
#endif /* CONFIG_DEBUG_FS */
#endif /* __QAIC_DEBUGFS_H__ */
