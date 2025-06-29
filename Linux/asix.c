/*
 * ASIX AX8817X based USB 2.0 Ethernet Devices
 * Copyright (C) 2003-2005 David Hollis <dhollis@davehollis.com>
 * Copyright (C) 2005 Phil Chang <pchang23@sbcglobal.net>
 * Copyright (c) 2002-2003 TiVo Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* debug messages, extra info */
/* #define	DEBUG */

#include <linux/version.h>
/* #include <linux/config.h> */
#ifdef	CONFIG_USB_DEBUG
#   define DEBUG
#endif
#include <linux/module.h>
#include <linux/kmod.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/workqueue.h>
#include <linux/mii.h>
#include <linux/usb.h>
#include <linux/crc32.h>

#include "axusbnet.c"
#include "asix.h"

#define DRV_VERSION	"4.20.0"

static char version[] =
KERN_INFO "ASIX USB Ethernet Adapter:v" DRV_VERSION
//	" " __TIME__ " " __DATE__ "\n"
	"    http://www.asix.com.tw\n";

/* configuration of maximum bulk in size */
static int bsize = AX88772B_MAX_BULKIN_16K;
module_param(bsize, int, 0);
MODULE_PARM_DESC(bsize, "Maximum transfer size per bulk");

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
static void ax88772b_link_reset(void *data);
static void ax88772a_link_reset(void *data);
static void ax88772_link_reset(void *data);
#else
static void ax88772b_link_reset(struct work_struct *work);
static void ax88772a_link_reset(struct work_struct *work);
static void ax88772_link_reset(struct work_struct *work);
#endif
static int ax88772a_phy_powerup(struct usbnet *dev);
static void ax8817x_mdio_write_le(struct net_device *netdev, int phy_id,
				  int loc, int val);
static int ax8817x_mdio_read_le(struct net_device *netdev, int phy_id, int loc);
static int ax88772b_set_csums(struct usbnet *dev);

/* ASIX AX8817X based USB 2.0 Ethernet Devices */

static int ax8817x_read_cmd(struct usbnet *dev, u8 cmd, u16 value, u16 index,
			    u16 size, void *data)
{
	return usb_control_msg(
		dev->udev,
		usb_rcvctrlpipe(dev->udev, 0),
		cmd,
		USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		value,
		index,
		data,
		size,
		USB_CTRL_GET_TIMEOUT);
}

static int ax8817x_write_cmd(struct usbnet *dev, u8 cmd, u16 value, u16 index,
			     u16 size, void *data)
{
	return usb_control_msg(
		dev->udev,
		usb_sndctrlpipe(dev->udev, 0),
		cmd,
		USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		value,
		index,
		data,
		size,
		USB_CTRL_SET_TIMEOUT);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
static void ax8817x_async_cmd_callback(struct urb *urb, struct pt_regs *regs)
#else
static void ax8817x_async_cmd_callback(struct urb *urb)
#endif
{
	struct usb_ctrlrequest *req = (struct usb_ctrlrequest *)urb->context;

	if (urb->status < 0)
		printk(KERN_DEBUG "ax8817x_async_cmd_callback() failed with %d",
			urb->status);

	kfree(req);
	usb_free_urb(urb);
}

static int ax8817x_set_mac_addr(struct net_device *net, void *p)
{
	struct usbnet *dev = netdev_priv(net);
	struct sockaddr *addr = p;

	memcpy(net->dev_addr, addr->sa_data, ETH_ALEN);

	/* Set the MAC address */
	return ax8817x_write_cmd(dev, AX88772_CMD_WRITE_NODE_ID,
				 0, 0, ETH_ALEN, net->dev_addr);

}

static void ax88178_status(struct usbnet *dev, struct urb *urb)
{
	struct ax88172_int_data *event;
	struct ax88178_data *ax178dataptr = (struct ax88178_data *)dev->priv;
	int link;

	if (urb->actual_length < 8)
		return;

	if (ax178dataptr->EepromData == PHY_MODE_MAC_TO_MAC_GMII)
		return;

	event = urb->transfer_buffer;
	link = event->link & 0x01;
	if (netif_carrier_ok(dev->net) != link) {
		if (link) {
			netif_carrier_on(dev->net);
			axusbnet_defer_kevent(dev, EVENT_LINK_RESET);
		} else
			netif_carrier_off(dev->net);
		devwarn(dev, "ax88178 - Link status is: %d", link);
	}
}

static void ax8817x_status(struct usbnet *dev, struct urb *urb)
{
	struct ax88172_int_data *event;
	int link;

	if (urb->actual_length < 8)
		return;

	event = urb->transfer_buffer;
	link = event->link & 0x01;
	if (netif_carrier_ok(dev->net) != link) {
		if (link) {
			netif_carrier_on(dev->net);
			axusbnet_defer_kevent(dev, EVENT_LINK_RESET);
		} else
			netif_carrier_off(dev->net);
		devwarn(dev, "ax8817x - Link status is: %d", link);
	}
}

static void ax88772_status(struct usbnet *dev, struct urb *urb)
{
	struct ax88172_int_data *event;
	struct ax88772_data *ax772_data = (struct ax88772_data *)dev->priv;
	int link;

	if (urb->actual_length < 8)
		return;

	event = urb->transfer_buffer;
	link = event->link & 0x01;

	if (netif_carrier_ok(dev->net) != link) {
		if (link) {
			netif_carrier_on(dev->net);
			ax772_data->Event = AX_SET_RX_CFG;
		} else {
			netif_carrier_off(dev->net);
			if (ax772_data->Event == AX_NOP) {
				ax772_data->Event = PHY_POWER_DOWN;
				ax772_data->TickToExpire = 25;
			}
		}

		devwarn(dev, "ax88772 - Link status is: %d", link);
	}

	if (ax772_data->Event)
		queue_work(ax772_data->ax_work, &ax772_data->check_link);
}

static void ax88772a_status(struct usbnet *dev, struct urb *urb)
{
	struct ax88172_int_data *event;
	struct ax88772a_data *ax772a_data = (struct ax88772a_data *)dev->priv;
	int link;
	int powsave = (ax772a_data->EepromData >> 14);

	if (urb->actual_length < 8)
		return;

	event = urb->transfer_buffer;
	link = event->link & 0x01;

	if (netif_carrier_ok(dev->net) != link) {

		if (link) {
			netif_carrier_on(dev->net);
			ax772a_data->Event = AX_SET_RX_CFG;
		} else if ((powsave == 0x3) || (powsave == 0x1)) {
			netif_carrier_off(dev->net);
			if (ax772a_data->Event == AX_NOP) {
				ax772a_data->Event = CHK_CABLE_EXIST;
				ax772a_data->TickToExpire = 14;
			}
		} else {
			netif_carrier_off(dev->net);
			ax772a_data->Event = AX_NOP;
		}

		devwarn(dev, "ax88772a - Link status is: %d", link);
	}

	if (ax772a_data->Event)
		queue_work(ax772a_data->ax_work, &ax772a_data->check_link);
}

static int ax88772b_stop(struct usbnet *dev)
{
	u16 *medium;

	medium = kmalloc(2, GFP_ATOMIC);
	if (medium) {
		ax8817x_read_cmd(dev, AX_CMD_READ_MEDIUM_MODE, 0, 0, 2, medium);
		ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE,
				  (*medium & ~AX88772_MEDIUM_RX_ENABLE),
				  0, 0, NULL);

		kfree(medium);
		return 0;
	}
	return -EINVAL;
}

static int ax88772b_reset(struct usbnet *dev)
{
	int ret;

	/* Set the MAC address */
	ret = ax8817x_write_cmd(dev, AX88772_CMD_WRITE_NODE_ID,
				0, 0, ETH_ALEN, dev->net->dev_addr);
	if (ret < 0)
		deverr(dev, "set MAC address failed: %d", ret);

	/* stop MAC operation */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL, AX_RX_CTL_STOP,
				0, 0, NULL);
	if (ret < 0)
		deverr(dev, "Reset RX_CTL failed: %d", ret);

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE,
				AX88772_MEDIUM_DEFAULT, 0, 0,
				NULL);
	if (ret < 0)
		deverr(dev, "Write medium mode register: %d", ret);

	return ret;
}

static void ax88772b_status(struct usbnet *dev, struct urb *urb)
{
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;
	struct ax88172_int_data *event;
	int link;

	if (urb->actual_length < 8)
		return;

	if (ax772b_data->OperationMode == OPERATION_PHY_MODE)
		return;

	event = urb->transfer_buffer;
	if (ax772b_data->PhySelect == 0 &&
	    ax772b_data->OperationMode == OPERATION_MAC_MODE)
		link = (event->link & AX_INT_SPLS_LINK) >> 1;
	else
		link = event->link & AX_INT_PPLS_LINK;

	if (netif_carrier_ok(dev->net) != link) {
		if (link) {
			netif_carrier_on(dev->net);
			ax772b_data->Event = AX_SET_RX_CFG;
		} else {
			netif_carrier_off(dev->net);
			ax772b_data->time_to_chk = jiffies;
		}
		devwarn(dev, "ax88772b - Link status is: %d", link);
	}

	if (!link) {

		int no_cable = (event->link & AX_INT_CABOFF_UNPLUG) ? 1 : 0;

		if (no_cable) {
			if ((ax772b_data->psc &
			    (AX_SWRESET_IPPSL_0 | AX_SWRESET_IPPSL_1)) &&
			     !ax772b_data->pw_enabled) {
				/*
				 * AX88772B already entered power saving state
				 */
				ax772b_data->pw_enabled = 1;
			}
			if (ax772b_data->psc & AX_SWRESET_AUTODETACH)
				ax772b_data->Event = AX_CHK_AUTODETACH;

		} else {
			/* AX88772B resumed from power saving state */
			if (ax772b_data->pw_enabled ||
				(jiffies > (ax772b_data->time_to_chk +
				 AX88772B_WATCHDOG))) {
				if (ax772b_data->pw_enabled)
					ax772b_data->pw_enabled = 0;
				ax772b_data->Event = PHY_POWER_UP;
				ax772b_data->time_to_chk = jiffies;
			}
		}
	}

	if (ax772b_data->Event)
		queue_work(ax772b_data->ax_work, &ax772b_data->check_link);
}

static void ax88772c_status(struct usbnet *dev, struct urb *urb)
{
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;
	struct ax88172_int_data *event;
	int link;

	if (urb->actual_length < 8)
		return;

	if (ax772b_data->OperationMode == OPERATION_PHY_MODE)
		return;

	event = urb->transfer_buffer;
	if (ax772b_data->PhySelect == 0 &&
	    ax772b_data->OperationMode == OPERATION_MAC_MODE)
		link = (event->link & AX_INT_SPLS_LINK) >> 1;
	else
		link = event->link & AX_INT_PPLS_LINK;

	if (netif_carrier_ok(dev->net) != link) {
		if (link) {
			netif_carrier_on(dev->net);
			ax772b_data->Event = AX_SET_RX_CFG;
		} else {
			netif_carrier_off(dev->net);
			ax772b_data->time_to_chk = jiffies;
		}
		devwarn(dev, "ax88772c - Link status is: %d", link);
	}

	if (!link) {

		int no_cable = (event->link & AX_INT_CABOFF_UNPLUG) ? 1 : 0;

		if (no_cable) {
			if ((ax772b_data->psc &
			    (AX_SWRESET_IPPSL_0 | AX_SWRESET_IPPSL_1)) &&
			     !ax772b_data->pw_enabled) {
				/*
				 * AX88772B already entered power saving state
				 */
				ax772b_data->pw_enabled = 1;
			}
			if (ax772b_data->psc & AX_SWRESET_AUTODETACH)
				ax772b_data->Event = AX_CHK_AUTODETACH;
		} else {
			/* AX88772B resumed from power saving state */
			if (ax772b_data->pw_enabled ||
				(jiffies > (ax772b_data->time_to_chk +
				 AX88772B_WATCHDOG))) {
				if (ax772b_data->pw_enabled)
					ax772b_data->pw_enabled = 0;
				ax772b_data->Event = PHY_POWER_UP;
				ax772b_data->time_to_chk = jiffies;
			}
		}
	}

	if (ax772b_data->Event)
		queue_work(ax772b_data->ax_work, &ax772b_data->check_link);
}

void
ax8817x_write_cmd_async(struct usbnet *dev, u8 cmd, u16 value, u16 index,
				    u16 size, void *data)
{
	struct usb_ctrlrequest *req;
	int status;
	struct urb *urb;

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (urb == NULL) {
		deverr(dev, "Error allocating URB in write_cmd_async!");
		return;
	}

	req = kmalloc(sizeof(struct usb_ctrlrequest), GFP_ATOMIC);
	if (req == NULL) {
		deverr(dev, "Failed to allocate memory for control request");
		usb_free_urb(urb);
		return;
	}

	req->bRequestType = USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE;
	req->bRequest = cmd;
	req->wValue = cpu_to_le16(value);
	req->wIndex = cpu_to_le16(index);
	req->wLength = cpu_to_le16(size);

	usb_fill_control_urb(urb, dev->udev,
			     usb_sndctrlpipe(dev->udev, 0),
			     (void *)req, data, size,
			     ax8817x_async_cmd_callback, req);

	status = usb_submit_urb(urb, GFP_ATOMIC);
	if (status < 0) {
		deverr(dev, "Error submitting the control message: status=%d",
				status);
		kfree(req);
		usb_free_urb(urb);
	}
}

static void ax8817x_set_multicast(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;
	u8 rx_ctl = AX_RX_CTL_START | AX_RX_CTL_AB;
	int mc_count;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
	mc_count = net->mc_count;
#else
	mc_count = netdev_mc_count(net);
#endif

	if (net->flags & IFF_PROMISC) {
		rx_ctl |= AX_RX_CTL_PRO;
	} else if (net->flags & IFF_ALLMULTI
		   || mc_count > AX_MAX_MCAST) {
		rx_ctl |= AX_RX_CTL_AMALL;
	} else if (mc_count == 0) {
		/* just broadcast and directed */
	} else {
		/* We use the 20 byte dev->data
		 * for our 8 byte filter buffer
		 * to avoid allocating memory that
		 * is tricky to free later */
		u32 crc_bits;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
		struct dev_mc_list *mc_list = net->mc_list;
		int i;

		memset(data->multi_filter, 0, AX_MCAST_FILTER_SIZE);

		/* Build the multicast hash filter. */
		for (i = 0; i < net->mc_count; i++) {
			crc_bits =
			    ether_crc(ETH_ALEN,
				      mc_list->dmi_addr) >> 26;
			data->multi_filter[crc_bits >> 3] |=
			    1 << (crc_bits & 7);
			mc_list = mc_list->next;
		}
#else
		struct netdev_hw_addr *ha;
		memset(data->multi_filter, 0, AX_MCAST_FILTER_SIZE);
		netdev_for_each_mc_addr(ha, net) {
			crc_bits = ether_crc(ETH_ALEN, ha->addr) >> 26;
			data->multi_filter[crc_bits >> 3] |=
				1 << (crc_bits & 7);
		}
#endif
		ax8817x_write_cmd_async(dev, AX_CMD_WRITE_MULTI_FILTER, 0, 0,
				   AX_MCAST_FILTER_SIZE, data->multi_filter);

		rx_ctl |= AX_RX_CTL_AM;
	}

	ax8817x_write_cmd_async(dev, AX_CMD_WRITE_RX_CTL, rx_ctl, 0, 0, NULL);
}

