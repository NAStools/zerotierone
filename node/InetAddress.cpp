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

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <string>

#include "Constants.hpp"
#include "InetAddress.hpp"
#include "Utils.hpp"

namespace ZeroTier {

const InetAddress InetAddress::LO4((const void *)("\x7f\x00\x00\x01"),4,0);
const InetAddress InetAddress::LO6((const void *)("\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01"),16,0);

InetAddress::IpScope InetAddress::ipScope() const
	throw()
{
	switch(ss_family) {

		case AF_INET: {
			const uint32_t ip = Utils::ntoh((uint32_t)reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr);
			switch(ip >> 24) {
				case 0x00: return IP_SCOPE_NONE;                                      // 0.0.0.0/8 (reserved, never used)
				case 0x06: return IP_SCOPE_PSEUDOPRIVATE;                             // 6.0.0.0/8 (US Army)
				case 0x0a: return IP_SCOPE_PRIVATE;                                   // 10.0.0.0/8
				case 0x0b: return IP_SCOPE_PSEUDOPRIVATE;                             // 11.0.0.0/8 (US DoD)
				case 0x15: return IP_SCOPE_PSEUDOPRIVATE;                             // 21.0.0.0/8 (US DDN-RVN)
				case 0x16: return IP_SCOPE_PSEUDOPRIVATE;                             // 22.0.0.0/8 (US DISA)
				case 0x19: return IP_SCOPE_PSEUDOPRIVATE;                             // 25.0.0.0/8 (UK Ministry of Defense)
				case 0x1a: return IP_SCOPE_PSEUDOPRIVATE;                             // 26.0.0.0/8 (US DISA)
				case 0x1c: return IP_SCOPE_PSEUDOPRIVATE;                             // 28.0.0.0/8 (US DSI-North)
				case 0x1d: return IP_SCOPE_PSEUDOPRIVATE;                             // 29.0.0.0/8 (US DISA)
				case 0x1e: return IP_SCOPE_PSEUDOPRIVATE;                             // 30.0.0.0/8 (US DISA)
				case 0x2c: return IP_SCOPE_PSEUDOPRIVATE;                             // 44.0.0.0/8 (Amateur Radio)
				case 0x33: return IP_SCOPE_PSEUDOPRIVATE;                             // 51.0.0.0/8 (UK Department of Social Security)
				case 0x37: return IP_SCOPE_PSEUDOPRIVATE;                             // 55.0.0.0/8 (US DoD)
				case 0x38: return IP_SCOPE_PSEUDOPRIVATE;                             // 56.0.0.0/8 (US Postal Service)
				case 0x64:
					if ((ip & 0xffc00000) == 0x64400000) return IP_SCOPE_SHARED;        // 100.64.0.0/10
					break;
				case 0x7f: return IP_SCOPE_LOOPBACK;                                  // 127.0.0.0/8
				case 0xa9:
					if ((ip & 0xffff0000) == 0xa9fe0000) return IP_SCOPE_LINK_LOCAL;    // 169.254.0.0/16
					break;
				case 0xac:
					if ((ip & 0xfff00000) == 0xac100000) return IP_SCOPE_PRIVATE;       // 172.16.0.0/12
					break;
				case 0xc0:
					if ((ip & 0xffff0000) == 0xc0a80000) return IP_SCOPE_PRIVATE;				// 192.168.0.0/16
					break;
				case 0xff: return IP_SCOPE_NONE;                                      // 255.0.0.0/8 (broadcast, or unused/unusable)
			}
			switch(ip >> 28) {
				case 0xe: return IP_SCOPE_MULTICAST;                              // 224.0.0.0/4
				case 0xf: return IP_SCOPE_PSEUDOPRIVATE;                          // 240.0.0.0/4 ("reserved," usually unusable)
			}
			return IP_SCOPE_GLOBAL;
		}	break;

		case AF_INET6: {
			const unsigned char *ip = reinterpret_cast<const unsigned char *>(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr);
			if ((ip[0] & 0xf0) == 0xf0) {
				if (ip[0] == 0xff) return IP_SCOPE_MULTICAST;                              // ff00::/8
				if ((ip[0] == 0xfe)&&((ip[1] & 0xc0) == 0x80)) {
					unsigned int k = 2;
					while ((!ip[k])&&(k < 15)) ++k;
					if ((k == 15)&&(ip[15] == 0x01))
						return IP_SCOPE_LOOPBACK;                                              // fe80::1/128
					else return IP_SCOPE_LINK_LOCAL;                                         // fe80::/10
				}
				if ((ip[0] & 0xfe) == 0xfc) return IP_SCOPE_PRIVATE;                       // fc00::/7
			}
			unsigned int k = 0;
			while ((!ip[k])&&(k < 15)) ++k;
			if (k == 15) { // all 0's except last byte
				if (ip[15] == 0x01) return IP_SCOPE_LOOPBACK;                              // ::1/128
				if (ip[15] == 0x00) return IP_SCOPE_NONE;                                  // ::/128
			}
			return IP_SCOPE_GLOBAL;
		}	break;

	}

	return IP_SCOPE_NONE;
}

void InetAddress::set(const std::string &ip,unsigned int port)
	throw()
{
	memset(this,0,sizeof(InetAddress));
	if (ip.find(':') != std::string::npos) {
		struct sockaddr_in6 *sin6 = reinterpret_cast<struct sockaddr_in6 *>(this);
		ss_family = AF_INET6;
		sin6->sin6_port = Utils::hton((uint16_t)port);
		if (inet_pton(AF_INET6,ip.c_str(),(void *)&(sin6->sin6_addr.s6_addr)) <= 0)
			memset(this,0,sizeof(InetAddress));
	} else {
		struct sockaddr_in *sin = reinterpret_cast<struct sockaddr_in *>(this);
		ss_family = AF_INET;
		sin->sin_port = Utils::hton((uint16_t)port);
		if (inet_pton(AF_INET,ip.c_str(),(void *)&(sin->sin_addr.s_addr)) <= 0)
			memset(this,0,sizeof(InetAddress));
	}
}

void InetAddress::set(const void *ipBytes,unsigned int ipLen,unsigned int port)
	throw()
{
	memset(this,0,sizeof(InetAddress));
	if (ipLen == 4) {
		uint32_t ipb[1];
		memcpy(ipb,ipBytes,4);
		ss_family = AF_INET;
		reinterpret_cast<struct sockaddr_in *>(this)->sin_addr.s_addr = ipb[0];
		reinterpret_cast<struct sockaddr_in *>(this)->sin_port = Utils::hton((uint16_t)port);
	} else if (ipLen == 16) {
		ss_family = AF_INET6;
		memcpy(reinterpret_cast<struct sockaddr_in6 *>(this)->sin6_addr.s6_addr,ipBytes,16);
		reinterpret_cast<struct sockaddr_in6 *>(this)->sin6_port = Utils::hton((uint16_t)port);
	}
}

std::string InetAddress::toString() const
{
	char buf[128];
	switch(ss_family) {
		case AF_INET:
			Utils::snprintf(buf,sizeof(buf),"%d.%d.%d.%d/%d",
					(int)(reinterpret_cast<const unsigned char *>(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr)))[0],
					(int)(reinterpret_cast<const unsigned char *>(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr)))[1],
					(int)(reinterpret_cast<const unsigned char *>(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr)))[2],
					(int)(reinterpret_cast<const unsigned char *>(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr)))[3],
					(int)Utils::ntoh((uint16_t)(reinterpret_cast<const struct sockaddr_in *>(this)->sin_port))
				);
			return std::string(buf);
		case AF_INET6:
			Utils::snprintf(buf,sizeof(buf),"%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x/%d",
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[0]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[1]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[2]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[3]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[4]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[5]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[6]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[7]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[8]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[9]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[10]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[11]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[12]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[13]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[14]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[15]),
					(int)Utils::ntoh((uint16_t)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_port))
				);
			return std::string(buf);
	}
	return std::string();
}

