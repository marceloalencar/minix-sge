/* sge.c
 *
 * SiS 190/191 Ethernet Controller driver
 * 
 * Parts of this code are based on the FreeBSD implementation
 * (https://svnweb.freebsd.org/base/head/sys/dev/sge/), the
 * e1000 driver by Niek Linnenbank, and the official SiS 190/191
 * GNU/Linux driver by K.M. Liu.
 *
 * Created: May 2017 by Marcelo Alencar <marceloalves@ufpa.br>
 */
 
#include <minix/drivers.h>
#include <minix/netdriver.h>
#include <machine/pci.h>
#include <sys/mman.h>

#include "sge.h"

static int sge_init(unsigned int instance, netdriver_addr_t *addr,
	uint32_t *caps, unsigned int *ticks);
static void sge_stop(void);
static void sge_set_mode(unsigned int mode,
	const netdriver_addr_t *mcast_list, unsigned int mcast_count);
static void sge_set_hwaddr(const netdriver_addr_t *hwaddr);
static ssize_t sge_recv(struct netdriver_data *data, size_t max);
static int sge_send(struct netdriver_data *data, size_t size);
static unsigned int sge_get_link(uint32_t *media);
static void sge_intr(unsigned int mask);
static void sge_tick(void);

static int sge_probe(sge_t *e, int skip);
static void sge_init_hw(sge_t *e, netdriver_addr_t *addr);
static void sge_reset_hw(sge_t *e);
static uint32_t sge_reg_read(sge_t *e, uint32_t reg);
static void sge_reg_write(sge_t *e, uint32_t reg, uint32_t value);
static void sge_init_addr(sge_t *e, netdriver_addr_t *addr);
static uint16_t read_eeprom(sge_t *e, int reg);
static void sge_init_buf(sge_t *e);
static int sge_mii_probe(sge_t *e);
static uint16_t sge_default_phy(sge_t *e);
static uint16_t sge_reset_phy(sge_t *e, uint32_t addr);
static void sge_phymode(sge_t *e);
static void sge_macmode(sge_t *e);
static uint16_t sge_mii_read(sge_t *e, uint32_t phy, uint32_t reg);
static void sge_mii_write(sge_t *e, uint32_t phy, uint32_t reg, uint32_t data);

static int sge_instance;
static sge_t sge_state;

static const struct netdriver sge_table = {
	.ndr_name	= "sge",
	.ndr_init	= sge_init,
	.ndr_stop	= sge_stop,
	.ndr_set_mode	= sge_set_mode,
	.ndr_set_hwaddr	= sge_set_hwaddr,
	.ndr_recv	= sge_recv,
	.ndr_send	= sge_send,
	.ndr_get_link	= sge_get_link,
	.ndr_intr	= sge_intr,
	.ndr_tick	= sge_tick
};

/*===========================================================================*
 *                                    main                                   *
 *===========================================================================*/
int
main(int argc, char *argv[])
{
	/* This is the main driver task. */
	env_setargs(argc, argv);

	/* Function call table for netdriver. (As in netdriver.h) */
	netdriver_task(&sge_table);

	return 0;
}

/*===========================================================================*
 *                               sge_init                                    *
 *===========================================================================*/
static int
sge_init(unsigned int instance, netdriver_addr_t *addr, uint32_t *caps,
	unsigned int *ticks)
{
	/* Create, initialize status struct */
	
	sge_t *e;
	int r;

	sge_instance = instance;

	/* Clear state */
	memset(&sge_state, 0, sizeof(sge_state));
	
	e = &sge_state;
	
	/* Perform calibration. */
	if ((r = tsc_calibrate()) != OK)
		panic("tsc_calibrate failed: %d\n", r);

	/* Try to find a matching device. */
	if (!sge_probe(e, instance))
		return ENXIO;

	/* Initialize the hardware, and return its ethernet address. */
	sge_init_hw(e, addr);

	*caps = NDEV_CAP_MCAST | NDEV_CAP_BCAST | NDEV_CAP_HWADDR;
	*ticks = sys_hz() / 10; /* update statistics 10x/sec */

	return OK;
}

