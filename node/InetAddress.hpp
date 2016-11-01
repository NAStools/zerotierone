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

#ifndef ZT_INETADDRESS_HPP
#define ZT_INETADDRESS_HPP

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <string>

#include "Constants.hpp"
#include "../include/ZeroTierOne.h"
#include "Utils.hpp"
#include "MAC.hpp"
#include "Buffer.hpp"

namespace ZeroTier {

/**
 * Maximum integer value of enum IpScope
 */
#define ZT_INETADDRESS_MAX_SCOPE 7

/**
 * Extends sockaddr_storage with friendly C++ methods
 *
 * This is basically a "mixin" for sockaddr_storage. It adds methods and
 * operators, but does not modify the structure. This can be cast to/from
 * sockaddr_storage and used interchangeably. DO NOT change this by e.g.
 * adding non-static fields, since much code depends on this identity.
 */
struct InetAddress : public sockaddr_storage
{
	/**
	 * Loopback IPv4 address (no port)
	 */
	static const InetAddress LO4;

	/**
	 * Loopback IPV6 address (no port)
	 */
	static const InetAddress LO6;

	/**
	 * IP address scope
	 *
	 * Note that these values are in ascending order of path preference and
	 * MUST remain that way or Path must be changed to reflect. Also be sure
	 * to change ZT_INETADDRESS_MAX_SCOPE if the max changes.
	 */
	enum IpScope
	{
		IP_SCOPE_NONE = 0,          // NULL or not an IP address
		IP_SCOPE_MULTICAST = 1,     // 224.0.0.0 and other V4/V6 multicast IPs
		IP_SCOPE_LOOPBACK = 2,      // 127.0.0.1, ::1, etc.
		IP_SCOPE_PSEUDOPRIVATE = 3, // 28.x.x.x, etc. -- unofficially unrouted IPv4 blocks often "bogarted"
		IP_SCOPE_GLOBAL = 4,        // globally routable IP address (all others)
		IP_SCOPE_LINK_LOCAL = 5,    // 169.254.x.x, IPv6 LL
		IP_SCOPE_SHARED = 6,        // 100.64.0.0/10, shared space for e.g. carrier-grade NAT
		IP_SCOPE_PRIVATE = 7        // 10.x.x.x, 192.168.x.x, etc.
	};

	InetAddress() throw() { memset(this,0,sizeof(InetAddress)); }
	InetAddress(const InetAddress &a) throw() { memcpy(this,&a,sizeof(InetAddress)); }
	InetAddress(const InetAddress *a) throw() { memcpy(this,a,sizeof(InetAddress)); }
	InetAddress(const struct sockaddr_storage &ss) throw() { *this = ss; }
	InetAddress(const struct sockaddr_storage *ss) throw() { *this = ss; }
	InetAddress(const struct sockaddr &sa) throw() { *this = sa; }
	InetAddress(const struct sockaddr *sa) throw() { *this = sa; }
	InetAddress(const struct sockaddr_in &sa) throw() { *this = sa; }
	InetAddress(const struct sockaddr_in *sa) throw() { *this = sa; }
	InetAddress(const struct sockaddr_in6 &sa) throw() { *this = sa; }
	InetAddress(const struct sockaddr_in6 *sa) throw() { *this = sa; }
	InetAddress(const void *ipBytes,unsigned int ipLen,unsigned int port) throw() { this->set(ipBytes,ipLen,port); }
	InetAddress(const uint32_t ipv4,unsigned int port) throw() { this->set(&ipv4,4,port); }
	InetAddress(const std::string &ip,unsigned int port) throw() { this->set(ip,port); }
	InetAddress(const std::string &ipSlashPort) throw() { this->fromString(ipSlashPort); }
	InetAddress(const char *ipSlashPort) throw() { this->fromString(std::string(ipSlashPort)); }

	inline InetAddress &operator=(const InetAddress &a)
		throw()
	{
		if (&a != this)
			memcpy(this,&a,sizeof(InetAddress));
		return *this;
	}

	inline InetAddress &operator=(const InetAddress *a)
		throw()
	{
		if (a != this)
			memcpy(this,a,sizeof(InetAddress));
		return *this;
	}

	inline InetAddress &operator=(const struct sockaddr_storage &ss)
		throw()
	{
		if (reinterpret_cast<const InetAddress *>(&ss) != this)
			memcpy(this,&ss,sizeof(InetAddress));
		return *this;
	}

