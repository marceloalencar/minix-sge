/* sge.h
 *
 * SiS 190/191 Fast Ethernet Controller driver
 * 
 * Parts of this code are based on the FreeBSD implementation.
 * (https://svnweb.freebsd.org/base/head/sys/dev/sge/), and
 * e1000 driver from Niek Linnenbank.
 *
 * Created: May 2017 by Marcelo Alencar <marceloalves@ufpa.br>
 */
 
#ifndef _SGE_H_
#define _SGE_H_

#include <net/gen/ether.h> 
#include <net/gen/eth_io.h> 

/* MAC Override */
#define SGE_ENVVAR		"SGEETH"

/* Device IDs */
#define SGE_DEV_0190	0x0190 /* SiS 190 PCI Fast Ethernet Adapter */
#define SGE_DEV_0191	0x0191 /* SiS 191 PCI Gigabit Ethernet Adapter */

/* Ethernet driver statuses */
#define SGE_DETECTED		(1 << 0)
#define SGE_ENABLED		(1 << 1)
#define SGE_READING		(1 << 2)
#define SGE_WRITING		(1 << 3)
#define SGE_RECEIVED		(1 << 4)
#define SGE_TRANSMIT		(1 << 5)

/* Ethernet driver modes */
#define SGE_PROMISC		(1 << 0)
#define SGE_MULTICAST		(1 << 1)
#define SGE_BROADCAST		(1 << 2)

/* Speed/Duplex */
#define SGE_SPEED_10		10
#define SGE_SPEED_100		100
#define SGE_SPEED_1000		1000
#define SGE_DUPLEX_ON		1
#define SGE_DUPLEX_OFF		0

/* Buffer info */
#define SGE_IOVEC_NR		16
#define SGE_BUF_SIZE		2048
#define SGE_RXDESC_NR		32
#define SGE_TXDESC_NR		32
#define SGE_RX_TOTALSIZE		SGE_RXDESC_NR*sizeof(sge_desc_t)
#define SGE_TX_TOTALSIZE		SGE_TXDESC_NR*sizeof(sge_desc_t)
#define SGE_DESC_FINAL		0x80000000

/* Register Addresses */
#define	SGE_REG_TX_CTL			0x00 /* Tx Host Control/status Register */
#define	SGE_REG_TX_DESC			0x04 /* Tx Home Descriptor Base Register */
#define	SGE_REG_RESERVED0		0x08 /* Reserved */
#define	SGE_REG_TX_NEXT			0x0c /* Tx Next Descriptor Control/Status Register */

#define	SGE_REG_RX_CTL			0x10 /* Rx Host Control/status Register */
#define	SGE_REG_RX_DESC			0x14 /* Rx Home Descriptor Base Register */
#define	SGE_REG_RESERVED1		0x18 /* Reserved */
#define	SGE_REG_RX_NEXT			0x1c /* Rx Next Descriptor Control/Status Register */

#define	SGE_REG_INTRSTATUS		0x20 /* Interrupt Source Register */
#define	SGE_REG_INTRMASK		0x24 /* Interrupt Mask Register */
#define	SGE_REG_INTRCONTROL		0x28 /* Interrupt Control Register */
#define	SGE_REG_INTRTIMER		0x2c /* Interupt Timer Register */

#define	SGE_REG_PMCONTROL		0x30 /* Power Management Control/Status Register */
#define	SGE_REG_RESERVED2		0x34 /* Reserved */
#define	SGE_REG_EEPROMCONTROL		0x38 /* EEPROM Control/Status Register */
#define	SGE_REG_EEPROMINTERFACE		0x3c /* EEPROM Interface Register */
#define	SGE_REG_STATIONCONTROL		0x40 /* Station Control/Status Register */
#define	SGE_REG_GMIICONTROL		0x44 /* Station Management Interface Register */
#define	SGE_REG_GMACIOCR		0x48 /* GMAC IO Compensation Register */
#define	SGE_REG_GMACIOCTL		0x4c /* GMAC IO Control Register */
#define	SGE_REG_TXMACCONTROL		0x50 /* Tx MAC Control Register */
#define	SGE_REG_TXMACTIMELIMIT		0x54 /* Tx MAC Timer/TryLimit Register */
#define	SGE_REG_RGMIIDELAY		0x58 /* RGMII Tx Internal Delay Control Register */
#define	SGE_REG_RESERVED3		0x5c /* Reserved */
#define	SGE_REG_RXMACCONTROL		0x60 /* Rx MAC Control Register */
#define	SGE_REG_RXMACADDR		0x62 /* Rx MAC Unicast Address Register */
#define	SGE_REG_RXHASHTABLE		0x68 /* Rx MAC Multicast Hash Table Register 1 */
#define	SGE_REG_RXHASHTABLE2		0x6c /* Rx MAC Multicast Hash Table Register 2 */
#define	SGE_REG_RXWAKEONLAN		0x70 /* Rx WOL Control Register */
#define	SGE_REG_RXWAKEONLANDATA		0x74 /* Rx WOL Data Access Register */
#define	SGE_REG_RXMPSCONTROL		0x78 /* Rx MPS Control Register */
#define	SGE_REG_RESERVED4		0x7c /* Reserved */

