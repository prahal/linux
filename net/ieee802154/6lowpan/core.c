/* Copyright 2011, Siemens AG
 * written by Alexander Smirnov <alex.bluesman.smirnov@gmail.com>
 */

/* Based on patches from Jon Smirl <jonsmirl@gmail.com>
 * Copyright (c) 2011 Jon Smirl <jonsmirl@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* Jon's code is based on 6lowpan implementation for Contiki which is:
 * Copyright (c) 2008, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/ieee802154.h>

#include <net/ipv6.h>

#include "6lowpan_i.h"

static int open_count;

static struct header_ops lowpan_header_ops = {
	.create	= lowpan_header_create,
};

static struct lock_class_key lowpan_tx_busylock;
static struct lock_class_key lowpan_netdev_xmit_lock_key;

static void lowpan_set_lockdep_class_one(struct net_device *dev,
					 struct netdev_queue *txq,
					 void *_unused)
{
	lockdep_set_class(&txq->_xmit_lock,
			  &lowpan_netdev_xmit_lock_key);
}

static int lowpan_dev_init(struct net_device *dev)
{
	netdev_for_each_tx_queue(dev, lowpan_set_lockdep_class_one, NULL);
	dev->qdisc_tx_busylock = &lowpan_tx_busylock;
	return 0;
}

static const struct net_device_ops lowpan_netdev_ops = {
	.ndo_init		= lowpan_dev_init,
	.ndo_start_xmit		= lowpan_xmit,
};

static void lowpan_setup(struct net_device *dev)
{
	dev->addr_len		= IEEE802154_ADDR_LEN;
	memset(dev->broadcast, 0xff, IEEE802154_ADDR_LEN);
	dev->type		= ARPHRD_6LOWPAN;
	/* Frame Control + Sequence Number + Address fields + Security Header */
	dev->hard_header_len	= 2 + 1 + 20 + 14;
	dev->needed_tailroom	= 2; /* FCS */
	dev->mtu		= IPV6_MIN_MTU;
	dev->tx_queue_len	= 0;
	dev->flags		= IFF_BROADCAST | IFF_MULTICAST;
	dev->watchdog_timeo	= 0;

	dev->netdev_ops		= &lowpan_netdev_ops;
	dev->header_ops		= &lowpan_header_ops;
	dev->destructor		= free_netdev;
	dev->features		|= NETIF_F_NETNS_LOCAL;
}

static int lowpan_validate(struct nlattr *tb[], struct nlattr *data[])
{
	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != IEEE802154_ADDR_LEN)
			return -EINVAL;
	}
	return 0;
}

static int lowpan_newlink(struct net *src_net, struct net_device *dev,
			  struct nlattr *tb[], struct nlattr *data[])
{
	struct net_device *real_dev;
	int ret;

	ASSERT_RTNL();

	pr_debug("adding new link\n");

	if (!tb[IFLA_LINK] ||
	    !net_eq(dev_net(dev), &init_net))
		return -EINVAL;
	/* find and hold real wpan device */
	real_dev = dev_get_by_index(dev_net(dev), nla_get_u32(tb[IFLA_LINK]));
	if (!real_dev)
		return -ENODEV;
	if (real_dev->type != ARPHRD_IEEE802154) {
		dev_put(real_dev);
		return -EINVAL;
	}

	if (real_dev->ieee802154_ptr->lowpan_dev) {
		dev_put(real_dev);
		return -EBUSY;
	}

	lowpan_dev_info(dev)->real_dev = real_dev;
	/* Set the lowpan hardware address to the wpan hardware address. */
	memcpy(dev->dev_addr, real_dev->dev_addr, IEEE802154_ADDR_LEN);

	lowpan_netdev_setup(dev, LOWPAN_LLTYPE_IEEE802154);

	ret = register_netdevice(dev);
	if (ret < 0) {
		dev_put(real_dev);
		return ret;
	}

	real_dev->ieee802154_ptr->lowpan_dev = dev;
	if (!open_count)
		lowpan_rx_init();

	open_count++;

	return 0;
}

static void lowpan_dellink(struct net_device *dev, struct list_head *head)
{
	struct lowpan_dev_info *lowpan_dev = lowpan_dev_info(dev);
	struct net_device *real_dev = lowpan_dev->real_dev;

	ASSERT_RTNL();

	open_count--;

	if (!open_count)
		lowpan_rx_exit();

	real_dev->ieee802154_ptr->lowpan_dev = NULL;
	unregister_netdevice(dev);
	dev_put(real_dev);
}

static struct rtnl_link_ops lowpan_link_ops __read_mostly = {
	.kind		= "lowpan",
	.priv_size	= LOWPAN_PRIV_SIZE(sizeof(struct lowpan_dev_info)),
	.setup		= lowpan_setup,
	.newlink	= lowpan_newlink,
	.dellink	= lowpan_dellink,
	.validate	= lowpan_validate,
};

static inline int __init lowpan_netlink_init(void)
{
	return rtnl_link_register(&lowpan_link_ops);
}

static inline void lowpan_netlink_fini(void)
{
	rtnl_link_unregister(&lowpan_link_ops);
}

static int lowpan_device_event(struct notifier_block *unused,
			       unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (dev->type != ARPHRD_IEEE802154)
		goto out;

	switch (event) {
	case NETDEV_UNREGISTER:
		/* Check if wpan interface is unregistered that we
		 * also delete possible lowpan interfaces which belongs
		 * to the wpan interface.
		 */
		if (dev->ieee802154_ptr && dev->ieee802154_ptr->lowpan_dev)
			lowpan_dellink(dev->ieee802154_ptr->lowpan_dev, NULL);
		break;
	default:
		break;
	}

out:
	return NOTIFY_DONE;
}

static struct notifier_block lowpan_dev_notifier = {
	.notifier_call = lowpan_device_event,
};

static int __init lowpan_init_module(void)
{
	int err = 0;

	err = lowpan_net_frag_init();
	if (err < 0)
		goto out;

	err = lowpan_netlink_init();
	if (err < 0)
		goto out_frag;

	err = register_netdevice_notifier(&lowpan_dev_notifier);
	if (err < 0)
		goto out_pack;

	return 0;

out_pack:
	lowpan_netlink_fini();
out_frag:
	lowpan_net_frag_exit();
out:
	return err;
}

static void __exit lowpan_cleanup_module(void)
{
	lowpan_netlink_fini();

	lowpan_net_frag_exit();

	unregister_netdevice_notifier(&lowpan_dev_notifier);
}

module_init(lowpan_init_module);
module_exit(lowpan_cleanup_module);
MODULE_LICENSE("GPL");
MODULE_ALIAS_RTNL_LINK("lowpan");
