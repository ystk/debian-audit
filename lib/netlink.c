/* netlink.c --
 * Copyright 2004, 2005, 2009 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *      Steve Grubb <sgrubb@redhat.com>
 *      Rickard E. (Rik) Faith <faith@redhat.com>
 */

#include "config.h"
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/poll.h>
#include "libaudit.h"
#include "private.h"

#ifndef NETLINK_AUDIT
#define NETLINK_AUDIT 9
#endif

static int adjust_reply(struct audit_reply *rep, int len);
static int check_ack(int fd, int seq);

/*
 * This function opens a connection to the kernel's audit
 * subsystem. You must be root for the call to succeed. On error,
 * a negative value is returned. On success, the file descriptor is
 * returned - which can be 0 or higher.
 */
int audit_open(void)
{
	int saved_errno;
	int fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_AUDIT);

	if (fd < 0) {
		saved_errno = errno;
		if (errno == EINVAL || errno == EPROTONOSUPPORT ||
				errno == EAFNOSUPPORT)
			audit_msg(LOG_ERR,
				"Error - audit support not in kernel"); 
		else
			audit_msg(LOG_ERR,
				"Error opening audit netlink socket (%s)", 
				strerror(errno));
		errno = saved_errno;
		return fd;
	}
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
		saved_errno = errno;
		close(fd);
		audit_msg(LOG_ERR, 
			"Error setting audit netlink socket CLOEXEC flag (%s)", 
			strerror(errno));
		errno = saved_errno;
		return -1;
	}
	return fd;
}


void audit_close(int fd)
{
	if (fd >= 0)
		close(fd);
}


/*
 * This function returns -1 on error, 0 if error response received,
 * and > 0 if packet OK.
 */
int audit_get_reply(int fd, struct audit_reply *rep, reply_t block, int peek)
{
	int len;
	struct sockaddr_nl nladdr;
	socklen_t nladdrlen = sizeof(nladdr);

	if (fd < 0)
		return -EBADF;

	if (block == GET_REPLY_NONBLOCKING)
		block = MSG_DONTWAIT;

retry:
	len = recvfrom(fd, &rep->msg, sizeof(rep->msg), block|peek,
		(struct sockaddr*)&nladdr, &nladdrlen);

	if (len < 0) {
		if (errno == EINTR)
			goto retry;
		if (errno != EAGAIN) {
			int saved_errno = errno;
			audit_msg(LOG_ERR, 
				"Error receiving audit netlink packet (%s)", 
				strerror(errno));
			errno = saved_errno;
		}
		return -errno;
	}
	if (nladdrlen != sizeof(nladdr)) {
		audit_msg(LOG_ERR, 
			"Bad address size reading audit netlink socket");
		return -EPROTO;
	}
	if (nladdr.nl_pid) {
		audit_msg(LOG_ERR, 
			"Spoofed packet received on audit netlink socket");
		return -EINVAL;
	}

	len = adjust_reply(rep, len);
	if (len == 0)
		len = -errno;
	return len; 
}
hidden_def(audit_get_reply)


/* 
 * This function returns 0 on error and len on success.
 */
