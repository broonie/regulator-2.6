/*
 * M66592 UDC (USB gadget)
 *
 * Copyright (C) 2006-2007 Renesas Solutions Corp.
 *
 * Author : Yoshihiro Shimoda <shimoda.yoshihiro@renesas.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/platform_device.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>

#include "m66592-udc.h"


MODULE_DESCRIPTION("M66592 USB gadget driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yoshihiro Shimoda");
MODULE_ALIAS("platform:m66592_udc");

#define DRIVER_VERSION	"18 Oct 2007"

/* module parameters */
#if defined(CONFIG_SUPERH_BUILT_IN_M66592)
static unsigned short endian = M66592_LITTLE;
module_param(endian, ushort, 0644);
MODULE_PARM_DESC(endian, "data endian: big=0, little=0 (default=0)");
#else
static unsigned short clock = M66592_XTAL24;
module_param(clock, ushort, 0644);
MODULE_PARM_DESC(clock, "input clock: 48MHz=32768, 24MHz=16384, 12MHz=0 "
		"(default=16384)");

static unsigned short vif = M66592_LDRV;
module_param(vif, ushort, 0644);
MODULE_PARM_DESC(vif, "input VIF: 3.3V=32768, 1.5V=0 (default=32768)");

static unsigned short endian;
module_param(endian, ushort, 0644);
MODULE_PARM_DESC(endian, "data endian: big=256, little=0 (default=0)");

static unsigned short irq_sense = M66592_INTL;
module_param(irq_sense, ushort, 0644);
MODULE_PARM_DESC(irq_sense, "IRQ sense: low level=2, falling edge=0 "
		"(default=2)");
#endif

static const char udc_name[] = "m66592_udc";
static const char *m66592_ep_name[] = {
	"ep0", "ep1", "ep2", "ep3", "ep4", "ep5", "ep6", "ep7"
};

static void disable_controller(struct m66592 *m66592);
static void irq_ep0_write(struct m66592_ep *ep, struct m66592_request *req);
static void irq_packet_write(struct m66592_ep *ep, struct m66592_request *req);
static int m66592_queue(struct usb_ep *_ep, struct usb_request *_req,
			gfp_t gfp_flags);

static void transfer_complete(struct m66592_ep *ep,
		struct m66592_request *req, int status);

/*-------------------------------------------------------------------------*/
static inline u16 get_usb_speed(struct m66592 *m66592)
{
	return (m66592_read(m66592, M66592_DVSTCTR) & M66592_RHST);
}

static void enable_pipe_irq(struct m66592 *m66592, u16 pipenum,
		unsigned long reg)
{
	u16 tmp;

	tmp = m66592_read(m66592, M66592_INTENB0);
	m66592_bclr(m66592, M66592_BEMPE | M66592_NRDYE | M66592_BRDYE,
			M66592_INTENB0);
	m66592_bset(m66592, (1 << pipenum), reg);
	m66592_write(m66592, tmp, M66592_INTENB0);
}

static void disable_pipe_irq(struct m66592 *m66592, u16 pipenum,
		unsigned long reg)
{
	u16 tmp;

	tmp = m66592_read(m66592, M66592_INTENB0);
	m66592_bclr(m66592, M66592_BEMPE | M66592_NRDYE | M66592_BRDYE,
			M66592_INTENB0);
	m66592_bclr(m66592, (1 << pipenum), reg);
	m66592_write(m66592, tmp, M66592_INTENB0);
}

static void m66592_usb_connect(struct m66592 *m66592)
{
	m66592_bset(m66592, M66592_CTRE, M66592_INTENB0);
	m66592_bset(m66592, M66592_WDST | M66592_RDST | M66592_CMPL,
			M66592_INTENB0);
	m66592_bset(m66592, M66592_BEMPE | M66592_BRDYE, M66592_INTENB0);

	m66592_bset(m66592, M66592_DPRPU, M66592_SYSCFG);
}

static void m66592_usb_disconnect(struct m66592 *m66592)
__releases(m66592->lock)
__acquires(m66592->lock)
{
	m66592_bclr(m66592, M66592_CTRE, M66592_INTENB0);
	m66592_bclr(m66592, M66592_WDST | M66592_RDST | M66592_CMPL,
			M66592_INTENB0);
	m66592_bclr(m66592, M66592_BEMPE | M66592_BRDYE, M66592_INTENB0);
	m66592_bclr(m66592, M66592_DPRPU, M66592_SYSCFG);

	m66592->gadget.speed = USB_SPEED_UNKNOWN;
	spin_unlock(&m66592->lock);
	m66592->driver->disconnect(&m66592->gadget);
	spin_lock(&m66592->lock);

	disable_controller(m66592);
	INIT_LIST_HEAD(&m66592->ep[0].queue);
}

static inline u16 control_reg_get_pid(struct m66592 *m66592, u16 pipenum)
{
	u16 pid = 0;
	unsigned long offset;

	if (pipenum == 0)
		pid = m66592_read(m66592, M66592_DCPCTR) & M66592_PID;
	else if (pipenum < M66592_MAX_NUM_PIPE) {
		offset = get_pipectr_addr(pipenum);
		pid = m66592_read(m66592, offset) & M66592_PID;
	} else
		pr_err("unexpect pipe num (%d)\n", pipenum);

	return pid;
}

static inline void control_reg_set_pid(struct m66592 *m66592, u16 pipenum,
		u16 pid)
{
	unsigned long offset;

	if (pipenum == 0)
		m66592_mdfy(m66592, pid, M66592_PID, M66592_DCPCTR);
	else if (pipenum < M66592_MAX_NUM_PIPE) {
		offset = get_pipectr_addr(pipenum);
		m66592_mdfy(m66592, pid, M66592_PID, offset);
	} else
		pr_err("unexpect pipe num (%d)\n", pipenum);
}

static inline void pipe_start(struct m66592 *m66592, u16 pipenum)
{
	control_reg_set_pid(m66592, pipenum, M66592_PID_BUF);
}

static inline void pipe_stop(struct m66592 *m66592, u16 pipenum)
{
	control_reg_set_pid(m66592, pipenum, M66592_PID_NAK);
}

static inline void pipe_stall(struct m66592 *m66592, u16 pipenum)
{
	control_reg_set_pid(m66592, pipenum, M66592_PID_STALL);
}

static inline u16 control_reg_get(struct m66592 *m66592, u16 pipenum)
{
	u16 ret = 0;
	unsigned long offset;

	if (pipenum == 0)
		ret = m66592_read(m66592, M66592_DCPCTR);
	else if (pipenum < M66592_MAX_NUM_PIPE) {
		offset = get_pipectr_addr(pipenum);
		ret = m66592_read(m66592, offset);
	} else
		pr_err("unexpect pipe num (%d)\n", pipenum);

	return ret;
}

static inline void control_reg_sqclr(struct m66592 *m66592, u16 pipenum)
{
	unsigned long offset;

	pipe_stop(m66592, pipenum);

	if (pipenum == 0)
		m66592_bset(m66592, M66592_SQCLR, M66592_DCPCTR);
	else if (pipenum < M66592_MAX_NUM_PIPE) {
		offset = get_pipectr_addr(pipenum);
		m66592_bset(m66592, M66592_SQCLR, offset);
	} else
		pr_err("unexpect pipe num(%d)\n", pipenum);
}