/* Registers Interface */
#define SGE_REGSC_FULL		0x0f000000
#define	SGE_REGSC_LOOPBACK		0x80000000
#define	SGE_REGSC_RGMII		0x00008000
#define	SGE_REGSC_FDX			0x00001000
#define	SGE_REGSC_SPEED_MASK		0x00000c00
#define	SGE_REGSC_SPEED_10		0x00000400
#define	SGE_REGSC_SPEED_100		0x00000800
#define	SGE_REGSC_SPEED_1000		0x00000c00

#define SGE_RXCTRL_BCAST		0x0800
#define	SGE_RXCTRL_MCAST		0x0400
#define	SGE_RXCTRL_MYPHYS		0x0200
#define	SGE_RXCTRL_ALLPHYS		0x0100

#define SGE_TXSTATUS_TXOWN		0x80000000
#define SGE_TXSTATUS_TXINT		0x40000000
#define SGE_TXSTATUS_THOL3		0x30000000
#define SGE_TXSTATUS_THOL2		0x20000000
#define SGE_TXSTATUS_THOL1		0x10000000
#define SGE_TXSTATUS_THOL0		0x00000000
#define SGE_TXSTATUS_LSEN		0x08000000
#define SGE_TXSTATUS_IPCS		0x04000000
#define SGE_TXSTATUS_TCPCS		0x02000000
#define SGE_TXSTATUS_UDPCS		0x01000000
#define SGE_TXSTATUS_BSTEN		0x00800000
#define SGE_TXSTATUS_EXTEN		0x00400000
#define SGE_TXSTATUS_DEFEN		0x00200000
#define SGE_TXSTATUS_BKFEN		0x00100000
#define SGE_TXSTATUS_CRSEN		0x00080000
#define SGE_TXSTATUS_COLSEN		0x00040000
#define SGE_TXSTATUS_CRCEN		0x00020000
#define SGE_TXSTATUS_PADEN		0x00010000

#define SGE_RXSTATUS_RXOWN		0x80000000
#define SGE_RXSTATUS_TAGON		0x80000000
#define SGE_RXSTATUS_RXINT		0x40000000
#define SGE_RXSTATUS_CRCOK		0x00010000
#define SGE_RXSTATUS_COLON		0x00020000
#define SGE_RXSTATUS_NIBON		0x00040000
#define SGE_RXSTATUS_MIIER		0x00080000
#define SGE_RXSTATUS_LIMIT		0x00200000
#define SGE_RXSTATUS_SHORT		0x00400000
#define SGE_RXSTATUS_ABORT		0x00800000

/* Interrupts */
#define	SGE_INTR_SOFT		0x40000000
#define	SGE_INTR_TIMER		0x20000000
#define	SGE_INTR_PAUSE_FRAME	0x00080000
#define	SGE_INTR_MAGIC_FRAME	0x00040000
#define	SGE_INTR_WAKE_FRAME		0x00020000
#define	SGE_INTR_LINK		0x00010000
#define	SGE_INTR_RX_IDLE		0x00000080
#define	SGE_INTR_RX_DONE		0x00000040
#define	SGE_INTR_TXQ1_IDLE		0x00000020
#define	SGE_INTR_TXQ1_DONE		0x00000010
#define	SGE_INTR_TX_IDLE		0x00000008
#define	SGE_INTR_TX_DONE		0x00000004
#define	SGE_INTR_RX_HALT		0x00000002
#define	SGE_INTR_TX_HALT		0x00000001
#define	SGE_INTRS							\
	(SGE_INTR_RX_IDLE | SGE_INTR_RX_DONE | SGE_INTR_TXQ1_IDLE |			\
	 SGE_INTR_TXQ1_DONE |SGE_INTR_TX_IDLE | SGE_INTR_TX_DONE |			\
	 SGE_INTR_TX_HALT | SGE_INTR_RX_HALT)

