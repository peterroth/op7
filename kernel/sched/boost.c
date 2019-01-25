/* Copyright (c) 2012-2019, The Linux Foundation. All rights reserved.
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

#include "sched.h"
#include "walt.h"
#include <linux/of.h>
#include <linux/sched/core_ctl.h>
#include <trace/events/sched.h>

/*
 * Scheduler boost is a mechanism to temporarily place tasks on CPUs
 * with higher capacity than those where a task would have normally
 * ended up with their load characteristics. Any entity enabling
 * boost is responsible for disabling it as well.
 */

unsigned int sysctl_sched_boost; /* To/from userspace */
unsigned int sched_boost_type; /* currently activated sched boost */
enum sched_boost_policy boost_policy;

static enum sched_boost_policy boost_policy_dt = SCHED_BOOST_NONE;
static DEFINE_MUTEX(boost_mutex);
static int boost_refcount[MAX_NUM_BOOST_TYPE];

/*
 * Scheduler boost type and boost policy might at first seem unrelated,
 * however, there exists a connection between them that will allow us
 * to use them interchangeably during placement decisions. We'll explain
 * the connection here in one possible way so that the implications are
 * clear when looking at placement policies.
 *
 * When policy = SCHED_BOOST_NONE, type is either none or RESTRAINED
 * When policy = SCHED_BOOST_ON_ALL or SCHED_BOOST_ON_BIG, type can
 * neither be none nor RESTRAINED.
 */
static void set_boost_policy(int type)
{
	if (type == NO_BOOST || type == RESTRAINED_BOOST) {
		boost_policy = SCHED_BOOST_NONE;
		return;
	}

	if (boost_policy_dt) {
		boost_policy = boost_policy_dt;
		return;
	}

	if (min_possible_efficiency != max_possible_efficiency) {
		boost_policy = SCHED_BOOST_ON_BIG;
		return;
	}

	boost_policy = SCHED_BOOST_ON_ALL;
}

static bool verify_boost_params(int type)
{
	return type >= RESTRAINED_BOOST_DISABLE && type <= RESTRAINED_BOOST;
}

static void _sched_set_boost(int type)
{
	switch (type) {
	case NO_BOOST: /* All boost clear */
		if (boost_refcount[FULL_THROTTLE_BOOST] > 0) {
			core_ctl_set_boost(false);
			walt_enable_frequency_aggregation(false);
			boost_refcount[FULL_THROTTLE_BOOST] = 0;
		}
		if (boost_refcount[CONSERVATIVE_BOOST] > 0) {
			restore_cgroup_boost_settings();
			boost_refcount[CONSERVATIVE_BOOST] = 0;
		}
		if (boost_refcount[RESTRAINED_BOOST] > 0) {
			walt_enable_frequency_aggregation(false);
			boost_refcount[RESTRAINED_BOOST] = 0;
		}
		break;

	case FULL_THROTTLE_BOOST:
	    boost_refcount[FULL_THROTTLE_BOOST]++;
		if (boost_refcount[FULL_THROTTLE_BOOST] == 1) {
			core_ctl_set_boost(true);
			restore_cgroup_boost_settings();
			if (!boost_refcount[RESTRAINED_BOOST])
				walt_enable_frequency_aggregation(true);
		}
		break;

	case CONSERVATIVE_BOOST:
	    boost_refcount[CONSERVATIVE_BOOST]++;
		if ((boost_refcount[CONSERVATIVE_BOOST] == 1) &&
				!boost_refcount[FULL_THROTTLE_BOOST])
			update_cgroup_boost_settings();
		break;

	case RESTRAINED_BOOST:
	    boost_refcount[RESTRAINED_BOOST]++;
		if (boost_refcount[RESTRAINED_BOOST] == 1 &&
		    !boost_refcount[FULL_THROTTLE_BOOST])
			walt_enable_frequency_aggregation(true);
		break;

	case FULL_THROTTLE_BOOST_DISABLE:
		if (boost_refcount[FULL_THROTTLE_BOOST] >= 1) {
			boost_refcount[FULL_THROTTLE_BOOST]--;
			if (!boost_refcount[FULL_THROTTLE_BOOST]) {
				core_ctl_set_boost(false);
				if (boost_refcount[CONSERVATIVE_BOOST] >= 1)
					update_cgroup_boost_settings();
				if (!boost_refcount[RESTRAINED_BOOST])
					walt_enable_frequency_aggregation(
								false);
			}
		}
		break;

	case CONSERVATIVE_BOOST_DISABLE:
		if (boost_refcount[CONSERVATIVE_BOOST] >= 1) {
			boost_refcount[CONSERVATIVE_BOOST]--;
			if (!boost_refcount[CONSERVATIVE_BOOST])
				restore_cgroup_boost_settings();
		}
		break;

	case RESTRAINED_BOOST_DISABLE:
		if (boost_refcount[RESTRAINED_BOOST] >= 1) {
			boost_refcount[RESTRAINED_BOOST]--;
			if (!boost_refcount[RESTRAINED_BOOST] &&
			    !boost_refcount[FULL_THROTTLE_BOOST])
				walt_enable_frequency_aggregation(false);
		}
		break;

	default:
		WARN_ON(1);
		return;
	}

	/* Aggregate final boost type */
	if (boost_refcount[FULL_THROTTLE_BOOST] >= 1)
		type = FULL_THROTTLE_BOOST;
	else if (boost_refcount[CONSERVATIVE_BOOST] >= 1)
		type = CONSERVATIVE_BOOST;
	else if (boost_refcount[RESTRAINED_BOOST] >= 1)
		type = RESTRAINED_BOOST;
	else
		type = NO_BOOST;

	sched_boost_type = type;
	sysctl_sched_boost = type;

	set_boost_policy(type);
	trace_sched_set_boost(type);
}

void sched_boost_parse_dt(void)
{
	struct device_node *sn;
	const char *boost_policy;

	sn = of_find_node_by_path("/sched-hmp");
	if (!sn)
		return;

	if (!of_property_read_string(sn, "boost-policy", &boost_policy)) {
		if (!strcmp(boost_policy, "boost-on-big"))
			boost_policy_dt = SCHED_BOOST_ON_BIG;
		else if (!strcmp(boost_policy, "boost-on-all"))
			boost_policy_dt = SCHED_BOOST_ON_ALL;
	}
}

int sched_set_boost(int type)
{
	int ret = 0;

	mutex_lock(&boost_mutex);
	if (verify_boost_params(type))
		_sched_set_boost(type);
	else
		ret = -EINVAL;
	mutex_unlock(&boost_mutex);
	return ret;
}

int sched_boost_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int ret;
	unsigned int *data = (unsigned int *)table->data;

	mutex_lock(&boost_mutex);

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write)
		goto done;

	if (verify_boost_params(*data))
		_sched_set_boost(*data);
	else
		ret = -EINVAL;

done:
	mutex_unlock(&boost_mutex);
	return ret;
}
