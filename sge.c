/* sge.c
 *
 * SiS 190/191 Fast Ethernet Controller driver
 * 
 * Parts of this code are based on the FreeBSD implementation
 * (https://svnweb.freebsd.org/base/head/sys/dev/sge/), and
 * e1000 driver from Niek Linnenbank.
 *
 * Created: May 2017 by Marcelo Alencar <marceloalves@ufpa.br>
 */
 
#include <minix/drivers.h>
#include <minix/netdriver.h>
#include <stdio.h>
#include <stdlib.h>
#include <machine/pci.h>
#include "sge.h"

static int sge_instance;
static sge_t sge_state;

static void sge_init(message *mp);
static void sge_init_pci(void);
static int sge_probe(sge_t *e, int skip);
static int sge_init_hw(sge_t *e);
static void sge_reset_hw(sge_t *e);
static void sge_interrupt(message *mp);
static void sge_stop(sge_t *e);
static uint32_t sge_reg_read(sge_t *e, uint32_t reg);
static void sge_reg_write(sge_t *e, uint32_t reg, uint32_t value);
static void sge_reg_set(sge_t *e, uint32_t reg, uint32_t value);
static void sge_reg_unset(sge_t *e, uint32_t reg, uint32_t value);
static void sge_writev_s(message *mp, int from_int);
static void sge_readv_s(message *mp, int from_int);
static void sge_getstat_s(message *mp);
static void reply(sge_t *e);
static void mess_reply(message *req, message *reply);

/* SEF functions and variables. */
static void sef_local_startup(void);
static int sef_cb_init_fresh(int type, sef_init_info_t *info);
static void sef_cb_signal_handler(int signo);

/*===========================================================================*
 *                                    main                                   *
 *===========================================================================*/
int main(int argc, char *argv[])
{
	/* This is the main driver task. */
	message m;
	int ipc_status;
	int r;

	/* SEF local startup. */
	env_setargs(argc, argv);
	sef_local_startup();
	
	
	/*
	 * Enter the main driver loop.
	 */
	while (TRUE)
	{
		if ((r= netdriver_receive(ANY, &m, &ipc_status)) != OK)
		{
			panic("netdriver_receive failed: %d", r);
		}
		
		if (is_ipc_notify(ipc_status))
		{
			switch (_ENDPOINT_P(m.m_source))
			{
			case HARDWARE:
				sge_interrupt(&m);
				break;
			case CLOCK:
				break;
			}
			continue;
		}
		switch (m.m_type)
		{
		case DL_CONF:       sge_init(&m);               break;
		case DL_GETSTAT_S:  sge_getstat_s(&m);          break;
		case DL_WRITEV_S:   sge_writev_s(&m, FALSE);    break;
		case DL_READV_S:    sge_readv_s(&m, FALSE);     break;
	    default:
			panic("illegal message: %d", m.m_type);
		}
	}
}

/*===========================================================================*
 *                          sef_local_startup                                *
 *===========================================================================*/
static void sef_local_startup()
{
	/* Register init callbacks. */
	sef_setcb_init_fresh(sef_cb_init_fresh);
	sef_setcb_init_lu(sef_cb_init_fresh);
	sef_setcb_init_restart(sef_cb_init_fresh);

	/* Register live update callbacks. */
	sef_setcb_lu_prepare(sef_cb_lu_prepare_always_ready);
	sef_setcb_lu_state_isvalid(sef_cb_lu_state_isvalid_workfree);

	/* Register signal callbacks. */
	sef_setcb_signal_handler(sef_cb_signal_handler);

	/* Let SEF perform startup. */
	sef_startup();
}

/*===========================================================================*
 *                          sef_cb_init_fresh                                *
 *===========================================================================*/
static int sef_cb_init_fresh(int UNUSED(type), sef_init_info_t *UNUSED(info))
{
	/* Initialize the SiS FE Driver. */
	long v;
	int r;

	v = 0;
	(void)env_parse("instance", "d", 0, &v, 0, 255);
	sge_instance = (int) v;

	/* Clear state. */
	memset(&sge_state, 0, sizeof(sge_state));

	/* Announce we are up! */
	netdriver_announce();

	return(OK);
}

/*===========================================================================*
 *                        sef_cb_signal_handler                              *
 *===========================================================================*/
static void sef_cb_signal_handler(int signo)
{
	sge_t *e;
	e = &sge_state;

	/* Only check for termination signal, ignore anything else. */
	if (signo != SIGTERM) return;

	sge_stop(e);
}

/*===========================================================================*
 *                               sge_init                                    *
 *===========================================================================*/