/* EEPROM Addresses */
#define	SGE_EEPADDR_SIG		0x00 /* EEPROM Signature */
#define	SGE_EEPADDR_CLK		0x01 /* EEPROM Clock */
#define	SGE_EEPADDR_INFO		0x02 /* EEPROM Info */
#define	SGE_EEPADDR_MAC		0x03 /* EEPROM MAC Address */

/* EEPROM Interface */
#define SGE_EEPROM_DATA		0xffff0000
#define SGE_EEPROM_DATA_SHIFT		16
#define SGE_EEPROM_OFFSET_SHIFT		10
#define SGE_EEPROM_READ		0x00000200
#define SGE_EEPROM_WRITE		0x00000100
#define SGE_EEPROM_REQ		0x00000080
#define SGE_EEPROM_DO		0x00000008
#define SGE_EEPROM_DI		0x00000004
#define SGE_EEPROM_CLK		0x00000002
#define SGE_EEPROM_CS		0x00000001

/* MII Addresses */
#define SGE_MIIADDR_CONTROL		0x00
#define SGE_MIIADDR_STATUS		0x01
#define SGE_MIIADDR_PHY_ID0		0x02
#define SGE_MIIADDR_PHY_ID1		0x03
#define SGE_MIIADDR_AUTO_ADV		0x04
#define SGE_MIIADDR_AUTO_LPAR		0x05
#define SGE_MIIADDR_AUTO_EXT		0x06
#define SGE_MIIADDR_AUTO_GADV		0x09
#define SGE_MIIADDR_AUTO_GLPAR		0x0a

/* MII Interface */
#define SGE_MIISTATUS_LINK		0x0004
#define SGE_MIISTATUS_AUTO_DONE		0x0020
#define SGE_MIISTATUS_CAN_TX		0x2000
#define SGE_MIISTATUS_CAN_TX_FDX		0x4000

#define SGE_MIICTRL_RST_AUTO		0x0200
#define SGE_MIICTRL_ISOLATE		0x0400
#define SGE_MIICTRL_AUTO		0x1000
#define SGE_MIICTRL_RESET		0x8000

#define SGE_MII_DATA		0xffff0000
#define SGE_MII_DATA_SHIFT		16
#define SGE_MII_REQ		0x00000010
#define SGE_MII_READ		0x00000000
#define SGE_MII_WRITE		0x00000020

#define SGE_MIIAUTON_NP		0x8000
#define SGE_MIIAUTON_TX		0x0080
#define SGE_MIIAUTON_TX_FULL		0x0100
#define SGE_MIIAUTON_T_FULL		0x0040

/* TX/RX Descriptor */
typedef struct sge_desc
{
	uint32_t pkt_size;
	uint32_t status;
	uint32_t buf_ptr;
	uint32_t flags;
} sge_desc_t;

typedef struct sge
{
	int irq;
	int irq_hook;
	int model;
	u8_t *regs;

	struct mii_phy *mii;
	struct mii_phy *first_mii;
	uint32_t cur_phy;

	int link_speed;
	int duplex_mode;
	int autoneg_done;

	sge_desc_t *rx_desc;
	phys_bytes rx_desc_p;
	int rx_desc_count;
	char *rx_buffer;
	phys_bytes rx_buffer_p;
	int rx_buffer_size;
	uint32_t cur_rx;

	sge_desc_t *tx_desc;
	phys_bytes tx_desc_p;
	int tx_desc_count;
	char *tx_buffer;
	phys_bytes tx_buffer_p;
	int tx_buffer_size;
	uint32_t cur_tx;

	int RGMII;
	int MAC_APC;
} sge_t;

struct mii_phy
{
	struct mii_phy *next;
	int addr;
	uint16_t id0;
	uint16_t id1;
	uint16_t status;
	uint16_t types;
};

#endif /* !_SGE_H_ */