static void ax88178_set_multicast(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;
	u16 rx_ctl = (AX_RX_CTL_START | AX_RX_CTL_AB |  AX_RX_CTL_MFB);
	int mc_count;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
	mc_count = net->mc_count;
#else
	mc_count = netdev_mc_count(net);
#endif

	if (net->flags & IFF_PROMISC) {
		rx_ctl |= AX_RX_CTL_PRO;
	} else if (net->flags & IFF_ALLMULTI
		   || mc_count > AX_MAX_MCAST) {
		rx_ctl |= AX_RX_CTL_AMALL;
	} else if (mc_count == 0) {
		/* just broadcast and directed */
	} else {
		/* We use the 20 byte dev->data
		 * for our 8 byte filter buffer
		 * to avoid allocating memory that
		 * is tricky to free later */
		u32 crc_bits;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
		struct dev_mc_list *mc_list = net->mc_list;
		int i;

		memset(data->multi_filter, 0, AX_MCAST_FILTER_SIZE);

		/* Build the multicast hash filter. */
		for (i = 0; i < net->mc_count; i++) {
			crc_bits =
			    ether_crc(ETH_ALEN,
				      mc_list->dmi_addr) >> 26;
			data->multi_filter[crc_bits >> 3] |=
			    1 << (crc_bits & 7);
			mc_list = mc_list->next;
		}
#else
		struct netdev_hw_addr *ha;
		memset(data->multi_filter, 0, AX_MCAST_FILTER_SIZE);
		netdev_for_each_mc_addr(ha, net) {
			crc_bits = ether_crc(ETH_ALEN, ha->addr) >> 26;
			data->multi_filter[crc_bits >> 3] |=
				1 << (crc_bits & 7);
		}
#endif
		ax8817x_write_cmd_async(dev, AX_CMD_WRITE_MULTI_FILTER, 0, 0,
				   AX_MCAST_FILTER_SIZE, data->multi_filter);

		rx_ctl |= AX_RX_CTL_AM;
	}

	ax8817x_write_cmd_async(dev, AX_CMD_WRITE_RX_CTL, rx_ctl, 0, 0, NULL);
}

static void ax88772b_set_multicast(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;
	u16 rx_ctl = (AX_RX_CTL_START | AX_RX_CTL_AB | AX_RX_HEADER_DEFAULT);
	int mc_count;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
	mc_count = net->mc_count;
#else
	mc_count = netdev_mc_count(net);
#endif

	if (net->flags & IFF_PROMISC) {
		rx_ctl |= AX_RX_CTL_PRO;
	} else if (net->flags & IFF_ALLMULTI
		   || mc_count > AX_MAX_MCAST) {
		rx_ctl |= AX_RX_CTL_AMALL;
	} else if (mc_count == 0) {
		/* just broadcast and directed */
	} else {
		/* We use the 20 byte dev->data
		 * for our 8 byte filter buffer
		 * to avoid allocating memory that
		 * is tricky to free later */
		u32 crc_bits;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
		struct dev_mc_list *mc_list = net->mc_list;
		int i;

		memset(data->multi_filter, 0, AX_MCAST_FILTER_SIZE);

		/* Build the multicast hash filter. */
		for (i = 0; i < net->mc_count; i++) {
			crc_bits =
			    ether_crc(ETH_ALEN,
				      mc_list->dmi_addr) >> 26;
			data->multi_filter[crc_bits >> 3] |=
			    1 << (crc_bits & 7);
			mc_list = mc_list->next;
		}
#else
		struct netdev_hw_addr *ha;
		memset(data->multi_filter, 0, AX_MCAST_FILTER_SIZE);
		netdev_for_each_mc_addr(ha, net) {
			crc_bits = ether_crc(ETH_ALEN, ha->addr) >> 26;
			data->multi_filter[crc_bits >> 3] |=
				1 << (crc_bits & 7);
		}
#endif
		ax8817x_write_cmd_async(dev, AX_CMD_WRITE_MULTI_FILTER, 0, 0,
				   AX_MCAST_FILTER_SIZE, data->multi_filter);

		rx_ctl |= AX_RX_CTL_AM;
	}

	ax8817x_write_cmd_async(dev, AX_CMD_WRITE_RX_CTL, rx_ctl, 0, 0, NULL);
}

static int ax8817x_mdio_read(struct net_device *netdev, int phy_id, int loc)
{
	struct usbnet *dev = netdev_priv(netdev);
	u16 *res, ret;
	u8* smsr;
	int i = 0;

	res = kmalloc(2, GFP_ATOMIC);
	if (!res)
		return 0;

	do {
		ax8817x_write_cmd(dev, AX_CMD_SET_SW_MII, 0, 0, 0, NULL);
		
		msleep(1);

		smsr = (u8*) res;
		ax8817x_read_cmd(dev, AX_CMD_READ_STATMNGSTS_REG, 0, 0, 1, smsr);
	} while (!(*smsr & AX_HOST_EN) && (i++ < 30));
	
	ax8817x_read_cmd(dev, AX_CMD_READ_MII_REG, phy_id, (__u16)loc, 2, res);
	ax8817x_write_cmd(dev, AX_CMD_SET_HW_MII, 0, 0, 0, NULL);

	ret = *res & 0xffff;
	kfree(res);

	return ret;
}

static int
ax8817x_swmii_mdio_read(struct net_device *netdev, int phy_id, int loc)
{
	struct usbnet *dev = netdev_priv(netdev);
	u16 *res;
	u16 ret;

	res = kmalloc(2, GFP_ATOMIC);
	if (!res)
		return 0;

	ax8817x_read_cmd(dev, AX_CMD_READ_MII_REG, phy_id,
				(__u16)loc, 2, res);

	ret = *res & 0xffff;
	kfree(res);

	return ret;
}

/* same as above, but converts resulting value to cpu byte order */
static int ax8817x_mdio_read_le(struct net_device *netdev, int phy_id, int loc)
{
	return le16_to_cpu(ax8817x_mdio_read(netdev, phy_id, loc));
}

static int
ax8817x_swmii_mdio_read_le(struct net_device *netdev, int phy_id, int loc)
{
	return le16_to_cpu(ax8817x_swmii_mdio_read(netdev, phy_id, loc));
}

static void
ax8817x_mdio_write(struct net_device *netdev, int phy_id, int loc, int val)
{
	struct usbnet *dev = netdev_priv(netdev);
	u16 *res;
	u8* smsr;
	int i = 0;

	res = kmalloc(2, GFP_ATOMIC);
	if (!res)
		return;
	*res = val;

	do {
		ax8817x_write_cmd(dev, AX_CMD_SET_SW_MII, 0, 0, 0, NULL);

		msleep(1);
		smsr = (u8 *) res;
		ax8817x_read_cmd(dev, AX_CMD_READ_STATMNGSTS_REG, 0, 0, 1, smsr);
	} while (!(*smsr & AX_HOST_EN) && (i++ < 30));	
		
	ax8817x_write_cmd(dev, AX_CMD_WRITE_MII_REG, phy_id,
			  (__u16)loc, 2, res);
	ax8817x_write_cmd(dev, AX_CMD_SET_HW_MII, 0, 0, 0, NULL);

	kfree(res);
}

static void ax8817x_swmii_mdio_write(struct net_device *netdev, int phy_id,
				     int loc, int val)
{
	struct usbnet *dev = netdev_priv(netdev);
	u16 *res;

	res = kmalloc(2, GFP_ATOMIC);
	if (!res)
		return;
	*res = val;

	ax8817x_write_cmd(dev, AX_CMD_WRITE_MII_REG, phy_id,
				(__u16)loc, 2, res);

	kfree(res);
}

static void
ax88772b_mdio_write(struct net_device *netdev, int phy_id, int loc, int val)
{
	struct usbnet *dev = netdev_priv(netdev);
	u16 *res;

	res = kmalloc(2, GFP_ATOMIC);
	if (!res)
		return;
	*res = val;

	ax8817x_write_cmd(dev, AX_CMD_SET_SW_MII, 0, 0, 0, NULL);
	ax8817x_write_cmd(dev, AX_CMD_WRITE_MII_REG, phy_id,
				(__u16)loc, 2, res);

	if (loc == MII_ADVERTISE) {
		*res = cpu_to_le16(BMCR_ANENABLE | BMCR_ANRESTART);
		ax8817x_write_cmd(dev, AX_CMD_WRITE_MII_REG, phy_id,
				(__u16)MII_BMCR, 2, res);
	}

	ax8817x_write_cmd(dev, AX_CMD_SET_HW_MII, 0, 0, 0, NULL);

	kfree(res);
}

/* same as above, but converts new value to le16 byte order before writing */
static void
ax8817x_mdio_write_le(struct net_device *netdev, int phy_id, int loc, int val)
{
	ax8817x_mdio_write(netdev, phy_id, loc, cpu_to_le16(val));
}

static void ax8817x_swmii_mdio_write_le(struct net_device *netdev,
			int phy_id, int loc, int val)
{
	ax8817x_swmii_mdio_write(netdev, phy_id, loc, cpu_to_le16(val));
}

static void
ax88772b_mdio_write_le(struct net_device *netdev, int phy_id, int loc, int val)
{
	ax88772b_mdio_write(netdev, phy_id, loc, cpu_to_le16(val));
}

static int ax88772_suspend(struct usb_interface *intf,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 10)
			   pm_message_t message)
#else
			   u32 message)
#endif
{
	struct usbnet *dev = usb_get_intfdata(intf);
	u16 *medium;

	medium = kmalloc(2, GFP_ATOMIC);
	if (!medium)
		return axusbnet_suspend(intf, message);

	ax8817x_read_cmd(dev, AX_CMD_READ_MEDIUM_MODE, 0, 0, 2, medium);
	ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE,
			(*medium & ~AX88772_MEDIUM_RX_ENABLE), 0, 0, NULL);

	kfree(medium);
	return axusbnet_suspend(intf, message);
}

static int ax88772b_suspend(struct usb_interface *intf,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 10)
			    pm_message_t message)
#else
			    u32 message)
#endif
{
	struct usbnet *dev = usb_get_intfdata(intf);
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;
	u16 *tmp16;
	u8 *opt;
//	u32 tmp32;

	tmp16 = kmalloc(2, GFP_ATOMIC);
	if (!tmp16)
		return axusbnet_suspend(intf, message);
	opt = (u8 *)tmp16;
#if 0
	/* Read Wake-up Frame Array Register (Mask Wakeup Timer) */
	ax8817x_read_cmd(dev, AX_CMD_READ_WKFARY, 0x0b, 0, 4, &tmp32);
	tmp32 &= 0xFFF0FFFF;
	/* 8 second */
	tmp32 |= 0xFFF2FFFF;
	ax8817x_write_cmd(dev, AX_CMD_WRITE_WKFARY, 0x0b, 0, 4, &tmp32);
#endif
	/* Preserve BMCR for restoring */
	ax772b_data->presvd_phy_bmcr = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_BMCR);

	/* Preserve Advertisement control reg for restoring */
	ax772b_data->presvd_phy_advertise = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_ADVERTISE);

	ax8817x_read_cmd(dev, AX_CMD_READ_MEDIUM_MODE, 0, 0, 2, tmp16);
	ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE,
			  (*tmp16 & ~AX88772_MEDIUM_RX_ENABLE),
			  0, 0, NULL);

	ax8817x_read_cmd(dev, AX_CMD_READ_MONITOR_MODE, 0, 0, 1, opt);
	if (!(*opt & AX_MONITOR_LINK) && !(*opt & AX_MONITOR_MAGIC)) {
		ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
				  AX_SWRESET_IPRL | AX_SWRESET_IPPD,
				  0, 0, NULL);
	} else {

		if (ax772b_data->psc & AX_SWRESET_WOLLP) {
			*tmp16 = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id,
						      MII_BMCR);
			ax8817x_mdio_write_le(dev->net, dev->mii.phy_id,
					      MII_BMCR, *tmp16 | BMCR_ANENABLE);

			ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
					  AX_SWRESET_IPRL | ax772b_data->psc,
					  0, 0, NULL);
		}

		if (ax772b_data->psc &
		    (AX_SWRESET_IPPSL_0 | AX_SWRESET_IPPSL_1)) {
			*opt |= AX_MONITOR_LINK;
			ax8817x_write_cmd(dev, AX_CMD_WRITE_MONITOR_MODE, *opt,
					  0, 0, NULL);
		}
	}

	kfree(tmp16);
	return axusbnet_suspend(intf, message);
}

static int ax88772_resume(struct usb_interface *intf)
{
	struct usbnet *dev = usb_get_intfdata(intf);

	netif_carrier_off(dev->net);

	return axusbnet_resume(intf);
}

static int ax88772b_resume(struct usb_interface *intf)
{
	struct usbnet *dev = usb_get_intfdata(intf);
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;

	if (ax772b_data->psc & AX_SWRESET_WOLLP) {
		ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
				  AX_SWRESET_IPRL | (ax772b_data->psc & 0x7FFF),
				  0, 0, NULL);
	}

	if (ax772b_data->psc & (AX_SWRESET_IPPSL_0 | AX_SWRESET_IPPSL_1))
		ax88772a_phy_powerup(dev);

	netif_carrier_off(dev->net);

	if (ax772b_data->OperationMode == OPERATION_PHY_MODE)
		netif_carrier_on(dev->net);

	return axusbnet_resume(intf);
}

static int ax88172_link_reset(struct usbnet *dev)
{
	u16 lpa;
	u16 adv;
	u16 res;
	u8 mode;

	mode = AX_MEDIUM_TX_ABORT_ALLOW | AX_MEDIUM_FLOW_CONTROL_EN;
	lpa = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_LPA);
	adv = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_ADVERTISE);
	res = mii_nway_result(lpa|adv);
	if (res & LPA_DUPLEX)
		mode |= AX_MEDIUM_FULL_DUPLEX;
	ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE, mode, 0, 0, NULL);

	return 0;
}

static void
ax8817x_get_wol(struct net_device *net, struct ethtool_wolinfo *wolinfo)
{
	struct usbnet *dev = netdev_priv(net);
	u8 *opt;

	wolinfo->supported = 0;
	wolinfo->wolopts = 0;

	opt = kmalloc(1, GFP_KERNEL);
	if (!opt)
		return;

	if (ax8817x_read_cmd(dev, AX_CMD_READ_MONITOR_MODE, 0, 0, 1, opt) < 0) {
		kfree(opt);
		return;
	}

	wolinfo->supported = WAKE_PHY | WAKE_MAGIC;

	if (*opt & AX_MONITOR_LINK)
		wolinfo->wolopts |= WAKE_PHY;
	if (*opt & AX_MONITOR_MAGIC)
		wolinfo->wolopts |= WAKE_MAGIC;

	kfree(opt);
}

static int
ax8817x_set_wol(struct net_device *net, struct ethtool_wolinfo *wolinfo)
{
	struct usbnet *dev = netdev_priv(net);
	u8 *opt;

	opt = kmalloc(1, GFP_KERNEL);
	if (!opt)
		return -ENOMEM;

	*opt = 0;
	if (wolinfo->wolopts & WAKE_PHY)
		*opt |= AX_MONITOR_LINK;
	if (wolinfo->wolopts & WAKE_MAGIC)
		*opt |= AX_MONITOR_MAGIC;

	ax8817x_write_cmd(dev, AX_CMD_WRITE_MONITOR_MODE, *opt, 0, 0, NULL);

	kfree(opt);
	return 0;
}

static int ax8817x_get_eeprom_len(struct net_device *net)
{
	return AX_EEPROM_LEN;
}

static int ax8817x_get_eeprom(struct net_device *net,
			      struct ethtool_eeprom *eeprom, u8 *data)
{
	struct usbnet *dev = netdev_priv(net);
	u16 *ebuf = (u16 *)data;
	int i;

	/* Crude hack to ensure that we don't overwrite memory
	 * if an odd length is supplied
	 */
	if (eeprom->len % 2)
		return -EINVAL;

	eeprom->magic = AX_EEPROM_MAGIC;

	/* ax8817x returns 2 bytes from eeprom on read */
	for (i = 0; i < eeprom->len / 2; i++) {
		if (ax8817x_read_cmd(dev, AX_CMD_READ_EEPROM,
				     eeprom->offset + i, 0, 2,
				     &ebuf[i]) < 0)
			return -EINVAL;
	}
	return 0;
}

static void ax8817x_get_drvinfo(struct net_device *net,
				struct ethtool_drvinfo *info)
{
	/* Inherit standard device info */
	axusbnet_get_drvinfo(net, info);
	info->eedump_len = 0x3e;
}

static int ax8817x_get_settings(struct net_device *net, struct ethtool_cmd *cmd)
{
	struct usbnet *dev = netdev_priv(net);
	return mii_ethtool_gset(&dev->mii, cmd);
}

static int ax8817x_set_settings(struct net_device *net, struct ethtool_cmd *cmd)
{
	struct usbnet *dev = netdev_priv(net);
	return mii_ethtool_sset(&dev->mii, cmd);
}

/* We need to override some ethtool_ops so we require our
   own structure so we don't interfere with other usbnet
   devices that may be connected at the same time. */
