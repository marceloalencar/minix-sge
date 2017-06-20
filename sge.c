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
//	.ndr_set_caps	= sge_set_caps,
//	.ndr_set_flags	= sge_set_flags,
//	.ndr_set_media	= sge_set_media,
	.ndr_set_hwaddr	= sge_set_hwaddr,
	.ndr_recv	= sge_recv,
	.ndr_send	= sge_send,
	.ndr_get_link	= sge_get_link,
	.ndr_intr	= sge_intr,
	.ndr_tick	= sge_tick
//,	.ndr_other	= sge_other
};

/*===========================================================================*
 *                                    main                                   *
 *===========================================================================*/
int main(int argc, char *argv[])
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
static int sge_init(unsigned int instance, netdriver_addr_t *addr,
	uint32_t *caps, unsigned int *ticks)
{
	/* Create, initialize status struct. Search for hardware */
	
	sge_t *e;
	int r;

	sge_instance = instance;

	/* Clear state */
	memset(&sge_state, 0, sizeof(sge_state));
	
	e = &sge_state;
	
	/* Perform calibration. */
	if ((r = tsc_calibrate()) != OK)
		panic("tsc_calibrate failed: %d\n", r);

	/* See if we can find a matching device. */
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
static int sge_probe(sge_t *e, int skip)
{
	int r, devind, ioflag;
	u16_t vid, did, cr;
	u32_t status;
	u32_t base, size;
	char *dname;

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

	/* Where to read MAC from? *
	 * isAPC = 0: From EEPROM  *
	 * isAPC = 1: From APC     */
	int isAPC = pci_attr_r8(devind, 0x73);
	e->MAC_APC = ((isAPC & 0x1) == 0) ? 0 : 1;

	return TRUE;
}

/*===========================================================================*
 *                             sge_init_hw                                   *
 *===========================================================================*/
static void sge_init_hw(sge_t *e, netdriver_addr_t *addr)
{
	int r, i;
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
static void sge_reset_hw(sge_t *e)
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
	sge_reg_write(e, SGE_REG_RXMACCONTROL, 0x00000252);

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
static void sge_init_addr(sge_t *e, netdriver_addr_t *addr)
{
	static char eakey[] = SGE_ENVVAR "#_EA";
	static char eafmt[] = "x:x:x:x:x:x";
	u16_t val;
	u16_t rgmii;
	int i;
	long v;
	
	/* Do we have a user defined ethernet address? */
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
			 * ISA Bridge read not implemented.
			 */
			panic("Read MAC from APC not implemented...\n");
		}
		/* Read from EEPROM. */
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
static void sge_set_hwaddr(const netdriver_addr_t *hwaddr)
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
static void sge_init_buf(sge_t *e)
{
	/* This function initializes the TX/RX rings, used for DMA transfers */
	int i;
	phys_bytes rx_buff_p;
	phys_bytes tx_buff_p;
	phys_bytes rx_desc_p;
	phys_bytes tx_desc_p;

	/* Number of descriptors */
	e->rx_desc_count = SGE_RXDESC_NR;
	e->tx_desc_count = SGE_TXDESC_NR;

	if (!e->rx_desc)
	{
		/* Allocate RX descriptors. */
		/* rx_desc is the RX ring space. rx_desc_p is a pointer to it. */
		if ((e->rx_desc = alloc_contig(SGE_RX_TOTALSIZE + 15, AC_ALIGN4K,
			&rx_desc_p)) == NULL)
		{
			panic("%s: Failed to allocate RX descriptors.\n", netdriver_name());
		}
		memset(e->rx_desc, 0, SGE_RX_TOTALSIZE + 15);

		/* Allocate RX buffers */
		e->rx_buffer_size = SGE_RXDESC_NR * SGE_BUF_SIZE;

		if ((e->rx_buffer = alloc_contig(e->rx_buffer_size,
			AC_ALIGN4K, &rx_buff_p)) == NULL)
		{
			panic("%s: Failed to allocate RX buffers.\n", netdriver_name());
		}

		e->cur_rx = 0;

		/* Setup receive descriptors. */
		for (i = 0; i < SGE_RXDESC_NR; i++)
		{
			e->rx_desc[i].pkt_size = 0;
			/* RX descriptors are initially held by hardware */
			e->rx_desc[i].status = SGE_RXSTATUS_RXOWN | SGE_RXSTATUS_RXINT;
			e->rx_desc[i].buf_ptr = rx_buff_p + (i * SGE_BUF_SIZE);
			e->rx_desc[i].flags = (SGE_BUF_SIZE & 0xfff8);
		}
		/* Last descriptor is marked as final */
		e->rx_desc[SGE_RXDESC_NR - 1].flags |= SGE_DESC_FINAL;
	}

	if (!e->tx_desc)
	{
		/* Allocate TX descriptors. */
		/* tx_desc is the TX ring space. tx_desc_p is a pointer to it. */
		if ((e->tx_desc = alloc_contig(SGE_TX_TOTALSIZE + 15, AC_ALIGN4K,
			&tx_desc_p)) == NULL)
		{
			panic("%s: Failed to allocate TX descriptors.\n", netdriver_name());
		}
		memset(e->tx_desc, 0, SGE_TX_TOTALSIZE + 15);

		/* Allocate TX buffers */
		e->tx_buffer_size = SGE_TXDESC_NR * SGE_BUF_SIZE;

		if ((e->tx_buffer = alloc_contig(e->tx_buffer_size,
			AC_ALIGN4K, &tx_buff_p)) == NULL)
		{
			panic("%s: Failed to allocate TX buffers.\n", netdriver_name());
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
static int sge_mii_probe(sge_t *e)
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
		panic("sge: No transceiver found!\n");
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
			printf("sge: reset phy and link down now\n");
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
static uint16_t sge_default_phy(sge_t *e)
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
static uint16_t sge_reset_phy(sge_t *e, uint32_t addr)
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
 *                              sge_phymode                                  *
 *===========================================================================*/
/* Might be "obsoleted" */
static void sge_phymode(sge_t *e)
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
static void sge_macmode(sge_t *e)
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
			printf("sge: Unsupported link speed.\n");
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
static void sge_stop(void)
{
	sge_t *e;
	uint32_t val;
	
	sge_reset_hw(e);

	sge_reg_write(e, SGE_REG_INTRMASK, 0x0);
	micro_delay(2000);

	val = sge_reg_read(e, SGE_REG_INTRCONTROL) | 0x8000;
	sge_reg_write(e, SGE_REG_INTRCONTROL, val);
	micro_delay(50);
	sge_reg_write(e, SGE_REG_INTRCONTROL, val & ~0x8000);
	
	free_contig(e->rx_desc, SGE_RX_TOTALSIZE + 15);
	free_contig(e->rx_buffer, e->rx_buffer_size);
	free_contig(e->tx_desc, SGE_TX_TOTALSIZE + 15);
	free_contig(e->tx_buffer, e->tx_buffer_size);
	free_contig(e->mii, sizeof(struct mii_phy));
}

/*===========================================================================*
 *                             sge_set_mode                                  *
 *===========================================================================*/
static void sge_set_mode(unsigned int mode,
	const netdriver_addr_t *mcast_list, unsigned int mcast_count)
{
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
static ssize_t sge_recv(struct netdriver_data *data, size_t max)
{
	sge_t *e = &sge_state;
	sge_desc_t *desc;
	
	int r, i, bytes = 0;
	uint32_t command;
	uint32_t current;
	uint32_t status;
	uint32_t pkt_size;
	char *ptr;
	size_t size;

	printf("sge_recv()\n");

	/* Search for packet not owned by the card. */
	for (int i = 0; i < SGE_RXDESC_NR; i++)
	{
		current = (e->cur_rx + i) % SGE_RXDESC_NR;
		desc = &e->rx_desc[current];
		if (!(desc->status & SGE_RXSTATUS_RXOWN))
		{
			pkt_size = desc->pkt_size & 0xffff;
			break;
		}
	}
	/* Give up if none found. */
	if (desc->status & SGE_RXSTATUS_RXOWN)
	{
		return SUSPEND;
	}

	/* Copy the packet to the caller. */
	ptr = e->rx_buffer + (current * SGE_BUF_SIZE);
	
	if (size > max)
		size = max;
		
	netdriver_copyout(data, 0, ptr, size);

	/* Flip ownership back to the card */
	desc->pkt_size = 0;
	desc->status = SGE_RXSTATUS_RXOWN | SGE_RXSTATUS_RXINT;

	/* Update current and reenable. */
	e->cur_rx = (current + 1) % SGE_RXDESC_NR;
	command = sge_reg_read(e, SGE_REG_RX_CTL);
	sge_reg_write(e, SGE_REG_RX_CTL, 0x10 | command);
	
	/* Return the size of the received packet. */
	return pkt_size;
}

/*===========================================================================*
 *                               sge_send                                    *
 *===========================================================================*/
static int sge_send(struct netdriver_data *data, size_t size)
{
	sge_t *e = &sge_state;
	sge_desc_t *desc;
	int r, i, bytes = 0;
	uint32_t command;
	uint32_t current;
	uint32_t status;
	char *ptr;

	printf("sge_send()\n");

	if (!(e->autoneg_done))
	{
		return SUSPEND;
	}

	current = e->cur_tx % SGE_TXDESC_NR;
	desc = &e->tx_desc[current];

	if (size > SGE_BUF_SIZE)
		panic("packet too large to send.\n");

	/* Copy the packet from the caller. */
	ptr = e->tx_buffer + (current * SGE_BUF_SIZE);

	netdriver_copyin(data, 0, ptr, size);

	/* Mark this descriptor ready. */
	desc->pkt_size = size & 0xffff;
	desc->status = (SGE_TXSTATUS_PADEN | SGE_TXSTATUS_CRCEN |
		SGE_TXSTATUS_DEFEN | SGE_TXSTATUS_THOL3 | SGE_TXSTATUS_TXINT);
	desc->buf_ptr = e->tx_buffer_p + (current * SGE_BUF_SIZE);
	desc->flags |= size & 0xffff;
	if (e->duplex_mode == 0)
	{
		desc->status |= (SGE_TXSTATUS_COLSEN | SGE_TXSTATUS_CRSEN |
			SGE_TXSTATUS_BKFEN);
		if (e->link_speed == SGE_SPEED_1000)
			desc->status |= (SGE_TXSTATUS_EXTEN | SGE_TXSTATUS_BSTEN);
	}
	desc->status |= SGE_TXSTATUS_TXOWN;

	/* Move to next descriptor. */
	current = (current + 1) % SGE_TXDESC_NR;
	e->cur_tx = current;
	
	/* Start transmission. */
	command = sge_reg_read(e, SGE_REG_TX_CTL);
	sge_reg_write(e, SGE_REG_TX_CTL, 0x10 | command);

	return OK;
}

/*===========================================================================*
 *                             sge_get_link                                  *
 *===========================================================================*/
static unsigned int sge_get_link(uint32_t *media)
{
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
static void sge_intr(unsigned int mask)
{
	sge_t *e;
	u32_t status;

	e = &sge_state;

	/*
	 * Check the card for interrupt reason(s).
	 */
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
			netdriver_send();
		if (status & (SGE_INTR_RX_DONE | SGE_INTR_RX_IDLE))
			netdriver_recv();
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
static void sge_tick(void)
{
	/* Do periodically processing */
	sge_t *e;
	
	e = &sge_state;
	
	/* Update statistics */
	netdriver_stat_ierror(0);
	netdriver_stat_coll(0);
}

/*===========================================================================*
 *                              sge_reg_read                                 *
 *===========================================================================*/
static uint32_t sge_reg_read(sge_t *e, uint32_t reg)
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
static void sge_reg_write(sge_t *e, uint32_t reg, uint32_t value)
{
	/* Write to memory mapped register. */
	*(volatile u32_t *)(e->regs + reg) = value;
}

/*===========================================================================*
 *                              sge_mii_read                                 *
 *===========================================================================*/
static uint16_t sge_mii_read(sge_t *e, uint32_t phy, uint32_t reg)
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
static void sge_mii_write(sge_t *e, uint32_t phy, uint32_t reg, uint32_t data)
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
static uint16_t read_eeprom(sge_t *e, int reg)
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
