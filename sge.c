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
static uint16_t read_eeprom(sge_t *e, int reg);
static int sge_mii_probe(sge_t *e);
static uint16_t sge_mii_read(sge_t *e, uint32_t phy, uint32_t reg);
static void sge_mii_write(sge_t *e, uint32_t phy, uint32_t reg, uint32_t data);
static void sge_writev_s(message *mp, int from_int);
static void sge_readv_s(message *mp, int from_int);
static void sge_getstat_s(message *mp);
static uint16_t sge_default_phy(sge_t *e);
static uint16_t sge_reset_phy(sge_t *e, uint32_t addr);
static void sge_phymode(sge_t *e);
static void sge_macmode(sge_t *e);
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
		}
		e->model = SGE_DEV_0190;
		break;
	case SGE_DEV_0191:
		/* Inform the user about the new card. */
		if (!(dname = pci_dev_name(vid, did)))
		{
			dname = "SiS 191 PCI Gigabit Ethernet Adapter";
		}
		e->model = SGE_DEV_0191;
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
	int isAPC = pci_attr_r8(devind, 0x73);
	if ((isAPC & 0x1) == 0)
	{
		e->MAC_APC = 0;
	}
	else
	{
		e->MAC_APC = 1;
	}

	return TRUE;
}

/*===========================================================================*
 *                              sge_init_hw                                  *
 *===========================================================================*/