/*===========================================================================*
 *                              sge_probe                                    *
 *===========================================================================*/
static int
sge_probe(sge_t *e, int skip)
{
	int r, devind, ioflag;
	u16_t vid, did, cr;
	u32_t status;
	u32_t base, size;

	/* Initialize communication to the PCI driver. */
	pci_init();

	/*
	 * Attempt to iterate the PCI bus. Start at the beginning.
	 */
	if ((r = pci_first_dev(&devind, &vid, &did)) == 0)
		return FALSE;

	/* Loop devices on the PCI bus. */
	while (skip--)
	{
		if (!(r = pci_next_dev(&devind, &vid, &did)))
			return FALSE;
	}

	/* Set card specific properties. */
	switch (did)
	{
	case SGE_DEV_0190:
		e->model = SGE_DEV_0190;
		break;
	case SGE_DEV_0191:
		e->model = SGE_DEV_0191;
		break;
	default:
		break;
	}

	/* Reserve PCI resources found. */
	pci_reserve(devind);
	
	/* Read PCI configuration. */
	e->irq = pci_attr_r8(devind, PCI_ILR);

	if ((r = pci_get_bar(devind, PCI_BAR, &base, &size, &ioflag)) != OK)
		panic("failed to get PCI BAR (%d)\n", r);
	if (ioflag)
		panic("PCI BAR is not for memory\n");

	e->regs = vm_map_phys(SELF, (void *) base, size);
	if (e->regs == MAP_FAILED)
		panic("failed to map hardware registers from PCI\n");

	/* Enable bus mastering, if necessary */
	cr = pci_attr_r16(devind, PCI_CR);
	if (!(cr & PCI_CR_MAST_EN))
		pci_attr_w16(devind, PCI_CR, cr | PCI_CR_MAST_EN);

	/* Where to read MAC from?          *
	 * isAPC = 0: From EEPROM           *
	 * isAPC = 1: From APC CMOS RAM     */
	int isAPC = pci_attr_r8(devind, 0x73);
	e->MAC_APC = ((isAPC & 0x1) == 0) ? 0 : 1;

	return TRUE;
}

/*===========================================================================*
 *                             sge_init_hw                                   *
 *===========================================================================*/
static void
sge_init_hw(sge_t *e, netdriver_addr_t *addr)
{
	int r;
	uint32_t control;
	
	e->irq_hook = e->irq;
	
	/*
	 * Set the interrupt handler and policy. Do not automatically
	 * re-enable interrupts. Return the IRQ line number on interrupts.
	 */
	if ((r = sys_irqsetpolicy(e->irq, 0, &e->irq_hook)) != OK)
		panic("sys_irqsetpolicy failed: %d\n", r);
	if ((r = sys_irqenable(&e->irq_hook)) != OK)
		panic("sys_irqenable failed: %d\n", r);
		
	/* Reset hardware. */
	sge_reset_hw(e);
	
	/* Initialization routine */
	sge_init_addr(e, addr);
	sge_init_buf(e);
	sge_mii_probe(e);
	
	/* Enable interrupts */
	sge_reg_write(e, SGE_REG_INTRMASK, SGE_INTRS);
	
	/* Enable TX/RX */
	control = sge_reg_read(e, SGE_REG_TX_CTL);
	sge_reg_write(e, SGE_REG_TX_CTL, control | 0x1);
	control = sge_reg_read(e, SGE_REG_RX_CTL);
	sge_reg_write(e, SGE_REG_RX_CTL, control | 0x1 | 0x10);
}

/*===========================================================================*
 *                             sge_reset_hw                                  *
 *===========================================================================*/