std::string InetAddress::toIpString() const
{
	char buf[128];
	switch(ss_family) {
		case AF_INET:
			Utils::snprintf(buf,sizeof(buf),"%d.%d.%d.%d",
					(int)(reinterpret_cast<const unsigned char *>(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr)))[0],
					(int)(reinterpret_cast<const unsigned char *>(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr)))[1],
					(int)(reinterpret_cast<const unsigned char *>(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr)))[2],
					(int)(reinterpret_cast<const unsigned char *>(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr)))[3]
				);
			return std::string(buf);
		case AF_INET6:
			Utils::snprintf(buf,sizeof(buf),"%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x:%.2x%.2x",
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[0]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[1]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[2]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[3]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[4]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[5]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[6]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[7]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[8]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[9]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[10]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[11]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[12]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[13]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[14]),
					(int)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr[15])
				);
			return std::string(buf);
	}
	return std::string();
}

void InetAddress::fromString(const std::string &ipSlashPort)
{
	const std::size_t slashAt = ipSlashPort.find('/');
	if (slashAt == std::string::npos) {
		set(ipSlashPort,0);
	} else {
		long p = strtol(ipSlashPort.substr(slashAt+1).c_str(),(char **)0,10);
		if ((p > 0)&&(p <= 0xffff))
			set(ipSlashPort.substr(0,slashAt),(unsigned int)p);
		else set(ipSlashPort.substr(0,slashAt),0);
	}
}