static int sge_init_hw(e)
sge_t *e;
{
	int r, i;
	uint32_t control;
	uint16_t filter;

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

	if (sge_mii_probe(e) == 0)
	{
		return -ENODEV;
	}

	sge_reg_write(e, SGE_REG_RXMACADDR, 0);

	/* Set filter */
	filter = sge_reg_read(e, SGE_REG_RXMACCONTROL);
	/* disable disable packet filtering before address is set */
	sge_reg_write(e, SGE_REG_RXMACCONTROL, filter & ~0xf00);
	/* Write MAC to registers */
	for (i = 0; i < 6 ; i++)
	{
		uint8_t w;

		w = (uint8_t) e->address.ea_addr[i];
		sge_reg_write(e, SGE_REG_RXMACADDR + i, w);
	}
	/* Enable filter = */
	filter &= ~(SGE_RXCTRL_BCAST | SGE_RXCTRL_ALLPHYS | SGE_RXCTRL_MCAST);
	filter |= SGE_RXCTRL_MYPHYS | SGE_RXCTRL_BCAST;

	sge_reg_write(e, SGE_REG_RXMACCONTROL, filter);
	sge_reg_write(e, SGE_REG_RXHASHTABLE, 0xffffffff);
	sge_reg_write(e, SGE_REG_RXHASHTABLE2, 0xffffffff);

	/* Enable interrupts */
	sge_reg_write(e, SGE_REG_INTRMASK, SGE_INTRS);

	/* Enable TX/RX */
	control = sge_reg_read(e, SGE_REG_TX_CTL);
	sge_reg_write(e, SGE_REG_TX_CTL, control | 0x1);
	control = sge_reg_read(e, SGE_REG_RX_CTL);
	sge_reg_write(e, SGE_REG_RX_CTL, control | 0x1 | 0x10);

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
		if (e->MAC_APC)
		{
			/*
			// ISA Bridge read not implemented.
			*/
			panic("Read MAC from APC not implemented...\n");
		}
		/* Standalone SiS190 has MAC in EEPROM. */
		else
		{
			for (i = 0; i < 3; i++)
			{
				val = read_eeprom(e, SGE_EEPADDR_MAC + i);
				e->address.ea_addr[(i * 2)] = (val & 0xff);
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
}

/*===========================================================================*
 *                              sge_init_buf                                 *
 *===========================================================================*/
static void sge_init_buf(e)
sge_t *e;
{
	/* This function initializes the TX/RX rings, used for DMA transfers */
	int i;
	phys_bytes rx_buff_p;
	phys_bytes tx_buff_p;

	e->rx_desc_count = SGE_RXDESC_NR;
	e->tx_desc_count = SGE_TXDESC_NR;

	if (!e->rx_desc)
	{
		/* Allocate RX descriptors. */
		/* rx_desc is the RX ring space. rx_desc_p is a pointer to it. */
		if ((e->rx_desc = alloc_contig(SGE_RX_TOTALSIZE + 15, AC_ALIGN4K,
			&e->rx_desc_p)) == NULL)
		{
			panic("%s: Failed to allocate RX descriptors.\n", e->name);
		}
		memset(e->rx_desc, 0, SGE_RX_TOTALSIZE + 15);

		/* Allocate RX buffers */
		e->rx_buffer_size = SGE_RXDESC_NR * SGE_BUF_SIZE;

		if ((e->rx_buffer = alloc_contig(e->rx_buffer_size,
			AC_ALIGN4K, &rx_buff_p)) == NULL)
		{
			panic("%s: Failed to allocate RX buffers.\n", e->name);
		}

		e->cur_rx = 0;

		/* Setup receive descriptors. */
		for (i = 0; i < SGE_RXDESC_NR; i++)
		{
			e->tx_desc[i].pkt_size = 0;
			/* RX descriptors are initially held by hardware */
			e->tx_desc[i].status = SGE_RXINFO_RXOWN | SGE_RXINFO_RXINT;
			e->tx_desc[i].buf_ptr = tx_buff_p + (i * SGE_BUF_SIZE);
			e->tx_desc[i].flags = 0;
			/* Last descriptor is marked as final */
			if (i == SGE_RXDESC_NR - 1)
			{
				e->rx_desc[i].flags = SGE_DESC_FINAL;
			}
			else
			{
				e->rx_desc[i].flags = 0;
			}
			e->tx_desc[i].flags |= (SGE_BUF_SIZE & 0xfff8);
		}
	}

	if (!e->tx_desc)
	{
		/* Allocate TX descriptors. */
		/* tx_desc is the TX ring space. tx_desc_p is a pointer to it. */
		if ((e->tx_desc = alloc_contig(SGE_TX_TOTALSIZE + 15, AC_ALIGN4K,
			&e->tx_desc_p)) == NULL)
		{
			panic("%s: Failed to allocate TX descriptors.\n", e->name);
		}
		memset(e->tx_desc, 0, SGE_TX_TOTALSIZE + 15);

		/* Allocate TX buffers */
		e->tx_buffer_size = SGE_TXDESC_NR * SGE_BUF_SIZE;

		if ((e->tx_buffer = alloc_contig(e->tx_buffer_size,
			AC_ALIGN4K, &tx_buff_p)) == NULL)
		{
			panic("%s: Failed to allocate TX buffers.\n", e->name);
		}

		e->cur_tx = 0;

		/* Setup receive descriptors. */
		for (i = 0; i < SGE_TXDESC_NR; i++)
		{
			e->tx_desc[i].pkt_size = 0;
			e->tx_desc[i].status = 0;
			e->tx_desc[i].buf_ptr = 0;
			e->tx_desc[i].flags = 0;
		}
		/* Last descriptor is marked as final */
		e->tx_desc[SGE_TXDESC_NR - 1].flags = SGE_DESC_FINAL;
	}

	/* Inform card where the buffer is */
	sge_reg_write(e, SGE_REG_TX_DESC, tx_buff_p);
	sge_reg_write(e, SGE_REG_RX_DESC, rx_buff_p);
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
	sge_t *e = &sge_state;
	sge_tx_desc_t *desc;
	iovec_s_t iovec[SGE_IOVEC_NR];
	int r, i, bytes = 0, size;
	uint32_t command;
	uint32_t current;
	uint32_t status;

	printf("%s: sge_writev_s() from interrupt? %d\n", e->name, from_int ? 1 : 0);

	/* Are we called from the interrupt handler? */
	if (!from_int)
	{
		if (!(e->autoneg_done))
		{
			return;
		}

		/* Copy write message. */
		e->tx_message = *mp;
		e->client = mp->m_source;
		e->status |= SGE_WRITING;

		/*
		 * Copy the I/O vector table.
		 */
		if ((r = sys_safecopyfrom(e->tx_message.m_source,
			e->tx_message.m_net_netdrv_dl_writev_s.grant, 0,
			(vir_bytes) iovec,
			e->tx_message.m_net_netdrv_dl_writev_s.count *
			sizeof(iovec_s_t))) != OK)
		{
			panic("sys_safecopyfrom() failed: %d", r);
		}

		current = e->cur_tx % SGE_TXDESC_NR;
		desc = &e->tx_desc[current];

		/* Loop vector elements. */
		for (i = 0; i < e->tx_message.m_net_netdrv_dl_writev_s.count; i++)
		{
			size = iovec[i].iov_size < (SGE_BUF_SIZE - bytes) ?
				iovec[i].iov_size : (SGE_BUF_SIZE - bytes);

			/* Copy bytes to TX queue buffers. */
			if ((r = sys_safecopyfrom(e->tx_message.m_source,
				iovec[i].iov_grant, 0,
				(vir_bytes) e->tx_buffer +
				(current * SGE_BUF_SIZE),
				size)) != OK)
			{
				panic("sys_safecopyfrom() failed: %d", r);
			}
			/* Mark this descriptor ready. */
			desc->PktSize = size & 0xffff;
			desc->EOD |= size & 0xffff;
			desc->cmdsts = (SGE_TXSTATUS_PADEN | SGE_TXSTATUS_CRCEN |
				SGE_TXSTATUS_DEFEN | SGE_TXSTATUS_THOL3 | SGE_TXSTATUS_TXINT);
			if (e->duplex_mode == 0)
			{
				desc->cmdsts |= (SGE_TXSTATUS_COLSEN | SGE_TXSTATUS_CRSEN |
					SGE_TXSTATUS_BKFEN);
				if (e->link_speed == SGE_SPEED_1000)
					desc->cmdsts |= (SGE_TXSTATUS_EXTEN | SGE_TXSTATUS_BSTEN);
			}

			/* Move to next descriptor. */
			current = (current + 1) % SGE_TXDESC_NR;
			desc = &e->tx_desc[current];
			bytes +=  size;
		}
		/* Increment tail. Start transmission. */
		e->cur_tx = current;
		command = sge_reg_read(e, SGE_REG_TX_CTL);
		sge_reg_write(e, SGE_REG_TX_CTL, 0x10 | command);
	}
	else
	{
		e->status |= SGE_TRANSMIT;
	}
	reply(e);
}

/*===========================================================================*
 *                              sge_readv_s                                  *
 *===========================================================================*/
static void sge_readv_s(mp, from_int)
message *mp;
int from_int;
{
	sge_t *e = &sge_state;
	sge_rx_desc_t *desc;
	iovec_s_t iovec[SGE_IOVEC_NR];
	int r, i, bytes = 0, size;
	uint32_t command;
	uint32_t current;
	uint32_t status;
	uint32_t pkt_size;

	printf("%s: sge_readv_s() from interrupt? %d\n", e->name, from_int ? 1 : 0);

	/* Are we called from the interrupt handler? */
	if (!from_int)
	{
		/* Copy read message. */
		e->rx_message = *mp;
		e->client = mp->m_source;
		e->status |= SGE_READING;
		e->rx_size = 0;
	}

	if (e->status == SGE_READING)
	{
		/*
		 * Copy the I/O vector table.
		 */
		if ((r = sys_safecopyfrom(e->rx_message.m_source,
			e->rx_message.m_net_netdrv_dl_readv_s.grant, 0,
			(vir_bytes) iovec,
			e->rx_message.m_net_netdrv_dl_readv_s.count *
			sizeof(iovec_s_t))) != OK)
		{
			panic("sys_safecopyfrom() failed: %d", r);
		}

		current = e->cur_rx % SGE_RXDESC_NR;
		desc = &e->rx_desc[current];
		pkt_size = desc->StsSize & 0xffff;

		/* Copy to vector elements. */
		for (i = 0; i < e->rx_message.m_net_netdrv_dl_readv_s.count &&
			bytes < pkt_size; i++)
		{
			size = iovec[i].iov_size < (pkt_size - bytes) ?
				iovec[i].iov_size : (pkt_size - bytes);

			if ((r = sys_safecopyto(e->rx_message.m_source, iovec[i].iov_grant,
				0, (vir_bytes) e->rx_buffer + bytes +
				(current * SGE_BUF_SIZE),
				size)) != OK)
			{
				panic("sys_safecopyto() failed: %d", r);
			}
			bytes += size;

			desc->StsSize = 0;
			desc->PktInfo = SGE_RXINFO_RXOWN | SGE_RXINFO_RXINT;

			/* Move to next descriptor. */
			current = (current + 1) % SGE_RXDESC_NR;
			desc = &e->rx_desc[current];
			bytes += size;
		}
		/* Update current and reenable. */
		e->cur_rx = current;
		command = sge_reg_read(e, SGE_REG_RX_CTL);
		sge_reg_write(e, SGE_REG_RX_CTL, 0x10 | command);

		e->rx_size = bytes;
		e->status |= SGE_RECEIVED;
	}
	reply(e);
}

/*===========================================================================*
 *                             sge_getstat_s                                 *
 *===========================================================================*/
static void sge_getstat_s(mp)
message *mp;
{
	printf("sge: getstat_s() not implemented!\n");
}

/*===========================================================================*
 *                             sge_interrupt                                 *
 *===========================================================================*/
static void sge_interrupt(mp)
message *mp;
{
	sge_t *e;
	u32_t status;

	/*
	 * Check the card for interrupt reason(s).
	 */
	e = &sge_state;

	status = sge_reg_read(e, SGE_REG_INTRSTATUS);;
	if (!(status == 0xffffffff || (status & SGE_INTRS) == 0))
	{
		//Acknowledge and disable interrupts
		sge_reg_write(e, SGE_REG_INTRSTATUS, status);
		sge_reg_write(e, SGE_REG_INTRMASK, 0);

		/* Read the Interrupt Cause Read register. */
		if ((status & SGE_INTRS) == 0)
		{
			/* Nothing */
			return;
		}
		sge_reg_write(e, SGE_REG_INTRSTATUS, status);
		if (status & (SGE_INTR_TX_DONE | SGE_INTR_TX_IDLE))
			/* Tx interrupt */
			sge_writev_s(&e->tx_message, TRUE);
		if (status & (SGE_INTR_RX_DONE | SGE_INTR_RX_IDLE))
			/* Rx interrupt */
			sge_readv_s(&e->rx_message, TRUE);
		if (status & SGE_INTR_LINK)
			printf("%s: Link changed.\n", e->name);
	}

	/* Re-enable interrupts. */
	sge_reg_write(e, SGE_REG_INTRMASK, SGE_INTRS);
	if (sys_irqenable(&e->irq_hook) != OK)
	{
		panic("failed to re-enable IRQ");
	}
}

/*===========================================================================*
 *                                sge_stop                                   *
 *===========================================================================*/
static void sge_stop(e)
sge_t *e;
{
	uint32_t val;
	printf("%s: stopping...\n", e->name);

	sge_reset_hw(e);

	sge_reg_write(e, SGE_REG_INTRMASK, 0x0);
	micro_delay(2000);

	val = sge_reg_read(e, SGE_REG_INTRCONTROL) | 0x8000;
	sge_reg_write(e, SGE_REG_INTRCONTROL, val);
	micro_delay(50);
	sge_reg_write(e, SGE_REG_INTRCONTROL, val & ~0x8000);

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
 *                              sge_mii_read                                 *
 *===========================================================================*/
static uint16_t sge_mii_read(e, phy, reg)
sge_t *e;
uint32_t phy;
uint32_t reg;
{
	u32_t read_cmd;
	u32_t data;

	phy = (phy & 0x1f) << 6;
	reg = (reg & 0x1f) << 11;
	read_cmd = SGE_MII_REQ | SGE_MII_READ | phy | reg;

	sge_reg_write(e, SGE_REG_GMIICONTROL, read_cmd);
	micro_delay(50);

	do
	{
		data = sge_reg_read(e, SGE_REG_GMIICONTROL);
		micro_delay(50);
	} while ((data & SGE_MII_REQ) != 0);

	return (u16_t)((data & SGE_MII_DATA) >> SGE_MII_DATA_SHIFT);
}

/*===========================================================================*
 *                             sge_mii_write                                 *
 *===========================================================================*/
static void sge_mii_write(e, phy, reg, data)
sge_t *e;
uint32_t phy;
uint32_t reg;
uint32_t data;
{
	u32_t write_cmd;

	phy = (phy & 0x1f) << 6;
	reg = (reg & 0x1f) << 11;
	data = (data & 0xffff) << SGE_MII_DATA_SHIFT;
	write_cmd = SGE_MII_REQ | SGE_MII_WRITE | phy | reg | data;

	sge_reg_write(e, SGE_REG_GMIICONTROL, write_cmd);
	micro_delay(500);

	do
	{
		data = sge_reg_read(e, SGE_REG_GMIICONTROL);
		micro_delay(50);
	} while ((data & SGE_MII_REQ) != 0);
}

/*===========================================================================*
 *                              read_eeprom                                  *
 *===========================================================================*/
static uint16_t read_eeprom(e, reg)
sge_t *e;
int reg;
{
	u32_t data;
	u32_t read_cmd;

	/* Request EEPROM read. */
	read_cmd = SGE_EEPROM_REQ | SGE_EEPROM_READ |
		(reg << SGE_EEPROM_OFFSET_SHIFT);
	sge_reg_write(e, SGE_REG_EEPROMINTERFACE, read_cmd);

	/* Wait 500ms */
	micro_delay(500);

	/* Wait until ready. */
	do
	{
		data = sge_reg_read(e, SGE_REG_EEPROMINTERFACE);
		micro_delay(100);
	} while ((data & SGE_EEPROM_REQ) != 0);

	return (u16_t)((data & SGE_EEPROM_DATA) >> SGE_EEPROM_DATA_SHIFT);
}

/*===========================================================================*
 *                             sge_mii_probe                                 *
 *===========================================================================*/
static int sge_mii_probe(e)
sge_t *e;
{
	int timeout = 10000;
	int autoneg_done = 0;
	struct mii_phy *phy;
	u32_t addr;
	u16_t status;
	u16_t link_status = SGE_MIISTATUS_LINK;

	/* Search for PHY */
	for (addr = 0; addr < 32; addr++)
	{
		status = sge_mii_read(e, addr, SGE_MIIADDR_STATUS);
		status = sge_mii_read(e, addr, SGE_MIIADDR_STATUS);

		if (status == 0xffff || status == 0)
			continue;

		phy = alloc_contig(sizeof(struct mii_phy), 0, NULL);
		phy->id0 = sge_mii_read(e, addr, SGE_MIIADDR_PHY_ID0);
		phy->id1 = sge_mii_read(e, addr, SGE_MIIADDR_PHY_ID1);		
		phy->addr = addr;
		phy->status = status;
		phy->types = 0x2;
		phy->next = e->mii;
		e->mii = phy;
		e->first_mii = phy;
	}

	if (e->mii == NULL)
	{
		printf("%s: No transceiver found!\n", e->name);
		return 0;
	}

	e->mii = NULL;

	sge_default_phy(e);

	status = sge_reset_phy(e, e->cur_phy);

	if(status & SGE_MIISTATUS_LINK)
	{
		int i;
		for (i = 0; i < timeout; i++)
		{
			if (!link_status)
				break;

			micro_delay(1000);

			link_status = link_status ^ (sge_mii_read(e, e->cur_phy,
				SGE_MIIADDR_STATUS) & link_status);
		}

		if (i == timeout)
		{
			printf("%s: reset phy and link down now\n", e->name);
			return -ETIME;
		}
	}

	status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_STATUS);
	if (status & SGE_MIISTATUS_LINK)
	{
		for(int i = 0; i < 1000; i++)
		{
			status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_STATUS);
			if(status & SGE_MIISTATUS_AUTO_DONE)
			{
				autoneg_done = 1;
				break;
			}
			micro_delay(100);
		}
	}

	if(autoneg_done)
	{
		sge_phymode(e);
		sge_macmode(e);
	}

	if (e->mii->status & SGE_MIISTATUS_LINK)
	{
		if(e->RGMII)
		{
			sge_reg_write(e, SGE_REG_RGMIIDELAY, 0x0441);
			sge_reg_write(e, SGE_REG_RGMIIDELAY, 0x0440);
		}
	}

	return 1;
}

/*===========================================================================*
 *                            sge_default_phy                                *
 *===========================================================================*/
static uint16_t sge_default_phy(e)
sge_t *e;
{
	struct mii_phy *phy = NULL;
	struct mii_phy *default_phy = NULL;
	u16_t status;

	for(phy = e->first_mii; phy; phy = phy->next)
	{
		status = sge_mii_read(e, phy->addr, SGE_MIIADDR_STATUS);
		status = sge_mii_read(e, phy->addr, SGE_MIIADDR_STATUS);
		
		if ((status & SGE_MIISTATUS_LINK) && !default_phy && (phy->types != 0))
		{
			default_phy = phy;
		}
		else
		{
			status = sge_mii_read(e, phy->addr, SGE_MIIADDR_CONTROL);
			sge_mii_write(e, phy->addr, SGE_MIIADDR_CONTROL,
				status | SGE_MIICTRL_AUTO | SGE_MIICTRL_ISOLATE);
			if (phy->types == 0x02)
				default_phy = phy;
		 }
	}

	if (!default_phy)
		default_phy = e->first_mii;

	if(e->mii != default_phy )
	{
		e->mii = default_phy;
		e->cur_phy = default_phy->addr;
	}

	status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_CONTROL);
	status = status & ~SGE_MIICTRL_ISOLATE;

	sge_mii_write(e, e->cur_phy, SGE_MIIADDR_CONTROL, status);
	status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_STATUS);
	status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_STATUS);

	return status;
}