	inline InetAddress &operator=(const struct sockaddr_storage *ss)
		throw()
	{
		if (reinterpret_cast<const InetAddress *>(ss) != this)
			memcpy(this,ss,sizeof(InetAddress));
		return *this;
	}

	inline InetAddress &operator=(const struct sockaddr_in &sa)
		throw()
	{
		if (reinterpret_cast<const InetAddress *>(&sa) != this) {
			memset(this,0,sizeof(InetAddress));
			memcpy(this,&sa,sizeof(struct sockaddr_in));
		}
		return *this;
	}

	inline InetAddress &operator=(const struct sockaddr_in *sa)
		throw()
	{
		if (reinterpret_cast<const InetAddress *>(sa) != this) {
			memset(this,0,sizeof(InetAddress));
			memcpy(this,sa,sizeof(struct sockaddr_in));
		}
		return *this;
	}

	inline InetAddress &operator=(const struct sockaddr_in6 &sa)
		throw()
	{
		if (reinterpret_cast<const InetAddress *>(&sa) != this) {
			memset(this,0,sizeof(InetAddress));
			memcpy(this,&sa,sizeof(struct sockaddr_in6));
		}
		return *this;
	}

	inline InetAddress &operator=(const struct sockaddr_in6 *sa)
		throw()
	{
		if (reinterpret_cast<const InetAddress *>(sa) != this) {
			memset(this,0,sizeof(InetAddress));
			memcpy(this,sa,sizeof(struct sockaddr_in6));
		}
		return *this;
	}

	inline InetAddress &operator=(const struct sockaddr &sa)
		throw()
	{
		if (reinterpret_cast<const InetAddress *>(&sa) != this) {
			memset(this,0,sizeof(InetAddress));
			switch(sa.sa_family) {
				case AF_INET:
					memcpy(this,&sa,sizeof(struct sockaddr_in));
					break;
				case AF_INET6:
					memcpy(this,&sa,sizeof(struct sockaddr_in6));
					break;
			}
		}
		return *this;
	}

	inline InetAddress &operator=(const struct sockaddr *sa)
		throw()
	{
		if (reinterpret_cast<const InetAddress *>(sa) != this) {
			memset(this,0,sizeof(InetAddress));
			switch(sa->sa_family) {
				case AF_INET:
					memcpy(this,sa,sizeof(struct sockaddr_in));
					break;
				case AF_INET6:
					memcpy(this,sa,sizeof(struct sockaddr_in6));
					break;
			}
		}
		return *this;
	}

	/**
	 * @return IP scope classification (e.g. loopback, link-local, private, global)
	 */
	IpScope ipScope() const
		throw();

	/**
	 * Set from a string-format IP and a port
	 *
	 * @param ip IP address in V4 or V6 ASCII notation
	 * @param port Port or 0 for none
	 */
	void set(const std::string &ip,unsigned int port)
		throw();

	/**
	 * Set from a raw IP and port number
	 *
	 * @param ipBytes Bytes of IP address in network byte order
	 * @param ipLen Length of IP address: 4 or 16
	 * @param port Port number or 0 for none
	 */
	void set(const void *ipBytes,unsigned int ipLen,unsigned int port)
		throw();

	/**
	 * Set the port component
	 *
	 * @param port Port, 0 to 65535
	 */
	inline void setPort(unsigned int port)
	{
		switch(ss_family) {
			case AF_INET:
				reinterpret_cast<struct sockaddr_in *>(this)->sin_port = Utils::hton((uint16_t)port);
				break;
			case AF_INET6:
				reinterpret_cast<struct sockaddr_in6 *>(this)->sin6_port = Utils::hton((uint16_t)port);
				break;
		}
	}

	/**
	 * @return True if this network/netmask route describes a default route (e.g. 0.0.0.0/0)
	 */
	inline bool isDefaultRoute() const
	{
		switch(ss_family) {
			case AF_INET:
				return ( (reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr == 0) && (reinterpret_cast<const struct sockaddr_in *>(this)->sin_port == 0) );
			case AF_INET6:
				const uint8_t *ipb = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr);
				for(int i=0;i<16;++i) {
					if (ipb[i])
						return false;
				}
				return (reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_port == 0);
		}
		return false;
	}

	/**
	 * @return ASCII IP/port format representation
	 */
	std::string toString() const;

	/**
	 * @return IP portion only, in ASCII string format
	 */
	std::string toIpString() const;

	/**
	 * @param ipSlashPort ASCII IP/port format notation
	 */
	void fromString(const std::string &ipSlashPort);