InetAddress InetAddress::netmask() const
{
	InetAddress r(*this);
	switch(r.ss_family) {
		case AF_INET:
			reinterpret_cast<struct sockaddr_in *>(&r)->sin_addr.s_addr = Utils::hton((uint32_t)(0xffffffff << (32 - netmaskBits())));
			break;
		case AF_INET6: {
			uint64_t nm[2];
			const unsigned int bits = netmaskBits();
			nm[0] = Utils::hton((uint64_t)((bits >= 64) ? 0xffffffffffffffffULL : (0xffffffffffffffffULL << (64 - bits))));
			nm[1] = Utils::hton((uint64_t)((bits <= 64) ? 0ULL : (0xffffffffffffffffULL << (128 - bits))));
			memcpy(reinterpret_cast<struct sockaddr_in6 *>(&r)->sin6_addr.s6_addr,nm,16);
		}	break;
	}
	return r;
}

InetAddress InetAddress::broadcast() const
{
	if (ss_family == AF_INET) {
		InetAddress r(*this);
		reinterpret_cast<struct sockaddr_in *>(&r)->sin_addr.s_addr |= Utils::hton((uint32_t)(0xffffffff >> netmaskBits()));
		return r;
	}
	return InetAddress();
}

InetAddress InetAddress::network() const
{
	InetAddress r(*this);
	switch(r.ss_family) {
		case AF_INET:
			reinterpret_cast<struct sockaddr_in *>(&r)->sin_addr.s_addr &= Utils::hton((uint32_t)(0xffffffff << (32 - netmaskBits())));
			break;
		case AF_INET6: {
			uint64_t nm[2];
			const unsigned int bits = netmaskBits();
			memcpy(nm,reinterpret_cast<struct sockaddr_in6 *>(&r)->sin6_addr.s6_addr,16);
			nm[0] &= Utils::hton((uint64_t)((bits >= 64) ? 0xffffffffffffffffULL : (0xffffffffffffffffULL << (64 - bits))));
			nm[1] &= Utils::hton((uint64_t)((bits <= 64) ? 0ULL : (0xffffffffffffffffULL << (128 - bits))));
			memcpy(reinterpret_cast<struct sockaddr_in6 *>(&r)->sin6_addr.s6_addr,nm,16);
		}	break;
	}
	return r;
}

bool InetAddress::containsAddress(const InetAddress &addr) const
{
	if (addr.ss_family == ss_family) {
		switch(ss_family) {
			case AF_INET: {
				const unsigned int bits = netmaskBits();
				if (bits == 0)
					return true;
				return ( (Utils::ntoh((uint32_t)reinterpret_cast<const struct sockaddr_in *>(&addr)->sin_addr.s_addr) >> (32 - bits)) == (Utils::ntoh((uint32_t)reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr) >> (32 - bits)) );
			}
			case AF_INET6: {
				const InetAddress mask(netmask());
				const uint8_t *m = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(&mask)->sin6_addr.s6_addr);
				const uint8_t *a = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(&addr)->sin6_addr.s6_addr);
				const uint8_t *b = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr);
				for(unsigned int i=0;i<16;++i) {
					if ((a[i] & m[i]) != b[i])
						return false;
				}
				return true;
			}
		}
	}
	return false;
}