static struct ethtool_ops ax8817x_ethtool_ops = {
	.get_drvinfo		= ax8817x_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_msglevel		= axusbnet_get_msglevel,
	.set_msglevel		= axusbnet_set_msglevel,
	.get_wol		= ax8817x_get_wol,
	.set_wol		= ax8817x_set_wol,
	.get_eeprom_len	= ax8817x_get_eeprom_len,
	.get_eeprom		= ax8817x_get_eeprom,
	.get_settings		= ax8817x_get_settings,
	.set_settings		= ax8817x_set_settings,
};

static int ax8817x_ioctl(struct net_device *net, struct ifreq *rq, int cmd)
{
	struct usbnet *dev = netdev_priv(net);

	return generic_mii_ioctl(&dev->mii, if_mii(rq), cmd, NULL);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 29)
static const struct net_device_ops ax88x72_netdev_ops = {
	.ndo_open			= axusbnet_open,
	.ndo_stop			= axusbnet_stop,
	.ndo_start_xmit	= axusbnet_start_xmit,
	.ndo_tx_timeout	= axusbnet_tx_timeout,
	.ndo_change_mtu	= axusbnet_change_mtu,
	.ndo_get_stats		= axusbnet_get_stats,
	.ndo_do_ioctl		= ax8817x_ioctl,
	.ndo_set_mac_address		= ax8817x_set_mac_addr,
	.ndo_validate_addr		= eth_validate_addr,
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3, 2, 0)
	.ndo_set_multicast_list	= ax8817x_set_multicast,
#else
	.ndo_set_rx_mode	= ax8817x_set_multicast,
#endif
};
#endif

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 29)
static const struct net_device_ops ax88178_netdev_ops = {
	.ndo_open			= axusbnet_open,
	.ndo_stop			= axusbnet_stop,
	.ndo_start_xmit	= axusbnet_start_xmit,
	.ndo_tx_timeout	= axusbnet_tx_timeout,
	.ndo_change_mtu	= axusbnet_change_mtu,
	.ndo_get_stats		= axusbnet_get_stats,
	.ndo_do_ioctl		= ax8817x_ioctl,
	.ndo_set_mac_address		= ax8817x_set_mac_addr,
	.ndo_validate_addr		= eth_validate_addr,
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3, 2, 0)
	.ndo_set_multicast_list	= ax88178_set_multicast,
#else
	.ndo_set_rx_mode	= ax88178_set_multicast,
#endif
};
#endif

static int access_eeprom_mac(struct usbnet *dev, u8 *buf, u8 offset, bool wflag)
{
	int ret = 0, i;
	u16* tmp = (u16*)buf;

	if (wflag) {
		ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_EEPROM_EN,
						0, 0, 0, NULL);
		if (ret < 0)
			 return ret;

		mdelay(15);
	}

	for (i = 0; i < (ETH_ALEN >> 1); i++) {
		if (wflag) {
			u16 wd = cpu_to_le16(*(tmp + i));
			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_EEPROM, offset + i,
						wd, 0, NULL);
			if (ret < 0)
				break;

			mdelay(15);
		}
		else {
			ret = ax8817x_read_cmd(dev, AX_CMD_READ_EEPROM,
					       offset + i, 0, 2, tmp + i);
			if (ret < 0)
				break;
		}
	}

	if (!wflag) {
		if (ret < 0) {
			#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
				netdev_dbg(dev->net, "Failed to read MAC address from EEPROM: %d\n", ret);
			#else
				devdbg(dev, "Failed to read MAC address from EEPROM: %d\n", ret);
			#endif
			return ret;
		}
		memcpy(dev->net->dev_addr, buf, ETH_ALEN);
	}
	else {
		ax8817x_write_cmd(dev, AX_CMD_WRITE_EEPROM_DIS,
				  0, 0, 0, NULL);
		if (ret < 0)
			 return ret;

		/* reload eeprom data */
		ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					AXGPIOS_RSE, 0, 0, NULL);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int ax8817x_check_ether_addr(struct usbnet *dev)
{
	unsigned char *tmp = (unsigned char*)dev->net->dev_addr;
	u8 default_mac[6] = {0, 0x0e, 0xc6, 0x87, 0x72, 0x01};

	if (((*((u8*)tmp) == 0) && (*((u8*)tmp + 1) == 0) && (*((u8*)tmp + 2) == 0)) ||
	    !is_valid_ether_addr((u8*)tmp) ||
	    !memcmp(dev->net->dev_addr, default_mac, ETH_ALEN)) {
		int i;

		printk("Found invalid EEPROM MAC address value ");

		for (i = 0; i < ETH_ALEN; i++) {
			printk("%02X", *((u8*)tmp + i));
			if (i != 5)
				printk("-");
		}
		printk("\n");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 4, 0)
		eth_hw_addr_random(dev->net);
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)
		dev->net->addr_assign_type |= NET_ADDR_RANDOM;
#endif
		random_ether_addr(dev->net->dev_addr); 
#endif
		*tmp = 0;
		*(tmp + 1) = 0x0E;
		*(tmp + 2) = 0xC6;
		*(tmp + 3) = 0x8F;

		return -EADDRNOTAVAIL;	
	} 
	return 0;
}

static int ax8817x_get_mac(struct usbnet *dev, u8* buf)
{
	int ret, i;
	

	ret = access_eeprom_mac(dev, buf, 0x04, 0);
	if (ret < 0)
		goto out;

	if (ax8817x_check_ether_addr(dev)) {
		ret = access_eeprom_mac(dev, dev->net->dev_addr, 0x04, 1);
		if (ret < 0) {
			deverr(dev, "Failed to write MAC to EEPROM: %d", ret);
			goto out;
		}

		msleep(5);

		ret = ax8817x_read_cmd(dev, AX88772_CMD_READ_NODE_ID,
				       0, 0, ETH_ALEN, buf);
		if (ret < 0) {
			deverr(dev, "Failed to read MAC address: %d", ret);
			goto out;
		}

		for (i = 0; i < ETH_ALEN; i++)
			if (*(dev->net->dev_addr + i) != *((u8*)buf + i)) {
				devwarn(dev, "Found invalid EEPROM part or non-EEPROM");
				break;
			}
	}

	memcpy(dev->net->perm_addr, dev->net->dev_addr, ETH_ALEN);

	/* Set the MAC address */
	ax8817x_write_cmd (dev, AX88772_CMD_WRITE_NODE_ID, 0, 0,
			   ETH_ALEN, dev->net->dev_addr);
	
	if (ret < 0) {
		deverr(dev, "Failed to write MAC address: %d", ret);
		goto out;
	}

	return 0;
out:
	return ret;
}

static int ax8817x_bind(struct usbnet *dev, struct usb_interface *intf)
{
	int ret = 0;
	void *buf;
	int i;
	unsigned long gpio_bits = dev->driver_info->data;
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;

	axusbnet_get_endpoints(dev, intf);

	buf = kmalloc(ETH_ALEN, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out1;
	}

	/* Toggle the GPIOs in a manufacturer/model specific way */
	for (i = 2; i >= 0; i--) {
		ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					(gpio_bits >> (i * 8)) & 0xff,
					0, 0, NULL);
		if (ret < 0)
			goto out2;

		msleep(5);
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL, 0x80, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "send AX_CMD_WRITE_RX_CTL failed: %d", ret);
		goto out2;
	}

	/* Get the MAC address */
	memset(buf, 0, ETH_ALEN);
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_NODE_ID, 0, 0, 6, buf);
	if (ret < 0) {
		deverr(dev, "read AX_CMD_READ_NODE_ID failed: %d", ret);
		goto out2;
	}
	memcpy(dev->net->dev_addr, buf, ETH_ALEN);

	/* Get the PHY id */
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_PHY_ID, 0, 0, 2, buf);
	if (ret < 0) {
		deverr(dev, "error on read AX_CMD_READ_PHY_ID: %02x", ret);
		goto out2;
	} else if (ret < 2) {
		/* this should always return 2 bytes */
		deverr(dev, "Read PHYID returned less than 2 bytes: ret=%02x",
				ret);
		ret = -EIO;
		goto out2;
	}

	/* Initialize MII structure */
	dev->mii.dev = dev->net;
	dev->mii.mdio_read = ax8817x_mdio_read_le;
	dev->mii.mdio_write = ax8817x_mdio_write_le;
	dev->mii.phy_id_mask = 0x3f;
	dev->mii.reg_num_mask = 0x1f;
	dev->mii.phy_id = *((u8 *)buf + 1);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	dev->net->do_ioctl = ax8817x_ioctl;
	dev->net->set_multicast_list = ax8817x_set_multicast;
	dev->net->set_mac_address = ax8817x_set_mac_addr;
#else
	dev->net->netdev_ops = &ax88x72_netdev_ops;
#endif

	dev->net->ethtool_ops = &ax8817x_ethtool_ops;

	/* Register suspend and resume functions */
	data->suspend = axusbnet_suspend;
	data->resume = axusbnet_resume;

	ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_BMCR, BMCR_RESET);
	ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_ADVERTISE,
		ADVERTISE_ALL | ADVERTISE_CSMA | ADVERTISE_PAUSE_CAP);
	mii_nway_restart(&dev->mii);

	printk(version);

	return 0;
out2:
	kfree(buf);
out1:
	return ret;
}

static struct ethtool_ops ax88772_ethtool_ops = {
	.get_drvinfo		= ax8817x_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_msglevel		= axusbnet_get_msglevel,
	.set_msglevel		= axusbnet_set_msglevel,
	.get_wol		= ax8817x_get_wol,
	.set_wol		= ax8817x_set_wol,
	.get_eeprom_len		= ax8817x_get_eeprom_len,
	.get_eeprom		= ax8817x_get_eeprom,
	.get_settings		= ax8817x_get_settings,
	.set_settings		= ax8817x_set_settings,
};

static int ax88772_bind(struct usbnet *dev, struct usb_interface *intf)
{
	int ret;
	void *buf;
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;
	struct ax88772_data *ax772_data = NULL;

	axusbnet_get_endpoints(dev, intf);

	buf = kmalloc(6, GFP_KERNEL);
	if (!buf) {
		deverr(dev, "Cannot allocate memory for buffer");
		ret = -ENOMEM;
		goto out1;
	}

	ax772_data = kmalloc(sizeof(*ax772_data), GFP_KERNEL);
	if (!ax772_data) {
		deverr(dev, "Cannot allocate memory for AX88772 data");
		kfree(buf);
		return -ENOMEM;
	}

	memset(ax772_data, 0, sizeof(*ax772_data));
	dev->priv = ax772_data;

	ax772_data->ax_work = create_singlethread_workqueue("ax88772");
	if (!ax772_data->ax_work) {
		kfree(ax772_data);
		kfree(buf);
		return -ENOMEM;
	}

	ax772_data->dev = dev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	INIT_WORK(&ax772_data->check_link, ax88772_link_reset, dev);
#else
	INIT_WORK(&ax772_data->check_link, ax88772_link_reset);
#endif

	/* reload eeprom data */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS, 0x00B0, 0, 0, NULL);
	if (ret < 0)
		goto out2;

	msleep(5);

	/* Initialize MII structure */
	dev->mii.dev = dev->net;
	dev->mii.mdio_read = ax8817x_mdio_read_le;
	dev->mii.mdio_write = ax8817x_mdio_write_le;
	dev->mii.phy_id_mask = 0xff;
	dev->mii.reg_num_mask = 0xff;

	/* Get the PHY id */
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_PHY_ID, 0, 0, 2, buf);
	if (ret < 0) {
		deverr(dev, "Error reading PHY ID: %02x", ret);
		goto out2;
	} else if (ret < 2) {
		/* this should always return 2 bytes */
		deverr(dev, "Read PHYID returned less than 2 bytes: ret=%02x",
		    ret);
		ret = -EIO;
		goto out2;
	}
	dev->mii.phy_id = *((u8 *)buf + 1);

	if (dev->mii.phy_id == 0x10) {
		ret = ax8817x_write_cmd(dev, AX_CMD_SW_PHY_SELECT,
					0x0001, 0, 0, NULL);
		if (ret < 0) {
			deverr(dev, "Select PHY #1 failed: %d", ret);
			goto out2;
		}

		ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_IPPD,
					0, 0, NULL);
		if (ret < 0) {
			deverr(dev, "Failed to power down PHY: %d", ret);
			goto out2;
		}

		msleep(150);

		ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_CLEAR,
					0, 0, NULL);
		if (ret < 0) {
			deverr(dev, "Failed to perform software reset: %d",
			       ret);
			goto out2;
		}

		msleep(150);

		ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
					AX_SWRESET_IPRL | AX_SWRESET_PRL,
					0, 0, NULL);
		if (ret < 0) {
			deverr(dev,
			      "Failed to set PHY reset control: %d", ret);
			goto out2;
		}
	} else {
		ret = ax8817x_write_cmd(dev, AX_CMD_SW_PHY_SELECT,
					0x0000, 0, 0, NULL);
		if (ret < 0) {
			deverr(dev, "Select PHY #1 failed: %d", ret);
			goto out2;
		}

		ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
					AX_SWRESET_IPPD | AX_SWRESET_PRL,
					0, 0, NULL);
		if (ret < 0) {
			deverr(dev, "Failed to power down internal PHY: %d",
			       ret);
			goto out2;
		}
	}

	msleep(150);

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL,
				0x0000, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to reset RX_CTL: %d", ret);
		goto out2;
	}

	/* Get the MAC address */
	memset(buf, 0, ETH_ALEN);
	ret = ax8817x_get_mac(dev, buf);
	if (ret < 0) {
		deverr(dev, "Get HW address failed: %d", ret);
		goto out2;
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_SET_SW_MII, 0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Enabling software MII failed: %d", ret);
		goto out2;
	}

	if (dev->mii.phy_id == 0x10) {
		ret = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, 2);
		if (ret != 0x003b) {
			deverr(dev, "Read PHY register 2 must be 0x3b00: %d",
			       ret);
			goto out2;
		}

		ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_PRL,
					0, 0, NULL);
		if (ret < 0) {
			deverr(dev, "Set external PHY reset pin level: %d",
			       ret);
			goto out2;
		}
		msleep(150);

		ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
					AX_SWRESET_IPRL | AX_SWRESET_PRL,
					0, 0, NULL);
		if (ret < 0) {
			deverr(dev,
			       "Set Internal/External PHY reset control: %d",
			       ret);
			goto out2;
		}
		msleep(150);
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	dev->net->do_ioctl = ax8817x_ioctl;
	dev->net->set_multicast_list = ax8817x_set_multicast;
	dev->net->set_mac_address = ax8817x_set_mac_addr;
#else
	dev->net->netdev_ops = &ax88x72_netdev_ops;
#endif

	dev->net->ethtool_ops = &ax88772_ethtool_ops;

	/* Register suspend and resume functions */
	data->suspend = ax88772_suspend;
	data->resume = ax88772_resume;

	ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_BMCR, BMCR_RESET);
	ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_ADVERTISE,
			ADVERTISE_ALL | ADVERTISE_CSMA);

	mii_nway_restart(&dev->mii);
	ax772_data->autoneg_start = jiffies;
	ax772_data->Event = WAIT_AUTONEG_COMPLETE;

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE, 0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Write medium mode register: %d", ret);
		goto out2;
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_IPG0,
				AX88772_IPG0_DEFAULT |
				(AX88772_IPG1_DEFAULT << 8),
				AX88772_IPG2_DEFAULT, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Write IPG,IPG1,IPG2 failed: %d", ret);
		goto out2;
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_SET_HW_MII, 0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to set hardware MII: %02x", ret);
		goto out2;
	}

	/* Set RX_CTL to default values with 2k buffer, and enable cactus */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL, 0x0088, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Reset RX_CTL failed: %d", ret);
		goto out2;
	}

	/* Asix framing packs multiple eth frames into a 2K usb bulk transfer */
	if (dev->driver_info->flags & FLAG_FRAMING_AX) {
		/* hard_mtu  is still the default - the device does not support
		   jumbo eth frames */
		dev->rx_urb_size = 2048;
	}

	kfree(buf);
	printk(version);
	return 0;

out2:
	destroy_workqueue(ax772_data->ax_work);
	kfree(ax772_data);
	kfree(buf);
out1:
	return ret;
}

static void ax88772_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct ax88772_data *ax772_data = (struct ax88772_data *)dev->priv;

	if (ax772_data) {
		flush_workqueue(ax772_data->ax_work);
		destroy_workqueue(ax772_data->ax_work);

		/* stop MAC operation */
		ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL, AX_RX_CTL_STOP,
				  0, 0, NULL);

		/* Power down PHY */
		ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_IPPD,
				  0, 0, NULL);

		kfree(ax772_data);
	}
}