static void sge_init(message *mp)
{
	static int first_time = 1;
	message reply_mess;
	sge_t *e;

	/* Configure PCI devices, if needed. */
	if (first_time)
	{
		first_time = 0;
		sge_init_pci();
	}
	e = &sge_state;

	/* Initialize hardware, if needed. */
	if (!(e->status & SGE_ENABLED) && !(sge_init_hw(e)))
	{
		reply_mess.m_type  = DL_CONF_REPLY;
		reply_mess.m_netdrv_net_dl_conf.stat = ENXIO;
		mess_reply(mp, &reply_mess);
		return;
	}
	/* Reply back to INET. */
	reply_mess.m_type  = DL_CONF_REPLY;
	reply_mess.m_netdrv_net_dl_conf.stat = OK;
	memcpy(reply_mess.m_netdrv_net_dl_conf.hw_addr, e->address.ea_addr,
		sizeof(reply_mess.m_netdrv_net_dl_conf.hw_addr));
	mess_reply(mp, &reply_mess);
}

/*===========================================================================*
 *                             sge_init_pci                                  *
 *===========================================================================*/
static void sge_init_pci()
{
	sge_t *e;

	/* Initialize the PCI bus. */
	pci_init();

	/* Try to detect sge's. */
	e = &sge_state;
	strlcpy(e->name, "sge#0", sizeof(e->name));
	e->name[4] += sge_instance;
	sge_probe(e, sge_instance);
}

/*===========================================================================*
 *                               sge_probe                                   *
 *===========================================================================*/
static int sge_probe(sge_t *e, int skip)
{
	int r, devind, ioflag;
	u16_t vid, did, cr;
	u32_t status[2];
	u32_t base, size;
	u32_t gfpreg, sector_base_addr;
	char *dname;

	/*
	 * Attempt to iterate the PCI bus. Start at the beginning.
	 */
	if ((r = pci_first_dev(&devind, &vid, &did)) == 0)
	{
		return FALSE;
	}
	/* Loop devices on the PCI bus. */
	while (skip--)
	{
		printf("%s: probe() devind %d vid 0x%x did 0x%x\n",
				e->name, devind, vid, did);
		if (!(r = pci_next_dev(&devind, &vid, &did)))
		{
			return FALSE;
		}
	}

	/*
	 * Successfully detected an SiS Ethernet Controller on the PCI bus.
	 */
	e->status = SGE_DETECTED;

	/*
	 * Set card specific properties.
	 */
	switch (did)
	{
	case SGE_DEV_0190:
		printf("Found SiS 0190.\n");
		break;
	case SGE_DEV_0191:
		printf("Found SiS 0191.\n");
		break;
	default:
		printf("That may be a SiS device.\n");
		break;
	}

	/* Inform the user about the new card. */
	if (!(dname = pci_dev_name(vid, did)))
	{
		dname = "SiS 190/191 PCI Fast/Gigabit Ethernet Adapter";
	}

	printf("%s: %s (%04x/%04x/%02x) at %s\n",
		e->name, dname, vid, did, e->revision, 
		pci_slot_name(devind));

	/* Reserve PCI resources found. */
	if ((r = pci_reserve_ok(devind)) != OK)
	{
		panic("failed to reserve PCI device: %d", r);
	}
	/* Read PCI configuration. */
	e->irq   = pci_attr_r8(devind, PCI_ILR);

	if ((r = pci_get_bar(devind, PCI_BAR, &base, &size, &ioflag)) != OK)
		panic("failed to get PCI BAR (%d)", r);
	if (ioflag) panic("PCI BAR is not for memory");

	e->regs  = vm_map_phys(SELF, (void *) base, size);
	if (e->regs == (u8_t *) -1) {
		panic("failed to map hardware registers from PCI");
	}

	cr = pci_attr_r16(devind, PCI_CR);
	if (!(cr & PCI_CR_MAST_EN))
		pci_attr_w16(devind, PCI_CR, cr | PCI_CR_MAST_EN);

	printf("%s: using I/O address %p, IRQ %d\n",
		e->name, e->regs, e->irq);

	return TRUE;
}

/*===========================================================================*
 *                              sge_init_hw                                  *
 *===========================================================================*/
static int sge_init_hw(e)
sge_t *e;
{
	int r, i;

	e->status = SGE_ENABLED;
	e->irq_hook = e->irq;
	
	/*
	 * Set the interrupt handler and policy. Do not automatically
	 * re-enable interrupts. Return the IRQ line number on interrupts.
	 */
	if ((r = sys_irqsetpolicy(e->irq, 0, &e->irq_hook)) != OK)
	{
		panic("sys_irqsetpolicy failed: %d", r);
	}
	if ((r = sys_irqenable(&e->irq_hook)) != OK)
	{
		panic("sys_irqenable failed: %d", r);
	}
	/* Reset hardware. */
	sge_reset_hw(e);

	/* Initialization routine */
	printf("%s: MEM at %p, IRQ %d\n", e->name, e->regs, e->irq);

	return TRUE;
}