static void
sge_reset_hw(sge_t *e)
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
		sge_reg_write(e, SGE_REG_STATIONCONTROL, 0x04008001);
	else
		sge_reg_write(e, SGE_REG_STATIONCONTROL, 0x04000001);
	
	sge_reg_write(e, SGE_REG_GMACIOCR, 0x0);
	sge_reg_write(e, SGE_REG_GMACIOCTL, 0x0);

	sge_reg_write(e, SGE_REG_TXMACCONTROL, 0x00002364);
	sge_reg_write(e, SGE_REG_TXMACTIMELIMIT, 0x0000000f);

	sge_reg_write(e, SGE_REG_RGMIIDELAY, 0x0);
	sge_reg_write(e, SGE_REG_RESERVED3, 0x0);
	sge_reg_write(e, SGE_REG_RXMACCONTROL, 0x12);

	sge_reg_write(e, SGE_REG_RXHASHTABLE, 0x0);
	sge_reg_write(e, SGE_REG_RXHASHTABLE2, 0x0);

	sge_reg_write(e, SGE_REG_RXWAKEONLAN, 0x80ff0000);
	sge_reg_write(e, SGE_REG_RXWAKEONLANDATA, 0x80ff0000);
	sge_reg_write(e, SGE_REG_RXMPSCONTROL, 0x0);
	sge_reg_write(e, SGE_REG_RESERVED4, 0x0);
}

/*===========================================================================*
 *                             sge_init_addr                                 *
 *===========================================================================*/
static void
sge_init_addr(sge_t *e, netdriver_addr_t *addr)
{
	static char eakey[] = SGE_ENVVAR "#_EA";
	static char eafmt[] = "x:x:x:x:x:x";
	u16_t val;
	u16_t rgmii;
	int i;
	long v;
	
	/* Do we have a user defined MAC address? */
	eakey[sizeof(SGE_ENVVAR)-1] = '0' + sge_instance;

	for (i = 0; i < 6; i++)
	{
		if (env_parse(eakey, eafmt, i, &v, 0x00L, 0xFFL) != EP_SET)
			break;
		else
			addr->na_addr[i] = v;
	}
	
	/* Nothing was read or not everything was read? */
	if (i != 6)
	{
		/* Read from APC */
		if (e->MAC_APC)
		{
			/*
			 * Can't read from PCI to ISA bridge right now...
			 */
			panic("Read MAC from APC not implemented...\n");
		}
		/* Read MAC from EEPROM. */
		else
		{
			for (i = 0; i < 3; i++)
			{
				val = read_eeprom(e, SGE_EEPADDR_MAC + i);
				addr->na_addr[(i * 2)] = (val & 0xff);
				addr->na_addr[(i * 2) + 1] = (val & 0xff00) >> 8;
			}
			rgmii = read_eeprom(e, SGE_EEPADDR_INFO);
			e->RGMII = ((rgmii & 0x80) != 0) ? 1 : 0;
		}
	}
	
	/* Set address */
	sge_set_hwaddr(addr);
}

/*===========================================================================*
 *                            sge_set_hwaddr                                 *
 *===========================================================================*/
static void
sge_set_hwaddr(const netdriver_addr_t *hwaddr)
{
	/* Set hardware address */
	sge_t *e;
	int i;
	uint16_t filter;
	
	e = &sge_state;
	
	/* Prepare for address */
	sge_reg_write(e, SGE_REG_RXMACADDR, 0);

	/* disable packet filtering before address is set */
	filter = sge_reg_read(e, SGE_REG_RXMACCONTROL);
	filter &= ~(SGE_RXCTRL_BCAST | SGE_RXCTRL_ALLPHYS | SGE_RXCTRL_MCAST |
		SGE_RXCTRL_MYPHYS);
	sge_reg_write(e, SGE_REG_RXMACCONTROL, filter);
	
	/* Write MAC to registers */
	for (i = 0; i < 6 ; i++)
	{
		uint8_t w;

		w = (uint8_t) &hwaddr->na_addr[i];
		sge_reg_write(e, SGE_REG_RXMACADDR + i, w);
	}

	/* Enable filter */
	filter |= (SGE_RXCTRL_MYPHYS | SGE_RXCTRL_BCAST);
	sge_reg_write(e, SGE_REG_RXMACCONTROL, filter);
	sge_reg_write(e, SGE_REG_RXHASHTABLE, 0xffffffff);
	sge_reg_write(e, SGE_REG_RXHASHTABLE2, 0xffffffff);
}

