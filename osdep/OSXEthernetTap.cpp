/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2016  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <signal.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/cdefs.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <sys/sysctl.h>
#include <netinet6/in6_var.h>
#include <netinet/in_var.h>
#include <netinet/icmp6.h>

// OSX compile fix... in6_var defines this in a struct which namespaces it for C++ ... why?!?
struct prf_ra {
	u_char onlink : 1;
	u_char autonomous : 1;
	u_char reserved : 6;
} prf_ra;

#include <netinet6/nd6.h>
#include <ifaddrs.h>

// These are KERNEL_PRIVATE... why?
#ifndef SIOCAUTOCONF_START
#define SIOCAUTOCONF_START _IOWR('i', 132, struct in6_ifreq)    /* accept rtadvd on this interface */
#endif
#ifndef SIOCAUTOCONF_STOP
#define SIOCAUTOCONF_STOP _IOWR('i', 133, struct in6_ifreq)    /* stop accepting rtadv for this interface */
#endif

// --------------------------------------------------------------------------
// --------------------------------------------------------------------------
// This source is from:
// http://www.opensource.apple.com/source/Libinfo/Libinfo-406.17/gen.subproj/getifmaddrs.c?txt
// It's here because OSX 10.6 does not have this convenience function.

#define	SALIGN	(sizeof(uint32_t) - 1)
#define	SA_RLEN(sa)	((sa)->sa_len ? (((sa)->sa_len + SALIGN) & ~SALIGN) : \
(SALIGN + 1))
#define	MAX_SYSCTL_TRY	5
#define	RTA_MASKS	(RTA_GATEWAY | RTA_IFP | RTA_IFA)

/* FreeBSD uses NET_RT_IFMALIST and RTM_NEWMADDR from <sys/socket.h> */
/* We can use NET_RT_IFLIST2 and RTM_NEWMADDR2 on Darwin */
//#define DARWIN_COMPAT

//#ifdef DARWIN_COMPAT
#define GIM_SYSCTL_MIB NET_RT_IFLIST2
#define GIM_RTM_ADDR RTM_NEWMADDR2
//#else
//#define GIM_SYSCTL_MIB NET_RT_IFMALIST
//#define GIM_RTM_ADDR RTM_NEWMADDR
//#endif

// Not in 10.6 includes so use our own
struct _intl_ifmaddrs {
	struct _intl_ifmaddrs *ifma_next;
	struct sockaddr *ifma_name;
	struct sockaddr *ifma_addr;
	struct sockaddr *ifma_lladdr;
};

