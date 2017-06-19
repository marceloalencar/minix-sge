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

static void sge_init(unsigned int instance, netdriver_addr_t *addr,
	uint32_t *caps, unsigned int *ticks);
static void sge_stop(void);
static void sge_set_mode(unsigned int, const netdriver_addr_t *,
	unsigned int);
static void sge_set_hwaddr(const netdriver_addr_t *);
static ssize_t sge_recv(struct netdriver_data *data, size_t max);
static int sge_send(struct netdriver_data *data, size_t size);
static unsigned int sge_get_link(uint32_t *);
static void sge_intr(unsigned int mask);
static void sge_tick(void);

static int sge_probe(sge_t *e, int skip);
static void sge_init_hw(sge_t *e, netdriver_addr_t *addr);

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
static void sge_init(unsigned int instance, netdriver_addr_t *addr,
	uint32_t *caps, unsigned int *ticks)
{
	sge_t *e;
	int r;

	sge_instance = instance;

	/* Clear state */
	memset(&sge_state, 0, sizeof(sge_state));
	
	e = &sge_state;
	
	/* Perform calibration. */
	if ((r = tsc_calibrate()) != OK)
		panic("tsc_calibrate failed: %d", r);

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
 *                               sge_stop                                    *
 *===========================================================================*/
static void sge_stop(void)
{

}
/*===========================================================================*
 *                             sge_set_mode                                  *
 *===========================================================================*/
static void sge_set_mode(unsigned int, const netdriver_addr_t *,
	unsigned int)
{

}

/*===========================================================================*
 *                            sge_set_hwaddr                                 *
 *===========================================================================*/
static void sge_set_hwaddr(const netdriver_addr_t *)
{

}

/*===========================================================================*
 *                               sge_recv                                    *
 *===========================================================================*/
static ssize_t sge_recv(struct netdriver_data *data, size_t max)
{

}

/*===========================================================================*
 *                               sge_send                                    *
 *===========================================================================*/
static int sge_send(struct netdriver_data *data, size_t size)
{

}

/*===========================================================================*
 *                             sge_get_link                                  *
 *===========================================================================*/
static unsigned int sge_get_link(uint32_t *)
{

}

/*===========================================================================*
 *                               sge_intr                                    *
 *===========================================================================*/
static void sge_intr(unsigned int mask)
{

}

/*===========================================================================*
 *                               sge_tick                                    *
 *===========================================================================*/
static void sge_tick(void)
{

}

/*===========================================================================*
 *                              sge_probe                                    *
 *===========================================================================*/
static int sge_probe(sge_t *e, int skip)
{

}

/*===========================================================================*
 *                             sge_init_hw                                   *
 *===========================================================================*/
static void sge_init_hw(sge_t *e, netdriver_addr_t *addr)
{

}