/*===========================================================================*
 *                              sge_init_buf                                 *
 *===========================================================================*/
static void
sge_init_buf(sge_t *e)
{
	/* This function initializes the TX/RX rings, used for DMA transfers */
	int i, rx_align, tx_align;
	phys_bytes rx_buff_p;
	phys_bytes tx_buff_p;
	phys_bytes rx_desc_p;
	phys_bytes tx_desc_p;

	/* Number of descriptors */
	e->rx_desc_count = SGE_RXDESC_NR;
	e->tx_desc_count = SGE_TXDESC_NR;

	if (!e->rx_desc)
	{
		/* Allocate RX descriptors.    */
		/* rx_desc: Virtual address    */
		/* rx_desc_p: Physical address */
		if ((e->rx_desc = alloc_contig(SGE_RXD_TOTALSIZE + 15, AC_ALIGN4K,
			&rx_desc_p)) == NULL)
		{
			panic("%s: Failed to allocate RX descriptors.\n",
				netdriver_name());
		}
		memset(e->rx_desc, 0, SGE_RXD_TOTALSIZE + 15);

		/* Allocate RX buffers */
		if ((e->rx_buffer = alloc_contig(SGE_RXB_TOTALSIZE + 15, AC_ALIGN4K,
			&rx_buff_p)) == NULL)
		{
			panic("%s: Failed to allocate RX buffers.\n", netdriver_name());
		}
		memset(e->rx_buffer, 0, SGE_RXB_TOTALSIZE + 15);

		/* Align addresses to multiple of 16 bit */
		rx_align = ((rx_buff_p + 0xf) & ~0xf) - rx_buff_p;
		rx_buff_p = ((rx_buff_p + 0xf) & ~0xf);

		e->cur_rx = 0;

		/* Setup receive descriptors. */
		for (i = 0; i < SGE_RXDESC_NR; i++)
		{
			/* RX descriptors are initially held by hardware */
			e->rx_desc[i].pkt_size = 0;
			e->rx_desc[i].status = SGE_RXSTATUS_RXOWN | SGE_RXSTATUS_RXINT;
			e->rx_desc[i].buf_ptr = rx_buff_p + (i * SGE_BUF_SIZE);
			e->rx_desc[i].flags = (SGE_BUF_SIZE & 0xfff8);
		}
		/* Last descriptor is marked as final */
		e->rx_desc[SGE_RXDESC_NR - 1].flags |= SGE_DESC_FINAL;
	}

	if (!e->tx_desc)
	{
		/* Allocate TX descriptors.    */
		/* tx_desc: Virtual address    */
		/* tx_desc_p: Physical address */
		if ((e->tx_desc = alloc_contig(SGE_TXD_TOTALSIZE + 15, AC_ALIGN4K,
			&tx_desc_p)) == NULL)
		{
			panic("%s: Failed to allocate TX descriptors.\n",
				netdriver_name());
		}
		memset(e->tx_desc, 0, SGE_TXD_TOTALSIZE + 15);

		/* Allocate TX buffers */
		if ((e->tx_buffer = alloc_contig(SGE_TXB_TOTALSIZE + 15, AC_ALIGN4K,
			&tx_buff_p)) == NULL)
		{
			panic("%s: Failed to allocate TX buffers.\n", netdriver_name());
		}
		memset(e->tx_buffer, 0, SGE_TXB_TOTALSIZE + 15);

		/* Align addresses to multiple of 16 bit */
		tx_align = ((tx_buff_p + 0xf) & ~0xf) - tx_buff_p;
		tx_buff_p = ((tx_buff_p + 0xf) & ~0xf);

		e->cur_tx = 0;

		/* Setup receive descriptors. */
		for (i = 0; i < SGE_TXDESC_NR; i++)
		{
			/* TX descriptors will be filled by software */
			e->tx_desc[i].pkt_size = 0;
			e->tx_desc[i].status = 0;
			e->tx_desc[i].buf_ptr = 0;
			e->tx_desc[i].flags = 0;
		}
		/* Last descriptor is marked as final */
		e->tx_desc[SGE_TXDESC_NR - 1].flags = SGE_DESC_FINAL;
	}

	/* Apply alignment to virtual addresses, and store at status */
	e->tx_buffer += tx_align;
	e->rx_buffer += rx_align;
	e->tx_buffer_p = tx_buff_p;
	e->rx_buffer_p = rx_buff_p;
	e->tx_desc_p = tx_desc_p;
	e->rx_desc_p = rx_desc_p;

	/* Inform card where the buffer is */
	sge_reg_write(e, SGE_REG_TX_DESC, tx_desc_p);
	sge_reg_write(e, SGE_REG_RX_DESC, rx_desc_p);
}

