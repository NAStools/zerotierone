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
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "../node/Constants.hpp"

#ifdef __UNIX_LIKE__
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <dirent.h>
#include <netdb.h>
#endif

#ifdef __WINDOWS__
#include <windows.h>
#include <wincrypt.h>
#include <ShlObj.h>
#include <netioapi.h>
#include <iphlpapi.h>
#endif

#include "OSUtils.hpp"

namespace ZeroTier {

#ifdef __UNIX_LIKE__
bool OSUtils::redirectUnixOutputs(const char *stdoutPath,const char *stderrPath)
	throw()
{
	int fdout = ::open(stdoutPath,O_WRONLY|O_CREAT,0600);
	if (fdout > 0) {
		int fderr;
		if (stderrPath) {
			fderr = ::open(stderrPath,O_WRONLY|O_CREAT,0600);
			if (fderr <= 0) {
				::close(fdout);
				return false;
			}
		} else fderr = fdout;
		::close(STDOUT_FILENO);
		::close(STDERR_FILENO);
		::dup2(fdout,STDOUT_FILENO);
		::dup2(fderr,STDERR_FILENO);
		return true;
	}
	return false;
}
#endif // __UNIX_LIKE__

std::vector<std::string> OSUtils::listDirectory(const char *path)
{
	std::vector<std::string> r;

#ifdef __WINDOWS__
	HANDLE hFind;
	WIN32_FIND_DATAA ffd;
	if ((hFind = FindFirstFileA((std::string(path) + "\\*").c_str(),&ffd)) != INVALID_HANDLE_VALUE) {
		do {
			if ((strcmp(ffd.cFileName,"."))&&(strcmp(ffd.cFileName,".."))&&((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0))
				r.push_back(std::string(ffd.cFileName));
		} while (FindNextFileA(hFind,&ffd));
		FindClose(hFind);
	}
#else
	struct dirent de;
	struct dirent *dptr;
	DIR *d = opendir(path);
	if (!d)
		return r;
	dptr = (struct dirent *)0;
	for(;;) {
		if (readdir_r(d,&de,&dptr))
			break;
		if (dptr) {
			if ((strcmp(dptr->d_name,"."))&&(strcmp(dptr->d_name,".."))&&(dptr->d_type != DT_DIR))
				r.push_back(std::string(dptr->d_name));
		} else break;
	}
	closedir(d);
#endif

	return r;
}

void OSUtils::lockDownFile(const char *path,bool isDir)
{
#ifdef __UNIX_LIKE__
	chmod(path,isDir ? 0700 : 0600);
#else
#ifdef __WINDOWS__
	{
		STARTUPINFOA startupInfo;
		PROCESS_INFORMATION processInfo;

		startupInfo.cb = sizeof(startupInfo);
		memset(&startupInfo,0,sizeof(STARTUPINFOA));
		memset(&processInfo,0,sizeof(PROCESS_INFORMATION));
		if (CreateProcessA(NULL,(LPSTR)(std::string("C:\\Windows\\System32\\icacls.exe \"") + path + "\" /inheritance:d /Q").c_str(),NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startupInfo,&processInfo)) {
			WaitForSingleObject(processInfo.hProcess,INFINITE);
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
		}

		startupInfo.cb = sizeof(startupInfo);
		memset(&startupInfo,0,sizeof(STARTUPINFOA));
		memset(&processInfo,0,sizeof(PROCESS_INFORMATION));
		if (CreateProcessA(NULL,(LPSTR)(std::string("C:\\Windows\\System32\\icacls.exe \"") + path + "\" /remove *S-1-5-32-545 /Q").c_str(),NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startupInfo,&processInfo)) {
			WaitForSingleObject(processInfo.hProcess,INFINITE);
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
		}
	}
#endif
#endif
}

uint64_t OSUtils::getLastModified(const char *path)
{
	struct stat s;
	if (stat(path,&s))
		return 0;
	return (((uint64_t)s.st_mtime) * 1000ULL);
}

bool OSUtils::fileExists(const char *path,bool followLinks)
{
	struct stat s;
#ifdef __UNIX_LIKE__
	if (!followLinks)
		return (lstat(path,&s) == 0);
#endif
	return (stat(path,&s) == 0);
}

int64_t OSUtils::getFileSize(const char *path)
{
	struct stat s;
	if (stat(path,&s))
		return -1;
#ifdef __WINDOWS__
	return s.st_size;
#else
	if (S_ISREG(s.st_mode))
		return s.st_size;
#endif
	return -1;
}

std::vector<InetAddress> OSUtils::resolve(const char *name)
{
	std::vector<InetAddress> r;
	std::vector<InetAddress>::iterator i;
	InetAddress tmp;
	struct addrinfo *ai = (struct addrinfo *)0,*p;
	if (!getaddrinfo(name,(const char *)0,(const struct addrinfo *)0,&ai)) {
		try {
			p = ai;
			while (p) {
				if ((p->ai_addr)&&((p->ai_addr->sa_family == AF_INET)||(p->ai_addr->sa_family == AF_INET6))) {
					tmp = *(p->ai_addr);
					for(i=r.begin();i!=r.end();++i) {
						if (i->ipsEqual(tmp))
							goto skip_add_inetaddr;
					}
					r.push_back(tmp);
				}
skip_add_inetaddr:
				p = p->ai_next;
			}
		} catch ( ... ) {}
		freeaddrinfo(ai);
	}
	std::sort(r.begin(),r.end());
	return r;
}

bool OSUtils::readFile(const char *path,std::string &buf)
{
	char tmp[1024];
	FILE *f = fopen(path,"rb");
	if (f) {
		for(;;) {
			long n = (long)fread(tmp,1,sizeof(tmp),f);
			if (n > 0)
				buf.append(tmp,n);
			else break;
		}
		fclose(f);
		return true;
	}
	return false;
}

bool OSUtils::writeFile(const char *path,const void *buf,unsigned int len)
{
	FILE *f = fopen(path,"wb");
	if (f) {
		if ((long)fwrite(buf,1,len,f) != (long)len) {
			fclose(f);
			return false;
		} else {
			fclose(f);
			return true;
		}
	}
	return false;
}

std::string OSUtils::platformDefaultHomePath()
{
#ifdef __UNIX_LIKE__

#ifdef __APPLE__
	// /Library/... on Apple
	return std::string("/Library/Application Support/ZeroTier/One");
#else

#ifdef __BSD__
	// BSD likes /var/db instead of /var/lib
	return std::string("/var/db/zerotier-one");
#else
	// Use /var/lib for Linux and other *nix
	return std::string("/var/lib/zerotier-one");
#endif

#endif

#else // not __UNIX_LIKE__

#ifdef __WINDOWS__
	// Look up app data folder on Windows, e.g. C:\ProgramData\...
	char buf[16384];
	if (SUCCEEDED(SHGetFolderPathA(NULL,CSIDL_COMMON_APPDATA,NULL,0,buf)))
		return (std::string(buf) + "\\ZeroTier\\One");
	else return std::string("C:\\ZeroTier\\One");
#else

	return (std::string(ZT_PATH_SEPARATOR_S) + "ZeroTier" + ZT_PATH_SEPARATOR_S + "One"); // UNKNOWN PLATFORM

#endif

#endif // __UNIX_LIKE__ or not...
}

// Used to convert HTTP header names to ASCII lower case
const unsigned char OSUtils::TOLOWER_TABLE[256] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, ' ', '!', '"', '#', '$', '%', '&', 0x27, '(', ')', '*', '+', ',', '-', '.', '/', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?', '@', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '{', '|', '}', '~', '_', '`', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '{', '|', '}', '~', 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff };

} // namespace ZeroTier
