# Makefile for the SiS 190/191 Ethernet Controller driver.
PROG=	sge
SRCS=	sge.c

FILES=$(PROG).conf
FILESNAME=$(PROG)
FILESDIR= /etc/system.conf.d

DPADD+=	${LIBNETDRIVER} ${LIBSYS}
LDADD+=	-lnetdriver -lsys

.include <minix.service.mk>
