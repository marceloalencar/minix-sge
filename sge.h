/* sge.h
 *
 * SiS 190/191 Fast Ethernet Controller driver
 * 
 * Parts of this code are based on the FreeBSD implementation.
 * (https://svnweb.freebsd.org/base/head/sys/dev/sge/)
 *
 * Created: May 2017 by Marcelo Alencar <marceloalves@ufpa.br>
 */
 
#ifndef _SGE_H_
#define _SGE_H_

#include <net/gen/ether.h> 
#include <net/gen/eth_io.h> 

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
#define	SGE_REG_TX_CTL			0x00
#define	SGE_REG_TX_DESC			0x04
#define	SGE_REG_RESERVED0		0x08
#define	SGE_REG_TX_NEXT			0x0c

#define	SGE_REG_RX_CTL			0x10
#define	SGE_REG_RX_DESC			0x14
#define	SGE_REG_RESERVED1		0x18
#define	SGE_REG_RX_NEXT			0x1c

#define	SGE_REG_INTRSTATUS		0x20
#define	SGE_REG_INTRMASK		0x24
#define	SGE_REG_INTRCONTROL		0x28
#define	SGE_REG_INTRTIMER		0x2c

#define	SGE_REG_PMCONTROL		0x30
#define	SGE_REG_RESERVED2		0x34
#define	SGE_REG_ROMCONTROL		0x38
#define	SGE_REG_ROMINTERFACE		0x3c
#define	SGE_REG_STATIONCONTROL		0x40
#define	SGE_REG_GMIICONTROL		0x44
#define	SGE_REG_GMACIOCR		0x48
#define	SGE_REG_GMACIOCTL		0x4c
#define	SGE_REG_TXMACCONTROL		0x50
#define	SGE_REG_TXMACTIMELIMIT		0x54
#define	SGE_REG_RGMIIDELAY		0x58
#define	SGE_REG_RESERVED3		0x5c
#define	SGE_REG_RXMACCONTROL		0x60	/* 1  WORD */
#define	SGE_REG_RXMACADDR		0x62	/* 6x BYTE */
#define	SGE_REG_RXHASHTABLE		0x68	/* 1 LONG */
#define	SGE_REG_RXHASHTABLE2		0x6c	/* 1 LONG */
#define	SGE_REG_RXWAKEONLAN		0x70
#define	SGE_REG_RXWAKEONLANDATA		0x74
#define	SGE_REG_RXMPSCONTROL		0x78
#define	SGE_REG_RESERVED4		0x7c

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
}
sge_t;

#endif /* !_SGE_H_ */
