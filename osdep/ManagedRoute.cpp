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

#include "../node/Constants.hpp"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __WINDOWS__
#include <WinSock2.h>
#include <Windows.h>
#include <netioapi.h>
#include <IPHlpApi.h>
#endif

#ifdef __UNIX_LIKE__
#include <unistd.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <net/if.h>
#ifdef __BSD__
#include <net/if_dl.h>
#include <sys/sysctl.h>
#endif
#include <ifaddrs.h>
#endif

#include <vector>
#include <algorithm>
#include <utility>

#include "ManagedRoute.hpp"

#define ZT_BSD_ROUTE_CMD "/sbin/route"
#define ZT_LINUX_IP_COMMAND "/sbin/ip"
#define ZT_LINUX_IP_COMMAND_2 "/usr/sbin/ip"

// NOTE: BSD is mostly tested on Apple/Mac but is likely to work on other BSD too

namespace ZeroTier {

namespace {

// Fork a target into two more specific targets e.g. 0.0.0.0/0 -> 0.0.0.0/1, 128.0.0.0/1
// If the target is already maximally-specific, 'right' will be unchanged and 'left' will be 't'
static void _forkTarget(const InetAddress &t,InetAddress &left,InetAddress &right)
{
	const unsigned int bits = t.netmaskBits() + 1;
	left = t;
	if ((t.ss_family == AF_INET)&&(bits <= 32)) {
		left.setPort(bits);
		right = t;
		reinterpret_cast<struct sockaddr_in *>(&right)->sin_addr.s_addr ^= Utils::hton((uint32_t)(1 << (32 - bits)));
		right.setPort(bits);
	} else if ((t.ss_family == AF_INET6)&&(bits <= 128)) {
		left.setPort(bits);
		right = t;
		uint8_t *b = reinterpret_cast<uint8_t *>(reinterpret_cast<struct sockaddr_in6 *>(&right)->sin6_addr.s6_addr);
		b[bits / 8] ^= 1 << (8 - (bits % 8));
		right.setPort(bits);
	}
}

#ifdef __BSD__ // ------------------------------------------------------------
#define ZT_ROUTING_SUPPORT_FOUND 1

struct _RTE
{
	InetAddress target;
	InetAddress via;
	char device[128];
	int metric;
	bool ifscope;
};

static std::vector<_RTE> _getRTEs(const InetAddress &target,bool contains)
{
	std::vector<_RTE> rtes;
	int mib[6];
	size_t needed;

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = 0;
	mib[4] = NET_RT_DUMP;
	mib[5] = 0;
	if (!sysctl(mib,6,NULL,&needed,NULL,0)) {
		if (needed <= 0)
			return rtes;

		char *buf = (char *)::malloc(needed);
		if (buf) {
			if (!sysctl(mib,6,buf,&needed,NULL,0)) {
		    struct rt_msghdr *rtm;
				for(char *next=buf,*end=buf+needed;next<end;) {
					rtm = (struct rt_msghdr *)next;
					char *saptr = (char *)(rtm + 1);
					char *saend = next + rtm->rtm_msglen;

					InetAddress sa_t,sa_v;
					int deviceIndex = -9999;

					if (((rtm->rtm_flags & RTF_LLINFO) == 0)&&((rtm->rtm_flags & RTF_HOST) == 0)&&((rtm->rtm_flags & RTF_UP) != 0)&&((rtm->rtm_flags & RTF_MULTICAST) == 0)) {
						int which = 0;
						while (saptr < saend) {
							struct sockaddr *sa = (struct sockaddr *)saptr;
							unsigned int salen = sa->sa_len;
							if (!salen)
								break;

							// Skip missing fields in rtm_addrs bit field
							while ((rtm->rtm_addrs & 1) == 0) {
								rtm->rtm_addrs >>= 1;
								++which;
								if (which > 6)
									break;
							}
							if (which > 6)
								break;

							rtm->rtm_addrs >>= 1;
							switch(which++) {
								case 0:
									//printf("RTA_DST\n");
									if (sa->sa_family == AF_INET6) {
										struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sa;
										if ((sin6->sin6_addr.s6_addr[0] == 0xfe)&&((sin6->sin6_addr.s6_addr[1] & 0xc0) == 0x80)) {
											// BSD uses this fucking strange in-band signaling method to encode device scope IDs for IPv6 addresses... probably a holdover from very early versions of the spec.
											unsigned int interfaceIndex = ((((unsigned int)sin6->sin6_addr.s6_addr[2]) << 8) & 0xff) | (((unsigned int)sin6->sin6_addr.s6_addr[3]) & 0xff);
											sin6->sin6_addr.s6_addr[2] = 0;
											sin6->sin6_addr.s6_addr[3] = 0;
											if (!sin6->sin6_scope_id)
												sin6->sin6_scope_id = interfaceIndex;
										}
									}
									sa_t = *sa;
									break;
								case 1:
									//printf("RTA_GATEWAY\n");
									switch(sa->sa_family) {
										case AF_LINK:
											deviceIndex = (int)((const struct sockaddr_dl *)sa)->sdl_index;
											break;
										case AF_INET:
										case AF_INET6:
											sa_v = *sa;
											break;
									}
									break;
								case 2: {
									//printf("RTA_NETMASK\n");
									if (sa_t.ss_family == AF_INET6) {
										salen = sizeof(struct sockaddr_in6);
										unsigned int bits = 0;
										for(int i=0;i<16;++i) {
											unsigned char c = (unsigned char)((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr[i];
											if (c == 0xff)
												bits += 8;
											else break;
										}
										sa_t.setPort(bits);
									} else if (sa_t.ss_family == AF_INET) {
										salen = sizeof(struct sockaddr_in);
										sa_t.setPort((unsigned int)Utils::countBits((uint32_t)((const struct sockaddr_in *)sa)->sin_addr.s_addr));
									}
								}	break;
								/*
								case 3:
									//printf("RTA_GENMASK\n");
									break;
								case 4:
									//printf("RTA_IFP\n");
									break;
								case 5:
									//printf("RTA_IFA\n");
									break;
								case 6:
									//printf("RTA_AUTHOR\n");
									break;
								*/
							}

							saptr += salen;
						}

						if (((contains)&&(sa_t.containsAddress(target)))||(sa_t == target)) {
							rtes.push_back(_RTE());
							rtes.back().target = sa_t;
							rtes.back().via = sa_v;
							if (deviceIndex >= 0) {
								if_indextoname(deviceIndex,rtes.back().device);
							} else {
								rtes.back().device[0] = (char)0;
							}
							rtes.back().metric = ((int)rtm->rtm_rmx.rmx_hopcount < 0) ? 0 : (int)rtm->rtm_rmx.rmx_hopcount;
						}
					}

					next = saend;
				}
			}

			::free(buf);
		}
	}

	return rtes;
}

static void _routeCmd(const char *op,const InetAddress &target,const InetAddress &via,const char *ifscope,const char *localInterface)
{
	long p = (long)fork();
	if (p > 0) {
		int exitcode = -1;
		::waitpid(p,&exitcode,0);
	} else if (p == 0) {
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);
		if (via) {
			if ((ifscope)&&(ifscope[0])) {
				::execl(ZT_BSD_ROUTE_CMD,ZT_BSD_ROUTE_CMD,op,"-ifscope",ifscope,((target.ss_family == AF_INET6) ? "-inet6" : "-inet"),target.toString().c_str(),via.toIpString().c_str(),(const char *)0);
			} else {
				::execl(ZT_BSD_ROUTE_CMD,ZT_BSD_ROUTE_CMD,op,((target.ss_family == AF_INET6) ? "-inet6" : "-inet"),target.toString().c_str(),via.toIpString().c_str(),(const char *)0);
			}
		} else if ((localInterface)&&(localInterface[0])) {
			if ((ifscope)&&(ifscope[0])) {
				::execl(ZT_BSD_ROUTE_CMD,ZT_BSD_ROUTE_CMD,op,"-ifscope",ifscope,((target.ss_family == AF_INET6) ? "-inet6" : "-inet"),target.toString().c_str(),"-interface",localInterface,(const char *)0);
			} else {
				::execl(ZT_BSD_ROUTE_CMD,ZT_BSD_ROUTE_CMD,op,((target.ss_family == AF_INET6) ? "-inet6" : "-inet"),target.toString().c_str(),"-interface",localInterface,(const char *)0);
			}
		}
		::_exit(-1);
	}
}

#endif // __BSD__ ------------------------------------------------------------

#ifdef __LINUX__ // ----------------------------------------------------------
#define ZT_ROUTING_SUPPORT_FOUND 1

static void _routeCmd(const char *op,const InetAddress &target,const InetAddress &via,const char *localInterface)
{
	long p = (long)fork();
	if (p > 0) {
		int exitcode = -1;
		::waitpid(p,&exitcode,0);
	} else if (p == 0) {
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);
		if (via) {
			::execl(ZT_LINUX_IP_COMMAND,ZT_LINUX_IP_COMMAND,(target.ss_family == AF_INET6) ? "-6" : "-4","route",op,target.toString().c_str(),"via",via.toIpString().c_str(),(const char *)0);
			::execl(ZT_LINUX_IP_COMMAND_2,ZT_LINUX_IP_COMMAND_2,(target.ss_family == AF_INET6) ? "-6" : "-4","route",op,target.toString().c_str(),"via",via.toIpString().c_str(),(const char *)0);
		} else if ((localInterface)&&(localInterface[0])) {
			::execl(ZT_LINUX_IP_COMMAND,ZT_LINUX_IP_COMMAND,(target.ss_family == AF_INET6) ? "-6" : "-4","route",op,target.toString().c_str(),"dev",localInterface,(const char *)0);
			::execl(ZT_LINUX_IP_COMMAND_2,ZT_LINUX_IP_COMMAND_2,(target.ss_family == AF_INET6) ? "-6" : "-4","route",op,target.toString().c_str(),"dev",localInterface,(const char *)0);
		}
		::_exit(-1);
	}
}

#endif // __LINUX__ ----------------------------------------------------------

#ifdef __WINDOWS__ // --------------------------------------------------------
#define ZT_ROUTING_SUPPORT_FOUND 1

static bool _winRoute(bool del,const NET_LUID &interfaceLuid,const NET_IFINDEX &interfaceIndex,const InetAddress &target,const InetAddress &via)
{
	MIB_IPFORWARD_ROW2 rtrow;
	InitializeIpForwardEntry(&rtrow);
	rtrow.InterfaceLuid.Value = interfaceLuid.Value;
	rtrow.InterfaceIndex = interfaceIndex;
	if (target.ss_family == AF_INET) {
		rtrow.DestinationPrefix.Prefix.si_family = AF_INET;
		rtrow.DestinationPrefix.Prefix.Ipv4.sin_family = AF_INET;
		rtrow.DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr = reinterpret_cast<const struct sockaddr_in *>(&target)->sin_addr.S_un.S_addr;
		if (via.ss_family == AF_INET) {
			rtrow.NextHop.si_family = AF_INET;
			rtrow.NextHop.Ipv4.sin_family = AF_INET;
			rtrow.NextHop.Ipv4.sin_addr.S_un.S_addr = reinterpret_cast<const struct sockaddr_in *>(&via)->sin_addr.S_un.S_addr;
		}
	} else if (target.ss_family == AF_INET6) {
		rtrow.DestinationPrefix.Prefix.si_family = AF_INET6;
		rtrow.DestinationPrefix.Prefix.Ipv6.sin6_family = AF_INET6;
		memcpy(rtrow.DestinationPrefix.Prefix.Ipv6.sin6_addr.u.Byte,reinterpret_cast<const struct sockaddr_in6 *>(&target)->sin6_addr.u.Byte,16);
		if (via.ss_family == AF_INET6) {
			rtrow.NextHop.si_family = AF_INET6;
			rtrow.NextHop.Ipv6.sin6_family = AF_INET6;
			memcpy(rtrow.NextHop.Ipv6.sin6_addr.u.Byte,reinterpret_cast<const struct sockaddr_in6 *>(&via)->sin6_addr.u.Byte,16);
		}
	} else {
		return false;
	}
	rtrow.DestinationPrefix.PrefixLength = target.netmaskBits();
	rtrow.SitePrefixLength = rtrow.DestinationPrefix.PrefixLength;
	rtrow.ValidLifetime = 0xffffffff;
	rtrow.PreferredLifetime = 0xffffffff;
	rtrow.Metric = -1;
	rtrow.Protocol = MIB_IPPROTO_NETMGMT;
	rtrow.Loopback = FALSE;
	rtrow.AutoconfigureAddress = FALSE;
	rtrow.Publish = FALSE;
	rtrow.Immortal = FALSE;
	rtrow.Age = 0;
	rtrow.Origin = NlroManual;
	if (del) {
		return (DeleteIpForwardEntry2(&rtrow) == NO_ERROR);
	} else {
		NTSTATUS r = CreateIpForwardEntry2(&rtrow);
		if (r == NO_ERROR) {
			return true;
		} else if (r == ERROR_OBJECT_ALREADY_EXISTS) {
			return (SetIpForwardEntry2(&rtrow) == NO_ERROR);
		} else {
			return false;
		}
	}
}

#endif // __WINDOWS__ --------------------------------------------------------

#ifndef ZT_ROUTING_SUPPORT_FOUND
#error "ManagedRoute.cpp has no support for managing routes on this platform! You'll need to check and see if one of the existing ones will work and make sure proper defines are set, or write one. Please do a Github pull request if you do this for a new OS."
#endif

} // anonymous namespace

/* Linux NOTE: for default route override, some Linux distributions will
 * require a change to the rp_filter parameter. A value of '1' will prevent
 * default route override from working properly.
 *
 * sudo sysctl -w net.ipv4.conf.all.rp_filter=2
 *
 * Add to /etc/sysctl.conf or /etc/sysctl.d/... to make permanent.
 *
 * This is true of CentOS/RHEL 6+ and possibly others. This is because
 * Linux default route override implies asymmetric routes, which then
 * trigger Linux's "martian packet" filter. */

bool ManagedRoute::sync()
{
#ifdef __WINDOWS__
	NET_LUID interfaceLuid;
	interfaceLuid.Value = (ULONG64)Utils::hexStrToU64(_device); // on Windows we use the hex LUID as the "interface name" for ManagedRoute
	NET_IFINDEX interfaceIndex = -1;
	if (ConvertInterfaceLuidToIndex(&interfaceLuid,&interfaceIndex) != NO_ERROR)
		return false;
#endif

	if ((_target.isDefaultRoute())||((_target.ss_family == AF_INET)&&(_target.netmaskBits() < 32))) {
		/* In ZeroTier we create two more specific routes for every one route. We
		 * do this for default routes and IPv4 routes other than /32s. If there
		 * is a pre-existing system route that this route will override, we create
		 * two more specific interface-bound shadow routes for it.
		 *
		 * This means that ZeroTier can *itself* continue communicating over
		 * whatever physical routes might be present while simultaneously
		 * overriding them for general system traffic. This is mostly for
		 * "full tunnel" VPN modes of operation, but might be useful for
		 * virtualizing physical networks in a hybrid design as well. */

		// Generate two more specific routes than target with one extra bit
		InetAddress leftt,rightt;
		_forkTarget(_target,leftt,rightt);

#ifdef __BSD__ // ------------------------------------------------------------

		// Find lowest metric system route that this route should override (if any)
		InetAddress newSystemVia;
		char newSystemDevice[128];
		newSystemDevice[0] = (char)0;
		int systemMetric = 9999999;
		std::vector<_RTE> rtes(_getRTEs(_target,false));
		for(std::vector<_RTE>::iterator r(rtes.begin());r!=rtes.end();++r) {
			if (r->via) {
				if ((!newSystemVia)||(r->metric < systemMetric)) {
					newSystemVia = r->via;
					Utils::scopy(newSystemDevice,sizeof(newSystemDevice),r->device);
					systemMetric = r->metric;
				}
			}
		}
		if ((newSystemVia)&&(!newSystemDevice[0])) {
			rtes = _getRTEs(newSystemVia,true);
			for(std::vector<_RTE>::iterator r(rtes.begin());r!=rtes.end();++r) {
				if (r->device[0]) {
					Utils::scopy(newSystemDevice,sizeof(newSystemDevice),r->device);
					break;
				}
			}
		}

		// Shadow system route if it exists, also delete any obsolete shadows
		// and replace them with the new state. sync() is called periodically to
		// allow us to do that if underlying connectivity changes.
		if ( ((_systemVia != newSystemVia)||(strcmp(_systemDevice,newSystemDevice))) && (strcmp(_device,newSystemDevice)) ) {
			if ((_systemVia)&&(_systemDevice[0])) {
				_routeCmd("delete",leftt,_systemVia,_systemDevice,(const char *)0);
				_routeCmd("delete",rightt,_systemVia,_systemDevice,(const char *)0);
			}

			_systemVia = newSystemVia;
			Utils::scopy(_systemDevice,sizeof(_systemDevice),newSystemDevice);

			if ((_systemVia)&&(_systemDevice[0])) {
				_routeCmd("add",leftt,_systemVia,_systemDevice,(const char *)0);
				_routeCmd("change",leftt,_systemVia,_systemDevice,(const char *)0);
				_routeCmd("add",rightt,_systemVia,_systemDevice,(const char *)0);
				_routeCmd("change",rightt,_systemVia,_systemDevice,(const char *)0);
			}
		}

		// Apply overriding non-device-scoped routes
		if (!_applied) {
			if (_via) {
				_routeCmd("add",leftt,_via,(const char *)0,(const char *)0);
				_routeCmd("change",leftt,_via,(const char *)0,(const char *)0);
				_routeCmd("add",rightt,_via,(const char *)0,(const char *)0);
				_routeCmd("change",rightt,_via,(const char *)0,(const char *)0);
			} else if (_device[0]) {
				_routeCmd("add",leftt,_via,(const char *)0,_device);
				_routeCmd("change",leftt,_via,(const char *)0,_device);
				_routeCmd("add",rightt,_via,(const char *)0,_device);
				_routeCmd("change",rightt,_via,(const char *)0,_device);
			}

			_applied = true;
		}

#endif // __BSD__ ------------------------------------------------------------

#ifdef __LINUX__ // ----------------------------------------------------------

		if (!_applied) {
			_routeCmd("replace",leftt,_via,(_via) ? _device : (const char *)0);
			_routeCmd("replace",rightt,_via,(_via) ? _device : (const char *)0);
			_applied = true;
		}

#endif // __LINUX__ ----------------------------------------------------------

#ifdef __WINDOWS__ // --------------------------------------------------------

		if (!_applied) {
			_winRoute(false,interfaceLuid,interfaceIndex,leftt,_via);
			_winRoute(false,interfaceLuid,interfaceIndex,rightt,_via);
			_applied = true;
		}

#endif // __WINDOWS__ --------------------------------------------------------

	} else {

#ifdef __BSD__ // ------------------------------------------------------------

		if (!_applied) {
			if (_via) {
				_routeCmd("add",_target,_via,(const char *)0,(const char *)0);
				_routeCmd("change",_target,_via,(const char *)0,(const char *)0);
			} else if (_device[0]) {
				_routeCmd("add",_target,_via,(const char *)0,_device);
				_routeCmd("change",_target,_via,(const char *)0,_device);
			}
			_applied = true;
		}

#endif // __BSD__ ------------------------------------------------------------

#ifdef __LINUX__ // ----------------------------------------------------------

		if (!_applied) {
			_routeCmd("replace",_target,_via,(_via) ? _device : (const char *)0);
			_applied = true;
		}

#endif // __LINUX__ ----------------------------------------------------------

#ifdef __WINDOWS__ // --------------------------------------------------------

		if (!_applied) {
			_winRoute(false,interfaceLuid,interfaceIndex,_target,_via);
			_applied = true;
		}

#endif // __WINDOWS__ --------------------------------------------------------

	}

	return true;
}

void ManagedRoute::remove()
{
#ifdef __WINDOWS__
	NET_LUID interfaceLuid;
	interfaceLuid.Value = (ULONG64)Utils::hexStrToU64(_device); // on Windows we use the hex LUID as the "interface name" for ManagedRoute
	NET_IFINDEX interfaceIndex = -1;
	if (ConvertInterfaceLuidToIndex(&interfaceLuid,&interfaceIndex) != NO_ERROR)
		return;
#endif

	if (_applied) {
		if ((_target.isDefaultRoute())||((_target.ss_family == AF_INET)&&(_target.netmaskBits() < 32))) {
			InetAddress leftt,rightt;
			_forkTarget(_target,leftt,rightt);

#ifdef __BSD__ // ------------------------------------------------------------

			if ((_systemVia)&&(_systemDevice[0])) {
				_routeCmd("delete",leftt,_systemVia,_systemDevice,(const char *)0);
				_routeCmd("delete",rightt,_systemVia,_systemDevice,(const char *)0);
			}
			if (_via) {
				_routeCmd("delete",leftt,_via,(const char *)0,(const char *)0);
				_routeCmd("delete",rightt,_via,(const char *)0,(const char *)0);
			} else if (_device[0]) {
				_routeCmd("delete",leftt,_via,(const char *)0,_device);
				_routeCmd("delete",rightt,_via,(const char *)0,_device);
			}

#endif // __BSD__ ------------------------------------------------------------

#ifdef __LINUX__ // ----------------------------------------------------------

			_routeCmd("del",leftt,_via,(_via) ? _device : (const char *)0);
			_routeCmd("del",rightt,_via,(_via) ? _device : (const char *)0);

#endif // __LINUX__ ----------------------------------------------------------

#ifdef __WINDOWS__ // --------------------------------------------------------

			_winRoute(true,interfaceLuid,interfaceIndex,leftt,_via);
			_winRoute(true,interfaceLuid,interfaceIndex,rightt,_via);

#endif // __WINDOWS__ --------------------------------------------------------

		} else {

#ifdef __BSD__ // ------------------------------------------------------------

		if (_via) {
			_routeCmd("delete",_target,_via,(const char *)0,(const char *)0);
		} else if (_device[0]) {
			_routeCmd("delete",_target,_via,(const char *)0,_device);
		}

#endif // __BSD__ ------------------------------------------------------------

#ifdef __LINUX__ // ----------------------------------------------------------

			_routeCmd("del",_target,_via,(_via) ? _device : (const char *)0);

#endif // __LINUX__ ----------------------------------------------------------

#ifdef __WINDOWS__ // --------------------------------------------------------

			_winRoute(true,interfaceLuid,interfaceIndex,_target,_via);

#endif // __WINDOWS__ --------------------------------------------------------

		}
	}

	_target.zero();
	_via.zero();
	_systemVia.zero();
	_device[0] = (char)0;
	_systemDevice[0] = (char)0;
	_applied = false;
}

} // namespace ZeroTier