	/**
	 * @return Port or 0 if no port component defined
	 */
	inline unsigned int port() const
		throw()
	{
		switch(ss_family) {
			case AF_INET: return Utils::ntoh((uint16_t)(reinterpret_cast<const struct sockaddr_in *>(this)->sin_port));
			case AF_INET6: return Utils::ntoh((uint16_t)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_port));
			default: return 0;
		}
	}

	/**
	 * Alias for port()
	 *
	 * This just aliases port() to make code more readable when netmask bits
	 * are stuffed there, as they are in Network, EthernetTap, and a few other
	 * spots.
	 *
	 * @return Netmask bits
	 */
	inline unsigned int netmaskBits() const throw() { return port(); }

	/**
	 * Alias for port()
	 *
	 * This just aliases port() because for gateways we use this field to
	 * store the gateway metric.
	 *
	 * @return Gateway metric
	 */
	inline unsigned int metric() const throw() { return port(); }

	/**
	 * Construct a full netmask as an InetAddress
	 *
	 * @return Netmask such as 255.255.255.0 if this address is /24 (port field will be unchanged)
	 */
	InetAddress netmask() const;

	/**
	 * Constructs a broadcast address from a network/netmask address
	 *
	 * This is only valid for IPv4 and will return a NULL InetAddress for other
	 * address families.
	 *
	 * @return Broadcast address (only IP portion is meaningful)
	 */
	InetAddress broadcast() const;

	/**
	 * Return the network -- a.k.a. the IP ANDed with the netmask
	 *
	 * @return Network e.g. 10.0.1.0/24 from 10.0.1.200/24
	 */
	InetAddress network() const;

	/**
	 * Test whether this IP/netmask contains this address
	 *
	 * @param addr Address to check
	 * @return True if this IP/netmask (route) contains this address
	 */
	bool containsAddress(const InetAddress &addr) const;

	/**
	 * @return True if this is an IPv4 address
	 */
	inline bool isV4() const throw() { return (ss_family == AF_INET); }

	/**
	 * @return True if this is an IPv6 address
	 */
	inline bool isV6() const throw() { return (ss_family == AF_INET6); }

	/**
	 * @return pointer to raw address bytes or NULL if not available
	 */
	inline const void *rawIpData() const
		throw()
	{
		switch(ss_family) {
			case AF_INET: return (const void *)&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr);
			case AF_INET6: return (const void *)(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr);
			default: return 0;
		}
	}

	/**
	 * Performs an IP-only comparison or, if that is impossible, a memcmp()
	 *
	 * @param a InetAddress to compare again
	 * @return True if only IP portions are equal (false for non-IP or null addresses)
	 */
	inline bool ipsEqual(const InetAddress &a) const
	{
		if (ss_family == a.ss_family) {
			if (ss_family == AF_INET)
				return (reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr == reinterpret_cast<const struct sockaddr_in *>(&a)->sin_addr.s_addr);
			if (ss_family == AF_INET6)
				return (memcmp(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr,reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_addr.s6_addr,16) == 0);
			return (memcmp(this,&a,sizeof(InetAddress)) == 0);
		}
		return false;
	}

	/**
	 * Set to null/zero
	 */
	inline void zero() throw() { memset(this,0,sizeof(InetAddress)); }

	/**
	 * Check whether this is a network/route rather than an IP assignment
	 *
	 * A network is an IP/netmask where everything after the netmask is
	 * zero e.g. 10.0.0.0/8.
	 *
	 * @return True if everything after netmask bits is zero
	 */
	bool isNetwork() const
		throw();

	/**
	 * @return True if address family is non-zero
	 */
	inline operator bool() const throw() { return (ss_family != 0); }

	template<unsigned int C>
	inline void serialize(Buffer<C> &b) const
	{
		// This is used in the protocol and must be the same as describe in places
		// like VERB_HELLO in Packet.hpp.
		switch(ss_family) {
			case AF_INET:
				b.append((uint8_t)0x04);
				b.append(&(reinterpret_cast<const struct sockaddr_in *>(this)->sin_addr.s_addr),4);
				b.append((uint16_t)port()); // just in case sin_port != uint16_t
				return;
			case AF_INET6:
				b.append((uint8_t)0x06);
				b.append(reinterpret_cast<const struct sockaddr_in6 *>(this)->sin6_addr.s6_addr,16);
				b.append((uint16_t)port()); // just in case sin_port != uint16_t
				return;
			default:
				b.append((uint8_t)0);
				return;
		}
	}

	template<unsigned int C>
	inline unsigned int deserialize(const Buffer<C> &b,unsigned int startAt = 0)
	{
		memset(this,0,sizeof(InetAddress));
		unsigned int p = startAt;
		switch(b[p++]) {
			case 0:
				return 1;
			case 0x01:
				// TODO: Ethernet address (but accept for forward compatibility)
				return 7;
			case 0x02:
				// TODO: Bluetooth address (but accept for forward compatibility)
				return 7;
			case 0x03:
				// TODO: Other address types (but accept for forward compatibility)
				// These could be extended/optional things like AF_UNIX, LTE Direct, shared memory, etc.
				return (unsigned int)(b.template at<uint16_t>(p) + 3); // other addresses begin with 16-bit non-inclusive length
			case 0x04:
				ss_family = AF_INET;
				memcpy(&(reinterpret_cast<struct sockaddr_in *>(this)->sin_addr.s_addr),b.field(p,4),4); p += 4;
				reinterpret_cast<struct sockaddr_in *>(this)->sin_port = Utils::hton(b.template at<uint16_t>(p)); p += 2;
				break;
			case 0x06:
				ss_family = AF_INET6;
				memcpy(reinterpret_cast<struct sockaddr_in6 *>(this)->sin6_addr.s6_addr,b.field(p,16),16); p += 16;
				reinterpret_cast<struct sockaddr_in *>(this)->sin_port = Utils::hton(b.template at<uint16_t>(p)); p += 2;
				break;
			default:
				throw std::invalid_argument("invalid serialized InetAddress");
		}
		return (p - startAt);
	}

	bool operator==(const InetAddress &a) const throw();
	bool operator<(const InetAddress &a) const throw();
	inline bool operator!=(const InetAddress &a) const throw() { return !(*this == a); }
	inline bool operator>(const InetAddress &a) const throw() { return (a < *this); }
	inline bool operator<=(const InetAddress &a) const throw() { return !(a < *this); }
	inline bool operator>=(const InetAddress &a) const throw() { return !(*this < a); }

	/**
	 * @param mac MAC address seed
	 * @return IPv6 link-local address
	 */
	static InetAddress makeIpv6LinkLocal(const MAC &mac);

	/**
	 * Compute private IPv6 unicast address from network ID and ZeroTier address
	 *
	 * This generates a private unicast IPv6 address that is mostly compliant
	 * with the letter of RFC4193 and certainly compliant in spirit.
	 *
	 * RFC4193 specifies a format of:
	 *
	 * | 7 bits |1|  40 bits   |  16 bits  |          64 bits           |
	 * | Prefix |L| Global ID  | Subnet ID |        Interface ID        |
	 *
	 * The 'L' bit is set to 1, yielding an address beginning with 0xfd. Then
	 * the network ID is filled into the global ID, subnet ID, and first byte
	 * of the "interface ID" field. Since the first 40 bits of the network ID
	 * is the unique ZeroTier address of its controller, this makes a very
	 * good random global ID. Since network IDs have 24 more bits, we let it
	 * overflow into the interface ID.
	 *
	 * After that we pad with two bytes: 0x99, 0x93, namely the default ZeroTier
	 * port in hex.
	 *
	 * Finally we fill the remaining 40 bits of the interface ID field with
	 * the 40-bit unique ZeroTier device ID of the network member.
	 *
	 * This yields a valid RFC4193 address with a random global ID, a
	 * meaningful subnet ID, and a unique interface ID, all mappable back onto
	 * ZeroTier space.
	 *
	 * This in turn could allow us, on networks numbered this way, to emulate
	 * IPv6 NDP and eliminate all multicast. This could be beneficial for
	 * small devices and huge networks, e.g. IoT applications.
	 *
	 * The returned address is given an odd prefix length of /88, since within
	 * a given network only the last 40 bits (device ID) are variable. This
	 * is a bit unusual but as far as we know should not cause any problems with
	 * any non-braindead IPv6 stack.
	 *
	 * @param nwid 64-bit network ID
	 * @param zeroTierAddress 40-bit device address (in least significant 40 bits, highest 24 bits ignored)
	 * @return IPv6 private unicast address with /88 netmask
	 */
	static InetAddress makeIpv6rfc4193(uint64_t nwid,uint64_t zeroTierAddress);

	/**
	 * Compute a private IPv6 "6plane" unicast address from network ID and ZeroTier address
	 */
	static InetAddress makeIpv66plane(uint64_t nwid,uint64_t zeroTierAddress);
};

} // namespace ZeroTier

#endif