static inline int _intl_getifmaddrs(struct _intl_ifmaddrs **pif)
{
	int icnt = 1;
	int dcnt = 0;
	int ntry = 0;
	size_t len;
	size_t needed;
	int mib[6];
	int i;
	char *buf;
	char *data;
	char *next;
	char *p;
	struct ifma_msghdr2 *ifmam;
	struct _intl_ifmaddrs *ifa, *ift;
	struct rt_msghdr *rtm;
	struct sockaddr *sa;

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;             /* protocol */
	mib[3] = 0;             /* wildcard address family */
	mib[4] = GIM_SYSCTL_MIB;
	mib[5] = 0;             /* no flags */
	do {
		if (sysctl(mib, 6, NULL, &needed, NULL, 0) < 0)
			return (-1);
		if ((buf = (char *)malloc(needed)) == NULL)
			return (-1);
		if (sysctl(mib, 6, buf, &needed, NULL, 0) < 0) {
			if (errno != ENOMEM || ++ntry >= MAX_SYSCTL_TRY) {
				free(buf);
				return (-1);
			}
			free(buf);
			buf = NULL;
		}
	} while (buf == NULL);

	for (next = buf; next < buf + needed; next += rtm->rtm_msglen) {
		rtm = (struct rt_msghdr *)(void *)next;
		if (rtm->rtm_version != RTM_VERSION)
			continue;
		switch (rtm->rtm_type) {
			case GIM_RTM_ADDR:
				ifmam = (struct ifma_msghdr2 *)(void *)rtm;
				if ((ifmam->ifmam_addrs & RTA_IFA) == 0)
					break;
				icnt++;
				p = (char *)(ifmam + 1);
				for (i = 0; i < RTAX_MAX; i++) {
					if ((RTA_MASKS & ifmam->ifmam_addrs &
						 (1 << i)) == 0)
						continue;
					sa = (struct sockaddr *)(void *)p;
					len = SA_RLEN(sa);
					dcnt += len;
					p += len;
				}
				break;
		}
	}

	data = (char *)malloc(sizeof(struct _intl_ifmaddrs) * icnt + dcnt);
	if (data == NULL) {
		free(buf);
		return (-1);
	}

	ifa = (struct _intl_ifmaddrs *)(void *)data;
	data += sizeof(struct _intl_ifmaddrs) * icnt;

	memset(ifa, 0, sizeof(struct _intl_ifmaddrs) * icnt);
	ift = ifa;

	for (next = buf; next < buf + needed; next += rtm->rtm_msglen) {
		rtm = (struct rt_msghdr *)(void *)next;
		if (rtm->rtm_version != RTM_VERSION)
			continue;

		switch (rtm->rtm_type) {
			case GIM_RTM_ADDR:
				ifmam = (struct ifma_msghdr2 *)(void *)rtm;
				if ((ifmam->ifmam_addrs & RTA_IFA) == 0)
					break;

				p = (char *)(ifmam + 1);
				for (i = 0; i < RTAX_MAX; i++) {
					if ((RTA_MASKS & ifmam->ifmam_addrs &
						 (1 << i)) == 0)
						continue;
					sa = (struct sockaddr *)(void *)p;
					len = SA_RLEN(sa);
					switch (i) {
						case RTAX_GATEWAY:
							ift->ifma_lladdr =
							(struct sockaddr *)(void *)data;
							memcpy(data, p, len);
							data += len;
							break;

						case RTAX_IFP:
							ift->ifma_name =
							(struct sockaddr *)(void *)data;
							memcpy(data, p, len);
							data += len;
							break;

						case RTAX_IFA:
							ift->ifma_addr =
							(struct sockaddr *)(void *)data;
							memcpy(data, p, len);
							data += len;
							break;

						default:
							data += len;
							break;
					}
					p += len;
				}
				ift->ifma_next = ift + 1;
				ift = ift->ifma_next;
				break;
		}
	}

	free(buf);

	if (ift > ifa) {
		ift--;
		ift->ifma_next = NULL;
		*pif = ifa;
	} else {
		*pif = NULL;
		free(ifa);
	}
	return (0);
}

static inline void _intl_freeifmaddrs(struct _intl_ifmaddrs *ifmp)
{
	free(ifmp);
}

// --------------------------------------------------------------------------
// --------------------------------------------------------------------------

#include <string>
#include <map>
#include <set>
#include <algorithm>

#include "../node/Constants.hpp"
#include "../node/Utils.hpp"
#include "../node/Mutex.hpp"
#include "../node/Dictionary.hpp"
#include "OSUtils.hpp"
#include "OSXEthernetTap.hpp"

// ff:ff:ff:ff:ff:ff with no ADI
static const ZeroTier::MulticastGroup _blindWildcardMulticastGroup(ZeroTier::MAC(0xff),0);

static inline bool _setIpv6Stuff(const char *ifname,bool performNUD,bool acceptRouterAdverts)
{
	struct in6_ndireq nd;
	struct in6_ifreq ifr;

	int s = socket(AF_INET6,SOCK_DGRAM,0);
	if (s <= 0)
		return false;

	memset(&nd,0,sizeof(nd));
	strncpy(nd.ifname,ifname,sizeof(nd.ifname));

	if (ioctl(s,SIOCGIFINFO_IN6,&nd)) {
		close(s);
		return false;
	}

	unsigned long oldFlags = (unsigned long)nd.ndi.flags;

	if (performNUD)
		nd.ndi.flags |= ND6_IFF_PERFORMNUD;
	else nd.ndi.flags &= ~ND6_IFF_PERFORMNUD;

	if (oldFlags != (unsigned long)nd.ndi.flags) {
		if (ioctl(s,SIOCSIFINFO_FLAGS,&nd)) {
			close(s);
			return false;
		}
	}

	memset(&ifr,0,sizeof(ifr));
	strncpy(ifr.ifr_name,ifname,sizeof(ifr.ifr_name));
	if (ioctl(s,acceptRouterAdverts ? SIOCAUTOCONF_START : SIOCAUTOCONF_STOP,&ifr)) {
		close(s);
		return false;
	}

	close(s);
	return true;
}