bool InetAddress::isNetwork() const
	throw()
{
	switch(ss_family) {
		case AF_INET: {
			unsigned int bits = netmaskBits();
			if (bits <= 0)
				return false;
			if (bits >= 32)
				return false;
			uint32_t ip = Utils::ntoh((uint32_t)reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr);
			return ((ip & (0xffffffff >> bits)) == 0);
		}
		case AF_INET6: {
			unsigned int bits = netmaskBits();
			if (bits <= 0)
				return false;
			if (bits >= 128)
				return false;
			const unsigned char *ip = reinterpret_cast<const unsigned char *>(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr);
			unsigned int p = bits / 8;
			if ((ip[p++] & (0xff >> (bits % 8))) != 0)
				return false;
			while (p < 16) {
				if (ip[p++])
					return false;
			}
			return true;
		}
	}
	return false;
}

bool InetAddress::operator==(const InetAddress &a) const
	throw()
{
	if (ss_family == a.ss_family) {
		switch(ss_family) {
			case AF_INET:
				return (
					(reinterpret_cast<const struct sockaddr_in *>(this)->sin_port == reinterpret_cast<const struct sockaddr_in *>(&a)->sin_port)&&
					(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr == reinterpret_cast<const struct sockaddr_in *>(&a)->sin_addr.s_addr));
				break;
			case AF_INET6:
				return (
					(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_port == reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_port)&&
					(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_flowinfo == reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_flowinfo)&&
					(memcmp(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr,reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_addr.s6_addr,16) == 0)&&
					(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_scope_id == reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_scope_id));
				break;
			default:
				return (memcmp(this,&a,sizeof(InetAddress)) == 0);
		}
	}
	return false;
}

bool InetAddress::operator<(const InetAddress &a) const
	throw()
{
	if (ss_family < a.ss_family)
		return true;
	else if (ss_family == a.ss_family) {
		switch(ss_family) {
			case AF_INET:
				if (reinterpret_cast<const struct sockaddr_in *>(this)->sin_port < reinterpret_cast<const struct sockaddr_in *>(&a)->sin_port)
					return true;
				else if (reinterpret_cast<const struct sockaddr_in *>(this)->sin_port == reinterpret_cast<const struct sockaddr_in *>(&a)->sin_port) {
					if (reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr < reinterpret_cast<const struct sockaddr_in *>(&a)->sin_addr.s_addr)
						return true;
				}
				break;
			case AF_INET6:
				if (reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_port < reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_port)
					return true;
				else if (reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_port == reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_port) {
					if (reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_flowinfo < reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_flowinfo)
						return true;
					else if (reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_flowinfo == reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_flowinfo) {
						if (memcmp(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr,reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_addr.s6_addr,16) < 0)
							return true;
						else if (memcmp(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr,reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_addr.s6_addr,16) == 0) {
							if (reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_scope_id < reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_scope_id)
								return true;
						}
					}
				}
				break;
			default:
				return (memcmp(this,&a,sizeof(InetAddress)) < 0);
		}
	}
	return false;
}

InetAddress InetAddress::makeIpv6LinkLocal(const MAC &mac)
{
	struct sockaddr_in6 sin6;
	sin6.sin6_family = AF_INET6;
	sin6.sin6_addr.s6_addr[0] = 0xfe;
	sin6.sin6_addr.s6_addr[1] = 0x80;
	sin6.sin6_addr.s6_addr[2] = 0x00;
	sin6.sin6_addr.s6_addr[3] = 0x00;
	sin6.sin6_addr.s6_addr[4] = 0x00;
	sin6.sin6_addr.s6_addr[5] = 0x00;
	sin6.sin6_addr.s6_addr[6] = 0x00;
	sin6.sin6_addr.s6_addr[7] = 0x00;
	sin6.sin6_addr.s6_addr[8] = mac[0] & 0xfd;
	sin6.sin6_addr.s6_addr[9] = mac[1];
	sin6.sin6_addr.s6_addr[10] = mac[2];
	sin6.sin6_addr.s6_addr[11] = 0xff;
	sin6.sin6_addr.s6_addr[12] = 0xfe;
	sin6.sin6_addr.s6_addr[13] = mac[3];
	sin6.sin6_addr.s6_addr[14] = mac[4];
	sin6.sin6_addr.s6_addr[15] = mac[5];
	sin6.sin6_port = Utils::hton((uint16_t)64);
	return InetAddress(sin6);
}

