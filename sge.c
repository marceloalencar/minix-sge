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
static void sge_init_addr(sge_t *e);
static void sge_init_buf(sge_t *e);
static void sge_reset_hw(sge_t *e);
static void sge_interrupt(message *mp);
static void sge_stop(sge_t *e);
static uint32_t sge_reg_read(sge_t *e, uint32_t reg);
static void sge_reg_write(sge_t *e, uint32_t reg, uint32_t value);
static void sge_reg_set(sge_t *e, uint32_t reg, uint32_t value);
static void sge_reg_unset(sge_t *e, uint32_t reg, uint32_t value);
static u16_t read_eeprom(void *e, int reg);
static void sge_writev_s(message *mp, int from_int);
static void sge_readv_s(message *mp, int from_int);
static void sge_getstat_s(message *mp);
static void reply(sge_t *e);
static void mess_reply(message *req, message *reply);
static void sge_dump(message *m);

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
			case TTY_PROC_NR:
				sge_dump(&m);
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
	int r, fkeys, sfkeys;
	long v;

	/* Request function key for debug dumps */
	fkeys = sfkeys = 0;
	bit_set(sfkeys, 7);
	if ((r = fkey_map(&fkeys, &sfkeys)) != OK)
		printf("sge: couldn't bind Shift+F7 key (%d)\n", r);

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
		/* Inform the user about the new card. */
		if (!(dname = pci_dev_name(vid, did)))
		{
			dname = "SiS 190 PCI Fast Ethernet Adapter";
			printf("%s: %s (%04x/%04x/%02x) at %s\n",
			e->name, dname, vid, did, e->revision, 
			pci_slot_name(devind));
		}
		break;
	case SGE_DEV_0191:
		/* Inform the user about the new card. */
		if (!(dname = pci_dev_name(vid, did)))
		{
			dname = "SiS 191 PCI Gigabit Ethernet Adapter";
			printf("%s: %s (%04x/%04x/%02x) at %s\n",
			e->name, dname, vid, did, e->revision, 
			pci_slot_name(devind));
		}
		break;
	default:
		break;
	}

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

	/* Where to read MAC from? */
	e->MAC_APC = pci_attr_r8(devind, 0x73);

	printf("%s: Is MAC at APC? %d\n",
		e->name, e->MAC_APC);

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
		printf("sys_irqsetpolicy failed: %d", r);
		return -EFAULT;
	}
	if ((r = sys_irqenable(&e->irq_hook)) != OK)
	{
		printf("sys_irqenable failed: %d", r);
		return -EFAULT;
	}
	/* Reset hardware. */
	sge_reset_hw(e);

	/* Initialization routine */
	sge_init_addr(e);
    sge_init_buf(e);

	return TRUE;
}


/*===========================================================================*
 *                             sge_init_addr                                 *
 *===========================================================================*/
static void sge_init_addr(e)
sge_t *e;
{
	static char eakey[] = SGE_ENVVAR "#_EA";
	static char eafmt[] = "x:x:x:x:x:x";
	u16_t val;
    int i;
    long v;
	
	/*
     * Do we have a user defined ethernet address?
     */
    eakey[sizeof(SGE_ENVVAR)-1] = '0' + sge_instance;
    
    for (i = 0; i < 6; i++)
	{
		if (env_parse(eakey, eafmt, i, &v, 0x00L, 0xFFL) != EP_SET)
			break;
		e->address.ea_addr[i] = v;
	}
	
	/* Nothing was read or not everything was read? */
	if (i != 6)
	{
		/* Embedded SiS190 has MAC in southbridge. */
		if (e->MAC_APC & 0x1)
		{
			/*
			// Implementar a conexão com a ponte ISA.
			*/
			e->address.ea_addr[0] = 0;
			e->address.ea_addr[1] = 0;
			e->address.ea_addr[2] = 0;
			e->address.ea_addr[3] = 0;
			e->address.ea_addr[4] = 0;
			e->address.ea_addr[5] = 0;
		}
		/* Standalone SiS190 has MAC in EEPROM. */
		else
		{
			for (i = 0; i < 3; i++)
			{
				val = read_eeprom(e, SGE_EEPADDR_MAC + i);
			    e->address.ea_addr[(i * 2)]     = (val & 0xff);
			    e->address.ea_addr[(i * 2) + 1] = (val & 0xff00) >> 8;
			}
			if ((read_eeprom(e, SGE_EEPADDR_INFO) & 0x80) != 0)
			{
				e->RGMII = 1;
			}
			else
			{
				e->RGMII = 0;
			}
		}
	}
	
	printf("%s: Ethernet Address %x:%x:%x:%x:%x:%x\n", e->name,
		    e->address.ea_addr[0], e->address.ea_addr[1],
		    e->address.ea_addr[2], e->address.ea_addr[3],
		    e->address.ea_addr[4], e->address.ea_addr[5]);
}