static inline int get_buffer_size(struct m66592 *m66592, u16 pipenum)
{
	u16 tmp;
	int size;

	if (pipenum == 0) {
		tmp = m66592_read(m66592, M66592_DCPCFG);
		if ((tmp & M66592_CNTMD) != 0)
			size = 256;
		else {
			tmp = m66592_read(m66592, M66592_DCPMAXP);
			size = tmp & M66592_MAXP;
		}
	} else {
		m66592_write(m66592, pipenum, M66592_PIPESEL);
		tmp = m66592_read(m66592, M66592_PIPECFG);
		if ((tmp & M66592_CNTMD) != 0) {
			tmp = m66592_read(m66592, M66592_PIPEBUF);
			size = ((tmp >> 10) + 1) * 64;
		} else {
			tmp = m66592_read(m66592, M66592_PIPEMAXP);
			size = tmp & M66592_MXPS;
		}
	}

	return size;
}

static inline void pipe_change(struct m66592 *m66592, u16 pipenum)
{
	struct m66592_ep *ep = m66592->pipenum2ep[pipenum];

	if (ep->use_dma)
		return;

	m66592_mdfy(m66592, pipenum, M66592_CURPIPE, ep->fifosel);

	ndelay(450);

	m66592_bset(m66592, M66592_MBW, ep->fifosel);
}

static int pipe_buffer_setting(struct m66592 *m66592,
		struct m66592_pipe_info *info)
{
	u16 bufnum = 0, buf_bsize = 0;
	u16 pipecfg = 0;

	if (info->pipe == 0)
		return -EINVAL;

	m66592_write(m66592, info->pipe, M66592_PIPESEL);

	if (info->dir_in)
		pipecfg |= M66592_DIR;
	pipecfg |= info->type;
	pipecfg |= info->epnum;
	switch (info->type) {
	case M66592_INT:
		bufnum = 4 + (info->pipe - M66592_BASE_PIPENUM_INT);
		buf_bsize = 0;
		break;
	case M66592_BULK:
		bufnum = m66592->bi_bufnum +
			 (info->pipe - M66592_BASE_PIPENUM_BULK) * 16;
		m66592->bi_bufnum += 16;
		buf_bsize = 7;
		pipecfg |= M66592_DBLB;
		if (!info->dir_in)
			pipecfg |= M66592_SHTNAK;
		break;
	case M66592_ISO:
		bufnum = m66592->bi_bufnum +
			 (info->pipe - M66592_BASE_PIPENUM_ISOC) * 16;
		m66592->bi_bufnum += 16;
		buf_bsize = 7;
		break;
	}
	if (m66592->bi_bufnum > M66592_MAX_BUFNUM) {
		pr_err("m66592 pipe memory is insufficient(%d)\n",
				m66592->bi_bufnum);
		return -ENOMEM;
	}

	m66592_write(m66592, pipecfg, M66592_PIPECFG);
	m66592_write(m66592, (buf_bsize << 10) | (bufnum), M66592_PIPEBUF);
	m66592_write(m66592, info->maxpacket, M66592_PIPEMAXP);
	if (info->interval)
		info->interval--;
	m66592_write(m66592, info->interval, M66592_PIPEPERI);

	return 0;
}

static void pipe_buffer_release(struct m66592 *m66592,
				struct m66592_pipe_info *info)
{
	if (info->pipe == 0)
		return;

	switch (info->type) {
	case M66592_BULK:
		if (is_bulk_pipe(info->pipe))
			m66592->bi_bufnum -= 16;
		break;
	case M66592_ISO:
		if (is_isoc_pipe(info->pipe))
			m66592->bi_bufnum -= 16;
		break;
	}

	if (is_bulk_pipe(info->pipe)) {
		m66592->bulk--;
	} else if (is_interrupt_pipe(info->pipe))
		m66592->interrupt--;
	else if (is_isoc_pipe(info->pipe)) {
		m66592->isochronous--;
		if (info->type == M66592_BULK)
			m66592->bulk--;
	} else
		pr_err("ep_release: unexpect pipenum (%d)\n",
				info->pipe);
}

static void pipe_initialize(struct m66592_ep *ep)
{
	struct m66592 *m66592 = ep->m66592;

	m66592_mdfy(m66592, 0, M66592_CURPIPE, ep->fifosel);

	m66592_write(m66592, M66592_ACLRM, ep->pipectr);
	m66592_write(m66592, 0, ep->pipectr);
	m66592_write(m66592, M66592_SQCLR, ep->pipectr);
	if (ep->use_dma) {
		m66592_mdfy(m66592, ep->pipenum, M66592_CURPIPE, ep->fifosel);

		ndelay(450);

		m66592_bset(m66592, M66592_MBW, ep->fifosel);
	}
}

static void m66592_ep_setting(struct m66592 *m66592, struct m66592_ep *ep,
		const struct usb_endpoint_descriptor *desc,
		u16 pipenum, int dma)
{
	if ((pipenum != 0) && dma) {
		if (m66592->num_dma == 0) {
			m66592->num_dma++;
			ep->use_dma = 1;
			ep->fifoaddr = M66592_D0FIFO;
			ep->fifosel = M66592_D0FIFOSEL;
			ep->fifoctr = M66592_D0FIFOCTR;
			ep->fifotrn = M66592_D0FIFOTRN;
#if !defined(CONFIG_SUPERH_BUILT_IN_M66592)
		} else if (m66592->num_dma == 1) {
			m66592->num_dma++;
			ep->use_dma = 1;
			ep->fifoaddr = M66592_D1FIFO;
			ep->fifosel = M66592_D1FIFOSEL;
			ep->fifoctr = M66592_D1FIFOCTR;
			ep->fifotrn = M66592_D1FIFOTRN;
#endif
		} else {
			ep->use_dma = 0;
			ep->fifoaddr = M66592_CFIFO;
			ep->fifosel = M66592_CFIFOSEL;
			ep->fifoctr = M66592_CFIFOCTR;
			ep->fifotrn = 0;
		}
	} else {
		ep->use_dma = 0;
		ep->fifoaddr = M66592_CFIFO;
		ep->fifosel = M66592_CFIFOSEL;
		ep->fifoctr = M66592_CFIFOCTR;
		ep->fifotrn = 0;
	}

	ep->pipectr = get_pipectr_addr(pipenum);
	ep->pipenum = pipenum;
	ep->ep.maxpacket = le16_to_cpu(desc->wMaxPacketSize);
	m66592->pipenum2ep[pipenum] = ep;
	m66592->epaddr2ep[desc->bEndpointAddress&USB_ENDPOINT_NUMBER_MASK] = ep;
	INIT_LIST_HEAD(&ep->queue);
}

static void m66592_ep_release(struct m66592_ep *ep)
{
	struct m66592 *m66592 = ep->m66592;
	u16 pipenum = ep->pipenum;

	if (pipenum == 0)
		return;

	if (ep->use_dma)
		m66592->num_dma--;
	ep->pipenum = 0;
	ep->busy = 0;
	ep->use_dma = 0;
}

