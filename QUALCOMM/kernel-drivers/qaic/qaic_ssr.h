/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021, 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __QAIC_SSR_H__
#define __QAIC_SSR_H__

#ifdef _QBP_INCLUDE_HAS_DRM_DEVICE
#include <drm/drm_device.h>
#else
#include <drm/drmP.h>
#endif

int qaic_ssr_register(void);
void qaic_ssr_unregister(void);
void clean_up_ssr(struct qaic_device *qdev);
int ssr_init(struct qaic_device *qdev, struct drm_device *drm);
#endif /* __QAIC_SSR_H__ */
