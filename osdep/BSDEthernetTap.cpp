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
#include <net/if.h>
#include <ifaddrs.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/route.h>

#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <utility>

#include "../node/Constants.hpp"
#include "../node/Utils.hpp"
#include "../node/Mutex.hpp"
#include "OSUtils.hpp"
#include "BSDEthernetTap.hpp"

#define ZT_BASE32_CHARS "0123456789abcdefghijklmnopqrstuv"

// ff:ff:ff:ff:ff:ff with no ADI
static const ZeroTier::MulticastGroup _blindWildcardMulticastGroup(ZeroTier::MAC(0xff),0);

namespace ZeroTier {

BSDEthernetTap::BSDEthernetTap(
	const char *homePath,
	const MAC &mac,
	unsigned int mtu,
	unsigned int metric,
	uint64_t nwid,
	const char *friendlyName,
	void (*handler)(void *,uint64_t,const MAC &,const MAC &,unsigned int,unsigned int,const void *,unsigned int),
	void *arg) :
	_handler(handler),
	_arg(arg),
	_nwid(nwid),
	_mtu(mtu),
	_metric(metric),
	_fd(0),
	_enabled(true)
{
	static Mutex globalTapCreateLock;
	char devpath[64],ethaddr[64],mtustr[32],metstr[32],tmpdevname[32];
	struct stat stattmp;

	// On FreeBSD at least we can rename, so use nwid to generate a deterministic unique zt#### name using base32
	// As a result we don't use desiredDevice
	_dev = "zt";
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 60) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 55) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 50) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 45) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 40) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 35) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 30) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 25) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 20) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 15) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 10) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)((nwid >> 5) & 0x1f)]);
	_dev.push_back(ZT_BASE32_CHARS[(unsigned long)(nwid & 0x1f)]);

	Mutex::Lock _gl(globalTapCreateLock);

	if (mtu > 2800)
		throw std::runtime_error("max tap MTU is 2800");

	// On BSD we create taps and they can have high numbers, so use ones starting
	// at 9993 to not conflict with other stuff. Then we rename it to zt<base32 of nwid>
	std::vector<std::string> devFiles(OSUtils::listDirectory("/dev"));
	for(int i=9993;i<(9993+128);++i) {
		Utils::snprintf(tmpdevname,sizeof(tmpdevname),"tap%d",i);
		Utils::snprintf(devpath,sizeof(devpath),"/dev/%s",tmpdevname);
		if (std::find(devFiles.begin(),devFiles.end(),std::string(tmpdevname)) == devFiles.end()) {
			long cpid = (long)vfork();
			if (cpid == 0) {
				::execl("/sbin/ifconfig","/sbin/ifconfig",tmpdevname,"create",(const char *)0);
				::_exit(-1);
			} else if (cpid > 0) {
				int exitcode = -1;
				::waitpid(cpid,&exitcode,0);
			} else throw std::runtime_error("fork() failed");

			if (!stat(devpath,&stattmp)) {
				cpid = (long)vfork();
				if (cpid == 0) {
					::execl("/sbin/ifconfig","/sbin/ifconfig",tmpdevname,"name",_dev.c_str(),(const char *)0);
					::_exit(-1);
				} else if (cpid > 0) {
					int exitcode = -1;
					::waitpid(cpid,&exitcode,0);
					if (exitcode)
						throw std::runtime_error("ifconfig rename operation failed");
				} else throw std::runtime_error("fork() failed");

				_fd = ::open(devpath,O_RDWR);
				if (_fd > 0)
					break;
				else throw std::runtime_error("unable to open created tap device");
			} else {
				throw std::runtime_error("cannot find /dev node for newly created tap device");
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

	// Set close-on-exec so that devices cannot persist if we fork/exec for update
	fcntl(_fd,F_SETFD,fcntl(_fd,F_GETFD) | FD_CLOEXEC);

	::pipe(_shutdownSignalPipe);

	_thread = Thread::start(this);
}

BSDEthernetTap::~BSDEthernetTap()
{
	::write(_shutdownSignalPipe[1],"\0",1); // causes thread to exit
	Thread::join(_thread);
	::close(_fd);
	::close(_shutdownSignalPipe[0]);
	::close(_shutdownSignalPipe[1]);

	long cpid = (long)vfork();
	if (cpid == 0) {
		::execl("/sbin/ifconfig","/sbin/ifconfig",_dev.c_str(),"destroy",(const char *)0);
		::_exit(-1);
	} else if (cpid > 0) {
		int exitcode = -1;
		::waitpid(cpid,&exitcode,0);
	}
}

void BSDEthernetTap::setEnabled(bool en)
{
	_enabled = en;
}

bool BSDEthernetTap::enabled() const
{
	return _enabled;
}

static bool ___removeIp(const std::string &_dev,const InetAddress &ip)
{
	long cpid = (long)vfork();
	if (cpid == 0) {
		execl("/sbin/ifconfig","/sbin/ifconfig",_dev.c_str(),"inet",ip.toIpString().c_str(),"-alias",(const char *)0);
		_exit(-1);
	} else if (cpid > 0) {
		int exitcode = -1;
		waitpid(cpid,&exitcode,0);
		return (exitcode == 0);
	}
	return false; // never reached, make compiler shut up about return value
}

bool BSDEthernetTap::addIp(const InetAddress &ip)
{
	if (!ip)
		return false;

	std::vector<InetAddress> allIps(ips());
	if (std::find(allIps.begin(),allIps.end(),ip) != allIps.end())
		return true; // IP/netmask already assigned

	// Remove and reconfigure if address is the same but netmask is different
	for(std::vector<InetAddress>::iterator i(allIps.begin());i!=allIps.end();++i) {
		if ((i->ipsEqual(ip))&&(i->netmaskBits() != ip.netmaskBits())) {
			if (___removeIp(_dev,*i))
				break;
		}
	}

	long cpid = (long)vfork();
	if (cpid == 0) {
		::execl("/sbin/ifconfig","/sbin/ifconfig",_dev.c_str(),ip.isV4() ? "inet" : "inet6",ip.toString().c_str(),"alias",(const char *)0);
		::_exit(-1);
	} else if (cpid > 0) {
		int exitcode = -1;
		::waitpid(cpid,&exitcode,0);
		return (exitcode == 0);
	}
	return false;
}

bool BSDEthernetTap::removeIp(const InetAddress &ip)
{
	if (!ip)
		return false;
	std::vector<InetAddress> allIps(ips());
	if (std::find(allIps.begin(),allIps.end(),ip) != allIps.end()) {
		if (___removeIp(_dev,ip))
			return true;
	}
	return false;
}

std::vector<InetAddress> BSDEthernetTap::ips() const
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
	std::unique(r.begin(),r.end());

	return r;
}

void BSDEthernetTap::put(const MAC &from,const MAC &to,unsigned int etherType,const void *data,unsigned int len)
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

std::string BSDEthernetTap::deviceName() const
{
	return _dev;
}

void BSDEthernetTap::setFriendlyName(const char *friendlyName)
{
}

void BSDEthernetTap::scanMulticastGroups(std::vector<MulticastGroup> &added,std::vector<MulticastGroup> &removed)
{
	std::vector<MulticastGroup> newGroups;

	struct ifmaddrs *ifmap = (struct ifmaddrs *)0;
	if (!getifmaddrs(&ifmap)) {
		struct ifmaddrs *p = ifmap;
		while (p) {
			if (p->ifma_addr->sa_family == AF_LINK) {
				struct sockaddr_dl *in = (struct sockaddr_dl *)p->ifma_name;
				struct sockaddr_dl *la = (struct sockaddr_dl *)p->ifma_addr;
				if ((la->sdl_alen == 6)&&(in->sdl_nlen <= _dev.length())&&(!memcmp(_dev.data(),in->sdl_data,in->sdl_nlen)))
					newGroups.push_back(MulticastGroup(MAC(la->sdl_data + la->sdl_nlen,6),0));
			}
			p = p->ifma_next;
		}
		freeifmaddrs(ifmap);
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

/*
bool BSDEthernetTap::updateMulticastGroups(std::set<MulticastGroup> &groups)
{
	std::set<MulticastGroup> newGroups;
	struct ifmaddrs *ifmap = (struct ifmaddrs *)0;
	if (!getifmaddrs(&ifmap)) {
		struct ifmaddrs *p = ifmap;
		while (p) {
			if (p->ifma_addr->sa_family == AF_LINK) {
				struct sockaddr_dl *in = (struct sockaddr_dl *)p->ifma_name;
				struct sockaddr_dl *la = (struct sockaddr_dl *)p->ifma_addr;
				if ((la->sdl_alen == 6)&&(in->sdl_nlen <= _dev.length())&&(!memcmp(_dev.data(),in->sdl_data,in->sdl_nlen)))
					newGroups.insert(MulticastGroup(MAC(la->sdl_data + la->sdl_nlen,6),0));
			}
			p = p->ifma_next;
		}
		freeifmaddrs(ifmap);
	}

	{
		std::set<InetAddress> allIps(ips());
		for(std::set<InetAddress>::const_iterator i(allIps.begin());i!=allIps.end();++i)
			newGroups.insert(MulticastGroup::deriveMulticastGroupForAddressResolution(*i));
	}

	bool changed = false;

	for(std::set<MulticastGroup>::iterator mg(newGroups.begin());mg!=newGroups.end();++mg) {
		if (!groups.count(*mg)) {
			groups.insert(*mg);
			changed = true;
		}
	}
	for(std::set<MulticastGroup>::iterator mg(groups.begin());mg!=groups.end();) {
		if ((!newGroups.count(*mg))&&(*mg != _blindWildcardMulticastGroup)) {
			groups.erase(mg++);
			changed = true;
		} else ++mg;
	}

	return changed;
}
*/

void BSDEthernetTap::threadMain()
	throw()
{
	fd_set readfds,nullfds;
	MAC to,from;
	int n,nfds,r;
	char getBuf[8194];

	// Wait for a moment after startup -- wait for Network to finish
	// constructing itself.
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
