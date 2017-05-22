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

static int sge_instance;

static void sge_init(message *mp);
static void sge_interrupt(message *mp);
static void sge_writev_s(message *mp, int from_int);
static void sge_readv_s(message *mp, int from_int);
static void sge_getstat_s(message *mp);

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
			}
			continue;
		}
		switch (m.m_type)
		{
		case DL_CONF:		sge_init(&m);				break;
		case DL_GETSTAT_S:	sge_getstat_s(&m);			break;
		case DL_WRITEV_S:	sge_writev_s(&m, FALSE);	break;
		case DL_READV_S:	sge_readv_s(&m, FALSE);		break;
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
	long v;
	int r;
	
	v = 0;
	(void)env_parse("instance", "d", 0, &v, 0, 255);
	sge_instance = (int) v;
	
	/* Announce we are up! */
    netdriver_announce();

    return(OK);
}

/*===========================================================================*
 *                        sef_cb_signal_handler                              *
 *===========================================================================*/
static void sef_cb_signal_handler(int signo)
{
	
}

/*===========================================================================*
 *                               sge_init                                    *
 *===========================================================================*/
static void sge_init(message *mp)
{
	
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