static int ax88772a_phy_powerup(struct usbnet *dev)
{
	int ret;
	/* set the embedded Ethernet PHY in power-down state */
	ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
				AX_SWRESET_IPPD | AX_SWRESET_IPRL, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to power down PHY: %d", ret);
		return ret;
	}

	msleep(10);

	/* set the embedded Ethernet PHY in power-up state */
	ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_IPRL,
				0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to reset PHY: %d", ret);
		return ret;
	}

	msleep(600);

	/* set the embedded Ethernet PHY in reset state */
	ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_CLEAR,
				0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to power up PHY: %d", ret);
		return ret;
	}

	/* set the embedded Ethernet PHY in power-up state */
	ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_IPRL,
				0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to reset PHY: %d", ret);
		return ret;
	}

	return 0;
}

static int ax88772a_bind(struct usbnet *dev, struct usb_interface *intf)
{
	int ret = -EIO;
	void *buf;
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;
	struct ax88772a_data *ax772a_data = NULL;

	printk(version);

	axusbnet_get_endpoints(dev, intf);

	buf = kmalloc(6, GFP_KERNEL);
	if (!buf) {
		deverr(dev, "Cannot allocate memory for buffer");
		ret = -ENOMEM;
		goto out1;
	}

	ax772a_data = kmalloc(sizeof(*ax772a_data), GFP_KERNEL);
	if (!ax772a_data) {
		deverr(dev, "Cannot allocate memory for AX88772A data");
		kfree(buf);
		return -ENOMEM;
	}
	memset(ax772a_data, 0, sizeof(*ax772a_data));
	dev->priv = ax772a_data;

	ax772a_data->ax_work = create_singlethread_workqueue("ax88772a");
	if (!ax772a_data->ax_work) {
		kfree(ax772a_data);
		kfree(buf);
		return -ENOMEM;
	}

	ax772a_data->dev = dev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	INIT_WORK(&ax772a_data->check_link, ax88772a_link_reset, dev);
#else
	INIT_WORK(&ax772a_data->check_link, ax88772a_link_reset);
#endif

	/* Get the EEPROM data*/
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_EEPROM, 0x0017, 0, 2,
			       (void *)&ax772a_data->EepromData);
	if (ret < 0) {
		deverr(dev, "read SROM address 17h failed: %d", ret);
		goto out2;
	}
	le16_to_cpus(&ax772a_data->EepromData);
	/* End of get EEPROM data */

	/* reload eeprom data */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
				AXGPIOS_RSE, 0, 0, NULL);
	if (ret < 0)
		goto out2;

	msleep(5);

	/* Initialize MII structure */
	dev->mii.dev = dev->net;
	dev->mii.mdio_read = ax8817x_mdio_read_le;
	dev->mii.mdio_write = ax8817x_mdio_write_le;
	dev->mii.phy_id_mask = 0xff;
	dev->mii.reg_num_mask = 0xff;

	/* Get the PHY id */
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_PHY_ID, 0, 0, 2, buf);
	if (ret < 0) {
		deverr(dev, "Error reading PHY ID: %02x", ret);
		goto out2;
	} else if (ret < 2) {
		/* this should always return 2 bytes */
		deverr(dev, "Read PHYID returned less than 2 bytes: ret=%02x",
			ret);
		goto out2;
	}
	dev->mii.phy_id = *((u8 *)buf + 1);

	if (dev->mii.phy_id != 0x10) {
		deverr(dev, "Got wrong PHY ID: %02x", dev->mii.phy_id);
		goto out2;
	}

	/* select the embedded 10/100 Ethernet PHY */
	ret = ax8817x_write_cmd(dev, AX_CMD_SW_PHY_SELECT,
			AX_PHYSEL_SSEN | AX_PHYSEL_PSEL | AX_PHYSEL_SSMII,
			0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Select PHY #1 failed: %d", ret);
		goto out2;
	}

	ret = ax88772a_phy_powerup(dev);
	if (ret < 0)
		goto out2;

	/* stop MAC operation */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL, AX_RX_CTL_STOP,
				0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Reset RX_CTL failed: %d", ret);
		goto out2;
	}

	/* Get the MAC address */
	memset(buf, 0, ETH_ALEN);
	ret = ax8817x_get_mac(dev, buf);
	if (ret < 0) {
		deverr(dev, "Get HW address failed: %d", ret);
		goto out2;
	}	

	/* make sure the driver can enable sw mii operation */
	ret = ax8817x_write_cmd(dev, AX_CMD_SET_SW_MII, 0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Enabling software MII failed: %d", ret);
		goto out2;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	dev->net->do_ioctl = ax8817x_ioctl;
	dev->net->set_multicast_list = ax8817x_set_multicast;
	dev->net->set_mac_address = ax8817x_set_mac_addr;
#else
	dev->net->netdev_ops = &ax88x72_netdev_ops;
#endif

	dev->net->ethtool_ops = &ax88772_ethtool_ops;

	/* Register suspend and resume functions */
	data->suspend = ax88772_suspend;
	data->resume = ax88772_resume;

	ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_BMCR, BMCR_RESET);
	ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_ADVERTISE,
			ADVERTISE_ALL | ADVERTISE_CSMA | ADVERTISE_PAUSE_CAP);

	mii_nway_restart(&dev->mii);
	ax772a_data->autoneg_start = jiffies;
	ax772a_data->Event = WAIT_AUTONEG_COMPLETE;

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE,
				0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Write medium mode register: %d", ret);
		goto out2;
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_IPG0,
			AX88772A_IPG0_DEFAULT | AX88772A_IPG1_DEFAULT << 8,
			AX88772A_IPG2_DEFAULT, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Write IPG,IPG1,IPG2 failed: %d", ret);
		goto out2;
	}

	memset(buf, 0, 4);
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_IPG012, 0, 0, 3, buf);
	*((u8 *)buf + 3) = 0x00;
	if (ret < 0) {
		deverr(dev, "Failed to read IPG,IPG1,IPG2 failed: %d", ret);
		goto out2;
	} else {
		__u32 tmp32 = *((u32*)buf);
		le32_to_cpus(&tmp32);
		if (tmp32 != (AX88772A_IPG2_DEFAULT << 16 |
			AX88772A_IPG1_DEFAULT << 8 | AX88772A_IPG0_DEFAULT)) {
			printk("Non-authentic ASIX product\nASIX does not support it\n");
			ret = -ENODEV;		
			goto out2;
		}
	}

	/* Set RX_CTL to default values with 2k buffer, and enable cactus */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL,
			(AX_RX_CTL_START | AX_RX_CTL_AB),
			0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Reset RX_CTL failed: %d", ret);
		goto out2;
	}

	/* Asix framing packs multiple eth frames into a 2K usb bulk transfer */
	if (dev->driver_info->flags & FLAG_FRAMING_AX) {
		/* hard_mtu  is still the default - the device does not support
		   jumbo eth frames */
		dev->rx_urb_size = 2048;
	}

	kfree(buf);

	return ret;
out2:
	destroy_workqueue(ax772a_data->ax_work);
	kfree(ax772a_data);
	kfree(buf);
out1:
	return ret;
}

static void ax88772a_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct ax88772a_data *ax772a_data = (struct ax88772a_data *)dev->priv;

	if (ax772a_data) {

		flush_workqueue(ax772a_data->ax_work);
		destroy_workqueue(ax772a_data->ax_work);

		/* stop MAC operation */
		ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL,
					AX_RX_CTL_STOP, 0, 0, NULL);

		/* Power down PHY */
		ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
					AX_SWRESET_IPPD, 0, 0, NULL);

		kfree(ax772a_data);
	}
}

static int ax88772b_set_csums(struct usbnet *dev)
{
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;
	u16 checksum;

	if (ax772b_data->checksum & AX_RX_CHECKSUM)
		checksum = AX_RXCOE_DEF_CSUM;
	else
		checksum = 0;

	ax8817x_write_cmd(dev, AX_CMD_WRITE_RXCOE_CTL,
				 checksum, 0, 0, NULL);

	if (ax772b_data->checksum & AX_TX_CHECKSUM)
		checksum = AX_TXCOE_DEF_CSUM;
	else
		checksum = 0;

	ax8817x_write_cmd(dev, AX_CMD_WRITE_TXCOE_CTL,
				 checksum, 0, 0, NULL);

	return 0;
}
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0)
static u32 ax88772b_get_tx_csum(struct net_device *netdev)
{
	struct usbnet *dev = netdev_priv(netdev);
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;

	return ax772b_data->checksum & AX_TX_CHECKSUM;
}

static u32 ax88772b_get_rx_csum(struct net_device *netdev)
{
	struct usbnet *dev = netdev_priv(netdev);
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;

	return ax772b_data->checksum & AX_RX_CHECKSUM;
}

static int ax88772b_set_rx_csum(struct net_device *netdev, u32 val)
{
	struct usbnet *dev = netdev_priv(netdev);
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;

	if (val)
		ax772b_data->checksum |= AX_RX_CHECKSUM;
	else
		ax772b_data->checksum &= ~AX_RX_CHECKSUM;

	return ax88772b_set_csums(dev);
}

static int ax88772b_set_tx_csum(struct net_device *netdev, u32 val)
{
	struct usbnet *dev = netdev_priv(netdev);
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;

	if (val)
		ax772b_data->checksum |= AX_TX_CHECKSUM;
	else
		ax772b_data->checksum &= ~AX_TX_CHECKSUM;

	ethtool_op_set_tx_csum(netdev, val);

	return ax88772b_set_csums(dev);
}
#endif
static struct ethtool_ops ax88772b_ethtool_ops = {
	.get_drvinfo		= ax8817x_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_msglevel		= axusbnet_get_msglevel,
	.set_msglevel		= axusbnet_set_msglevel,
	.get_wol		= ax8817x_get_wol,
	.set_wol		= ax8817x_set_wol,
	.get_eeprom_len		= ax8817x_get_eeprom_len,
	.get_eeprom		= ax8817x_get_eeprom,
	.get_settings		= ax8817x_get_settings,
	.set_settings		= ax8817x_set_settings,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0)
	.set_tx_csum		= ax88772b_set_tx_csum,
	.get_tx_csum		= ax88772b_get_tx_csum,
	.get_rx_csum		= ax88772b_get_rx_csum,
	.set_rx_csum		= ax88772b_set_rx_csum,
#endif
};

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 29)
static const struct net_device_ops ax88772b_netdev_ops = {
	.ndo_open		= axusbnet_open,
	.ndo_stop		= axusbnet_stop,
	.ndo_start_xmit		= axusbnet_start_xmit,
	.ndo_tx_timeout		= axusbnet_tx_timeout,
	.ndo_change_mtu		= axusbnet_change_mtu,
	.ndo_do_ioctl		= ax8817x_ioctl,
	.ndo_get_stats		= axusbnet_get_stats,
	.ndo_set_mac_address	= ax8817x_set_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3, 2, 0)
	.ndo_set_multicast_list	= ax88772b_set_multicast,
#else
	.ndo_set_rx_mode	= ax88772b_set_multicast,
#endif
};
#endif

static int ax88772b_bind(struct usbnet *dev, struct usb_interface *intf)
{
	int ret;
	void *buf;
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;
	struct ax88772b_data *ax772b_data;
	u16 *tmp16;
	u8  *tmp8;
	u8 tempphyselect;
	bool internalphy;

	printk(version);
	axusbnet_get_endpoints(dev, intf);
	buf = kmalloc(6, GFP_KERNEL);
	if (!buf) {
		deverr(dev, "Cannot allocate memory for buffer");
		return -ENOMEM;
	}
	tmp16 = (u16 *)buf;
	ax772b_data = kmalloc(sizeof(*ax772b_data), GFP_KERNEL);
	if (!ax772b_data) {
		deverr(dev, "Cannot allocate memory for AX88772B data");
		kfree(buf);
		return -ENOMEM;
	}
	memset(ax772b_data, 0, sizeof(*ax772b_data));
	dev->priv = ax772b_data;
	ax772b_data->ax_work = create_singlethread_workqueue("ax88772b");
	if (!ax772b_data->ax_work) {
		kfree(buf);
		kfree(ax772b_data);
		return -ENOMEM;
	}

	ax772b_data->dev = dev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
	INIT_WORK(&ax772b_data->check_link, ax88772b_link_reset, dev);
#else
	INIT_WORK(&ax772b_data->check_link, ax88772b_link_reset);
#endif

	tmp8 = (u8 *)buf;
	ret = ax8817x_read_cmd(dev, AX_CMD_SW_PHY_STATUS,
			       0, 0, 1, tmp8);

	if (ret < 0) {
		deverr(dev,
		       "read SW interface selection status register failed: %d\n",
		       ret);
		goto err_out;
	}
	tempphyselect = *tmp8;
	tempphyselect &= 0x0C;

	if (tempphyselect == AX_PHYSEL_SSRMII) {
		internalphy = false;
		ax772b_data->OperationMode = OPERATION_MAC_MODE;
		ax772b_data->PhySelect = 0x00;
	} else if (tempphyselect == AX_PHYSEL_SSRRMII) {
		internalphy = true;
		ax772b_data->OperationMode = OPERATION_PHY_MODE;
		ax772b_data->PhySelect = 0x00;
	} else if (tempphyselect == AX_PHYSEL_SSMII) {
		internalphy = true;
		ax772b_data->OperationMode = OPERATION_MAC_MODE;
		ax772b_data->PhySelect = 0x01;
	} else {
		deverr(dev, "Unknown MII type\n");
		goto err_out;
	}

	/* reload eeprom data */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS, AXGPIOS_RSE,
				0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to enable GPIO finction: %d", ret);
		goto err_out;
	}
	msleep(5);

	/* Get the EEPROM data*/
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_EEPROM, 0x18, 0, 2,
			       (void *)tmp16);
	if (ret < 0) {
		deverr(dev, "read SROM address 18h failed: %d", ret);
		goto err_out;
	}
	le16_to_cpus(tmp16);
	ax772b_data->psc = *tmp16 & 0xFF00;
	/* End of get EEPROM data */

	/* Get the MAC address */
	memset(buf, 0, ETH_ALEN);
	ret = ax8817x_get_mac(dev, buf);
	if (ret < 0) {
		deverr(dev, "Get HW address failed: %d", ret);
		goto err_out;
	}

	/* Initialize MII structure */
	dev->mii.dev = dev->net;
	dev->mii.mdio_read = ax8817x_mdio_read_le;
	dev->mii.mdio_write = ax88772b_mdio_write_le;
	dev->mii.phy_id_mask = 0xff;
	dev->mii.reg_num_mask = 0xff;

	/* Get the PHY id */
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_PHY_ID, 0, 0, 2, buf);
	if (ret < 0) {
		deverr(dev, "Error reading PHY ID: %02x", ret);
		goto err_out;
	} else if (ret < 2) {
		/* this should always return 2 bytes */
		deverr(dev, "Read PHYID returned less than 2 bytes: ret=%02x",
		    ret);
		ret = -EIO;
		goto err_out;
	}

	if (internalphy)
		dev->mii.phy_id = *((u8 *)buf + 1);
	else
		dev->mii.phy_id = *((u8 *)buf);

	ret = ax8817x_write_cmd(dev, AX_CMD_SW_PHY_SELECT,
				ax772b_data->PhySelect, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Select PHY #1 failed: %d", ret);
		goto err_out;
	}

#if 0
	/* select the embedded 10/100 Ethernet PHY */
	ret = ax8817x_write_cmd(dev, AX_CMD_SW_PHY_SELECT,
			AX_PHYSEL_SSEN | AX_PHYSEL_PSEL | AX_PHYSEL_SSMII,
			0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Select PHY #1 failed: %d", ret);
		goto err_out;
	}

	if (dev->mii.phy_id != 0x10) {
		deverr(dev, "Got wrong PHY ID: %02x", dev->mii.phy_id);
		ret = -EIO;
		goto err_out;
	}