/*===========================================================================*
 *                             sge_mii_probe                                 *
 *===========================================================================*/
static int
sge_mii_probe(sge_t *e)
{
	int timeout = 10000;
	int autoneg_done = 0;
	struct mii_phy *phy;
	u32_t addr;
	u16_t status;
	u16_t link_status = SGE_MIISTATUS_LINK;

	/* Search for PHY transceiver */
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
		panic("%s: No transceiver found!\n", netdriver_name());
		return -ENODEV;
	}

	e->mii = NULL;

	sge_default_phy(e);

	status = sge_reset_phy(e, e->cur_phy);

	if (status & SGE_MIISTATUS_LINK)
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
			printf("%s: PHY reset, media down.\n", netdriver_name());
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
static uint16_t
sge_default_phy(sge_t *e)
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
static uint16_t
sge_reset_phy(sge_t *e, uint32_t addr)
{
	u16_t status;

	status = sge_mii_read(e, addr, SGE_MIIADDR_STATUS);
	status = sge_mii_read(e, addr, SGE_MIIADDR_STATUS);

	sge_mii_write(e, addr, SGE_MIIADDR_CONTROL,
		(SGE_MIICTRL_RESET | SGE_MIICTRL_AUTO | SGE_MIICTRL_RST_AUTO));

	return status;
}

/*===========================================================================*
 *                              sge_phymode                                  *
 *===========================================================================*/
