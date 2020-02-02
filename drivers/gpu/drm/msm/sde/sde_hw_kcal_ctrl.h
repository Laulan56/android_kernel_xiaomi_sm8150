/*
 * Copyright (C) 2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define SDE_HW_KCAL_ENABLED		(1)

#define SDE_HW_KCAL_MIN_VALUE		(20)
#define SDE_HW_KCAL_INIT_RED		(256)
#define SDE_HW_KCAL_INIT_GREEN		(256)
#define SDE_HW_KCAL_INIT_BLUE		(256)

#define SDE_HW_KCAL_INIT_HUE		(0)
#define SDE_HW_KCAL_INIT_ADJ		(255)

struct sde_hw_kcal_pcc {
	u32 red;
	u32 green;
	u32 blue;
};

struct sde_hw_kcal_hsic {
	u32 hue;
	u32 saturation;
	u32 value;
	u32 contrast;
};

struct sde_hw_kcal {
	struct sde_hw_kcal_pcc pcc;
	struct sde_hw_kcal_hsic hsic;

	u32 enabled:1;
	u32 min_value;
};

#ifdef CONFIG_DRM_MSM_KCAL_CTRL
/**
 * sde_hw_kcal_get() - get a handle to internal kcal calibration plan.
 *
 * Pointer is used here for performance reasons. Races are expected in
 * color processing code.
 */
struct sde_hw_kcal *sde_hw_kcal_get(void);

/**
 * sde_hw_kcal_hsic_struct() - get hsic configuration structure with
 * applied adjustments from kcal.
 */
struct drm_msm_pa_hsic sde_hw_kcal_hsic_struct(void);

/**
 * sde_hw_kcal_pcc_adjust() - change rgb colors according to kcal setup.
 * @data: data array of pcc coefficients.
 * @plane: index of pcc color plane.
 */
void sde_hw_kcal_pcc_adjust(u32 *data, int plane);
#else
static inline struct sde_hw_kcal sde_hw_kcal_get(void)
{
	return (struct sde_hw_kcal) { .enabled = false };
}

static inline struct drm_msm_pa_hsic sde_hw_kcal_hsic_struct(void)
{
	return (struct drm_msm_pa_hsic) { .flags = 0 };
}

static inline void sde_hw_kcal_pcc_adjust(u32 *data, int plane)
{

}
#endif