/*===========================================================================*
 *                              sge_init_buf                                 *
 *===========================================================================*/
static void sge_init_buf(e)
sge_t *e;
{
	
}

/*===========================================================================*
 *                             sge_reset_hw                                  *
 *===========================================================================*/
static void sge_reset_hw(e)
sge_t *e;
{
	sge_reg_write(e, SGE_REG_INTRMASK, 0);
	sge_reg_write(e, SGE_REG_INTRSTATUS, 0xffffffff);
	
	sge_reg_write(e, SGE_REG_TX_CTL, 0x00001c00);
	sge_reg_write(e, SGE_REG_RX_CTL, 0x001e1c00);
	
	sge_reg_write(e, SGE_REG_INTRCONTROL, 0x8000);
	sge_reg_read(e, SGE_REG_INTRCONTROL);
	micro_delay(100);
	sge_reg_write(e, SGE_REG_INTRCONTROL, 0x0);

	sge_reg_write(e, SGE_REG_INTRMASK, 0);
	sge_reg_write(e, SGE_REG_INTRSTATUS, 0xffffffff);

	sge_reg_write(e, SGE_REG_TX_DESC, 0x0);
	sge_reg_write(e, SGE_REG_RESERVED0, 0x0);
	sge_reg_write(e, SGE_REG_RX_DESC, 0x0);
	sge_reg_write(e, SGE_REG_RESERVED1, 0x0);

	sge_reg_write(e, SGE_REG_PMCONTROL, 0xffc00000);
	sge_reg_write(e, SGE_REG_RESERVED2, 0x0);
	
	if (e->RGMII)
	{
		sge_reg_write(e, SGE_REG_STATIONCONTROL, 0x04008001);
	}
	else
	{
		sge_reg_write(e, SGE_REG_STATIONCONTROL, 0x04000001);
	}
	
	sge_reg_write(e, SGE_REG_GMACIOCR, 0x0);
	sge_reg_write(e, SGE_REG_GMACIOCTL, 0x0);

	sge_reg_write(e, SGE_REG_TXMACCONTROL, 0x00002364);
	sge_reg_write(e, SGE_REG_TXMACTIMELIMIT, 0x0000000f);

	sge_reg_write(e, SGE_REG_RGMIIDELAY, 0x0);
	sge_reg_write(e, SGE_REG_RESERVED3, 0x0);
	sge_reg_write(e, SGE_REG_RXMACCONTROL, 0x00000252);

	sge_reg_write(e, SGE_REG_RXHASHTABLE, 0x0);
	sge_reg_write(e, SGE_REG_RXHASHTABLE2, 0x0);

	sge_reg_write(e, SGE_REG_RXWAKEONLAN, 0x80ff0000);
	sge_reg_write(e, SGE_REG_RXWAKEONLANDATA, 0x80ff0000);
	sge_reg_write(e, SGE_REG_RXMPSCONTROL, 0x0);
	sge_reg_write(e, SGE_REG_RESERVED4, 0x0);
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
	sge_t *e;
	u32_t cause;

	/*
	 * Check the card for interrupt reason(s).
	 */
	e = &sge_state;

	/* Re-enable interrupts. */
	if (sys_irqenable(&e->irq_hook) != OK)
	{
		panic("failed to re-enable IRQ");
	}

	/* Read the Interrupt Cause Read register. */
	
}

/*===========================================================================*
 *                                sge_stop                                   *
 *===========================================================================*/
static void sge_stop(e)
sge_t *e;
{
	printf("%s: stopping...\n", e->name);

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
 *                              read_eeprom                                  *
 *===========================================================================*/
static u16_t read_eeprom(v, reg)
void *v;
int reg;
{
	sge_t *e = (sge_t *) v;
	u32_t data;
	u32_t read_cmd;

	/* Request EEPROM read. */
	read_cmd = SGE_EEPROM_REQ | SGE_EEPROM_OPER | (reg << SGE_EEPROM_OFFSET_SHIFT);
	sge_reg_write(e, SGE_REG_EEPROMINTERFACE, read_cmd);

	/* Wait 500ms */
	micro_delay(500);

	/* Wait until ready. */
	while (!((data = (sge_reg_read(e, SGE_REG_EEPROMINTERFACE))) & SGE_EEPROM_REQ));

	return (u16_t)((data & SGE_EEPROM_DATA) >> SGE_EEPROM_DATA_SHIFT);
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

/*===========================================================================*
 *                               sge_dump                                    *
 *===========================================================================*/
static void sge_dump(m)
message *m;
{
	sge_t *e;
	e = &sge_state;
	
	long i;

	printf("Show Mac Registers\n");
	for(i = 0; i < 0x80; i+=4)
	{
		if((i%16) == 0)
			printf("%2.2xh: ", (char)i);
		
		printf("%8.8x ", sge_reg_read(e, i));
		
		if((i%16) == 12)
			printf("\n");
	
	}
	printf("\n");	
}