InetAddress InetAddress::makeIpv6rfc4193(uint64_t nwid,uint64_t zeroTierAddress)
{
	InetAddress r;
	struct sockaddr_in6 *const sin6 = reinterpret_cast<struct sockaddr_in6 *>(&r);
	sin6->sin6_family = AF_INET6;
	sin6->sin6_addr.s6_addr[0] = 0xfd;
	sin6->sin6_addr.s6_addr[1] = (uint8_t)(nwid >> 56);
	sin6->sin6_addr.s6_addr[2] = (uint8_t)(nwid >> 48);
	sin6->sin6_addr.s6_addr[3] = (uint8_t)(nwid >> 40);
	sin6->sin6_addr.s6_addr[4] = (uint8_t)(nwid >> 32);
	sin6->sin6_addr.s6_addr[5] = (uint8_t)(nwid >> 24);
	sin6->sin6_addr.s6_addr[6] = (uint8_t)(nwid >> 16);
	sin6->sin6_addr.s6_addr[7] = (uint8_t)(nwid >> 8);
	sin6->sin6_addr.s6_addr[8] = (uint8_t)nwid;
	sin6->sin6_addr.s6_addr[9] = 0x99;
	sin6->sin6_addr.s6_addr[10] = 0x93;
	sin6->sin6_addr.s6_addr[11] = (uint8_t)(zeroTierAddress >> 32);
	sin6->sin6_addr.s6_addr[12] = (uint8_t)(zeroTierAddress >> 24);
	sin6->sin6_addr.s6_addr[13] = (uint8_t)(zeroTierAddress >> 16);
	sin6->sin6_addr.s6_addr[14] = (uint8_t)(zeroTierAddress >> 8);
	sin6->sin6_addr.s6_addr[15] = (uint8_t)zeroTierAddress;
	sin6->sin6_port = Utils::hton((uint16_t)88); // /88 includes 0xfd + network ID, discriminating by device ID below that
	return r;
}

InetAddress InetAddress::makeIpv66plane(uint64_t nwid,uint64_t zeroTierAddress)
{
	nwid ^= (nwid >> 32);
	InetAddress r;
	struct sockaddr_in6 *const sin6 = reinterpret_cast<struct sockaddr_in6 *>(&r);
	sin6->sin6_family = AF_INET6;
	sin6->sin6_addr.s6_addr[0] = 0xfc;
	sin6->sin6_addr.s6_addr[1] = (uint8_t)(nwid >> 24);
	sin6->sin6_addr.s6_addr[2] = (uint8_t)(nwid >> 16);
	sin6->sin6_addr.s6_addr[3] = (uint8_t)(nwid >> 8);
	sin6->sin6_addr.s6_addr[4] = (uint8_t)nwid;
	sin6->sin6_addr.s6_addr[5] = (uint8_t)(zeroTierAddress >> 32);
	sin6->sin6_addr.s6_addr[6] = (uint8_t)(zeroTierAddress >> 24);
	sin6->sin6_addr.s6_addr[7] = (uint8_t)(zeroTierAddress >> 16);
	sin6->sin6_addr.s6_addr[8] = (uint8_t)(zeroTierAddress >> 8);
	sin6->sin6_addr.s6_addr[9] = (uint8_t)zeroTierAddress;
	sin6->sin6_addr.s6_addr[15] = 0x01;
	sin6->sin6_port = Utils::hton((uint16_t)40);
	return r;
}

} // namespace ZeroTier