/*===========================================================================*
 *                             sge_reset_hw                                  *
 *===========================================================================*/
static void sge_reset_hw(e)
sge_t *e;
{
	/* Reset routine goes here */

	tickdelay(1);
}

/*===========================================================================*
 *                             sge_writev_s                                  *
 *===========================================================================*/
static void sge_writev_s(mp, from_int)
message *mp;
int from_int;
{
	
}

/*===========================================================================*
 *                              sge_readv_s                                  *
 *===========================================================================*/
static void sge_readv_s(mp, from_int)
message *mp;
int from_int;
{
	
}

/*===========================================================================*
 *                             sge_getstat_s                                 *
 *===========================================================================*/
static void sge_getstat_s(mp)
message *mp;
{
	
}

/*===========================================================================*
 *                             sge_interrupt                                 *
 *===========================================================================*/
static void sge_interrupt(mp)
message *mp;
{
	
}

/*===========================================================================*
 *                                sge_stop                                   *
 *===========================================================================*/
static void sge_stop(e)
sge_t *e;
{
	printf("%s: MEM at %p, IRQ %d\n", e->name, e->regs, e->irq);

	sge_reset_hw(e);

	exit(EXIT_SUCCESS);
}

/*===========================================================================*
 *                              sge_reg_read                                 *
 *===========================================================================*/
static uint32_t sge_reg_read(e, reg)
sge_t *e;
uint32_t reg;
{
	uint32_t value;

	/* Read from memory mapped register. */
	value = *(volatile u32_t *)(e->regs + reg);

	/* Return the result. */    
	return value;
}

/*===========================================================================*
 *                              sge_reg_write                                *
 *===========================================================================*/
static void sge_reg_write(e, reg, value)
sge_t *e;
uint32_t reg;
uint32_t value;
{
	/* Write to memory mapped register. */
	*(volatile u32_t *)(e->regs + reg) = value;
}

/*===========================================================================*
 *                               sge_reg_set                                 *
 *===========================================================================*/
static void sge_reg_set(e, reg, value)
sge_t *e;
uint32_t reg;
uint32_t value;
{
	uint32_t data;

	/* First read the current value. */
	data = sge_reg_read(e, reg);

	/* Set value, and write back. */
	sge_reg_write(e, reg, data | value);
}

/*===========================================================================*
 *                             sge_reg_unset                                 *
 *===========================================================================*/
static void sge_reg_unset(e, reg, value)
sge_t *e;
uint32_t reg;
uint32_t value;
{
	uint32_t data;

	/* First read the current value. */
	data = sge_reg_read(e, reg);

	/* Unset value, and write back. */
	sge_reg_write(e, reg, data & ~value);
}

/*===========================================================================*
 *                                 reply                                     *
 *===========================================================================*/
static void reply(e)
sge_t *e;
{
	message msg;
	int r;

	/* Only reply to client for read/write request. */
	if (!(e->status & SGE_READING ||
		e->status & SGE_WRITING))
	{
		return;
    }
	/* Construct reply message. */
	msg.m_type   = DL_TASK_REPLY;
	msg.m_netdrv_net_dl_task.flags = DL_NOFLAGS;
	msg.m_netdrv_net_dl_task.count = 0;

	/* Did we successfully receive packet(s)? */
	if (e->status & SGE_READING &&
	e->status & SGE_RECEIVED)
	{
		msg.m_netdrv_net_dl_task.flags |= DL_PACK_RECV;
		msg.m_netdrv_net_dl_task.count =
			e->rx_size >= ETH_MIN_PACK_SIZE ?
				e->rx_size  : ETH_MIN_PACK_SIZE;

	    /* Clear flags. */
		e->status &= ~(SGE_READING | SGE_RECEIVED);
	}
	/* Did we successfully transmit packet(s)? */
	if (e->status & SGE_TRANSMIT &&
		e->status & SGE_WRITING)
	{
		msg.m_netdrv_net_dl_task.flags |= DL_PACK_SEND;
		
		/* Clear flags. */
		e->status &= ~(SGE_WRITING | SGE_TRANSMIT);
	}

	/* Acknowledge to INET. */
	if ((r = ipc_send(e->client, &msg)) != OK)
	{
		panic("ipc_send() failed: %d", r);
	}
}

/*===========================================================================*
 *                              mess_reply                                   *
 *===========================================================================*/
static void mess_reply(req, reply_mess)
message *req;
message *reply_mess;
{
	if (ipc_send(req->m_source, reply_mess) != OK)
	{
		panic("unable to send reply message");
	}
}
