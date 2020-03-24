/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/ratelimit.h>
#include <dsp/hw-vote-rsc.h>

struct mutex hw_vote_lock;
static bool is_init_done;

int hw_vote_rsc_enable(struct clk* vote_handle)
{
	int ret = 0;

	if (!is_init_done || vote_handle == NULL) {
		pr_err_ratelimited("%s: init failed or vote handle NULL\n",
				   __func__);
		return -EINVAL;
	}

	mutex_lock(&hw_vote_lock);
	ret = clk_prepare_enable(vote_handle);
	mutex_unlock(&hw_vote_lock);

	trace_printk("%s\n", __func__);
	return ret;
}
EXPORT_SYMBOL(hw_vote_rsc_enable);

void hw_vote_rsc_disable(struct clk* vote_handle)
{
	if (!is_init_done || vote_handle == NULL) {
		pr_err_ratelimited("%s: init failed or vote handle NULL\n",
				   __func__);
		return;
	}

	mutex_lock(&hw_vote_lock);
	clk_disable_unprepare(vote_handle);
	mutex_unlock(&hw_vote_lock);
	trace_printk("%s\n", __func__);
}
EXPORT_SYMBOL(hw_vote_rsc_disable);

void hw_vote_rsc_reset(struct clk* vote_handle)
{
	int count = 0;

	if (!is_init_done || vote_handle == NULL) {
		pr_err_ratelimited("%s: init failed or vote handle NULL\n",
				   __func__);
		return;
	}

	mutex_lock(&hw_vote_lock);
	while (__clk_is_enabled(vote_handle)) {
		clk_disable_unprepare(vote_handle);
		count++;
	}
	pr_debug("%s: Vote count after SSR: %d\n", __func__, count);
	trace_printk("%s: Vote count after SSR: %d\n", __func__, count);

	while (count--)
		clk_prepare_enable(vote_handle);
	mutex_unlock(&hw_vote_lock);
}
EXPORT_SYMBOL(hw_vote_rsc_reset);

void hw_vote_rsc_init()
{
	mutex_init(&hw_vote_lock);
	is_init_done = true;
}

void hw_vote_rsc_exit()
{
	mutex_destroy(&hw_vote_lock);
	is_init_done = false;
}
