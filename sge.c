/* sge.c
 *
 * SiS 190/191 Fast Ethernet Controller driver
 * 
 * Parts of this code are based on the FreeBSD implementation.
 * (https://svnweb.freebsd.org/base/head/sys/dev/sge/)
 *
 * Created: May 2017 by Marcelo Alencar <marceloalves@ufpa.br>
 */
 
#include <minix/drivers.h>
#include <minix/netdriver.h>

#include <stdio.h>
#include <stdlib.h>
#include "sge.h"

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
	
//	while (TRUE)
//	{
//		if ((r= netdriver_receive(ANY, &m, &ipc_status)) != OK)
//		{
//			panic("netdriver_receive failed: %d", r);
//		}
//		
//		if (is_ipc_notify(ipc_status))
//		{
//			switch (_ENDPOINT_P(m.m_source))
//			{
//			case HARDWARE:
//				sge_interrupt(&m);
//				break;
//			case CLOCK:
//				break;
//			}
//			continue;
//		}
//		switch (m.m_type)
//		{
//		case DL_CONF:		sge_conf(&m);				break;
//		case DL_GETSTAT_S:	sge_getstat(&m);			break;
//		case DL_WRITEV_S:	sge_writev(&m, FALSE);		break;
//		case DL_READV_S:	sge_readv(&m, FALSE);		break;
//	    default:
//			panic("illegal message: %d", m.m_type);
//		}
//	}
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
	
	switch(type)
	{
	case SEF_INIT_FRESH:
		printf("%s", STARTUP_MESSAGE);
		break;
	}
	
	/* Announce we are up! */
    netdriver_announce();

    return(OK);
}