static int alloc_pipe_config(struct m66592_ep *ep,
		const struct usb_endpoint_descriptor *desc)
{
	struct m66592 *m66592 = ep->m66592;
	struct m66592_pipe_info info;
	int dma = 0;
	int *counter;
	int ret;

	ep->desc = desc;

	BUG_ON(ep->pipenum);

	switch (desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_BULK:
		if (m66592->bulk >= M66592_MAX_NUM_BULK) {
			if (m66592->isochronous >= M66592_MAX_NUM_ISOC) {
				pr_err("bulk pipe is insufficient\n");
				return -ENODEV;
			} else {
				info.pipe = M66592_BASE_PIPENUM_ISOC
						+ m66592->isochronous;
				counter = &m66592->isochronous;
			}
		} else {
			info.pipe = M66592_BASE_PIPENUM_BULK + m66592->bulk;
			counter = &m66592->bulk;
		}
		info.type = M66592_BULK;
		dma = 1;
		break;
	case USB_ENDPOINT_XFER_INT:
		if (m66592->interrupt >= M66592_MAX_NUM_INT) {
			pr_err("interrupt pipe is insufficient\n");
			return -ENODEV;
		}
		info.pipe = M66592_BASE_PIPENUM_INT + m66592->interrupt;
		info.type = M66592_INT;
		counter = &m66592->interrupt;
		break;
	case USB_ENDPOINT_XFER_ISOC:
		if (m66592->isochronous >= M66592_MAX_NUM_ISOC) {
			pr_err("isochronous pipe is insufficient\n");
			return -ENODEV;
		}
		info.pipe = M66592_BASE_PIPENUM_ISOC + m66592->isochronous;
		info.type = M66592_ISO;
		counter = &m66592->isochronous;
		break;
	default:
		pr_err("unexpect xfer type\n");
		return -EINVAL;
	}
	ep->type = info.type;

	info.epnum = desc->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
	info.maxpacket = le16_to_cpu(desc->wMaxPacketSize);
	info.interval = desc->bInterval;
	if (desc->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
		info.dir_in = 1;
	else
		info.dir_in = 0;

	ret = pipe_buffer_setting(m66592, &info);
	if (ret < 0) {
		pr_err("pipe_buffer_setting fail\n");
		return ret;
	}

	(*counter)++;
	if ((counter == &m66592->isochronous) && info.type == M66592_BULK)
		m66592->bulk++;

	m66592_ep_setting(m66592, ep, desc, info.pipe, dma);
	pipe_initialize(ep);

	return 0;
}

static int free_pipe_config(struct m66592_ep *ep)
{
	struct m66592 *m66592 = ep->m66592;
	struct m66592_pipe_info info;

	info.pipe = ep->pipenum;
	info.type = ep->type;
	pipe_buffer_release(m66592, &info);
	m66592_ep_release(ep);

	return 0;
}

/*-------------------------------------------------------------------------*/
static void pipe_irq_enable(struct m66592 *m66592, u16 pipenum)
{
	enable_irq_ready(m66592, pipenum);
	enable_irq_nrdy(m66592, pipenum);
}

static void pipe_irq_disable(struct m66592 *m66592, u16 pipenum)
{
	disable_irq_ready(m66592, pipenum);
	disable_irq_nrdy(m66592, pipenum);
}

/* if complete is true, gadget driver complete function is not call */
static void control_end(struct m66592 *m66592, unsigned ccpl)
{
	m66592->ep[0].internal_ccpl = ccpl;
	pipe_start(m66592, 0);
	m66592_bset(m66592, M66592_CCPL, M66592_DCPCTR);
}

static void start_ep0_write(struct m66592_ep *ep, struct m66592_request *req)
{
	struct m66592 *m66592 = ep->m66592;

	pipe_change(m66592, ep->pipenum);
	m66592_mdfy(m66592, M66592_ISEL | M66592_PIPE0,
			(M66592_ISEL | M66592_CURPIPE),
			M66592_CFIFOSEL);
	m66592_write(m66592, M66592_BCLR, ep->fifoctr);
	if (req->req.length == 0) {
		m66592_bset(m66592, M66592_BVAL, ep->fifoctr);
		pipe_start(m66592, 0);
		transfer_complete(ep, req, 0);
	} else {
		m66592_write(m66592, ~M66592_BEMP0, M66592_BEMPSTS);
		irq_ep0_write(ep, req);
	}
}

static void start_packet_write(struct m66592_ep *ep, struct m66592_request *req)
{
	struct m66592 *m66592 = ep->m66592;
	u16 tmp;

	pipe_change(m66592, ep->pipenum);
	disable_irq_empty(m66592, ep->pipenum);
	pipe_start(m66592, ep->pipenum);

	tmp = m66592_read(m66592, ep->fifoctr);
	if (unlikely((tmp & M66592_FRDY) == 0))
		pipe_irq_enable(m66592, ep->pipenum);
	else
		irq_packet_write(ep, req);
}

static void start_packet_read(struct m66592_ep *ep, struct m66592_request *req)
{
	struct m66592 *m66592 = ep->m66592;
	u16 pipenum = ep->pipenum;

	if (ep->pipenum == 0) {
		m66592_mdfy(m66592, M66592_PIPE0,
				(M66592_ISEL | M66592_CURPIPE),
				M66592_CFIFOSEL);
		m66592_write(m66592, M66592_BCLR, ep->fifoctr);
		pipe_start(m66592, pipenum);
		pipe_irq_enable(m66592, pipenum);
	} else {
		if (ep->use_dma) {
			m66592_bset(m66592, M66592_TRCLR, ep->fifosel);
			pipe_change(m66592, pipenum);
			m66592_bset(m66592, M66592_TRENB, ep->fifosel);
			m66592_write(m66592,
				(req->req.length + ep->ep.maxpacket - 1)
					/ ep->ep.maxpacket,
				ep->fifotrn);
		}
		pipe_start(m66592, pipenum);	/* trigger once */
		pipe_irq_enable(m66592, pipenum);
	}
}

static void start_packet(struct m66592_ep *ep, struct m66592_request *req)
{
	if (ep->desc->bEndpointAddress & USB_DIR_IN)
		start_packet_write(ep, req);
	else
		start_packet_read(ep, req);
}

static void start_ep0(struct m66592_ep *ep, struct m66592_request *req)
{
	u16 ctsq;

	ctsq = m66592_read(ep->m66592, M66592_INTSTS0) & M66592_CTSQ;

	switch (ctsq) {
	case M66592_CS_RDDS:
		start_ep0_write(ep, req);
		break;
	case M66592_CS_WRDS:
		start_packet_read(ep, req);
		break;

	case M66592_CS_WRND:
		control_end(ep->m66592, 0);
		break;
	default:
		pr_err("start_ep0: unexpect ctsq(%x)\n", ctsq);
		break;
	}
}

#if defined(CONFIG_SUPERH_BUILT_IN_M66592)
static void init_controller(struct m66592 *m66592)
{
	m66592_bset(m66592, M66592_HSE, M66592_SYSCFG);		/* High spd */
	m66592_bclr(m66592, M66592_USBE, M66592_SYSCFG);
	m66592_bclr(m66592, M66592_DPRPU, M66592_SYSCFG);
	m66592_bset(m66592, M66592_USBE, M66592_SYSCFG);

	/* This is a workaound for SH7722 2nd cut */
	m66592_bset(m66592, 0x8000, M66592_DVSTCTR);
	m66592_bset(m66592, 0x1000, M66592_TESTMODE);
	m66592_bclr(m66592, 0x8000, M66592_DVSTCTR);

	m66592_bset(m66592, M66592_INTL, M66592_INTENB1);

	m66592_write(m66592, 0, M66592_CFBCFG);
	m66592_write(m66592, 0, M66592_D0FBCFG);
	m66592_bset(m66592, endian, M66592_CFBCFG);
	m66592_bset(m66592, endian, M66592_D0FBCFG);
}
#else	/* #if defined(CONFIG_SUPERH_BUILT_IN_M66592) */
static void init_controller(struct m66592 *m66592)
{
	m66592_bset(m66592, (vif & M66592_LDRV) | (endian & M66592_BIGEND),
			M66592_PINCFG);
	m66592_bset(m66592, M66592_HSE, M66592_SYSCFG);		/* High spd */
	m66592_mdfy(m66592, clock & M66592_XTAL, M66592_XTAL, M66592_SYSCFG);

	m66592_bclr(m66592, M66592_USBE, M66592_SYSCFG);
	m66592_bclr(m66592, M66592_DPRPU, M66592_SYSCFG);
	m66592_bset(m66592, M66592_USBE, M66592_SYSCFG);

	m66592_bset(m66592, M66592_XCKE, M66592_SYSCFG);

	msleep(3);

	m66592_bset(m66592, M66592_RCKE | M66592_PLLC, M66592_SYSCFG);

	msleep(1);

	m66592_bset(m66592, M66592_SCKE, M66592_SYSCFG);

	m66592_bset(m66592, irq_sense & M66592_INTL, M66592_INTENB1);
	m66592_write(m66592, M66592_BURST | M66592_CPU_ADR_RD_WR,
			M66592_DMA0CFG);
}
#endif	/* #if defined(CONFIG_SUPERH_BUILT_IN_M66592) */

static void disable_controller(struct m66592 *m66592)
{
#if !defined(CONFIG_SUPERH_BUILT_IN_M66592)
	m66592_bclr(m66592, M66592_SCKE, M66592_SYSCFG);
	udelay(1);
	m66592_bclr(m66592, M66592_PLLC, M66592_SYSCFG);
	udelay(1);
	m66592_bclr(m66592, M66592_RCKE, M66592_SYSCFG);
	udelay(1);
	m66592_bclr(m66592, M66592_XCKE, M66592_SYSCFG);
#endif
}

static void m66592_start_xclock(struct m66592 *m66592)
{
#if !defined(CONFIG_SUPERH_BUILT_IN_M66592)
	u16 tmp;

	tmp = m66592_read(m66592, M66592_SYSCFG);
	if (!(tmp & M66592_XCKE))
		m66592_bset(m66592, M66592_XCKE, M66592_SYSCFG);
#endif
}

/*-------------------------------------------------------------------------*/
static void transfer_complete(struct m66592_ep *ep,
		struct m66592_request *req, int status)
__releases(m66592->lock)
__acquires(m66592->lock)
{
	int restart = 0;

	if (unlikely(ep->pipenum == 0)) {
		if (ep->internal_ccpl) {
			ep->internal_ccpl = 0;
			return;
		}
	}

	list_del_init(&req->queue);
	if (ep->m66592->gadget.speed == USB_SPEED_UNKNOWN)
		req->req.status = -ESHUTDOWN;
	else
		req->req.status = status;

	if (!list_empty(&ep->queue))
		restart = 1;

	spin_unlock(&ep->m66592->lock);
	req->req.complete(&ep->ep, &req->req);
	spin_lock(&ep->m66592->lock);

	if (restart) {
		req = list_entry(ep->queue.next, struct m66592_request, queue);
		if (ep->desc)
			start_packet(ep, req);
	}
}

static void irq_ep0_write(struct m66592_ep *ep, struct m66592_request *req)
{
	int i;
	u16 tmp;
	unsigned bufsize;
	size_t size;
	void *buf;
	u16 pipenum = ep->pipenum;
	struct m66592 *m66592 = ep->m66592;

	pipe_change(m66592, pipenum);
	m66592_bset(m66592, M66592_ISEL, ep->fifosel);

	i = 0;
	do {
		tmp = m66592_read(m66592, ep->fifoctr);
		if (i++ > 100000) {
			pr_err("pipe0 is busy. maybe cpu i/o bus "
				"conflict. please power off this controller.");
			return;
		}
		ndelay(1);
	} while ((tmp & M66592_FRDY) == 0);

	/* prepare parameters */
	bufsize = get_buffer_size(m66592, pipenum);
	buf = req->req.buf + req->req.actual;
	size = min(bufsize, req->req.length - req->req.actual);

	/* write fifo */
	if (req->req.buf) {
		if (size > 0)
			m66592_write_fifo(m66592, ep->fifoaddr, buf, size);
		if ((size == 0) || ((size % ep->ep.maxpacket) != 0))
			m66592_bset(m66592, M66592_BVAL, ep->fifoctr);
	}

	/* update parameters */
	req->req.actual += size;

	/* check transfer finish */
	if ((!req->req.zero && (req->req.actual == req->req.length))
			|| (size % ep->ep.maxpacket)
			|| (size == 0)) {
		disable_irq_ready(m66592, pipenum);
		disable_irq_empty(m66592, pipenum);
	} else {
		disable_irq_ready(m66592, pipenum);
		enable_irq_empty(m66592, pipenum);
	}
	pipe_start(m66592, pipenum);
}

static void irq_packet_write(struct m66592_ep *ep, struct m66592_request *req)
{
	u16 tmp;
	unsigned bufsize;
	size_t size;
	void *buf;
	u16 pipenum = ep->pipenum;
	struct m66592 *m66592 = ep->m66592;

	pipe_change(m66592, pipenum);
	tmp = m66592_read(m66592, ep->fifoctr);
	if (unlikely((tmp & M66592_FRDY) == 0)) {
		pipe_stop(m66592, pipenum);
		pipe_irq_disable(m66592, pipenum);
		pr_err("write fifo not ready. pipnum=%d\n", pipenum);
		return;
	}

	/* prepare parameters */
	bufsize = get_buffer_size(m66592, pipenum);
	buf = req->req.buf + req->req.actual;
	size = min(bufsize, req->req.length - req->req.actual);

	/* write fifo */
	if (req->req.buf) {
		m66592_write_fifo(m66592, ep->fifoaddr, buf, size);
		if ((size == 0)
				|| ((size % ep->ep.maxpacket) != 0)
				|| ((bufsize != ep->ep.maxpacket)
					&& (bufsize > size)))
			m66592_bset(m66592, M66592_BVAL, ep->fifoctr);
	}

	/* update parameters */
	req->req.actual += size;
	/* check transfer finish */
	if ((!req->req.zero && (req->req.actual == req->req.length))
			|| (size % ep->ep.maxpacket)
			|| (size == 0)) {
		disable_irq_ready(m66592, pipenum);
		enable_irq_empty(m66592, pipenum);
	} else {
		disable_irq_empty(m66592, pipenum);
		pipe_irq_enable(m66592, pipenum);
	}
}

static void irq_packet_read(struct m66592_ep *ep, struct m66592_request *req)
{
	u16 tmp;
	int rcv_len, bufsize, req_len;
	int size;
	void *buf;
	u16 pipenum = ep->pipenum;
	struct m66592 *m66592 = ep->m66592;
	int finish = 0;

	pipe_change(m66592, pipenum);
	tmp = m66592_read(m66592, ep->fifoctr);
	if (unlikely((tmp & M66592_FRDY) == 0)) {
		req->req.status = -EPIPE;
		pipe_stop(m66592, pipenum);
		pipe_irq_disable(m66592, pipenum);
		pr_err("read fifo not ready");
		return;
	}

	/* prepare parameters */
	rcv_len = tmp & M66592_DTLN;
	bufsize = get_buffer_size(m66592, pipenum);

	buf = req->req.buf + req->req.actual;
	req_len = req->req.length - req->req.actual;
	if (rcv_len < bufsize)
		size = min(rcv_len, req_len);
	else
		size = min(bufsize, req_len);

	/* update parameters */
	req->req.actual += size;

	/* check transfer finish */
	if ((!req->req.zero && (req->req.actual == req->req.length))
			|| (size % ep->ep.maxpacket)
			|| (size == 0)) {
		pipe_stop(m66592, pipenum);
		pipe_irq_disable(m66592, pipenum);
		finish = 1;
	}

	/* read fifo */
	if (req->req.buf) {
		if (size == 0)
			m66592_write(m66592, M66592_BCLR, ep->fifoctr);
		else
			m66592_read_fifo(m66592, ep->fifoaddr, buf, size);
	}

	if ((ep->pipenum != 0) && finish)
		transfer_complete(ep, req, 0);
}

static void irq_pipe_ready(struct m66592 *m66592, u16 status, u16 enb)
{
	u16 check;
	u16 pipenum;
	struct m66592_ep *ep;
	struct m66592_request *req;

	if ((status & M66592_BRDY0) && (enb & M66592_BRDY0)) {
		m66592_write(m66592, ~M66592_BRDY0, M66592_BRDYSTS);
		m66592_mdfy(m66592, M66592_PIPE0, M66592_CURPIPE,
				M66592_CFIFOSEL);

		ep = &m66592->ep[0];
		req = list_entry(ep->queue.next, struct m66592_request, queue);
		irq_packet_read(ep, req);
	} else {
		for (pipenum = 1; pipenum < M66592_MAX_NUM_PIPE; pipenum++) {
			check = 1 << pipenum;
			if ((status & check) && (enb & check)) {
				m66592_write(m66592, ~check, M66592_BRDYSTS);
				ep = m66592->pipenum2ep[pipenum];
				req = list_entry(ep->queue.next,
						 struct m66592_request, queue);
				if (ep->desc->bEndpointAddress & USB_DIR_IN)
					irq_packet_write(ep, req);
				else
					irq_packet_read(ep, req);
			}
		}
	}
}

static void irq_pipe_empty(struct m66592 *m66592, u16 status, u16 enb)
{
	u16 tmp;
	u16 check;
	u16 pipenum;
	struct m66592_ep *ep;
	struct m66592_request *req;

	if ((status & M66592_BEMP0) && (enb & M66592_BEMP0)) {
		m66592_write(m66592, ~M66592_BEMP0, M66592_BEMPSTS);

		ep = &m66592->ep[0];
		req = list_entry(ep->queue.next, struct m66592_request, queue);
		irq_ep0_write(ep, req);
	} else {
		for (pipenum = 1; pipenum < M66592_MAX_NUM_PIPE; pipenum++) {
			check = 1 << pipenum;
			if ((status & check) && (enb & check)) {
				m66592_write(m66592, ~check, M66592_BEMPSTS);
				tmp = control_reg_get(m66592, pipenum);
				if ((tmp & M66592_INBUFM) == 0) {
					disable_irq_empty(m66592, pipenum);
					pipe_irq_disable(m66592, pipenum);
					pipe_stop(m66592, pipenum);
					ep = m66592->pipenum2ep[pipenum];
					req = list_entry(ep->queue.next,
							 struct m66592_request,
							 queue);
					if (!list_empty(&ep->queue))
						transfer_complete(ep, req, 0);
				}
			}
		}
	}
}

static void get_status(struct m66592 *m66592, struct usb_ctrlrequest *ctrl)
__releases(m66592->lock)
__acquires(m66592->lock)
{
	struct m66592_ep *ep;
	u16 pid;
	u16 status = 0;
	u16 w_index = le16_to_cpu(ctrl->wIndex);

	switch (ctrl->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		status = 1 << USB_DEVICE_SELF_POWERED;
		break;
	case USB_RECIP_INTERFACE:
		status = 0;
		break;
	case USB_RECIP_ENDPOINT:
		ep = m66592->epaddr2ep[w_index & USB_ENDPOINT_NUMBER_MASK];
		pid = control_reg_get_pid(m66592, ep->pipenum);
		if (pid == M66592_PID_STALL)
			status = 1 << USB_ENDPOINT_HALT;
		else
			status = 0;
		break;
	default:
		pipe_stall(m66592, 0);
		return;		/* exit */
	}

	m66592->ep0_data = cpu_to_le16(status);
	m66592->ep0_req->buf = &m66592->ep0_data;
	m66592->ep0_req->length = 2;
	/* AV: what happens if we get called again before that gets through? */
	spin_unlock(&m66592->lock);
	m66592_queue(m66592->gadget.ep0, m66592->ep0_req, GFP_KERNEL);
	spin_lock(&m66592->lock);
}

static void clear_feature(struct m66592 *m66592, struct usb_ctrlrequest *ctrl)
{
	switch (ctrl->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		control_end(m66592, 1);
		break;
	case USB_RECIP_INTERFACE:
		control_end(m66592, 1);
		break;
	case USB_RECIP_ENDPOINT: {
		struct m66592_ep *ep;
		struct m66592_request *req;
		u16 w_index = le16_to_cpu(ctrl->wIndex);

		ep = m66592->epaddr2ep[w_index & USB_ENDPOINT_NUMBER_MASK];
		pipe_stop(m66592, ep->pipenum);
		control_reg_sqclr(m66592, ep->pipenum);

		control_end(m66592, 1);

		req = list_entry(ep->queue.next,
		struct m66592_request, queue);
		if (ep->busy) {
			ep->busy = 0;
			if (list_empty(&ep->queue))
				break;
			start_packet(ep, req);
		} else if (!list_empty(&ep->queue))
			pipe_start(m66592, ep->pipenum);
		}
		break;
	default:
		pipe_stall(m66592, 0);
		break;
	}
}

static void set_feature(struct m66592 *m66592, struct usb_ctrlrequest *ctrl)
{

	switch (ctrl->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_DEVICE:
		control_end(m66592, 1);
		break;
	case USB_RECIP_INTERFACE:
		control_end(m66592, 1);
		break;
	case USB_RECIP_ENDPOINT: {
		struct m66592_ep *ep;
		u16 w_index = le16_to_cpu(ctrl->wIndex);

		ep = m66592->epaddr2ep[w_index & USB_ENDPOINT_NUMBER_MASK];
		pipe_stall(m66592, ep->pipenum);

		control_end(m66592, 1);
		}
		break;
	default:
		pipe_stall(m66592, 0);
		break;
	}
}

/* if return value is true, call class driver's setup() */
static int setup_packet(struct m66592 *m66592, struct usb_ctrlrequest *ctrl)
{
	u16 *p = (u16 *)ctrl;
	unsigned long offset = M66592_USBREQ;
	int i, ret = 0;

	/* read fifo */
	m66592_write(m66592, ~M66592_VALID, M66592_INTSTS0);

	for (i = 0; i < 4; i++)
		p[i] = m66592_read(m66592, offset + i*2);

	/* check request */
	if ((ctrl->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
		switch (ctrl->bRequest) {
		case USB_REQ_GET_STATUS:
			get_status(m66592, ctrl);
			break;
		case USB_REQ_CLEAR_FEATURE:
			clear_feature(m66592, ctrl);
			break;
		case USB_REQ_SET_FEATURE:
			set_feature(m66592, ctrl);
			break;
		default:
			ret = 1;
			break;
		}
	} else
		ret = 1;
	return ret;
}

static void m66592_update_usb_speed(struct m66592 *m66592)
{
	u16 speed = get_usb_speed(m66592);

	switch (speed) {
	case M66592_HSMODE:
		m66592->gadget.speed = USB_SPEED_HIGH;
		break;
	case M66592_FSMODE:
		m66592->gadget.speed = USB_SPEED_FULL;
		break;
	default:
		m66592->gadget.speed = USB_SPEED_UNKNOWN;
		pr_err("USB speed unknown\n");
	}
}

static void irq_device_state(struct m66592 *m66592)
{
	u16 dvsq;

	dvsq = m66592_read(m66592, M66592_INTSTS0) & M66592_DVSQ;
	m66592_write(m66592, ~M66592_DVST, M66592_INTSTS0);

	if (dvsq == M66592_DS_DFLT) {	/* bus reset */
		m66592->driver->disconnect(&m66592->gadget);
		m66592_update_usb_speed(m66592);
	}
	if (m66592->old_dvsq == M66592_DS_CNFG && dvsq != M66592_DS_CNFG)
		m66592_update_usb_speed(m66592);
	if ((dvsq == M66592_DS_CNFG || dvsq == M66592_DS_ADDS)
			&& m66592->gadget.speed == USB_SPEED_UNKNOWN)
		m66592_update_usb_speed(m66592);

	m66592->old_dvsq = dvsq;
}

static void irq_control_stage(struct m66592 *m66592)
__releases(m66592->lock)
__acquires(m66592->lock)
{
	struct usb_ctrlrequest ctrl;
	u16 ctsq;

	ctsq = m66592_read(m66592, M66592_INTSTS0) & M66592_CTSQ;
	m66592_write(m66592, ~M66592_CTRT, M66592_INTSTS0);

	switch (ctsq) {
	case M66592_CS_IDST: {
		struct m66592_ep *ep;
		struct m66592_request *req;
		ep = &m66592->ep[0];
		req = list_entry(ep->queue.next, struct m66592_request, queue);
		transfer_complete(ep, req, 0);
		}
		break;

	case M66592_CS_RDDS:
	case M66592_CS_WRDS:
	case M66592_CS_WRND:
		if (setup_packet(m66592, &ctrl)) {
			spin_unlock(&m66592->lock);
			if (m66592->driver->setup(&m66592->gadget, &ctrl) < 0)
				pipe_stall(m66592, 0);
			spin_lock(&m66592->lock);
		}
		break;
	case M66592_CS_RDSS:
	case M66592_CS_WRSS:
		control_end(m66592, 0);
		break;
	default:
		pr_err("ctrl_stage: unexpect ctsq(%x)\n", ctsq);
		break;
	}
}

static irqreturn_t m66592_irq(int irq, void *_m66592)
{
	struct m66592 *m66592 = _m66592;
	u16 intsts0;
	u16 intenb0;
	u16 brdysts, nrdysts, bempsts;
	u16 brdyenb, nrdyenb, bempenb;
	u16 savepipe;
	u16 mask0;

	spin_lock(&m66592->lock);

	intsts0 = m66592_read(m66592, M66592_INTSTS0);
	intenb0 = m66592_read(m66592, M66592_INTENB0);

#if defined(CONFIG_SUPERH_BUILT_IN_M66592)
	if (!intsts0 && !intenb0) {
		/*
		 * When USB clock stops, it cannot read register. Even if a
		 * clock stops, the interrupt occurs. So this driver turn on
		 * a clock by this timing and do re-reading of register.
		 */
		m66592_start_xclock(m66592);
		intsts0 = m66592_read(m66592, M66592_INTSTS0);
		intenb0 = m66592_read(m66592, M66592_INTENB0);
	}
#endif

	savepipe = m66592_read(m66592, M66592_CFIFOSEL);

	mask0 = intsts0 & intenb0;
	if (mask0) {
		brdysts = m66592_read(m66592, M66592_BRDYSTS);
		nrdysts = m66592_read(m66592, M66592_NRDYSTS);
		bempsts = m66592_read(m66592, M66592_BEMPSTS);
		brdyenb = m66592_read(m66592, M66592_BRDYENB);
		nrdyenb = m66592_read(m66592, M66592_NRDYENB);
		bempenb = m66592_read(m66592, M66592_BEMPENB);

		if (mask0 & M66592_VBINT) {
			m66592_write(m66592,  0xffff & ~M66592_VBINT,
					M66592_INTSTS0);
			m66592_start_xclock(m66592);

			/* start vbus sampling */
			m66592->old_vbus = m66592_read(m66592, M66592_INTSTS0)
					& M66592_VBSTS;
			m66592->scount = M66592_MAX_SAMPLING;

			mod_timer(&m66592->timer,
					jiffies + msecs_to_jiffies(50));
		}
		if (intsts0 & M66592_DVSQ)
			irq_device_state(m66592);

		if ((intsts0 & M66592_BRDY) && (intenb0 & M66592_BRDYE)
				&& (brdysts & brdyenb)) {
			irq_pipe_ready(m66592, brdysts, brdyenb);
		}
		if ((intsts0 & M66592_BEMP) && (intenb0 & M66592_BEMPE)
				&& (bempsts & bempenb)) {
			irq_pipe_empty(m66592, bempsts, bempenb);
		}

		if (intsts0 & M66592_CTRT)
			irq_control_stage(m66592);
	}

	m66592_write(m66592, savepipe, M66592_CFIFOSEL);

	spin_unlock(&m66592->lock);
	return IRQ_HANDLED;
}

static void m66592_timer(unsigned long _m66592)
{
	struct m66592 *m66592 = (struct m66592 *)_m66592;
	unsigned long flags;
	u16 tmp;

	spin_lock_irqsave(&m66592->lock, flags);
	tmp = m66592_read(m66592, M66592_SYSCFG);
	if (!(tmp & M66592_RCKE)) {
		m66592_bset(m66592, M66592_RCKE | M66592_PLLC, M66592_SYSCFG);
		udelay(10);
		m66592_bset(m66592, M66592_SCKE, M66592_SYSCFG);
	}
	if (m66592->scount > 0) {
		tmp = m66592_read(m66592, M66592_INTSTS0) & M66592_VBSTS;
		if (tmp == m66592->old_vbus) {
			m66592->scount--;
			if (m66592->scount == 0) {
				if (tmp == M66592_VBSTS)
					m66592_usb_connect(m66592);
				else
					m66592_usb_disconnect(m66592);
			} else {
				mod_timer(&m66592->timer,
					jiffies + msecs_to_jiffies(50));
			}
		} else {
			m66592->scount = M66592_MAX_SAMPLING;
			m66592->old_vbus = tmp;
			mod_timer(&m66592->timer,
					jiffies + msecs_to_jiffies(50));
		}
	}
	spin_unlock_irqrestore(&m66592->lock, flags);
}

/*-------------------------------------------------------------------------*/
static int m66592_enable(struct usb_ep *_ep,
			 const struct usb_endpoint_descriptor *desc)
{
	struct m66592_ep *ep;

	ep = container_of(_ep, struct m66592_ep, ep);
	return alloc_pipe_config(ep, desc);
}

static int m66592_disable(struct usb_ep *_ep)
{
	struct m66592_ep *ep;
	struct m66592_request *req;
	unsigned long flags;

	ep = container_of(_ep, struct m66592_ep, ep);
	BUG_ON(!ep);

	while (!list_empty(&ep->queue)) {
		req = list_entry(ep->queue.next, struct m66592_request, queue);
		spin_lock_irqsave(&ep->m66592->lock, flags);
		transfer_complete(ep, req, -ECONNRESET);
		spin_unlock_irqrestore(&ep->m66592->lock, flags);
	}

	pipe_irq_disable(ep->m66592, ep->pipenum);
	return free_pipe_config(ep);
}

static struct usb_request *m66592_alloc_request(struct usb_ep *_ep,
						gfp_t gfp_flags)
{
	struct m66592_request *req;

	req = kzalloc(sizeof(struct m66592_request), gfp_flags);
	if (!req)
		return NULL;

	INIT_LIST_HEAD(&req->queue);

	return &req->req;
}

static void m66592_free_request(struct usb_ep *_ep, struct usb_request *_req)
{
	struct m66592_request *req;

	req = container_of(_req, struct m66592_request, req);
	kfree(req);
}

static int m66592_queue(struct usb_ep *_ep, struct usb_request *_req,
			gfp_t gfp_flags)
{
	struct m66592_ep *ep;
	struct m66592_request *req;
	unsigned long flags;
	int request = 0;

	ep = container_of(_ep, struct m66592_ep, ep);
	req = container_of(_req, struct m66592_request, req);

	if (ep->m66592->gadget.speed == USB_SPEED_UNKNOWN)
		return -ESHUTDOWN;

	spin_lock_irqsave(&ep->m66592->lock, flags);

	if (list_empty(&ep->queue))
		request = 1;

	list_add_tail(&req->queue, &ep->queue);
	req->req.actual = 0;
	req->req.status = -EINPROGRESS;

	if (ep->desc == NULL)	/* control */
		start_ep0(ep, req);
	else {
		if (request && !ep->busy)
			start_packet(ep, req);
	}

	spin_unlock_irqrestore(&ep->m66592->lock, flags);

	return 0;
}

static int m66592_dequeue(struct usb_ep *_ep, struct usb_request *_req)
{
	struct m66592_ep *ep;
	struct m66592_request *req;
	unsigned long flags;

	ep = container_of(_ep, struct m66592_ep, ep);
	req = container_of(_req, struct m66592_request, req);

	spin_lock_irqsave(&ep->m66592->lock, flags);
	if (!list_empty(&ep->queue))
		transfer_complete(ep, req, -ECONNRESET);
	spin_unlock_irqrestore(&ep->m66592->lock, flags);

	return 0;
}

static int m66592_set_halt(struct usb_ep *_ep, int value)
{
	struct m66592_ep *ep;
	struct m66592_request *req;
	unsigned long flags;
	int ret = 0;

	ep = container_of(_ep, struct m66592_ep, ep);
	req = list_entry(ep->queue.next, struct m66592_request, queue);

	spin_lock_irqsave(&ep->m66592->lock, flags);
	if (!list_empty(&ep->queue)) {
		ret = -EAGAIN;
		goto out;
	}
	if (value) {
		ep->busy = 1;
		pipe_stall(ep->m66592, ep->pipenum);
	} else {
		ep->busy = 0;
		pipe_stop(ep->m66592, ep->pipenum);
	}

out:
	spin_unlock_irqrestore(&ep->m66592->lock, flags);
	return ret;
}

static void m66592_fifo_flush(struct usb_ep *_ep)
{
	struct m66592_ep *ep;
	unsigned long flags;

	ep = container_of(_ep, struct m66592_ep, ep);
	spin_lock_irqsave(&ep->m66592->lock, flags);
	if (list_empty(&ep->queue) && !ep->busy) {
		pipe_stop(ep->m66592, ep->pipenum);
		m66592_bclr(ep->m66592, M66592_BCLR, ep->fifoctr);
	}
	spin_unlock_irqrestore(&ep->m66592->lock, flags);
}

static struct usb_ep_ops m66592_ep_ops = {
	.enable		= m66592_enable,
	.disable	= m66592_disable,

	.alloc_request	= m66592_alloc_request,
	.free_request	= m66592_free_request,

	.queue		= m66592_queue,
	.dequeue	= m66592_dequeue,

	.set_halt	= m66592_set_halt,
	.fifo_flush	= m66592_fifo_flush,
};

/*-------------------------------------------------------------------------*/
static struct m66592 *the_controller;

int usb_gadget_register_driver(struct usb_gadget_driver *driver)
{
	struct m66592 *m66592 = the_controller;
	int retval;

	if (!driver
			|| driver->speed != USB_SPEED_HIGH
			|| !driver->bind
			|| !driver->setup)
		return -EINVAL;
	if (!m66592)
		return -ENODEV;
	if (m66592->driver)
		return -EBUSY;

	/* hook up the driver */
	driver->driver.bus = NULL;
	m66592->driver = driver;
	m66592->gadget.dev.driver = &driver->driver;

	retval = device_add(&m66592->gadget.dev);
	if (retval) {
		pr_err("device_add error (%d)\n", retval);
		goto error;
	}

	retval = driver->bind (&m66592->gadget);
	if (retval) {
		pr_err("bind to driver error (%d)\n", retval);
		device_del(&m66592->gadget.dev);
		goto error;
	}

	m66592_bset(m66592, M66592_VBSE | M66592_URST, M66592_INTENB0);
	if (m66592_read(m66592, M66592_INTSTS0) & M66592_VBSTS) {
		m66592_start_xclock(m66592);
		/* start vbus sampling */
		m66592->old_vbus = m66592_read(m66592,
					 M66592_INTSTS0) & M66592_VBSTS;
		m66592->scount = M66592_MAX_SAMPLING;
		mod_timer(&m66592->timer, jiffies + msecs_to_jiffies(50));
	}

	return 0;

error:
	m66592->driver = NULL;
	m66592->gadget.dev.driver = NULL;

	return retval;
}
EXPORT_SYMBOL(usb_gadget_register_driver);

int usb_gadget_unregister_driver(struct usb_gadget_driver *driver)
{
	struct m66592 *m66592 = the_controller;
	unsigned long flags;

	if (driver != m66592->driver || !driver->unbind)
		return -EINVAL;

	spin_lock_irqsave(&m66592->lock, flags);
	if (m66592->gadget.speed != USB_SPEED_UNKNOWN)
		m66592_usb_disconnect(m66592);
	spin_unlock_irqrestore(&m66592->lock, flags);

	m66592_bclr(m66592, M66592_VBSE | M66592_URST, M66592_INTENB0);

	driver->unbind(&m66592->gadget);
	m66592->gadget.dev.driver = NULL;

	init_controller(m66592);
	disable_controller(m66592);

	device_del(&m66592->gadget.dev);
	m66592->driver = NULL;
	return 0;
}
EXPORT_SYMBOL(usb_gadget_unregister_driver);

/*-------------------------------------------------------------------------*/
static int m66592_get_frame(struct usb_gadget *_gadget)
{
	struct m66592 *m66592 = gadget_to_m66592(_gadget);
	return m66592_read(m66592, M66592_FRMNUM) & 0x03FF;
}

static struct usb_gadget_ops m66592_gadget_ops = {
	.get_frame		= m66592_get_frame,
};

static int __exit m66592_remove(struct platform_device *pdev)
{
	struct m66592		*m66592 = dev_get_drvdata(&pdev->dev);

	del_timer_sync(&m66592->timer);
	iounmap(m66592->reg);
	free_irq(platform_get_irq(pdev, 0), m66592);
	m66592_free_request(&m66592->ep[0].ep, m66592->ep0_req);
#if defined(CONFIG_SUPERH_BUILT_IN_M66592) && defined(CONFIG_HAVE_CLK)
	clk_disable(m66592->clk);
	clk_put(m66592->clk);
#endif
	kfree(m66592);
	return 0;
}

static void nop_completion(struct usb_ep *ep, struct usb_request *r)
{
}

#define resource_len(r) (((r)->end - (r)->start) + 1)

static int __init m66592_probe(struct platform_device *pdev)
{
	struct resource *res;
	int irq;
	void __iomem *reg = NULL;
	struct m66592 *m66592 = NULL;
#if defined(CONFIG_SUPERH_BUILT_IN_M66592) && defined(CONFIG_HAVE_CLK)
	char clk_name[8];
#endif
	int ret = 0;
	int i;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			(char *)udc_name);
	if (!res) {
		ret = -ENODEV;
		pr_err("platform_get_resource_byname error.\n");
		goto clean_up;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = -ENODEV;
		pr_err("platform_get_irq error.\n");
		goto clean_up;
	}

	reg = ioremap(res->start, resource_len(res));
	if (reg == NULL) {
		ret = -ENOMEM;
		pr_err("ioremap error.\n");
		goto clean_up;
	}

	/* initialize ucd */
	m66592 = kzalloc(sizeof(struct m66592), GFP_KERNEL);
	if (m66592 == NULL) {
		pr_err("kzalloc error\n");
		goto clean_up;
	}

	spin_lock_init(&m66592->lock);
	dev_set_drvdata(&pdev->dev, m66592);

	m66592->gadget.ops = &m66592_gadget_ops;
	device_initialize(&m66592->gadget.dev);
	dev_set_name(&m66592->gadget.dev, "gadget");
	m66592->gadget.is_dualspeed = 1;
	m66592->gadget.dev.parent = &pdev->dev;
	m66592->gadget.dev.dma_mask = pdev->dev.dma_mask;
	m66592->gadget.dev.release = pdev->dev.release;
	m66592->gadget.name = udc_name;

	init_timer(&m66592->timer);
	m66592->timer.function = m66592_timer;
	m66592->timer.data = (unsigned long)m66592;
	m66592->reg = reg;

	m66592->bi_bufnum = M66592_BASE_BUFNUM;

	ret = request_irq(irq, m66592_irq, IRQF_DISABLED | IRQF_SHARED,
			udc_name, m66592);
	if (ret < 0) {
		pr_err("request_irq error (%d)\n", ret);
		goto clean_up;
	}

#if defined(CONFIG_SUPERH_BUILT_IN_M66592) && defined(CONFIG_HAVE_CLK)
	snprintf(clk_name, sizeof(clk_name), "usbf%d", pdev->id);
	m66592->clk = clk_get(&pdev->dev, clk_name);
	if (IS_ERR(m66592->clk)) {
		dev_err(&pdev->dev, "cannot get clock \"%s\"\n", clk_name);
		ret = PTR_ERR(m66592->clk);
		goto clean_up2;
	}
	clk_enable(m66592->clk);
#endif
	INIT_LIST_HEAD(&m66592->gadget.ep_list);
	m66592->gadget.ep0 = &m66592->ep[0].ep;
	INIT_LIST_HEAD(&m66592->gadget.ep0->ep_list);
	for (i = 0; i < M66592_MAX_NUM_PIPE; i++) {
		struct m66592_ep *ep = &m66592->ep[i];

		if (i != 0) {
			INIT_LIST_HEAD(&m66592->ep[i].ep.ep_list);
			list_add_tail(&m66592->ep[i].ep.ep_list,
					&m66592->gadget.ep_list);
		}
		ep->m66592 = m66592;
		INIT_LIST_HEAD(&ep->queue);
		ep->ep.name = m66592_ep_name[i];
		ep->ep.ops = &m66592_ep_ops;
		ep->ep.maxpacket = 512;
	}
	m66592->ep[0].ep.maxpacket = 64;
	m66592->ep[0].pipenum = 0;
	m66592->ep[0].fifoaddr = M66592_CFIFO;
	m66592->ep[0].fifosel = M66592_CFIFOSEL;
	m66592->ep[0].fifoctr = M66592_CFIFOCTR;
	m66592->ep[0].fifotrn = 0;
	m66592->ep[0].pipectr = get_pipectr_addr(0);
	m66592->pipenum2ep[0] = &m66592->ep[0];
	m66592->epaddr2ep[0] = &m66592->ep[0];

	the_controller = m66592;

	m66592->ep0_req = m66592_alloc_request(&m66592->ep[0].ep, GFP_KERNEL);
	if (m66592->ep0_req == NULL)
		goto clean_up3;
	m66592->ep0_req->complete = nop_completion;

	init_controller(m66592);

	dev_info(&pdev->dev, "version %s\n", DRIVER_VERSION);
	return 0;

clean_up3:
#if defined(CONFIG_SUPERH_BUILT_IN_M66592) && defined(CONFIG_HAVE_CLK)
	clk_disable(m66592->clk);
	clk_put(m66592->clk);
clean_up2:
#endif
	free_irq(irq, m66592);
clean_up:
	if (m66592) {
		if (m66592->ep0_req)
			m66592_free_request(&m66592->ep[0].ep, m66592->ep0_req);
		kfree(m66592);
	}
	if (reg)
		iounmap(reg);

	return ret;
}

/*-------------------------------------------------------------------------*/
static struct platform_driver m66592_driver = {
	.remove =	__exit_p(m66592_remove),
	.driver		= {
		.name =	(char *) udc_name,
		.owner	= THIS_MODULE,
	},
};

static int __init m66592_udc_init(void)
{
	return platform_driver_probe(&m66592_driver, m66592_probe);
}
module_init(m66592_udc_init);

static void __exit m66592_udc_cleanup(void)
{
	platform_driver_unregister(&m66592_driver);
}
module_exit(m66592_udc_cleanup);