#endif
	ret = ax88772a_phy_powerup(dev);
	if (ret < 0)
		goto err_out;

	/* stop MAC operation */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL,
				AX_RX_CTL_STOP, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Reset RX_CTL failed: %d", ret);
		goto err_out;
	}

	/* make sure the driver can enable sw mii operation */
	ret = ax8817x_write_cmd(dev, AX_CMD_SET_SW_MII, 0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Enabling software MII failed: %d", ret);
		goto err_out;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	dev->net->do_ioctl = ax8817x_ioctl;
	dev->net->set_multicast_list = ax88772b_set_multicast;
	dev->net->set_mac_address = ax88772b_set_mac_addr;
#else
	dev->net->netdev_ops = &ax88772b_netdev_ops;
#endif

	dev->net->ethtool_ops = &ax88772b_ethtool_ops;

	/* Register suspend and resume functions */
	data->suspend = ax88772b_suspend;
	data->resume = ax88772b_resume;

	if (ax772b_data->OperationMode == OPERATION_PHY_MODE)
		ax8817x_mdio_write_le(dev->net, dev->mii.phy_id
						, MII_BMCR, 0x3900);

	if (dev->mii.phy_id != 0x10)
		ax8817x_mdio_write_le(dev->net, 0x10, MII_BMCR, 0x3900);

	if (dev->mii.phy_id == 0x10 && ax772b_data->OperationMode
						!= OPERATION_PHY_MODE) {

		*tmp16 = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, 0x12);
		ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, 0x12,
					((*tmp16 & 0xFF9F) | 0x0040));
	}

	ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_ADVERTISE,
			ADVERTISE_ALL | ADVERTISE_CSMA | ADVERTISE_PAUSE_CAP);

	mii_nway_restart(&dev->mii);

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE,
				0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to write medium mode: %d", ret);
		goto err_out;
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_IPG0,
			AX88772A_IPG0_DEFAULT | AX88772A_IPG1_DEFAULT << 8,
			AX88772A_IPG2_DEFAULT, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Failed to write interframe gap: %d", ret);
		goto err_out;
	}

	memset(buf, 0, 4);
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_IPG012, 0, 0, 3, buf);
	*((u8 *)buf + 3) = 0x00;
	if (ret < 0) {
		deverr(dev, "Failed to read IPG,IPG1,IPG2 failed: %d", ret);
		goto err_out;
	} else {
		__u32 tmp32 = *((u32*)buf);
		le32_to_cpus(&tmp32);
		if (tmp32 != (AX88772A_IPG2_DEFAULT << 16 |
			AX88772A_IPG1_DEFAULT << 8 | AX88772A_IPG0_DEFAULT)) {
			printk("Non-authentic ASIX product\nASIX does not support it\n");
			ret = -ENODEV;		
			goto err_out;
		}
	}

	dev->net->features |= NETIF_F_IP_CSUM;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 22)
	dev->net->features |= NETIF_F_IPV6_CSUM;
#endif

	ax772b_data->checksum = AX_RX_CHECKSUM | AX_TX_CHECKSUM;
	ret = ax88772b_set_csums(dev);
	if (ret < 0) {
		deverr(dev, "Write RX_COE/TX_COE failed: %d", ret);
		goto err_out;
	}

	dev->rx_size = bsize & 0x07;
	if (dev->udev->speed == USB_SPEED_HIGH) {

		ret = ax8817x_write_cmd(dev, 0x2A,
				AX88772B_BULKIN_SIZE[dev->rx_size].byte_cnt,
				AX88772B_BULKIN_SIZE[dev->rx_size].threshold,
				0, NULL);
		if (ret < 0) {
			deverr(dev, "Reset RX_CTL failed: %d", ret);
			goto err_out;
		}

		dev->rx_urb_size = AX88772B_BULKIN_SIZE[dev->rx_size].size;
	} else {
		ret = ax8817x_write_cmd(dev, 0x2A,
				0x8000, 0x8001, 0, NULL);
		if (ret < 0) {
			deverr(dev, "Reset RX_CTL failed: %d", ret);
			goto err_out;
		}
		dev->rx_urb_size = 2048;
	}

	/* Configure RX header type */
	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL,
		      (AX_RX_CTL_START | AX_RX_CTL_AB | AX_RX_HEADER_DEFAULT),
		      0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Reset RX_CTL failed: %d", ret);
		goto err_out;
	}

	/* Overwrite power saving configuration from eeprom */
	ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_IPRL |
				(ax772b_data->psc & 0x7FFF), 0, 0, NULL);

	if (ret < 0) {
		deverr(dev, "Failed to configure PHY power saving: %d", ret);
		goto err_out;
	}

	if (ax772b_data->OperationMode == OPERATION_PHY_MODE)
		netif_carrier_on(dev->net);

	kfree(buf);

	return ret;
err_out:
	destroy_workqueue(ax772b_data->ax_work);
	kfree(buf);
	kfree(ax772b_data);
	return ret;
}

static void ax88772b_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;

	if (ax772b_data) {

		flush_workqueue(ax772b_data->ax_work);
		destroy_workqueue(ax772b_data->ax_work);

		/* stop MAC operation */
		ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL,
					AX_RX_CTL_STOP, 0, 0, NULL);

		/* Power down PHY */
		ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
					AX_SWRESET_IPPD, 0, 0, NULL);

		kfree(ax772b_data);
	}
}

static int
ax88178_media_check(struct usbnet *dev, struct ax88178_data *ax178dataptr)
{
	int fullduplex, i = 0;
	u16 tempshort = 0;
	u16 media;
	u16 advertise, lpa, result, stat1000, _lpa, _stat1000, delay = 5 * HZ;
	unsigned long jtimeout;

	jtimeout = jiffies + delay;
	do {
		_lpa = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_LPA);
		_stat1000 = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id,
						  MII_STAT1000);

		lpa = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_LPA);
		stat1000 = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id,
						MII_STAT1000);

		if (time_after(jiffies, jtimeout))
			break;

	} while ((_lpa != lpa) || (_stat1000 != stat1000) || i++ < 3);

	advertise = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id,
					 MII_ADVERTISE);
	result = advertise & lpa;

	if ((ax178dataptr->PhyMode == PHY_MODE_MARVELL) &&
	    (ax178dataptr->LedMode == 1)) {
		tempshort = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id,
						 MARVELL_MANUAL_LED) & 0xfc0f;
	}

	fullduplex = 1;
	if (stat1000 & LPA_1000FULL) {
		media = MEDIUM_GIGA_MODE | MEDIUM_FULL_DUPLEX_MODE |
			MEDIUM_ENABLE_125MHZ | MEDIUM_ENABLE_RECEIVE;
		if ((ax178dataptr->PhyMode == PHY_MODE_MARVELL) &&
		    (ax178dataptr->LedMode == 1))
			tempshort |= 0x3e0;
	} else if (result & LPA_100FULL) {
		media = MEDIUM_FULL_DUPLEX_MODE | MEDIUM_ENABLE_RECEIVE |
			MEDIUM_MII_100M_MODE;
		if ((ax178dataptr->PhyMode == PHY_MODE_MARVELL) &&
		    (ax178dataptr->LedMode == 1))
			tempshort |= 0x3b0;
	} else if (result & LPA_100HALF) {
		fullduplex = 0;
		media = MEDIUM_ENABLE_RECEIVE | MEDIUM_MII_100M_MODE;
		if ((ax178dataptr->PhyMode == PHY_MODE_MARVELL) &&
		    (ax178dataptr->LedMode == 1))
			tempshort |= 0x3b0;
	} else if (result & LPA_10FULL) {
		media = MEDIUM_FULL_DUPLEX_MODE | MEDIUM_ENABLE_RECEIVE;
		if ((ax178dataptr->PhyMode == PHY_MODE_MARVELL) &&
		    (ax178dataptr->LedMode == 1))
			tempshort |= 0x2f0;
	} else {
		media = MEDIUM_ENABLE_RECEIVE;
		fullduplex = 0;
		if ((ax178dataptr->PhyMode == PHY_MODE_MARVELL) &&
		    (ax178dataptr->LedMode == 1))
				tempshort |= 0x02f0;
	}

	if ((ax178dataptr->PhyMode == PHY_MODE_MARVELL) &&
	    (ax178dataptr->LedMode == 1)) {
		ax8817x_mdio_write_le(dev->net,
			dev->mii.phy_id, MARVELL_MANUAL_LED, tempshort);
	}

	media |= 0x0004;
	if (ax178dataptr->UseRgmii)
		media |= 0x0008;
	if (fullduplex) {
		media |= 0x0020;  /* ebable tx flow control as default; */
		media |= 0x0010;  /* ebable rx flow control as default; */
	}

	return media;
}

static void Vitess_8601_Init(struct usbnet *dev, int state)
{
	u16 reg;

	switch (state) {
	case 0:	/* tx, rx clock skew */
		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id, 31, 1);
		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id, 28, 0);
		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id, 31, 0);
		break;

	case 1:
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 31, 0x52B5);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18, 0x009E);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, 0xDD39);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x87AA);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0xA7B4);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18,
				ax8817x_swmii_mdio_read_le(dev->net,
						dev->mii.phy_id, 18));

		reg = (ax8817x_swmii_mdio_read_le(dev->net,
				dev->mii.phy_id, 17) & ~0x003f) | 0x003c;
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, reg);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x87B4);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0xa794);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18,
				ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 18));

		reg = (ax8817x_swmii_mdio_read_le(dev->net,
				dev->mii.phy_id, 17) & ~0x003f) | 0x003e;
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, reg);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x8794);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18, 0x00f7);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, 0xbe36);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x879e);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0xa7a0);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18,
				ax8817x_swmii_mdio_read_le(dev->net,
						dev->mii.phy_id, 18));

		reg = (ax8817x_swmii_mdio_read_le(dev->net,
				dev->mii.phy_id, 17) & ~0x003f) | 0x0034;
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, reg);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x87a0);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18, 0x003c);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, 0xf3cf);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x87a2);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18, 0x003c);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, 0xf3cf);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x87a4);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18, 0x003c);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, 0xd287);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x87a6);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0xa7a8);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18,
				ax8817x_swmii_mdio_read_le(dev->net,
						dev->mii.phy_id, 18));

		reg = (ax8817x_swmii_mdio_read_le(dev->net,
				dev->mii.phy_id, 17) & ~0x0fff) | 0x0125;
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, reg);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x87a8);

		/* Enable Smart Pre-emphasis */
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0xa7fa);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 18,
				ax8817x_swmii_mdio_read_le(dev->net,
						dev->mii.phy_id, 18));

		reg = (ax8817x_swmii_mdio_read_le(dev->net,
				dev->mii.phy_id, 17) & ~0x0008) | 0x0008;

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 17, reg);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 16, 0x87fa);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 31, 0);

		break;
	}
}

static void
marvell_88E1510_magic_init(struct usbnet *dev)
{
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 22, 0xff);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 17, 0x214b);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 16, 0x2144);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 17, 0x0c28);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 16, 0x2146);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 17, 0xb233);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 16, 0x214d);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 17, 0xcc0c);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 16, 0x2159);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 22, 0x00fb);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 7, 0xc00d);
	ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 22, 0);
}

static int
ax88178_phy_init(struct usbnet *dev, struct ax88178_data *ax178dataptr)
{
	int i;
	u16 phyanar, phyauxctrl, phyctrl, tempshort, phyid1;
	u16 phyreg = 0;

	/* Disable MII operation of AX88178 Hardware */
	ax8817x_write_cmd(dev, AX_CMD_SET_SW_MII, 0x0000, 0, 0, NULL);


	/* Read SROM - MiiPhy Address (ID) */
	ax8817x_read_cmd(dev, AX_CMD_READ_PHY_ID, 0, 0, 2, &dev->mii.phy_id);
	le32_to_cpus(&dev->mii.phy_id);

	/* Initialize MII structure */
	dev->mii.phy_id >>= 8;
	dev->mii.phy_id &= PHY_ID_MASK;
	dev->mii.dev = dev->net;
	dev->mii.mdio_read = ax8817x_mdio_read_le;
	dev->mii.mdio_write = ax8817x_mdio_write_le;
	dev->mii.phy_id_mask = 0x3f;
	dev->mii.reg_num_mask = 0x1f;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 11)
	dev->mii.supports_gmii = 1;
