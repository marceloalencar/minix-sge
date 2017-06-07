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

/* Ethernet driver states */
#define SGE_DETECTED	(1 << 0)
#define SGE_ENABLED	(1 << 1)
#define SGE_READING	(1 << 2)
#define SGE_WRITING	(1 << 3)
#define SGE_RECEIVED	(1 << 4)
#define SGE_TRANSMIT	(1 << 5)

/* Registers */
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

/* EEPROM Addresses */
#define	SGE_EEPADDR_SIG		0x00 /* EEPROM Signature */
#define	SGE_EEPADDR_CLK		0x01 /* EEPROM Clock */
#define	SGE_EEPADDR_INFO		0x02 /* EEPROM Info */
#define	SGE_EEPADDR_MAC		0x03 /* EEPROM MAC Address */

/* EEPROM Interface */
#define SGE_EEPROM_DATA		0xffff0000
#define SGE_EEPROM_DATA_SHIFT		16
#define SGE_EEPROM_OFFSET_SHIFT		10
#define	SGE_EEPROM_OPER			0x00000300
#define SGE_EEPROM_OPER_SHIFT		8
#define SGE_EEPROM_OPER_READ		(2 << SGE_EEPROM_SHIFT)
#define SGE_EEPROM_OPER_WRITE		(1 << SGE_EEPROM_SHIFT)
#define SGE_EEPROM_REQ		0x00000080
#define SGE_EEPROM_DO		0x00000008
#define SGE_EEPROM_DI		0x00000004
#define SGE_EEPROM_CLK		0x00000002
#define SGE_EEPROM_CS		0x00000001

typedef struct sge
{
	char name[8];
	int status;
	int irq;
	int irq_hook;
	int revision;
	u8_t *regs;
	ether_addr_t address;
	
	int client;
	size_t rx_size;
	
	int RGMII;
}
sge_t;

#endif /* !_SGE_H_ */