static void
sge_phymode(sge_t *e)
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
		if (status & 0x200)
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
static void
sge_macmode(sge_t *e)
{
	u32_t status;

	status = sge_reg_read(e, SGE_REG_STATIONCONTROL);
	status = status & ~(SGE_REGSC_FULL | SGE_REGSC_FDX | SGE_REGSC_SPEED_MASK);

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
			printf("%s: Unsupported link speed.\n", netdriver_name());
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
 *                               sge_stop                                    *
 *===========================================================================*/
static void
sge_stop(void)
{
	sge_t *e;
	uint32_t val;

	e = &sge_state;
	sge_reset_hw(e);

	sge_reg_write(e, SGE_REG_INTRMASK, 0x0);
	micro_delay(2000);

	val = sge_reg_read(e, SGE_REG_INTRCONTROL) | 0x8000;
	sge_reg_write(e, SGE_REG_INTRCONTROL, val);
	micro_delay(50);
	sge_reg_write(e, SGE_REG_INTRCONTROL, val & ~0x8000);
}

/*===========================================================================*
 *                             sge_set_mode                                  *
 *===========================================================================*/
static void
sge_set_mode(unsigned int mode, const netdriver_addr_t *mcast_list,
	unsigned int mcast_count)
{
	/* Set RX mode according to settings from netdriver */
	sge_t *e;
	e = &sge_state;
	uint16_t filter;
	
	filter = sge_reg_read(e, SGE_REG_RXMACCONTROL);
	/* Disable all filters */
	filter &= ~(SGE_RXCTRL_BCAST | SGE_RXCTRL_ALLPHYS | SGE_RXCTRL_MCAST |
		SGE_RXCTRL_MYPHYS);
	sge_reg_write(e, SGE_REG_RXMACCONTROL, filter);
	
	filter |= SGE_RXCTRL_MYPHYS;
	if (mode & NDEV_MODE_PROMISC)
	{
		filter |= (SGE_RXCTRL_BCAST | SGE_RXCTRL_MCAST | SGE_RXCTRL_ALLPHYS);
	}
	if (mode & (NDEV_MODE_MCAST_LIST | NDEV_MODE_MCAST_ALL))
	{
		filter |= (SGE_RXCTRL_BCAST | SGE_RXCTRL_MCAST);
	}
	if (mode & NDEV_MODE_BCAST)
	{
		filter |= SGE_RXCTRL_BCAST;
	}

	sge_reg_write(e, SGE_REG_RXMACCONTROL, filter);
	sge_reg_write(e, SGE_REG_RXHASHTABLE, 0xffffffff);
	sge_reg_write(e, SGE_REG_RXHASHTABLE2, 0xffffffff);
}

/*===========================================================================*
 *                               sge_recv                                    *
 *===========================================================================*/
static ssize_t
sge_recv(struct netdriver_data *data, size_t max)
{
	sge_t *e = &sge_state;
	sge_desc_t *desc;
	
	uint32_t command;
	uint32_t current;
	char *ptr;
	size_t size;

	/* Select current packet descriptor from list */
	current = e->cur_rx % SGE_RXDESC_NR;
	desc = &e->rx_desc[current];

	/* Packet still held by the card? Give up */
	if (desc->status & SGE_RXSTATUS_RXOWN)
	{
		return SUSPEND;
	}

	/* Report any error status. */
	if (!(desc->status & SGE_RXSTATUS_CRCOK) || (desc->status &
		SGE_RXSTATUS_ERRORS))
	{
		netdriver_stat_ierror(1);
		desc->pkt_size = 0;
		desc->status = SGE_RXSTATUS_RXOWN | SGE_RXSTATUS_RXINT;
		return SUSPEND;
	}

	/* Get user data size. CRC is removed by hardware. */
	size = desc->pkt_size & 0xffff;

	/* Set address of data in buffer */
	ptr = e->rx_buffer + (current * SGE_BUF_SIZE);

// ------ DEBUG: PRINT PACKET TO SCREEN --------
//	int i;
//	printf("sge_recv()= Size: %d, Max: %d\n", size, max);
//	printf("[ ");
//	for(i = 0; i < size; i++)
//	{
//		printf("%02x ", (unsigned char)*(e->rx_buffer + (current * SGE_BUF_SIZE) + i));
//	}
//	printf("]\n");
//	printf("Addr: %x\n", desc->buf_ptr);
// ------ DEBUG: PRINT PACKET TO SCREEN --------

	/* Truncate packets bigger than MTU. */
	if (size > max)
		size = max;

	/* Copy the packet to the caller. */
	netdriver_copyout(data, 0, ptr, size);

	/* Flip ownership back to the card */
	desc->pkt_size = 0;
	desc->status = SGE_RXSTATUS_RXOWN | SGE_RXSTATUS_RXINT;

	/* Update current descriptor and reenable. */
	e->cur_rx = (current + 1) % SGE_RXDESC_NR;
	command = sge_reg_read(e, SGE_REG_RX_CTL);
	sge_reg_write(e, SGE_REG_RX_CTL, 0x10 | command);

	/* Return the size of the received packet. */
	return size;
}

/*===========================================================================*
 *                               sge_send                                    *
 *===========================================================================*/
static int
sge_send(struct netdriver_data *data, size_t size)
{
	sge_t *e = &sge_state;
	sge_desc_t *desc;

	uint32_t command;
	uint32_t current;
	char *ptr;

	/* Do not receive packets while autonegotiation is running */
	if (!(e->autoneg_done))
	{
		return SUSPEND;
	}

	/* Select current packet descriptor from list */
	current = e->cur_tx % SGE_TXDESC_NR;
	desc = &e->tx_desc[current];

	if (desc->status & SGE_TXSTATUS_ERRORS)
	{
		netdriver_stat_oerror(1);
	}

	/* Packet bigger than MTU. OS should already know this */
	if (size > SGE_BUF_SIZE)
		panic("packet too large to send.\n");

	/* Set address of data in buffer */
	ptr = e->tx_buffer + (current * SGE_BUF_SIZE);

	/* Insert padding to very small packets. */
	if (size < NDEV_ETH_PACKET_MIN)
	{
		memset(ptr + size, 0, NDEV_ETH_PACKET_MIN - size);
		size = NDEV_ETH_PACKET_MIN;
	}

	/* Copy the packet from the caller. */
	netdriver_copyin(data, 0, ptr, size);

// ------ DEBUG: PRINT PACKET TO SCREEN --------
//	int i;
//	printf("sge_send()= Size: %d\n", size);
//	printf("[ ");
//	for(i = 0; i < size; i++)
//	{
//		printf("%02x ", (unsigned char)*(e->tx_buffer + (current * SGE_BUF_SIZE) + i));
//	}
//	printf("]\n");
//	printf("Addr: %x\n", desc->buf_ptr);
// ------ DEBUG: PRINT PACKET TO SCREEN --------

	/* Mark this descriptor ready. */
	desc->pkt_size = size & 0xffff;
	desc->status = (SGE_TXSTATUS_PADEN | SGE_TXSTATUS_CRCEN |
		SGE_TXSTATUS_DEFEN | SGE_TXSTATUS_THOL3 | SGE_TXSTATUS_TXINT);
	desc->buf_ptr = e->tx_buffer_p + (current * SGE_BUF_SIZE);
	desc->flags |= size & 0xffff;
	if (e->duplex_mode == SGE_DUPLEX_OFF)
	{
		desc->status |= (SGE_TXSTATUS_COLSEN | SGE_TXSTATUS_CRSEN |
			SGE_TXSTATUS_BKFEN);
		if (e->link_speed == SGE_SPEED_1000)
			desc->status |= (SGE_TXSTATUS_EXTEN | SGE_TXSTATUS_BSTEN);
	}
	desc->status |= SGE_TXSTATUS_TXOWN;

	/* Update current descriptor and reenable. */
	e->cur_tx = (current + 1) % SGE_TXDESC_NR;
	command = sge_reg_read(e, SGE_REG_TX_CTL);
	sge_reg_write(e, SGE_REG_TX_CTL, 0x10 | command);

	return OK;
}

/*===========================================================================*
 *                             sge_get_link                                  *
 *===========================================================================*/
static unsigned int
sge_get_link(uint32_t *media)
{
	/* Get media status */
	sge_t *e;
	e = &sge_state;
	
	u32_t status;
	u16_t anadv;
	u16_t anrec;
	u16_t anexp;
	u16_t gadv;
	u16_t grec;
	status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_STATUS);
	status = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_STATUS);

	if (!(status & SGE_MIISTATUS_LINK))
		return NDEV_LINK_DOWN;

	anadv = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_AUTO_ADV);
	anrec = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_AUTO_LPAR);
	anexp = sge_mii_read(e, e->cur_phy, SGE_MIIADDR_AUTO_EXT);

	e->link_speed = SGE_SPEED_10;
	e->duplex_mode = SGE_DUPLEX_OFF;
	*media = IFM_ETHER;

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
			*media |= IFM_FDX;
		}
		else if (status & 0x100)
		{
			e->link_speed = SGE_SPEED_1000;
			e->duplex_mode = SGE_DUPLEX_OFF;
			*media |= IFM_HDX;
		}
		else
		{
			e->duplex_mode = SGE_DUPLEX_OFF;
			*media |= IFM_HDX;
		}
	}
	else
	{
		status = anadv & anrec;

		if (status & (SGE_MIIAUTON_TX | SGE_MIIAUTON_TX_FULL))
		{
			e->link_speed = SGE_SPEED_100;			
		}
		if (status & (SGE_MIIAUTON_TX_FULL | SGE_MIIAUTON_T_FULL))
		{
			e->duplex_mode = SGE_DUPLEX_ON;
			*media |= IFM_FDX;
		}
		else
		{
			e->duplex_mode = SGE_DUPLEX_OFF;
			*media |= IFM_HDX;
		}
	}

	switch (e->link_speed)
	{
		case SGE_SPEED_1000:
			*media |= IFM_1000_T;
			break;
		case SGE_SPEED_100:
			*media |= IFM_100_TX;
			break;
		case SGE_SPEED_10:
			*media |= IFM_10_T;
			break;
	}
	
	e->autoneg_done = 1;
	
	return NDEV_LINK_UP;
}