/*===========================================================================*
 *                             sge_reset_phy                                 *
 *===========================================================================*/
static uint16_t sge_reset_phy(e, addr)
sge_t *e;
uint32_t addr;
{
	int i = 0;
	u16_t status;

	status = sge_mii_read(e, addr, SGE_MIIADDR_STATUS);
	status = sge_mii_read(e, addr, SGE_MIIADDR_STATUS);

	sge_mii_write(e, addr, SGE_MIIADDR_CONTROL,
		(SGE_MIICTRL_RESET | SGE_MIICTRL_AUTO | SGE_MIICTRL_RST_AUTO));

	return status;
}

/*===========================================================================*
 *                              sge_phymode                                 *
 *===========================================================================*/
static void sge_phymode(e)
sge_t *e;
{
	u32_t status;
	u16_t anadv;
	u16_t anrec;
	u16_t anexp;
	u16_t gadv;
	u16_t grec;
	status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_STATUS);
	status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_STATUS);

	if (!(status & SGE_MIISTATUS_LINK))
		return;

	anadv = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_AUTO_ADV);
	anrec = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_AUTO_LPAR);
	anexp = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_AUTO_EXT);

	e->link_speed = SGE_SPEED_10;
	e->duplex_mode = SGE_DUPLEX_OFF;

	if((e->model == SGE_DEV_0191) && (anrec & SGE_MIIAUTON_NP)
		&& (anexp & 0x2))
	{
		gadv = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_AUTO_GADV);
		grec = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_AUTO_GLPAR);
		status = (gadv & (grec >> 2));
		if(status & 0x200)
		{
			e->link_speed = SGE_SPEED_1000;
			e->duplex_mode = SGE_DUPLEX_ON;
		}
		else if (status & 0x100)
		{
			e->link_speed = SGE_SPEED_1000;
			e->duplex_mode = SGE_DUPLEX_OFF;
		}
	}
	else
	{
		status = anadv & anrec;

		if (status & (SGE_MIIAUTON_TX | SGE_MIIAUTON_TX_FULL))
			e->link_speed = SGE_SPEED_100;
		if (status & (SGE_MIIAUTON_TX_FULL | SGE_MIIAUTON_T_FULL))
			e->duplex_mode = SGE_DUPLEX_ON;
	}

	e->autoneg_done = 1;
}

