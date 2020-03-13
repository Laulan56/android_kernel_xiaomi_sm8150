/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#ifndef HW_VOTE_RSC_H
#define HW_VOTE_RSC_H

int hw_vote_rsc_enable(struct clk* vote_handle);
void hw_vote_rsc_disable(struct clk* vote_handle);
void hw_vote_rsc_reset(struct clk* vote_handle);
void hw_vote_rsc_init(void);
void hw_vote_rsc_exit(void);

#endif /* HW_VOTE_RSC_H */