#endif

	if (ax178dataptr->PhyMode == PHY_MODE_MAC_TO_MAC_GMII) {
		ax178dataptr->UseRgmii = 0;
		ax178dataptr->MediaLink = MEDIUM_GIGA_MODE |
					  MEDIUM_FULL_DUPLEX_MODE |
					  MEDIUM_ENABLE_125MHZ |
					  MEDIUM_ENABLE_RECEIVE |
					  MEDIUM_ENABLE_RX_FLOWCTRL |
					  MEDIUM_ENABLE_TX_FLOWCTRL;
		goto SKIPPHYSETTING;
	}

	/* test read phy register 2 */
	if (!ax178dataptr->UseGpio0) {
		i = 1000;
		while (i--) {
			phyid1 = ax8817x_swmii_mdio_read_le(dev->net,
						dev->mii.phy_id, GMII_PHY_OUI);
			if ((phyid1 == 0x000f) || (phyid1 == 0x0141) ||
			    (phyid1 == 0x0282) || (phyid1 == 0x004d) ||
			    (phyid1 == 0x0243) || (phyid1 == 0x001C) ||
			    (phyid1 == 0x0007))
				break;
			msleep(5);
		}
		if (i < 0)
			return -EIO;
	}

	ax178dataptr->UseRgmii = 0;
	if (ax178dataptr->PhyMode == PHY_MODE_MARVELL) {
		phyreg = ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 27);
		if (!(phyreg & 4) && !(ax178dataptr->LedMode & 0x10)) {
			ax178dataptr->UseRgmii = 1;
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 20, 0x82);
			ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
		} else if (ax178dataptr->LedMode & 0x10) {

			ax178dataptr->UseRgmii = 1;
			ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
			marvell_88E1510_magic_init(dev);

			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 22, 2);

			phyreg = ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 21);

			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 21, phyreg | 0x30);

			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 22, 0);
		}
	} else if ((ax178dataptr->PhyMode == PHY_MODE_AGERE_V0) ||
		 (ax178dataptr->PhyMode == PHY_MODE_AGERE_V0_GMII)) {
		if (ax178dataptr->PhyMode == PHY_MODE_AGERE_V0) {
			ax178dataptr->UseRgmii = 1;
			ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
		}
	} else if (ax178dataptr->PhyMode == PHY_MODE_CICADA_V1) {
		/* not Cameo */
		if (!ax178dataptr->UseGpio0 || ax178dataptr->LedMode) {
			ax178dataptr->UseRgmii = 1;
			ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
		}

		for (i = 0; i < (sizeof(CICADA_FAMILY_HWINIT) /
				 sizeof(CICADA_FAMILY_HWINIT[0])); i++) {
			ax8817x_swmii_mdio_write_le(dev->net,
						    dev->mii.phy_id,
					CICADA_FAMILY_HWINIT[i].offset,
					CICADA_FAMILY_HWINIT[i].value);
		}

	} else if (ax178dataptr->PhyMode == PHY_MODE_CICADA_V2) {
		/* not Cameo */
		if (!ax178dataptr->UseGpio0 || ax178dataptr->LedMode) {
			ax178dataptr->UseRgmii = 1;
			ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
		}

		for (i = 0; i < (sizeof(CICADA_V2_HWINIT) /
				 sizeof(CICADA_V2_HWINIT[0])); i++) {
			ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, CICADA_V2_HWINIT[i].offset,
				CICADA_V2_HWINIT[i].value);
		}
	} else if (ax178dataptr->PhyMode == PHY_MODE_CICADA_V2_ASIX) {
		/* not Cameo */
		if (!ax178dataptr->UseGpio0 || ax178dataptr->LedMode) {
			ax178dataptr->UseRgmii = 1;
			ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
		}

		for (i = 0; i < (sizeof(CICADA_V2_HWINIT) /
				 sizeof(CICADA_V2_HWINIT[0])); i++) {
			ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, CICADA_V2_HWINIT[i].offset,
				CICADA_V2_HWINIT[i].value);
		}
	} else if (ax178dataptr->PhyMode == PHY_MODE_RTL8211CL) {
		ax178dataptr->UseRgmii = 1;
		ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
	} else if (ax178dataptr->PhyMode == PHY_MODE_RTL8211BN) {
		ax178dataptr->UseRgmii = 1;
		ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
	} else if (ax178dataptr->PhyMode == PHY_MODE_RTL8251CL) {
		ax178dataptr->UseRgmii = 1;
		ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
	} else if (ax178dataptr->PhyMode == PHY_MODE_VSC8601) {
		ax178dataptr->UseRgmii = 1;
		ax178dataptr->MediaLink |= MEDIUM_ENABLE_125MHZ;
		/* Vitess_8601_Init (dev, 0); */
	}

	if (ax178dataptr->PhyMode != PHY_MODE_ATTANSIC_V0) {
		/* software reset */
		ax8817x_swmii_mdio_write_le(
			dev->net, dev->mii.phy_id, GMII_PHY_CONTROL,
			ax8817x_swmii_mdio_read_le(
				dev->net, dev->mii.phy_id, GMII_PHY_CONTROL)
				| GMII_CONTROL_RESET);
		msleep(1);
	}

	if ((ax178dataptr->PhyMode == PHY_MODE_AGERE_V0) ||
	    (ax178dataptr->PhyMode == PHY_MODE_AGERE_V0_GMII)) {
		if (ax178dataptr->PhyMode == PHY_MODE_AGERE_V0) {
			i = 1000;
			while (i--) {
				ax8817x_swmii_mdio_write_le(dev->net,
						dev->mii.phy_id, 21, 0x1001);

				phyreg = ax8817x_swmii_mdio_read_le(dev->net,
						dev->mii.phy_id, 21);
				if ((phyreg & 0xf00f) == 0x1001)
					break;
			}
			if (i < 0)
				return -EIO;
		}

		if (ax178dataptr->LedMode == 4) {
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 28, 0x7417);
		} else if (ax178dataptr->LedMode == 9) {
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 28, 0x7a10);
		} else if (ax178dataptr->LedMode == 10) {
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 28, 0x7a13);
		}

		for (i = 0; i < (sizeof(AGERE_FAMILY_HWINIT) /
				 sizeof(AGERE_FAMILY_HWINIT[0])); i++) {
			ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, AGERE_FAMILY_HWINIT[i].offset,
				AGERE_FAMILY_HWINIT[i].value);
		}
	} else if (ax178dataptr->PhyMode == PHY_MODE_RTL8211CL) {

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 0x1f, 0x0005);
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 0x0c, 0);

		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id, 0x01,
					(ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 0x01) | 0x0080));
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, 0x1f, 0);

		if (ax178dataptr->LedMode == 12) {
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 0x1f, 0x0002);
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 0x1a, 0x00cb);
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 0x1f, 0);
		}
	} else if (ax178dataptr->PhyMode == PHY_MODE_VSC8601) {
		Vitess_8601_Init(dev, 1);
	}

	/* read phy register 0 */
	phyctrl = ax8817x_swmii_mdio_read_le(dev->net,
				dev->mii.phy_id, GMII_PHY_CONTROL);
	tempshort = phyctrl;
	phyctrl &= ~(GMII_CONTROL_POWER_DOWN | GMII_CONTROL_ISOLATE);
	if (phyctrl != tempshort) {
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, GMII_PHY_CONTROL, phyctrl);
	}

	/* LED */
	if (ax178dataptr->PhyMode == PHY_MODE_MARVELL) {
		if (ax178dataptr->LedMode == 1)	{

			phyreg = (ax8817x_swmii_mdio_read_le(dev->net,
				dev->mii.phy_id, 24) & 0xf8ff) | (1 + 0x100);
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 24, phyreg);
			phyreg = ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 25) & 0xfc0f;
		} else if (ax178dataptr->LedMode == 2) {

			phyreg = (ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 24) & 0xf886) |
					(1 + 0x10 + 0x300);
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 24, phyreg);
		} else if (ax178dataptr->LedMode == 5) {

			phyreg = (ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 24) & 0xf8be) |
					(1 + 0x40 + 0x300);
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 24, phyreg);
		} else if (ax178dataptr->LedMode == 7) {

			phyreg = (ax8817x_swmii_mdio_read_le(dev->net,
						dev->mii.phy_id, 24) & 0xf8ff) |
						(1 + 0x100);
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 24, phyreg);

		} else if (ax178dataptr->LedMode == 8) {

			phyreg = (ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 24) & 0xf8be) |
					(1 + 0x40 + 0x100);
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 24, phyreg);

		} else if (ax178dataptr->LedMode == 11) {

			phyreg = ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 24) & 0x4106;
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 24, phyreg);
		} else if (ax178dataptr->LedMode == 0x10) {
			/* MARVEL 88e1510 use default led setting */
		}

	} else if ((ax178dataptr->PhyMode == PHY_MODE_CICADA_V1) ||
		   (ax178dataptr->PhyMode == PHY_MODE_CICADA_V2) ||
		   (ax178dataptr->PhyMode == PHY_MODE_CICADA_V2_ASIX)) {

		if (ax178dataptr->LedMode == 3) {

			phyreg = (ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 27) & 0xFCFF) | 0x0100;
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 27, phyreg);
		}

	}

	if (ax178dataptr->PhyMode == PHY_MODE_MARVELL) {
		if (ax178dataptr->LedMode == 1)
			phyreg |= 0x3f0;
	}

	phyanar = 1 | (GMII_ANAR_PAUSE | GMII_ANAR_100TXFD | GMII_ANAR_100TX |
		       GMII_ANAR_10TFD | GMII_ANAR_10T | GMII_ANAR_ASYM_PAUSE);

	phyauxctrl = GMII_1000_AUX_CTRL_FD_CAPABLE;

	ax8817x_swmii_mdio_write_le(dev->net,
			dev->mii.phy_id, GMII_PHY_ANAR, phyanar);
	ax8817x_swmii_mdio_write_le(dev->net,
			dev->mii.phy_id, GMII_PHY_1000BT_CONTROL, phyauxctrl);

	if (ax178dataptr->PhyMode == PHY_MODE_VSC8601) {
		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id,
					    31, 0x52B5);
		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id,
					    16, 0xA7F8);

		tempshort = ax8817x_swmii_mdio_read_le(dev->net,
					dev->mii.phy_id, 17) & (~0x0018);

		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id,
					    17, tempshort);

		tempshort = ax8817x_swmii_mdio_read_le(dev->net,
						       dev->mii.phy_id, 18);

		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id, 18,
					    tempshort);

		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id,
					    16, 0x87F8);
		ax8817x_swmii_mdio_write_le(dev->net, dev->mii.phy_id,
					    31, 0);
	}

	if (ax178dataptr->PhyMode == PHY_MODE_ATTANSIC_V0) {
		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, GMII_PHY_CONTROL, 0x9000);

	} else {
		phyctrl &= ~GMII_CONTROL_LOOPBACK;
		phyctrl |= (GMII_CONTROL_ENABLE_AUTO | GMII_CONTROL_START_AUTO);

		ax8817x_swmii_mdio_write_le(dev->net,
				dev->mii.phy_id, GMII_PHY_CONTROL, phyctrl);
	}

	if (ax178dataptr->PhyMode == PHY_MODE_MARVELL) {
		if (ax178dataptr->LedMode == 1)
			ax8817x_swmii_mdio_write_le(dev->net,
					dev->mii.phy_id, 25, phyreg);
	}

SKIPPHYSETTING:

	ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE,
			ax178dataptr->MediaLink, 0, 0, NULL);

	ax8817x_write_cmd(dev, AX_CMD_WRITE_IPG0,
			AX88772_IPG0_DEFAULT | (AX88772_IPG1_DEFAULT << 8),
			AX88772_IPG2_DEFAULT, 0, NULL);

	msleep(1);

	ax8817x_write_cmd(dev, AX_CMD_SET_HW_MII, 0, 0, 0, NULL);

	return 0;
}

static int ax88178_bind(struct usbnet *dev, struct usb_interface *intf)
{
	int ret;
	void *buf;
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;
	struct ax88178_data *ax178dataptr = NULL;

	axusbnet_get_endpoints(dev, intf);

	buf = kmalloc(6, GFP_KERNEL);
	if (!buf) {
		deverr(dev, "Cannot allocate memory for buffer");
		return -ENOMEM;
	}

	/* allocate 178 data */
	ax178dataptr = kmalloc(sizeof(*ax178dataptr), GFP_KERNEL);
	if (!ax178dataptr) {
		deverr(dev, "Cannot allocate memory for AX88178 data");
		ret = -ENOMEM;
		goto error_out;
	}
	memset(ax178dataptr, 0, sizeof(struct ax88178_data));
	dev->priv = ax178dataptr;
	/* end of allocate 178 data */

	/* Get the EEPROM data*/
	ret = ax8817x_read_cmd(dev, AX_CMD_READ_EEPROM, 0x0017, 0, 2,
			       (void *)(&ax178dataptr->EepromData));
	if (ret < 0) {
		deverr(dev, "read SROM address 17h failed: %d", ret);
		goto error_out;
	}
	le16_to_cpus(&ax178dataptr->EepromData);
	/* End of get EEPROM data */

	if (ax178dataptr->EepromData == 0xffff) {
		ax178dataptr->PhyMode  = PHY_MODE_MARVELL;
		ax178dataptr->LedMode  = 0;
		ax178dataptr->UseGpio0 = 1; /* True */
	} else {
		ax178dataptr->PhyMode = (u8)(ax178dataptr->EepromData &
					     EEPROMMASK);
		ax178dataptr->LedMode = (u8)(ax178dataptr->EepromData >> 8);

		/* for buffalo new (use gpio2) */
		if (ax178dataptr->LedMode == 6)
			ax178dataptr->LedMode = 1;
		else if (ax178dataptr->LedMode == 1)
			ax178dataptr->BuffaloOld = 1;


		if (ax178dataptr->EepromData & 0x80)
			ax178dataptr->UseGpio0 = 0; /* MARVEL se and other */
		else
			ax178dataptr->UseGpio0 = 1; /* cameo */
	}

	if (ax178dataptr->UseGpio0) {

		if (ax178dataptr->PhyMode == PHY_MODE_MARVELL) {
			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					AXGPIOS_GPO0EN | AXGPIOS_RSE,
					0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(25);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					(AXGPIOS_GPO2 | AXGPIOS_GPO2EN |
					 AXGPIOS_GPO0EN), 0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(15);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					AXGPIOS_GPO2EN | AXGPIOS_GPO0EN,
					0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(245);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					(AXGPIOS_GPO2 | AXGPIOS_GPO2EN |
					 AXGPIOS_GPO0EN), 0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

		} else { /* vitesse */

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					(AXGPIOS_RSE | AXGPIOS_GPO0EN |
					 AXGPIOS_GPO0), 0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(25);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					(AXGPIOS_GPO0EN | AXGPIOS_GPO0 |
					 AXGPIOS_GPO2EN | AXGPIOS_GPO2),
					0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(25);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					(AXGPIOS_GPO0EN | AXGPIOS_GPO0 |
					 AXGPIOS_GPO2EN), 0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(245);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
						(AXGPIOS_GPO0EN | AXGPIOS_GPO0 |
						AXGPIOS_GPO2EN | AXGPIOS_GPO2),
						0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}
		}
	} else {	/* use gpio1 */
		ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
					(AXGPIOS_GPO1 | AXGPIOS_GPO1EN |
					AXGPIOS_RSE), 0, 0, NULL);
		if (ret < 0) {
			deverr(dev, "write GPIO failed: %d", ret);
			goto error_out;
		}

		if (ax178dataptr->BuffaloOld) {
			msleep(350);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
						AXGPIOS_GPO1EN, 0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(350);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
						AXGPIOS_GPO1EN | AXGPIOS_GPO1,
						0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}
		} else {
			msleep(25);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
						(AXGPIOS_GPO1EN | AXGPIOS_GPO1 |
						AXGPIOS_GPO2EN | AXGPIOS_GPO2),
						0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(25);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
						(AXGPIOS_GPO1EN | AXGPIOS_GPO1 |
						AXGPIOS_GPO2EN), 0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}

			msleep(245);

			ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_GPIOS,
						(AXGPIOS_GPO1EN | AXGPIOS_GPO1 |
						AXGPIOS_GPO2EN | AXGPIOS_GPO2),
						0, 0, NULL);
			if (ret < 0) {
				deverr(dev, "write GPIO failed: %d", ret);
				goto error_out;
			}
		}
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_SW_PHY_SELECT, 0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Select PHY failed: %d", ret);
		goto error_out;
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_IPPD |
				AX_SWRESET_PRL, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Issue sw reset failed: %d", ret);
		goto error_out;
	}

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL, 0, 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "Issue rx ctrl failed: %d", ret);
		goto error_out;
	}

	/* Get the MAC address */
	memset(buf, 0, ETH_ALEN);
	ax8817x_get_mac(dev, buf);
	if (ret < 0)
		goto error_out;
	/* End of get MAC address */

	ret = ax88178_phy_init(dev, ax178dataptr);
	if (ret < 0)
		goto error_out;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	dev->net->do_ioctl = ax8817x_ioctl;
	dev->net->set_multicast_list = ax88178_set_multicast;
	dev->net->set_mac_address = ax8817x_set_mac_addr;
#else
	dev->net->netdev_ops = &ax88178_netdev_ops;
#endif
	dev->net->ethtool_ops = &ax8817x_ethtool_ops;

	/* Register suspend and resume functions */
	data->suspend = ax88772_suspend;
	data->resume = ax88772_resume;

	if (dev->driver_info->flags & FLAG_FRAMING_AX)
		dev->rx_urb_size = 16384;

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL, (AX_RX_CTL_MFB |
				AX_RX_CTL_START | AX_RX_CTL_AB), 0, 0, NULL);
	if (ret < 0) {
		deverr(dev, "write RX ctrl reg failed: %d", ret);
		goto error_out;
	}

	kfree(buf);
	printk(version);
	return ret;

error_out:
	kfree(ax178dataptr);
	kfree(buf);
	return ret;
}

static void ax88178_unbind(struct usbnet *dev, struct usb_interface *intf)
{
	struct ax88178_data *ax178dataptr = (struct ax88178_data *)dev->priv;

	if (ax178dataptr) {

		/* stop MAC operation */
		ax8817x_write_cmd(dev, AX_CMD_WRITE_RX_CTL,
					AX_RX_CTL_STOP, 0, 0, NULL);

		kfree(ax178dataptr);
	}
}

static int ax88772_rx_fixup(struct usbnet *dev, struct sk_buff *skb)
{
	u8  *head;
	u32  header;
	char *packet;
	struct sk_buff *ax_skb;
	u16 size;

	head = (u8 *) skb->data;
	memcpy(&header, head, sizeof(header));
	le32_to_cpus(&header);
	packet = head + sizeof(header);

	skb_pull(skb, 4);

	while (skb->len > 0) {
		if ((short)(header & 0x00007ff) !=
		    ~((short)(((header & 0xffff0000) | 0xf8000000) >> 16))) {
			deverr(dev, "header length data is error 0x%08x, %d\n",
				header, skb->len);
		}
		/* get the packet length */
		size = (u16) (header & 0x00007ff);

		if ((skb->len) - ((size + 1) & 0xfffe) == 0) {

			/* Make sure ip header is aligned on 32-bit boundary */
			if (!((unsigned long)skb->data & 0x02)) {
				memmove(skb->data - 2, skb->data, size);
				skb->data -= 2;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
				skb->tail = skb->data + size;
#else
				skb_set_tail_pointer(skb, size);
#endif
			}
			skb->truesize = size + sizeof(struct sk_buff);
			skb->len = size;

			return 2;
		}

		if (size > ETH_FRAME_LEN) {
			deverr(dev, "invalid rx length %d", size);
			return 0;
		}
#ifndef RX_SKB_COPY
		ax_skb = skb_clone(skb, GFP_ATOMIC);
#else
		ax_skb = alloc_skb(size + NET_IP_ALIGN, GFP_ATOMIC);	
		skb_reserve(ax_skb, NET_IP_ALIGN);
#endif
		if (ax_skb) {
#ifndef RX_SKB_COPY
			/* Make sure ip header is aligned on 32-bit boundary */
			if (!((unsigned long)packet & 0x02)) {
				memmove(packet - 2, packet, size);
				packet -= 2;
			}
			ax_skb->data = packet;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
			ax_skb->tail = packet + size;
#else
			skb_set_tail_pointer(ax_skb, size);
#endif

#else
			skb_put(ax_skb, size);
			memcpy(ax_skb->data, packet , size);
#endif
			ax_skb->truesize = size + sizeof(struct sk_buff);
			axusbnet_skb_return(dev, ax_skb);

		} else {
			return 0;
		}

		skb_pull(skb, (size + 1) & 0xfffe);

		if (skb->len == 0)
			break;

		head = (u8 *) skb->data;
		memcpy(&header, head, sizeof(header));
		le32_to_cpus(&header);
		packet = head + sizeof(header);
		skb_pull(skb, 4);
	}

	if (skb->len < 0) {
		deverr(dev, "invalid rx length %d", skb->len);
		return 0;
	}
	return 1;
}

