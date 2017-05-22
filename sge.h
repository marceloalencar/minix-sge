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

#define SGE_DEV_0190	0x0190
#define SGE_DEV_0191	0x0191

#define SGE_DISABLED	0x0
#define SGE_DETECTED	0x1
#define SGE_ENABLED		0x2

typedef struct sge
{
	char name[8];
	int status;
	int irq;
	int irq_hook;
	u8_t *regs;
	ether_addr_t address;
}
sge_t;

#endif /* !_SGE_H_ */