static int adjust_reply(struct audit_reply *rep, int len)
{
	rep->type     = rep->msg.nlh.nlmsg_type;
	rep->len      = rep->msg.nlh.nlmsg_len;
	rep->nlh      = &rep->msg.nlh;
	rep->status   = NULL;
	rep->rule     = NULL;
	rep->login    = NULL;
	rep->message  = NULL;
	rep->error    = NULL;
	rep->signal_info = NULL;
	rep->conf     = NULL;
	if (!NLMSG_OK(rep->nlh, (unsigned int)len)) {
		if (len == sizeof(rep->msg)) {
			audit_msg(LOG_ERR, 
				"Netlink event from kernel is too big");
			errno = EFBIG;
		} else {
			audit_msg(LOG_ERR, 
			"Netlink message from kernel was not OK");
			errno = EBADE;
		}
		return 0;
	}

	/* Next we'll set the data structure to point to msg.data. This is
	 * to avoid having to use casts later. */
	switch (rep->type) {
		case NLMSG_ERROR: 
			rep->error   = NLMSG_DATA(rep->nlh); 
			break;
		case AUDIT_GET:   
			rep->status  = NLMSG_DATA(rep->nlh); 
			break;
		case AUDIT_LIST:  
			rep->rule    = NLMSG_DATA(rep->nlh); 
			break;
		case AUDIT_LIST_RULES:  
			rep->ruledata = NLMSG_DATA(rep->nlh); 
			break;
		case AUDIT_USER:
		case AUDIT_LOGIN:
		case AUDIT_KERNEL:
		case AUDIT_FIRST_USER_MSG...AUDIT_LAST_USER_MSG:
		case AUDIT_FIRST_USER_MSG2...AUDIT_LAST_USER_MSG2:
		case AUDIT_FIRST_EVENT...AUDIT_LAST_KERN_ANOM_MSG:
			rep->message = NLMSG_DATA(rep->nlh); 
			break;
		case AUDIT_SIGNAL_INFO:
			rep->signal_info = NLMSG_DATA(rep->nlh);
			break;
	}
	return len;
}


/*
 *  Return values:   success: positive non-zero sequence number
 *                   error:   -errno
 *                   short:   0
 */
int audit_send(int fd, int type, const void *data, unsigned int size)
{
	static int sequence = 0;
	struct audit_message req;
	int retval;
	struct sockaddr_nl addr;

	/* Due to user space library callbacks, there's a chance that
	   a -1 for the fd could be passed. Just check for and handle it. */
	if (fd < 0) {
		errno = EBADF;
		return -errno;
	}

	if (NLMSG_SPACE(size) > MAX_AUDIT_MESSAGE_LENGTH) {
		errno = EINVAL;
		return -errno;
	}

	if (++sequence < 0) 
		sequence = 1;

	memset(&req, 0, sizeof(req));
	req.nlh.nlmsg_len = NLMSG_SPACE(size);
	req.nlh.nlmsg_type = type;
	req.nlh.nlmsg_flags = NLM_F_REQUEST|NLM_F_ACK;
	req.nlh.nlmsg_seq = sequence;
	if (size && data)
		memcpy(NLMSG_DATA(&req.nlh), data, size);
	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = 0;
	addr.nl_groups = 0;

	do {
		retval = sendto(fd, &req, req.nlh.nlmsg_len, 0,
			(struct sockaddr*)&addr, sizeof(addr));
	} while (retval < 0 && errno == EINTR);
	if (retval == (int)req.nlh.nlmsg_len) {
		if ((retval = check_ack(fd, sequence)) == 0)
			return sequence;
		else
			return retval; 
	}
	if (retval < 0) 
		return -errno;

	return 0;
}

/*
 * This function will take a peek into the next packet and see if there's
 * an error. If so, the error is returned and its non-zero. Otherwise a 
 * zero is returned indicating that we don't know of any problems.
 */
static int check_ack(int fd, int seq)
{
	int rc, retries = 80;
	struct audit_reply rep;
	struct pollfd pfd[1];

retry:
	pfd[0].fd = fd;
	pfd[0].events = POLLIN;
	do {
		rc = poll(pfd, 1, 500); /* .5 second */
	} while (rc < 0 && errno == EINTR);

	/* We don't look at rc from above as it doesn't matter. We are 
	 * going to try to read nonblocking just in case packet shows up. */
	
	/* NOTE: whatever is returned is treated as the errno */
	rc = audit_get_reply(fd, &rep, GET_REPLY_NONBLOCKING, MSG_PEEK);
	if (rc == -EAGAIN && retries) {
		retries--;
		goto retry;
	} else if (rc < 0)
		return rc;
	else if (rc == 0)
		return -EINVAL; /* This can't happen anymore */
	else if (rc > 0 && rep.type == NLMSG_ERROR) {
		/* Eat the message */
		struct audit_reply rep2; /* throw away */
		(void)audit_get_reply(fd, &rep2, GET_REPLY_NONBLOCKING, 0);

		/* NLMSG_ERROR can indicate success, only report nonzero */ 
		if (rep.error->error) {
			errno = -rep.error->error;
			return rep.error->error;
		}
	}
	return 0;
}