static struct sk_buff *ax88772_tx_fixup(struct usbnet *dev, struct sk_buff *skb,
					gfp_t flags)
{
	int padlen = ((skb->len + 4) % 512) ? 0 : 4;
	u32 packet_len;
	u32 padbytes = 0xffff0000;

#if (!AX_FORCE_BUFF_ALIGN)
	int headroom = skb_headroom(skb);
	int tailroom = skb_tailroom(skb);

	if ((!skb_cloned(skb))
	    && ((headroom + tailroom) >= (4 + padlen))) {
		if ((headroom < 4) || (tailroom < padlen)) {
			skb->data = memmove(skb->head + 4, skb->data, skb->len);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
		skb->tail = skb->data + skb->len;
#else
		skb_set_tail_pointer(skb, skb->len);
#endif
		}
	} else
#endif
	{
		struct sk_buff *skb2;
		skb2 = skb_copy_expand(skb, 4, padlen, flags);
		dev_kfree_skb_any(skb);
		skb = skb2;
		if (!skb)
			return NULL;
	}

	skb_push(skb, 4);
	packet_len = (((skb->len - 4) ^ 0x0000ffff) << 16) + (skb->len - 4);
	cpu_to_le32s(&packet_len);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
	memcpy(skb->data, &packet_len, sizeof(packet_len));
#else
	skb_copy_to_linear_data(skb, &packet_len, sizeof(packet_len));
#endif

	if ((skb->len % 512) == 0) {
		cpu_to_le32s(&padbytes);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
		memcpy(skb->tail, &padbytes, sizeof(padbytes));
#else
		memcpy(skb_tail_pointer(skb), &padbytes, sizeof(padbytes));
#endif
		skb_put(skb, sizeof(padbytes));
	}
	return skb;
}

static void
ax88772b_rx_checksum(struct sk_buff *skb, struct ax88772b_rx_header *rx_hdr)
{
	skb->ip_summed = CHECKSUM_NONE;

	/* checksum error bit is set */
	if (rx_hdr->l3_csum_err || rx_hdr->l4_csum_err)
		return;

	/* It must be a TCP or UDP packet with a valid checksum */
	if ((rx_hdr->l4_type == AX_RXHDR_L4_TYPE_TCP) ||
	    (rx_hdr->l4_type == AX_RXHDR_L4_TYPE_UDP)) {
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	}
}

static int ax88772b_rx_fixup(struct usbnet *dev, struct sk_buff *skb)
{
	struct ax88772b_rx_header rx_hdr;
	struct sk_buff *ax_skb;
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;

	while (skb->len > 0) {

		le16_to_cpus((u16 *)skb->data);
		le16_to_cpus(((u16 *)skb->data) + 1);

		memcpy(&rx_hdr, skb->data, sizeof(struct ax88772b_rx_header));

		if ((short)rx_hdr.len != (~((short)rx_hdr.len_bar) & 0x7FF))
			return 0;

		if (rx_hdr.len > (ETH_FRAME_LEN + 4)) {
			deverr(dev, "invalid rx length %d", rx_hdr.len);
			return 0;
		}

		if (skb->len - ((rx_hdr.len +
				 sizeof(struct ax88772b_rx_header) + 3) &
				 0xfffc) == 0) {
			skb_pull(skb, sizeof(struct ax88772b_rx_header));
			skb->len = rx_hdr.len;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
			skb->tail = skb->data + rx_hdr.len;
#else
			skb_set_tail_pointer(skb, rx_hdr.len);
#endif
			skb->truesize = rx_hdr.len + sizeof(struct sk_buff);

			if (ax772b_data->checksum & AX_RX_CHECKSUM)
				ax88772b_rx_checksum(skb, &rx_hdr);

			return 2;
		}
#ifndef RX_SKB_COPY
		ax_skb = skb_clone(skb, GFP_ATOMIC);
#else
		ax_skb = alloc_skb(rx_hdr.len + NET_IP_ALIGN, GFP_ATOMIC);
		skb_reserve(ax_skb, NET_IP_ALIGN);	
#endif
		if (ax_skb) {
#ifndef RX_SKB_COPY
			ax_skb->len = rx_hdr.len;
			ax_skb->data = skb->data +
				       sizeof(struct ax88772b_rx_header);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
			ax_skb->tail = ax_skb->data + rx_hdr.len;
#else
			skb_set_tail_pointer(ax_skb, rx_hdr.len);
#endif

#else
			skb_put(ax_skb, rx_hdr.len);
			memcpy(ax_skb->data, skb->data + sizeof(struct ax88772b_rx_header), rx_hdr.len); 
#endif

			ax_skb->truesize = rx_hdr.len + sizeof(struct sk_buff);

			if (ax772b_data->checksum & AX_RX_CHECKSUM)
				ax88772b_rx_checksum(ax_skb, &rx_hdr);

			axusbnet_skb_return(dev, ax_skb);

		} else {
			return 0;
		}

		skb_pull(skb, ((rx_hdr.len +
				sizeof(struct ax88772b_rx_header) + 3)
				& 0xfffc));
	}

	if (skb->len < 0) {
		deverr(dev, "invalid rx length %d", skb->len);
		return 0;
	}
	return 1;
}

static struct sk_buff *
ax88772b_tx_fixup(struct usbnet *dev, struct sk_buff *skb, gfp_t flags)
{
	int padlen = ((skb->len + 4) % 512) ? 0 : 4;
	u32 packet_len;
	u32 padbytes = 0xffff0000;

#if (!AX_FORCE_BUFF_ALIGN)
	int headroom = skb_headroom(skb);
	int tailroom = skb_tailroom(skb);

	if ((!skb_cloned(skb))
	    && ((headroom + tailroom) >= (4 + padlen))) {
		if ((headroom < 4) || (tailroom < padlen)) {
			skb->data = memmove(skb->head + 4, skb->data, skb->len);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
			skb->tail = skb->data + skb->len;
#else
			skb_set_tail_pointer(skb, skb->len);
#endif
		}
	} else
#endif
	{
		struct sk_buff *skb2;
		skb2 = skb_copy_expand(skb, 4, padlen, flags);
		dev_kfree_skb_any(skb);
		skb = skb2;
		if (!skb)
			return NULL;
	}

	skb_push(skb, 4);
	packet_len = (((skb->len - 4) ^ 0x0000ffff) << 16) + (skb->len - 4);

	cpu_to_le32s(&packet_len);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
	memcpy(skb->data, &packet_len, sizeof(packet_len));
#else
	skb_copy_to_linear_data(skb, &packet_len, sizeof(packet_len));
#endif

	if ((skb->len % 512) == 0) {
		cpu_to_le32s(&padbytes);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 22)
		memcpy(skb->tail, &padbytes, sizeof(padbytes));
#else
		memcpy(skb_tail_pointer(skb), &padbytes, sizeof(padbytes));
#endif
		skb_put(skb, sizeof(padbytes));
	}

	return skb;
}

static const u8 chkcntsel[6][3] = {
	{12, 23, 31},
	{12, 31, 23},
	{23, 31, 12},
	{23, 12, 31},
	{31, 12, 23},
	{31, 23, 12}
};

static void ax88772_save_bmcr_anar(struct usbnet *dev)
{
	struct ax88772_data *ax772_data = (struct ax88772_data *)dev->priv;

	if (ax772_data) {
		/* Preserve BMCR for restoring */
		ax772_data->presvd_phy_bmcr =
			ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_BMCR);

		/* Preserve Advertisement control reg for restoring */
		ax772_data->presvd_phy_advertise =
			ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_ADVERTISE);
	}
}

static void ax88772_restore_bmcr_anar(struct usbnet *dev)
{
	struct ax88772_data *ax772_data = (struct ax88772_data *)dev->priv;

	if (ax772_data && ax772_data->presvd_phy_advertise && ax772_data->presvd_phy_bmcr) {
		/* Restore Advertisement control reg */
		ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_ADVERTISE,
				      ax772_data->presvd_phy_advertise);
		/* Restore BMCR */
		ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_BMCR,
				      ax772_data->presvd_phy_bmcr);
		ax772_data->presvd_phy_advertise = 0;
		ax772_data->presvd_phy_bmcr = 0;
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
static void ax88772_link_reset(void *data)
{
	struct usbnet *dev = (struct usbnet *)data;
	struct ax88772_data *ax772_data = (struct ax88772_data *)dev->priv;
#else
static void ax88772_link_reset(struct work_struct *work)
{
	struct ax88772_data *ax772_data = container_of(work,
						       struct ax88772_data,
						       check_link);
	struct usbnet *dev = ax772_data->dev;
#endif
	if (ax772_data->Event == AX_SET_RX_CFG) {
		u16 bmcr;
		u16 mode;

		ax772_data->Event = AX_NOP;

		mode = AX88772_MEDIUM_DEFAULT;

		bmcr = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id,
					    MII_BMCR);
		if (!(bmcr & BMCR_FULLDPLX))
			mode &= ~AX88772_MEDIUM_FULL_DUPLEX;
		if (!(bmcr & BMCR_SPEED100))
			mode &= ~AX88772_MEDIUM_100MB;

		ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE,
				  mode, 0, 0, NULL);
		return;
	}

	switch (ax772_data->Event) {
	case WAIT_AUTONEG_COMPLETE:
		if (jiffies > (ax772_data->autoneg_start + 5 * HZ)) {
			ax772_data->Event = PHY_POWER_DOWN;
			ax772_data->TickToExpire = 23;
		}
		break;
	case PHY_POWER_DOWN:
		if (ax772_data->TickToExpire == 23) {
			ax88772_save_bmcr_anar(dev);
			/* Set Phy Power Down */
			ax8817x_write_cmd(dev, AX_CMD_SW_RESET, AX_SWRESET_IPPD,
					  0, 0, NULL);
			--ax772_data->TickToExpire;
		} else if (--ax772_data->TickToExpire == 0) {
			/* Set Phy Power Up */
			ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
				AX_SWRESET_IPRL, 0, 0, NULL);
			ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
				AX_SWRESET_IPPD | AX_SWRESET_IPRL, 0, 0, NULL);
			msleep(10);
			ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
				AX_SWRESET_IPRL, 0, 0, NULL);
			msleep(60);
			ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
				AX_SWRESET_CLEAR, 0, 0, NULL);
			ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
				AX_SWRESET_IPRL, 0, 0, NULL);

			if (ax772_data->presvd_phy_advertise && ax772_data->presvd_phy_bmcr) {
				ax88772_restore_bmcr_anar(dev);
				
			} else {
				ax8817x_mdio_write_le(dev->net, dev->mii.phy_id,
						      MII_ADVERTISE,
						      ADVERTISE_ALL | ADVERTISE_CSMA |
						      ADVERTISE_PAUSE_CAP);
				mii_nway_restart(&dev->mii);
			}

			ax772_data->Event = PHY_POWER_UP;
			ax772_data->TickToExpire = 47;
		}
		break;
	case PHY_POWER_UP:
		if (--ax772_data->TickToExpire == 0) {
			ax772_data->Event = PHY_POWER_DOWN;
			ax772_data->TickToExpire = 23;
		}
		break;
	default:
		break;
	}
	return;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
static void ax88772a_link_reset(void *data)
{
	struct usbnet *dev = (struct usbnet *)data;
	struct ax88772a_data *ax772a_data = (struct ax88772a_data *)dev->priv;
#else
static void ax88772a_link_reset(struct work_struct *work)
{
	struct ax88772a_data *ax772a_data = container_of(work,
							 struct ax88772a_data,
							 check_link);
	struct usbnet *dev = ax772a_data->dev;
#endif
	int powsave = (ax772a_data->EepromData >> 14);
	u16 phy_reg;

	if (ax772a_data->Event == AX_SET_RX_CFG) {
		u16 bmcr;
		u16 mode;

		ax772a_data->Event = AX_NOP;

		mode = AX88772_MEDIUM_DEFAULT;

		bmcr = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id,
					    MII_BMCR);
		if (!(bmcr & BMCR_FULLDPLX))
			mode &= ~AX88772_MEDIUM_FULL_DUPLEX;
		if (!(bmcr & BMCR_SPEED100))
			mode &= ~AX88772_MEDIUM_100MB;

		ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE, mode,
				  0, 0, NULL);

		if (ax772a_data->presvd_phy_advertise && ax772a_data->presvd_phy_bmcr) {

			/* Restore Advertisement control reg */
			ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_ADVERTISE,
					      ax772a_data->presvd_phy_advertise);
			/* Restore BMCR */
			ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_BMCR,
					      ax772a_data->presvd_phy_bmcr);
			ax772a_data->presvd_phy_advertise = 0;
			ax772a_data->presvd_phy_bmcr = 0;
		}

		return;
	}

	switch (ax772a_data->Event) {
	case WAIT_AUTONEG_COMPLETE:
		if (jiffies > (ax772a_data->autoneg_start + 5 * HZ)) {
			ax772a_data->Event = CHK_CABLE_EXIST;
			ax772a_data->TickToExpire = 14;
		}
		break;
	case CHK_CABLE_EXIST:
		phy_reg = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, 0x12);
		if ((phy_reg != 0x8012) && (phy_reg != 0x8013)) {
			ax8817x_mdio_write_le(dev->net,
				dev->mii.phy_id, 0x16, 0x4040);
			mii_nway_restart(&dev->mii);
			ax772a_data->Event = CHK_CABLE_STATUS;
			ax772a_data->TickToExpire = 31;
		} else if (--ax772a_data->TickToExpire == 0) {
			mii_nway_restart(&dev->mii);
			ax772a_data->Event = CHK_CABLE_EXIST_AGAIN;
			if (powsave == 0x03) {
				ax772a_data->TickToExpire = 47;
			} else if (powsave == 0x01) {
				ax772a_data->DlyIndex = (u8)(jiffies % 6);
				ax772a_data->DlySel = 0;
				ax772a_data->TickToExpire =
			chkcntsel[ax772a_data->DlyIndex][ax772a_data->DlySel];
			}
		}
		break;
	case CHK_CABLE_EXIST_AGAIN:
		/* if cable disconnected */
		phy_reg = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, 0x12);
		if ((phy_reg != 0x8012) && (phy_reg != 0x8013)) {
			mii_nway_restart(&dev->mii);
			ax772a_data->Event = CHK_CABLE_STATUS;
			ax772a_data->TickToExpire = 31;
		} else if (--ax772a_data->TickToExpire == 0) {
			if (!ax772a_data->presvd_phy_advertise && !ax772a_data->presvd_phy_bmcr) {
				/* Preserve BMCR for restoring */
				ax772a_data->presvd_phy_bmcr =
					ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_BMCR);

				/* Preserve Advertisement control reg for restoring */
				ax772a_data->presvd_phy_advertise =
					ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_ADVERTISE);
			}


			/* Power down PHY */
			ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
					  AX_SWRESET_IPPD,
					  0, 0, NULL);
			ax772a_data->Event = PHY_POWER_DOWN;
			if (powsave == 0x03)
				ax772a_data->TickToExpire = 23;
			else if (powsave == 0x01)
				ax772a_data->TickToExpire = 31;
		}
		break;
	case PHY_POWER_DOWN:
		if (--ax772a_data->TickToExpire == 0)
			ax772a_data->Event = PHY_POWER_UP;
		break;
	case CHK_CABLE_STATUS:
		if (--ax772a_data->TickToExpire == 0) {
			ax8817x_mdio_write_le(dev->net,
					dev->mii.phy_id, 0x16, 0x4040);
			mii_nway_restart(&dev->mii);
			ax772a_data->Event = CHK_CABLE_EXIST_AGAIN;
			if (powsave == 0x03) {
				ax772a_data->TickToExpire = 47;
			} else if (powsave == 0x01) {
				ax772a_data->DlyIndex = (u8)(jiffies % 6);
				ax772a_data->DlySel = 0;
				ax772a_data->TickToExpire =
			chkcntsel[ax772a_data->DlyIndex][ax772a_data->DlySel];
			}
		}
		break;
	case PHY_POWER_UP:

		if (!ax772a_data->presvd_phy_advertise && !ax772a_data->presvd_phy_bmcr) {
			/* Preserve BMCR for restoring */
			ax772a_data->presvd_phy_bmcr =
				ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_BMCR);

			/* Preserve Advertisement control reg for restoring */
			ax772a_data->presvd_phy_advertise =
				ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_ADVERTISE);
		}

		ax88772a_phy_powerup(dev);

		ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_ADVERTISE,
			ADVERTISE_ALL | ADVERTISE_CSMA | ADVERTISE_PAUSE_CAP);

		mii_nway_restart(&dev->mii);

		ax772a_data->Event = CHK_CABLE_EXIST_AGAIN;

		if (powsave == 0x03) {
			ax772a_data->TickToExpire = 47;
		} else if (powsave == 0x01) {
			if (++ax772a_data->DlySel >= 3) {
				ax772a_data->DlyIndex = (u8)(jiffies % 6);
				ax772a_data->DlySel = 0;
			}
			ax772a_data->TickToExpire =
			chkcntsel[ax772a_data->DlyIndex][ax772a_data->DlySel];
		}
		break;
	default:
		break;
	}

	return;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 20)