/*===========================================================================*
 *                              sge_macmode                                  *
 *===========================================================================*/
static void sge_macmode(e)
sge_t *e;
{
	u32_t status;

	status = sge_reg_read(e, SGE_REG_STATIONCONTROL);
	status = status & ~(0x0f000000 | SGE_REGSC_FDX | SGE_REGSC_SPEED_MASK);

	switch (e->link_speed)
	{
		case SGE_SPEED_1000:
			status |= (SGE_REGSC_SPEED_1000 | (0x3 << 24) | (0x1 << 26));
			break;
		case SGE_SPEED_100:
			status |= (SGE_REGSC_SPEED_100 | (0x1 << 26));
			break;
		case SGE_SPEED_10:
			status |= (SGE_REGSC_SPEED_10 | (0x1 << 26));
			break;
		default:
			printf("%s: Unsupported link speed.\n", e->name);
	}

	if (e->duplex_mode)
	{
		status = status | SGE_REGSC_FDX;
	}

	if(e->RGMII)
	{
		status = status | (0x3 << 24);
	}

	sge_reg_write(e, SGE_REG_STATIONCONTROL, status);
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
	if (!(e->status & SGE_READING || e->status & SGE_WRITING))
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

	/* MAC Address */
	printf("%s: Ethernet Address %x:%x:%x:%x:%x:%x\n", e->name,
		e->address.ea_addr[0], e->address.ea_addr[1],
		e->address.ea_addr[2], e->address.ea_addr[3],
		e->address.ea_addr[4], e->address.ea_addr[5]);

	/* Link speed */
	printf("%s: Media Link On %d Mbps %s-duplex \n",
		e->name,
		e->link_speed,
		e->duplex_mode ? "full" : "half");

	/* PHY Transceiver */       
	printf("%s: PHY Transceiver (%0x/%0x) found at address %d\n", e->name,
		e->mii->id0, (e->mii->id1 & 0xFFF0), e->mii->addr);

	/* Mac Registers (Memory Mapped)*/
	printf("MAC Registers:\n");
	for(i = 0; i < 0x80; i+=4)
	{
		if((i%16) == 0)
			printf("%2.2xh: ", (char)i);

		printf("%8.8x ", sge_reg_read(e, i));

		if((i%16) == 12)
			printf("\n");
	}
	printf("\n");

	/* EEPROM */
	printf("EEPROM Dump:\n");
	for(i = 0; i < 0x10; i+=1)
	{
		if(i%0x8 == 0)
			printf("%2.2xh: ", (char)i);

		printf("%4.4x ", read_eeprom(e, i));

		if(i == 0x7)
			printf("\n");
	}
	printf("\n");

	/* EEPROM */
	printf("PHY Registers:\n");
	for(i = 0; i < 0x20; i+=1)
	{
		if((i%8) == 0 )
			printf("%2.2xh: ", (char)i);

		printf("%4.4x ", sge_mii_read(e, e->cur_phy, i));

		if((i%8)==7)
			printf("\n");
	}
	printf("\n");
}