/*===========================================================================*
 *                               sge_intr                                    *
 *===========================================================================*/
static void
sge_intr(unsigned int mask)
{
	sge_t *e;
	u32_t status;

	e = &sge_state;

	/* Check the card for interrupt reason(s) */
	status = sge_reg_read(e, SGE_REG_INTRSTATUS);;
	if (!(status == 0xffffffff || (status & SGE_INTRS) == 0))
	{
		//Acknowledge and disable interrupts
		sge_reg_write(e, SGE_REG_INTRSTATUS, status);
		sge_reg_write(e, SGE_REG_INTRMASK, 0);

		/* What happened?. */
		if ((status & SGE_INTRS) == 0)
		{
			/* Nothing */
			return;
		}
		sge_reg_write(e, SGE_REG_INTRSTATUS, status);
		/* Card ready to transmit new packets */
		if (status & (SGE_INTR_TX_DONE | SGE_INTR_TX_IDLE))
			netdriver_send();
		/* Card received new packets */
		if (status & (SGE_INTR_RX_DONE | SGE_INTR_RX_IDLE))
			netdriver_recv();
		/* Media status changed */
		if (status & SGE_INTR_LINK)
			netdriver_link();
	}

	/* Re-enable interrupts. */
	sge_reg_write(e, SGE_REG_INTRMASK, SGE_INTRS);
	if (sys_irqenable(&e->irq_hook) != OK)
	{
		panic("failed to re-enable IRQ\n");
	}
}

/*===========================================================================*
 *                               sge_tick                                    *
 *===========================================================================*/
static void
sge_tick(void)
{
	/* Do periodically processing */
	sge_t *e;

	e = &sge_state;

	/* Update statistics */
	netdriver_stat_ierror(0);
	netdriver_stat_oerror(0);
}

/*===========================================================================*
 *                              sge_reg_read                                 *
 *===========================================================================*/
static uint32_t
sge_reg_read(sge_t *e, uint32_t reg)
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
static void
sge_reg_write(sge_t *e, uint32_t reg, uint32_t value)
{
	/* Write to memory mapped register. */
	*(volatile u32_t *)(e->regs + reg) = value;
}

/*===========================================================================*
 *                              sge_mii_read                                 *
 *===========================================================================*/
static uint16_t
sge_mii_read(sge_t *e, uint32_t phy, uint32_t reg)
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
static void
sge_mii_write(sge_t *e, uint32_t phy, uint32_t reg, uint32_t data)
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
static uint16_t
read_eeprom(sge_t *e, int reg)
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