namespace ZeroTier {

static long globalTapsRunning = 0;
static Mutex globalTapCreateLock;

OSXEthernetTap::OSXEthernetTap(
	const char *homePath,
	const MAC &mac,
	unsigned int mtu,
	unsigned int metric,
	uint64_t nwid,
	const char *friendlyName,
	void (*handler)(void *,uint64_t,const MAC &,const MAC &,unsigned int,unsigned int,const void *data,unsigned int len),
	void *arg) :
	_handler(handler),
	_arg(arg),
	_nwid(nwid),
	_homePath(homePath),
	_mtu(mtu),
	_metric(metric),
	_fd(0),
	_enabled(true)
{
	char devpath[64],ethaddr[64],mtustr[32],metstr[32],nwids[32];
	struct stat stattmp;

	Utils::snprintf(nwids,sizeof(nwids),"%.16llx",nwid);

	if (mtu > 2800)
		throw std::runtime_error("max tap MTU is 2800");

	Mutex::Lock _gl(globalTapCreateLock);

	if (::stat("/dev/zt0",&stattmp)) {
		long kextpid = (long)vfork();
		if (kextpid == 0) {
			::chdir(homePath);
			OSUtils::redirectUnixOutputs("/dev/null",(const char *)0);
			::execl("/sbin/kextload","/sbin/kextload","-q","-repository",homePath,"tap.kext",(const char *)0);
			::_exit(-1);
		} else if (kextpid > 0) {
			int exitcode = -1;
			::waitpid(kextpid,&exitcode,0);
		}
		::usleep(500); // give tap device driver time to start up and try again
		if (::stat("/dev/zt0",&stattmp))
			throw std::runtime_error("/dev/zt# tap devices do not exist and cannot load tap.kext");
	}

	// Try to reopen the last device we had, if we had one and it's still unused.
	bool recalledDevice = false;
	std::string devmapbuf;
	Dictionary<8194> devmap;
	if (OSUtils::readFile((_homePath + ZT_PATH_SEPARATOR_S + "devicemap").c_str(),devmapbuf)) {
		devmap.load(devmapbuf.c_str());
		char desiredDevice[128];
		if (devmap.get(nwids,desiredDevice,sizeof(desiredDevice)) > 0) {
			Utils::snprintf(devpath,sizeof(devpath),"/dev/%s",desiredDevice);
			if (stat(devpath,&stattmp) == 0) {
				_fd = ::open(devpath,O_RDWR);
				if (_fd > 0) {
					_dev = desiredDevice;
					recalledDevice = true;
				}
			}
		}
	}

	// Open the first unused tap device if we didn't recall a previous one.
	if (!recalledDevice) {
		for(int i=0;i<64;++i) {
			Utils::snprintf(devpath,sizeof(devpath),"/dev/zt%d",i);
			if (stat(devpath,&stattmp))
				throw std::runtime_error("no more TAP devices available");
			_fd = ::open(devpath,O_RDWR);
			if (_fd > 0) {
				char foo[16];
				Utils::snprintf(foo,sizeof(foo),"zt%d",i);
				_dev = foo;
				break;
			}
		}
	}

	if (_fd <= 0)
		throw std::runtime_error("unable to open TAP device or no more devices available");

	if (fcntl(_fd,F_SETFL,fcntl(_fd,F_GETFL) & ~O_NONBLOCK) == -1) {
		::close(_fd);
		throw std::runtime_error("unable to set flags on file descriptor for TAP device");
	}

	// Configure MAC address and MTU, bring interface up
	Utils::snprintf(ethaddr,sizeof(ethaddr),"%.2x:%.2x:%.2x:%.2x:%.2x:%.2x",(int)mac[0],(int)mac[1],(int)mac[2],(int)mac[3],(int)mac[4],(int)mac[5]);
	Utils::snprintf(mtustr,sizeof(mtustr),"%u",_mtu);
	Utils::snprintf(metstr,sizeof(metstr),"%u",_metric);
	long cpid = (long)vfork();
	if (cpid == 0) {
		::execl("/sbin/ifconfig","/sbin/ifconfig",_dev.c_str(),"lladdr",ethaddr,"mtu",mtustr,"metric",metstr,"up",(const char *)0);
		::_exit(-1);
	} else if (cpid > 0) {
		int exitcode = -1;
		::waitpid(cpid,&exitcode,0);
		if (exitcode) {
			::close(_fd);
			throw std::runtime_error("ifconfig failure setting link-layer address and activating tap interface");
		}
	}

	_setIpv6Stuff(_dev.c_str(),true,false);

	// Set close-on-exec so that devices cannot persist if we fork/exec for update
	fcntl(_fd,F_SETFD,fcntl(_fd,F_GETFD) | FD_CLOEXEC);

	::pipe(_shutdownSignalPipe);

	++globalTapsRunning;

	devmap.erase(nwids);
	devmap.add(nwids,_dev.c_str());
	OSUtils::writeFile((_homePath + ZT_PATH_SEPARATOR_S + "devicemap").c_str(),(const void *)devmap.data(),devmap.sizeBytes());

	_thread = Thread::start(this);
}

OSXEthernetTap::~OSXEthernetTap()
{
	::write(_shutdownSignalPipe[1],"\0",1); // causes thread to exit
	Thread::join(_thread);

	::close(_fd);
	::close(_shutdownSignalPipe[0]);
	::close(_shutdownSignalPipe[1]);

	{
		Mutex::Lock _gl(globalTapCreateLock);
		if (--globalTapsRunning <= 0) {
			globalTapsRunning = 0; // sanity check -- should not be possible

			char tmp[16384];
			sprintf(tmp,"%s/%s",_homePath.c_str(),"tap.kext");
			long kextpid = (long)vfork();
			if (kextpid == 0) {
				OSUtils::redirectUnixOutputs("/dev/null",(const char *)0);
				::execl("/sbin/kextunload","/sbin/kextunload",tmp,(const char *)0);
				::_exit(-1);
			} else if (kextpid > 0) {
				int exitcode = -1;
				::waitpid(kextpid,&exitcode,0);
			}
		}
	}
}

void OSXEthernetTap::setEnabled(bool en)
{
	_enabled = en;
	// TODO: interface status change
}

bool OSXEthernetTap::enabled() const
{
	return _enabled;
}

bool OSXEthernetTap::addIp(const InetAddress &ip)
{
	if (!ip)
		return false;

	long cpid = (long)vfork();
	if (cpid == 0) {
		::execl("/sbin/ifconfig","/sbin/ifconfig",_dev.c_str(),(ip.ss_family == AF_INET6) ? "inet6" : "inet",ip.toString().c_str(),"alias",(const char *)0);
		::_exit(-1);
	} else if (cpid > 0) {
		int exitcode = -1;
		::waitpid(cpid,&exitcode,0);
		return (exitcode == 0);
	} // else return false...

	return false;
}

bool OSXEthernetTap::removeIp(const InetAddress &ip)
{
	if (!ip)
		return true;
	std::vector<InetAddress> allIps(ips());
	for(std::vector<InetAddress>::iterator i(allIps.begin());i!=allIps.end();++i) {
		if (*i == ip) {
			long cpid = (long)vfork();
			if (cpid == 0) {
				execl("/sbin/ifconfig","/sbin/ifconfig",_dev.c_str(),(ip.ss_family == AF_INET6) ? "inet6" : "inet",ip.toIpString().c_str(),"-alias",(const char *)0);
				_exit(-1);
			} else if (cpid > 0) {
				int exitcode = -1;
				waitpid(cpid,&exitcode,0);
				return (exitcode == 0);
			}
		}
	}
	return false;
}

std::vector<InetAddress> OSXEthernetTap::ips() const
{
	struct ifaddrs *ifa = (struct ifaddrs *)0;
	if (getifaddrs(&ifa))
		return std::vector<InetAddress>();

	std::vector<InetAddress> r;

	struct ifaddrs *p = ifa;
	while (p) {
		if ((!strcmp(p->ifa_name,_dev.c_str()))&&(p->ifa_addr)&&(p->ifa_netmask)&&(p->ifa_addr->sa_family == p->ifa_netmask->sa_family)) {
			switch(p->ifa_addr->sa_family) {
				case AF_INET: {
					struct sockaddr_in *sin = (struct sockaddr_in *)p->ifa_addr;
					struct sockaddr_in *nm = (struct sockaddr_in *)p->ifa_netmask;
					r.push_back(InetAddress(&(sin->sin_addr.s_addr),4,Utils::countBits((uint32_t)nm->sin_addr.s_addr)));
				}	break;
				case AF_INET6: {
					struct sockaddr_in6 *sin = (struct sockaddr_in6 *)p->ifa_addr;
					struct sockaddr_in6 *nm = (struct sockaddr_in6 *)p->ifa_netmask;
					uint32_t b[4];
					memcpy(b,nm->sin6_addr.s6_addr,sizeof(b));
					r.push_back(InetAddress(sin->sin6_addr.s6_addr,16,Utils::countBits(b[0]) + Utils::countBits(b[1]) + Utils::countBits(b[2]) + Utils::countBits(b[3])));
				}	break;
			}
		}
		p = p->ifa_next;
	}

	if (ifa)
		freeifaddrs(ifa);

	std::sort(r.begin(),r.end());
	r.erase(std::unique(r.begin(),r.end()),r.end());

	return r;
}

void OSXEthernetTap::put(const MAC &from,const MAC &to,unsigned int etherType,const void *data,unsigned int len)
{
	char putBuf[4096];
	if ((_fd > 0)&&(len <= _mtu)&&(_enabled)) {
		to.copyTo(putBuf,6);
		from.copyTo(putBuf + 6,6);
		*((uint16_t *)(putBuf + 12)) = htons((uint16_t)etherType);
		memcpy(putBuf + 14,data,len);
		len += 14;
		::write(_fd,putBuf,len);
	}
}

std::string OSXEthernetTap::deviceName() const
{
	return _dev;
}

void OSXEthernetTap::setFriendlyName(const char *friendlyName)
{
}

void OSXEthernetTap::scanMulticastGroups(std::vector<MulticastGroup> &added,std::vector<MulticastGroup> &removed)
{
	std::vector<MulticastGroup> newGroups;

	struct _intl_ifmaddrs *ifmap = (struct _intl_ifmaddrs *)0;
	if (!_intl_getifmaddrs(&ifmap)) {
		struct _intl_ifmaddrs *p = ifmap;
		while (p) {
			if (p->ifma_addr->sa_family == AF_LINK) {
				struct sockaddr_dl *in = (struct sockaddr_dl *)p->ifma_name;
				struct sockaddr_dl *la = (struct sockaddr_dl *)p->ifma_addr;
				if ((la->sdl_alen == 6)&&(in->sdl_nlen <= _dev.length())&&(!memcmp(_dev.data(),in->sdl_data,in->sdl_nlen)))
					newGroups.push_back(MulticastGroup(MAC(la->sdl_data + la->sdl_nlen,6),0));
			}
			p = p->ifma_next;
		}
		_intl_freeifmaddrs(ifmap);
	}

	std::vector<InetAddress> allIps(ips());
	for(std::vector<InetAddress>::iterator ip(allIps.begin());ip!=allIps.end();++ip)
		newGroups.push_back(MulticastGroup::deriveMulticastGroupForAddressResolution(*ip));

	std::sort(newGroups.begin(),newGroups.end());
	std::unique(newGroups.begin(),newGroups.end());

	for(std::vector<MulticastGroup>::iterator m(newGroups.begin());m!=newGroups.end();++m) {
		if (!std::binary_search(_multicastGroups.begin(),_multicastGroups.end(),*m))
			added.push_back(*m);
	}
	for(std::vector<MulticastGroup>::iterator m(_multicastGroups.begin());m!=_multicastGroups.end();++m) {
		if (!std::binary_search(newGroups.begin(),newGroups.end(),*m))
			removed.push_back(*m);
	}

	_multicastGroups.swap(newGroups);
}

void OSXEthernetTap::threadMain()
	throw()
{
	fd_set readfds,nullfds;
	MAC to,from;
	int n,nfds,r;
	char getBuf[8194];

	Thread::sleep(500);

	FD_ZERO(&readfds);
	FD_ZERO(&nullfds);
	nfds = (int)std::max(_shutdownSignalPipe[0],_fd) + 1;

	r = 0;
	for(;;) {
		FD_SET(_shutdownSignalPipe[0],&readfds);
		FD_SET(_fd,&readfds);
		select(nfds,&readfds,&nullfds,&nullfds,(struct timeval *)0);

		if (FD_ISSET(_shutdownSignalPipe[0],&readfds)) // writes to shutdown pipe terminate thread
			break;

		if (FD_ISSET(_fd,&readfds)) {
			n = (int)::read(_fd,getBuf + r,sizeof(getBuf) - r);
			if (n < 0) {
				if ((errno != EINTR)&&(errno != ETIMEDOUT))
					break;
			} else {
				// Some tap drivers like to send the ethernet frame and the
				// payload in two chunks, so handle that by accumulating
				// data until we have at least a frame.
				r += n;
				if (r > 14) {
					if (r > ((int)_mtu + 14)) // sanity check for weird TAP behavior on some platforms
						r = _mtu + 14;

					if (_enabled) {
						to.setTo(getBuf,6);
						from.setTo(getBuf + 6,6);
						unsigned int etherType = ntohs(((const uint16_t *)getBuf)[6]);
						// TODO: VLAN support
						_handler(_arg,_nwid,from,to,etherType,0,(const void *)(getBuf + 14),r - 14);
					}

					r = 0;
				}
			}
		}
	}
}

} // namespace ZeroTier