static void ax88772b_link_reset(void *data)
{
	struct usbnet *dev = (struct usbnet *)data;
	struct ax88772b_data *ax772b_data = (struct ax88772b_data *)dev->priv;
#else
static void ax88772b_link_reset(struct work_struct *work)
{
	struct ax88772b_data *ax772b_data = container_of(work,
							 struct ax88772b_data,
							 check_link);
	struct usbnet *dev = ax772b_data->dev;
#endif

	switch (ax772b_data->Event) {

	case AX_SET_RX_CFG:
	{
		u16 bmcr = ax8817x_mdio_read_le(dev->net,
					dev->mii.phy_id, MII_BMCR);
		u16 mode = AX88772_MEDIUM_DEFAULT;

		if (!(bmcr & BMCR_FULLDPLX))
			mode &= ~AX88772_MEDIUM_FULL_DUPLEX;
		if (!(bmcr & BMCR_SPEED100))
			mode &= ~AX88772_MEDIUM_100MB;

		ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE, mode,
				  0, 0, NULL);
		break;
	}
	case PHY_POWER_UP:
	{
		u16 tmp16;

		if (!ax772b_data->presvd_phy_advertise && !ax772b_data->presvd_phy_bmcr) {
			/* Preserve BMCR for restoring */
			ax772b_data->presvd_phy_bmcr =
				ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_BMCR);

			/* Preserve Advertisement control reg for restoring */
			ax772b_data->presvd_phy_advertise =
				ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, MII_ADVERTISE);
		}

		ax88772a_phy_powerup(dev);
		tmp16 = ax8817x_mdio_read_le(dev->net, dev->mii.phy_id, 0x12);
		ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, 0x12,
				((tmp16 & 0xFF9F) | 0x0040));

		/* Restore Advertisement control reg */
		ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_ADVERTISE,
				      ax772b_data->presvd_phy_advertise);
		/* Restore BMCR */
		ax8817x_mdio_write_le(dev->net, dev->mii.phy_id, MII_BMCR,
				      ax772b_data->presvd_phy_bmcr);
		ax772b_data->presvd_phy_advertise = 0;
		ax772b_data->presvd_phy_bmcr = 0;

		break;
	}

	case AX_CHK_AUTODETACH:
	{
		int ret;
		ret = ax8817x_write_cmd(dev, AX_CMD_SW_RESET,
					AX_SWRESET_IPRL |
					(ax772b_data->psc & 0x7FFF),
					0, 0, NULL);
		if (ret < 0) {
			deverr(dev, "Failed to configure PHY power saving: %d",
			       ret);
		}

		break;
	}
	default:
		break;
	}

	ax772b_data->Event = AX_NOP;
	
	return;
}

static int ax88178_set_media(struct usbnet *dev)
{
	int	ret;
	struct ax88178_data *ax178dataptr = (struct ax88178_data *)dev->priv;
	int media;

	media = ax88178_media_check(dev, ax178dataptr);
	if (media < 0)
		return media;

	ret = ax8817x_write_cmd(dev, AX_CMD_WRITE_MEDIUM_MODE, media, 0,
				0, NULL);
	if (ret < 0) {
		deverr(dev, "write mode medium reg failed: %d", ret);
		return ret;
	}

	return 0;
}

static int ax88178_link_reset(struct usbnet *dev)
{
	return ax88178_set_media(dev);
}

static int ax_suspend(struct usb_interface *intf,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 10)
			pm_message_t message)
#else
			u32 message)
#endif
{
	struct usbnet *dev = usb_get_intfdata(intf);
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;

	return data->suspend(intf, message);
}

static int ax_resume(struct usb_interface *intf)
{
	struct usbnet *dev = usb_get_intfdata(intf);
	struct ax8817x_data *data = (struct ax8817x_data *)&dev->data;

	return data->resume(intf);
}

static const struct driver_info ax88178_info = {
	.description = "ASIX AX88178 USB 2.0 Ethernet",
	.bind	= ax88178_bind,
	.unbind = ax88178_unbind,
	.status = ax88178_status,
	.link_reset = ax88178_link_reset,
	.reset	= ax88178_link_reset,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX,
#endif
	.rx_fixup = ax88772_rx_fixup,
	.tx_fixup = ax88772_tx_fixup,
};

static const struct driver_info belkin178_info = {
	.description = "Belkin Gigabit USB 2.0 Network Adapter",
	.bind	= ax88178_bind,
	.unbind	= ax88178_unbind,
	.status	= ax88178_status,
	.link_reset = ax88178_link_reset,
	.reset	= ax88178_link_reset,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX,
#endif
	.rx_fixup = ax88772_rx_fixup,
	.tx_fixup = ax88772_tx_fixup,
};

static const struct driver_info ax8817x_info = {
	.description = "ASIX AX8817x USB 2.0 Ethernet",
	.bind	= ax8817x_bind,
	.status	= ax8817x_status,
	.link_reset = ax88172_link_reset,
	.reset	= ax88172_link_reset,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER,
#endif
};

static const struct driver_info dlink_dub_e100_info = {
	.description = "DLink DUB-E100 USB Ethernet",
	.bind	= ax8817x_bind,
	.status	= ax8817x_status,
	.link_reset = ax88172_link_reset,
	.reset	= ax88172_link_reset,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER,
#endif
};

static const struct driver_info netgear_fa120_info = {
	.description = "Netgear FA-120 USB Ethernet",
	.bind	= ax8817x_bind,
	.status = ax8817x_status,
	.link_reset = ax88172_link_reset,
	.reset	= ax88172_link_reset,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER,
#endif
};

static const struct driver_info hawking_uf200_info = {
	.description = "Hawking UF200 USB Ethernet",
	.bind	= ax8817x_bind,
	.status = ax8817x_status,
	.link_reset = ax88172_link_reset,
	.reset	= ax88172_link_reset,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER,
#endif
};

static const struct driver_info ax88772_info = {
	.description = "ASIX AX88772 USB 2.0 Ethernet",
	.bind	= ax88772_bind,
	.unbind = ax88772_unbind,
	.status = ax88772_status,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX,
#endif
	.rx_fixup = ax88772_rx_fixup,
	.tx_fixup = ax88772_tx_fixup,
	.reset	= ax88772b_reset,
};

static const struct driver_info dlink_dub_e100b_info = {
	.description = "D-Link DUB-E100 USB 2.0 Fast Ethernet Adapter",
	.bind	= ax88772_bind,
	.unbind = ax88772_unbind,
	.status = ax88772_status,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX,
#endif
	.rx_fixup = ax88772_rx_fixup,
	.tx_fixup = ax88772_tx_fixup,
	.reset	= ax88772b_reset,
};

static const struct driver_info dlink_dub_e100_772b_info = {
	.description = "D-Link DUB-E100 USB 2.0 Fast Ethernet Adapter",
	.bind	= ax88772b_bind,
	.unbind = ax88772b_unbind,
	.status = ax88772b_status,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_HW_IP_ALIGNMENT | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_HW_IP_ALIGNMENT,
#endif
	.rx_fixup = ax88772b_rx_fixup,
	.tx_fixup = ax88772b_tx_fixup,
	.reset	= ax88772b_reset,
};

static const struct driver_info dlink_dub_e100_772c_info = {
	.description = "D-Link DUB-E100 USB 2.0 Fast Ethernet Adapter",
	.bind	= ax88772b_bind,
	.unbind = ax88772b_unbind,
	.status = ax88772b_status,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_HW_IP_ALIGNMENT | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_HW_IP_ALIGNMENT,
#endif
	.rx_fixup = ax88772b_rx_fixup,
	.tx_fixup = ax88772b_tx_fixup,
	.reset	= ax88772b_reset,
};

static const struct driver_info ax88772a_info = {
	.description = "ASIX AX88772A USB 2.0 Ethernet",
	.bind	= ax88772a_bind,
	.unbind = ax88772a_unbind,
	.status = ax88772a_status,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX,
#endif
	.rx_fixup = ax88772_rx_fixup,
	.tx_fixup = ax88772_tx_fixup,
	.reset	= ax88772b_reset,
};

static const struct driver_info ax88772b_info = {
	.description = "ASIX AX88772B USB 2.0 Ethernet",
	.bind	= ax88772b_bind,
	.unbind = ax88772b_unbind,
	.status = ax88772b_status,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_HW_IP_ALIGNMENT | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_HW_IP_ALIGNMENT,
#endif
	.rx_fixup = ax88772b_rx_fixup,
	.tx_fixup = ax88772b_tx_fixup,
	.reset	= ax88772b_reset,
};

static const struct driver_info ax88772c_info = {
	.description = "ASIX AX88772C USB 2.0 Ethernet",
	.bind	= ax88772b_bind,
	.unbind = ax88772b_unbind,
	.status = ax88772c_status,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop	= ax88772b_stop,
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_HW_IP_ALIGNMENT | FLAG_AVOID_UNLINK_URBS,
#else
	.flags	= FLAG_ETHER | FLAG_FRAMING_AX | FLAG_HW_IP_ALIGNMENT,
#endif
	.rx_fixup = ax88772b_rx_fixup,
	.tx_fixup = ax88772b_tx_fixup,
	.reset	= ax88772b_reset,
};

static const struct usb_device_id products[] = {
{
	/* 88178 */
	USB_DEVICE(0x0b95, 0x1780),
	.driver_info =	(unsigned long) &ax88178_info,
}, {
	/* 88178 for billianton linksys */
	USB_DEVICE(0x077b, 0x2226),
	.driver_info =	(unsigned long) &ax88178_info,
}, {
	/* ABOCOM for linksys */
	USB_DEVICE(0x1737, 0x0039),
	.driver_info =	(unsigned long) &ax88178_info,
}, {
	/* ABOCOM  for pci */
	USB_DEVICE(0x14ea, 0xab11),
	.driver_info =	(unsigned long) &ax88178_info,
}, {
        /* Buffalo LUA3-U2-AGT */
        USB_DEVICE(0x0411, 0x006e),
        .driver_info =  (unsigned long) &ax88178_info,
}, {
	/* Belkin */
	USB_DEVICE(0x050d, 0x5055),
	.driver_info =	(unsigned long) &belkin178_info,
}, {
	/* Linksys USB200M */
	USB_DEVICE(0x077b, 0x2226),
	.driver_info =	(unsigned long) &ax8817x_info,
}, {
	/* Netgear FA120 */
	USB_DEVICE(0x0846, 0x1040),
	.driver_info =  (unsigned long) &netgear_fa120_info,
}, {
	/* DLink DUB-E100 */
	USB_DEVICE(0x2001, 0x1a00),
	.driver_info =  (unsigned long) &dlink_dub_e100_info,
}, {
	/* DLink DUB-E100B */
	USB_DEVICE(0x2001, 0x3c05),
	.driver_info =  (unsigned long) &dlink_dub_e100b_info,
}, {
	/* DLink DUB-E100B */
	USB_DEVICE(0x07d1, 0x3c05),
	.driver_info =  (unsigned long) &dlink_dub_e100b_info,
}, {
	/* DLink DUB-E100 (AX88772B)*/
	USB_DEVICE_VER(0x2001, 0x1a02, 0, 1),
	.driver_info =  (unsigned long) &dlink_dub_e100_772b_info,
}, {
	/* DLink DUB-E100 (AX88772C)*/
	USB_DEVICE_VER(0x2001, 0x1a02, 0, 2),
	.driver_info =  (unsigned long) &dlink_dub_e100_772c_info,
}, {
	/* Intellinet, ST Lab USB Ethernet */
	USB_DEVICE(0x0b95, 0x1720),
	.driver_info =  (unsigned long) &ax8817x_info,
}, {
	/* Hawking UF200, TrendNet TU2-ET100 */
	USB_DEVICE(0x07b8, 0x420a),
	.driver_info =  (unsigned long) &hawking_uf200_info,
}, {
	/* Billionton Systems, USB2AR */
	USB_DEVICE(0x08dd, 0x90ff),
	.driver_info =  (unsigned long) &ax8817x_info,
}, {
	/* ATEN UC210T */
	USB_DEVICE(0x0557, 0x2009),
	.driver_info =  (unsigned long) &ax8817x_info,
}, {
	/* Buffalo LUA-U2-KTX */
	USB_DEVICE(0x0411, 0x003d),
	.driver_info =  (unsigned long) &ax8817x_info,
}, {
	/* Sitecom LN-029 "USB 2.0 10/100 Ethernet adapter" */
	USB_DEVICE(0x6189, 0x182d),
	.driver_info =  (unsigned long) &ax8817x_info,
}, {
	/* corega FEther USB2-TX */
	USB_DEVICE(0x07aa, 0x0017),
	.driver_info =  (unsigned long) &ax8817x_info,
}, {
	/* Surecom EP-1427X-2 */
	USB_DEVICE(0x1189, 0x0893),
	.driver_info = (unsigned long) &ax8817x_info,
}, {
	/* goodway corp usb gwusb2e */
	USB_DEVICE(0x1631, 0x6200),
	.driver_info = (unsigned long) &ax8817x_info,
}, {
	/* ASIX AX88772 10/100 */
	USB_DEVICE(0x0b95, 0x7720),
	.driver_info = (unsigned long) &ax88772_info,
}, {
	/* ASIX AX88772 10/100 */
	USB_DEVICE(0x125E, 0x180D),
	.driver_info = (unsigned long) &ax88772_info,
}, {
	/* ASIX AX88772A 10/100 */
	USB_DEVICE(0x0b95, 0x772A),
	.driver_info = (unsigned long) &ax88772a_info,
}, {
	/* ASIX AX88772A 10/100 */
	USB_DEVICE(0x0db0, 0xA877),
	.driver_info = (unsigned long) &ax88772a_info,
}, {
	/* ASIX AX88772A 10/100 */
	USB_DEVICE(0x0421, 0x772A),
	.driver_info = (unsigned long) &ax88772a_info,
}, {
	/* Linksys 200M */
	 USB_DEVICE(0x13B1, 0x0018),
	.driver_info = (unsigned long) &ax88772a_info,
}, {
	USB_DEVICE(0x05ac, 0x1402),
	.driver_info = (unsigned long) &ax88772a_info,
}, {
	/* ASIX AX88772B 10/100 */
	USB_DEVICE_VER(0x0b95, 0x772B, 0, 1),
	.driver_info = (unsigned long) &ax88772b_info,
}, {
	/* Asus AX88772B 10/100 */
	USB_DEVICE_VER(0x0b95, 0x7e2b, 0, 1),
	.driver_info = (unsigned long) &ax88772b_info,
}, {
	/* Lenovo AX88772B 10/100 */
	USB_DEVICE_VER(0x17ef, 0x7203, 0, 1),
	.driver_info = (unsigned long) &ax88772b_info,
},{
	/* ASIX AX88772B ver.2 10/100 */
	USB_DEVICE_VER(0x0b95, 0x772B, 0, 2),
	.driver_info = (unsigned long) &ax88772c_info,
},
	{ },		/* END */
};
MODULE_DEVICE_TABLE(usb, products);

static struct usb_driver asix_driver = {
	/* .owner =	THIS_MODULE, */
	.name =		"asix",
	.id_table =	products,
	.probe =	axusbnet_probe,
	.suspend =	ax_suspend,
	.resume =	ax_resume,
	.disconnect =	axusbnet_disconnect,
};

static int __init asix_init(void)
{
	return usb_register(&asix_driver);
}
module_init(asix_init);

static void __exit asix_exit(void)
{
	usb_deregister(&asix_driver);
}
module_exit(asix_exit);

MODULE_AUTHOR("David Hollis");
MODULE_DESCRIPTION("ASIX AX8817X based USB 2.0 Ethernet Devices");
MODULE_LICENSE("GPL");

