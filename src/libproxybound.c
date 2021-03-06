/***************************************************************************
                           libproxybound.c
                           ---------------

     copyright: intika      (C) 2019 intika@librefox.org
     copyright: rofl0r      (C) 2012 https://github.com/rofl0r
     copyright: haad        (C) 2012 https://github.com/haad
     copyright: netcreature (C) 2002 netcreature@users.sourceforge.net
    
 ***************************************************************************
 *   GPL                                                                   *
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#undef _GNU_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <dlfcn.h>

#include "core.h"
#include "common.h"

#define     satosin(x)      ((struct sockaddr_in *) &(x))
#define     SOCKADDR(x)     (satosin(x)->sin_addr.s_addr)
#define     SOCKADDR_2(x)     (satosin(x)->sin_addr)
#define     SOCKPORT(x)     (satosin(x)->sin_port)
#define     SOCKFAMILY(x)     (satosin(x)->sin_family)
#define     MAX_CHAIN 512

connect_t true_connect;
gethostbyname_t true_gethostbyname;
getaddrinfo_t true_getaddrinfo;
freeaddrinfo_t true_freeaddrinfo;
getnameinfo_t true_getnameinfo;
gethostbyaddr_t true_gethostbyaddr;

send_t true_send;
sendto_t true_sendto;
sendmsg_t true_sendmsg;
bind_t true_bind;

int tcp_read_time_out;
int tcp_connect_time_out;
chain_type proxybound_ct;
proxy_data proxybound_pd[MAX_CHAIN];
unsigned int proxybound_proxy_count = 0;
int proxybound_got_chain_data = 0;
unsigned int proxybound_max_chain = 1;
int proxybound_quiet_mode = 0;
int proxybound_allow_leak = 0;
int proxybound_allow_dns = 0;
int proxybound_working_indicator = 0;
int proxybound_resolver = 1;
localaddr_arg localnet_addr[MAX_LOCALNET];
size_t num_localnet_addr = 0;
unsigned int remote_dns_subnet = 224;
#ifdef THREAD_SAFE
pthread_once_t init_once = PTHREAD_ONCE_INIT;
#endif

static int init_l = 0;

static void init_additional_settings(chain_type *ct);

static inline void get_chain_data(proxy_data * pd, unsigned int *proxy_count, chain_type * ct);

static void manual_socks5_env(proxy_data * pd, unsigned int *proxy_count, chain_type * ct);

static int is_dns_port(unsigned short port);

static void create_tmp_proof_file();

static void* load_sym(char* symname, void* proxyfunc) {

	void *funcptr = dlsym(RTLD_NEXT, symname);
	
	if(!funcptr) {
		fprintf(stderr, "Cannot load symbol '%s' %s\n", symname, dlerror());
		exit(1);
	} else {
		PDEBUG("proxybound: loaded symbol '%s'" " real addr %p  wrapped addr %p\n", symname, funcptr, proxyfunc);
	}
	if(funcptr == proxyfunc) {
		PDEBUG("proxybound: circular reference detected, aborting!\n");
		abort();
	}
	return funcptr;
}

#define INIT() init_lib_wrapper(__FUNCTION__)

#define SETUP_SYM(X) do { true_ ## X = load_sym( # X, X ); } while(0)

static void do_init(void) {
	MUTEX_INIT(&internal_ips_lock, NULL);
	MUTEX_INIT(&hostdb_lock, NULL);
    
    //file to indicate that the injection is working
    char *env; env = getenv(PROXYBOUND_WORKING_INDICATOR_ENV_VAR);
	if(env && *env == '1') proxybound_working_indicator = 1;
    if (proxybound_working_indicator) create_tmp_proof_file();
    
    /* check for simple SOCKS5 proxy setup */
	manual_socks5_env(proxybound_pd, &proxybound_proxy_count, &proxybound_ct);
    
	/* read the config file */
	get_chain_data(proxybound_pd, &proxybound_proxy_count, &proxybound_ct);

	proxybound_write_log(LOG_PREFIX "DLL init\n");
	
	SETUP_SYM(connect);
	SETUP_SYM(gethostbyname);
	SETUP_SYM(getaddrinfo);
	SETUP_SYM(freeaddrinfo);
	SETUP_SYM(gethostbyaddr);
	SETUP_SYM(getnameinfo);
    
	SETUP_SYM(send);
	SETUP_SYM(sendto);
	SETUP_SYM(sendmsg);
	SETUP_SYM(bind);
	
	init_l = 1;
}

static void init_lib_wrapper(const char* caller) {
#ifndef DEBUG
	(void) caller;
#endif
#ifndef THREAD_SAFE
	if(init_l) return;
	PDEBUG("proxybound: %s called from %s\n", __FUNCTION__,  caller);
	do_init();
#else
	if(!init_l) PDEBUG("proxybound: %s called from %s\n", __FUNCTION__,  caller);
	pthread_once(&init_once, do_init);
#endif
}

static void create_tmp_proof_file() {
    FILE *fp;
    if ((fp = fopen("/tmp/proxybound.tmp", "w")) == NULL ) {exit(1); exit(1);}
    fprintf(fp, "injected\n");
    fflush(fp);fclose(fp);
}

/* if we use gcc >= 3, we can instruct the dynamic loader 
 * to call init_lib at link time. otherwise it gets loaded
 * lazily, which has the disadvantage that there's a potential
 * race condition if 2 threads call it before init_l is set 
 * and PTHREAD support was disabled */
#if __GNUC__ > 2
__attribute__((constructor))
static void gcc_init(void) {
	INIT();
}
#endif

static void init_additional_settings(chain_type *ct) {
	char *env;

	tcp_read_time_out = 4 * 1000;
	tcp_connect_time_out = 10 * 1000;
	*ct = DYNAMIC_TYPE;
    
	env = getenv(PROXYBOUND_ALLOW_LEAKS_ENV_VAR);
	if(env && *env == '1')
		proxybound_allow_leak = 1;
    
	env = getenv(PROXYBOUND_ALLOW_DNS_ENV_VAR);
	if(env && *env == '1')
		proxybound_allow_dns = 1;
    
	env = getenv(PROXYBOUND_QUIET_MODE_ENV_VAR);
	if(env && *env == '1')
		proxybound_quiet_mode = 1;
}

/* get configuration from config file */
static void get_chain_data(proxy_data * pd, unsigned int *proxy_count, chain_type * ct) {
	int count = 0, port_n = 0, list = 0;
	char buff[1024], type[1024], host[1024], user[1024];
	char *env;
	char local_in_addr_port[32];
	char local_in_addr[32], local_in_port[32], local_netmask[32];
	FILE *file = NULL;

	if(proxybound_got_chain_data)
		return;

	//Some defaults
    init_additional_settings(ct);
	
	env = get_config_path(getenv(PROXYBOUND_CONF_FILE_ENV_VAR), buff, sizeof(buff));
	file = fopen(env, "r");

	while(fgets(buff, sizeof(buff), file)) {
		if(buff[0] != '\n' && buff[strspn(buff, " ")] != '#') {
			/* proxylist has to come last */
			if(list) {
				if(count >= MAX_CHAIN)
					break;
				
				memset(&pd[count], 0, sizeof(proxy_data));

				pd[count].ps = PLAY_STATE;
				port_n = 0;

				sscanf(buff, "%s %s %d %s %s", type, host, &port_n, pd[count].user, pd[count].pass);

				pd[count].ip.as_int = (uint32_t) inet_addr(host);
				pd[count].port = htons((unsigned short) port_n);

				if(!strcmp(type, "http")) {
					pd[count].pt = HTTP_TYPE;
				} else if(!strcmp(type, "socks4")) {
					pd[count].pt = SOCKS4_TYPE;
				} else if(!strcmp(type, "socks5")) {
					pd[count].pt = SOCKS5_TYPE;
				} else
					continue;

				if(pd[count].ip.as_int && port_n && pd[count].ip.as_int != (uint32_t) - 1)
					count++;
			} else {
				if(strstr(buff, "[ProxyList]")) {
					list = 1;
				} else if(strstr(buff, "random_chain")) {
					*ct = RANDOM_TYPE;
				} else if(strstr(buff, "strict_chain")) {
					*ct = STRICT_TYPE;
				} else if(strstr(buff, "dynamic_chain")) {
					*ct = DYNAMIC_TYPE;
				} else if(strstr(buff, "tcp_read_time_out")) {
					sscanf(buff, "%s %d", user, &tcp_read_time_out);
				} else if(strstr(buff, "tcp_connect_time_out")) {
					sscanf(buff, "%s %d", user, &tcp_connect_time_out);
				} else if(strstr(buff, "remote_dns_subnet")) {
					sscanf(buff, "%s %d", user, &remote_dns_subnet);
					if(remote_dns_subnet >= 256) {
						fprintf(stderr,
							"remote_dns_subnet: invalid value. requires a number between 0 and 255.\n");
						exit(1);
					}
				} else if(strstr(buff, "localnet")) {
					if(sscanf(buff, "%s %21[^/]/%15s", user, local_in_addr_port, local_netmask) < 3) {
						fprintf(stderr, "localnet format error");
						exit(1);
					}
					/* clean previously used buffer */
					memset(local_in_port, 0, sizeof(local_in_port) / sizeof(local_in_port[0]));

					if(sscanf(local_in_addr_port, "%15[^:]:%5s", local_in_addr, local_in_port) < 2) {
						PDEBUG("proxybound: added localnet: netaddr=%s, netmask=%s\n",
						       local_in_addr, local_netmask);
					} else {
						PDEBUG("proxybound: added localnet: netaddr=%s, port=%s, netmask=%s\n",
						       local_in_addr, local_in_port, local_netmask);
					}
					if(num_localnet_addr < MAX_LOCALNET) {
						int error;
						error =
						    inet_pton(AF_INET, local_in_addr,
							      &localnet_addr[num_localnet_addr].in_addr);
						if(error <= 0) {
							fprintf(stderr, "localnet address error\n");
							exit(1);
						}
						error =
						    inet_pton(AF_INET, local_netmask,
							      &localnet_addr[num_localnet_addr].netmask);
						if(error <= 0) {
							fprintf(stderr, "localnet netmask error\n");
							exit(1);
						}
						if(local_in_port[0]) {
							localnet_addr[num_localnet_addr].port =
							    (short) atoi(local_in_port);
						} else {
							localnet_addr[num_localnet_addr].port = 0;
						}
						++num_localnet_addr;
					} else {
						fprintf(stderr, "# of localnet exceed %d.\n", MAX_LOCALNET);
					}
				} else if(strstr(buff, "chain_len")) {
					char *pc;
					int len;
					pc = strchr(buff, '=');
					len = atoi(++pc);
					proxybound_max_chain = (len ? len : 1);
				} else if(strstr(buff, "quiet_mode")) {
					proxybound_quiet_mode = 1;
				} else if(strstr(buff, "proxy_dns")) {
					proxybound_resolver = 1;
				}
			}
		}
	}
	fclose(file);
	*proxy_count = count;
	proxybound_got_chain_data = 1;
}

static void manual_socks5_env(proxy_data *pd, unsigned int *proxy_count, chain_type *ct) {
	char *port_string;
    char *host_string;

	if(proxybound_got_chain_data)
		return;

	init_additional_settings(ct);

    port_string = getenv(PROXYBOUND_SOCKS5_PORT_ENV_VAR);
	if(!port_string)
		return;
    
    host_string = getenv(PROXYBOUND_SOCKS5_HOST_ENV_VAR);
    if(!host_string)
        host_string = "127.0.0.1";

	memset(pd, 0, sizeof(proxy_data));

	pd[0].ps = PLAY_STATE;
	pd[0].ip.as_int = (uint32_t) inet_addr(host_string);
	pd[0].port = htons((unsigned short) strtol(port_string, NULL, 0));
	pd[0].pt = SOCKS5_TYPE;
	proxybound_max_chain = 1;

	if(getenv(PROXYBOUND_FORCE_DNS_ENV_VAR) && (*getenv(PROXYBOUND_FORCE_DNS_ENV_VAR) == '1'))
		proxybound_resolver = 1;

	*proxy_count = 1;
	proxybound_got_chain_data = 1;
}

static int is_dns_port(unsigned short port) {
    if ((port == 53) || (port == 853)) {
        if (proxybound_allow_dns) {PDEBUG("is_dns_port: allowing direct udp dns request on port: %d\n",port);}
        return 1;
    }
    return 0;
}

/**************************************************************************************************************************************************************/
/*******  HOOK FUNCTIONS  *************************************************************************************************************************************/

    // Sock family list (not complete) 
    // AF_UNIX_CCSID    /*     - Unix domain sockets 		*/
    // AF_UNIX          /*  1  - Unix domain sockets        */
    // AF_INET          /*  2  - Internet IP Protocol 	    */
    // AF_INET6         /*  10 - IPv6                       */
    // AF_UNSPEC	    /*  0                               */
    // AF_AX25			/*  3  - Amateur Radio AX.25 		*/
    // AF_IPX		    /*  4  - Novell IPX 			    */
    // AF_APPLETALK		/*  5  - Appletalk DDP 	           	*/
    // AF_NETROM		/*  6  - Amateur radio NetROM 	    */
    // AF_BRIDGE		/*  7  - Multiprotocol bridge 	    */
    // AF_AAL5			/*  8  - Reserved for Werner's ATM 	*/
    // AF_X25			/*  9  - Reserved for X.25 project 	*/
    // AF_MAX			/*  12 - For now..                  */
    // MSG_PROXY        /*  16 - ...                        */
    // PF_FILE          /*  ?? - ...                        */
    // ... 

    //SOCK_STREAM       // 1
    //SOCK_DGRAM        // 2
    //SOCK_SEQPACKET    // 5
    //SOCK_RAW          // 3
    //SOCK_RDM          // 4
    //SOCK_PACKET       // 10
    // ...

    //struct msghdr {
    //    void         *msg_name;       /* optional address */
    //    socklen_t     msg_namelen;    /* size of address */
    //    struct iovec *msg_iov;        /* scatter/gather array */
    //    size_t        msg_iovlen;     /* # elements in msg_iov */
    //    void         *msg_control;    /* ancillary data, see below */
    //    size_t        msg_controllen; /* ancillary data buffer len */
    //    int           msg_flags;      /* flags on received message */
    //};

/**************************************************************************************************************************************************************/

int connect(int sock, const struct sockaddr *addr, socklen_t len) {    
    PDEBUG("\n\n\n\n\n\n\n\n\n\n\n\n...CONNECT........................................................................................................... \n\n");

    if (true_connect == NULL) {
        PDEBUG("violation: connect: rejecting, unresolved symbol: connect\n");
        errno = ECONNREFUSED; return -1;
    }
    
    int socktype = 0, flags = 0, ret = 0;
    socklen_t optlen = 0;
    ip_type dest_ip;
    char ip[256];
    struct in_addr *p_addr_in;
    unsigned short port;
    size_t i;
    int remote_dns_connect = 0;
    INIT();
    optlen = sizeof(socktype);
    getsockopt(sock, SOL_SOCKET, SO_TYPE, &socktype, &optlen);    
    
    if (!socktype) {
        PDEBUG("violation: connect: allowing, no socket_type\n");
        return true_connect(sock, addr, len);
    }
    
    /*if ((SOCKFAMILY(*addr) < 1) && (!proxybound_allow_leak)) {
        PDEBUG("violation: connect: rejecting, unresolved, socket family\n");
        errno = ECONNREFUSED; return -1;
    }*/
    
    //Allow direct unix
    if ((SOCKFAMILY(*addr) != AF_INET) && (SOCKFAMILY(*addr) != AF_INET6) ) {
        PDEBUG("connect: requested SOCK =%d\n",socktype);
        PDEBUG("connect: requested SOCKFAMILY =%d\n",SOCKFAMILY(*addr));
        PDEBUG("-------------------------------------------------\n");
        PDEBUG("connect: allowing non inet connect()\n");
        return true_connect(sock, addr, len);
    }

    p_addr_in = &((struct sockaddr_in *) addr)->sin_addr;
    port = ntohs(((struct sockaddr_in *) addr)->sin_port);
    //inet_ntop - convert IPv4 and IPv6 addresses from binary to text form
    inet_ntop(AF_INET, p_addr_in, ip, sizeof(ip));

    #ifdef DEBUG
    //PDEBUG("connect: localnet: %s\n", inet_ntop(AF_INET, &in_addr_localnet, ip, sizeof(ip)));
    //PDEBUG("connect: netmask: %s\n" , inet_ntop(AF_INET, &in_addr_netmask, ip, sizeof(ip)));    
    if (strlen(ip) == 0) {PDEBUG("violation: connect: null ip\n");} else {PDEBUG("connect: target: %s\n", ip);}
    if (port < 0) {PDEBUG("violation: connect: null port\n");} else {PDEBUG("connect: port: %d\n", port);}    
    #endif

    //Allow direct local 127.x.x.x
    if ((strlen(ip) != 0) && (ip[0] == '1') && (ip[1] == '2') && (ip[2] == '7') && (ip[3] == '.')) {
        PDEBUG("connect: local ip detected... ignoring\n");
        return true_connect(sock, addr, len);
    }
    
	//Check if connect called from proxydns
    remote_dns_connect = (ntohl(p_addr_in->s_addr) >> 24 == remote_dns_subnet);
	for(i = 0; i < num_localnet_addr && !remote_dns_connect; i++) {
		if((localnet_addr[i].in_addr.s_addr & localnet_addr[i].netmask.s_addr) == (p_addr_in->s_addr & localnet_addr[i].netmask.s_addr)) {
			if(!localnet_addr[i].port || localnet_addr[i].port == port) {
				PDEBUG("connect: accessing localnet using true_connect\n");
				return true_connect(sock, addr, len);
			}
		}
	}
    
    //Block unsupported sock
    //WARNING: this block other unrelated network connect  
    if (socktype != SOCK_STREAM) {
        if (proxybound_allow_leak) {
            PDEBUG("connect: allowing leak connect()\n");
            return true_connect(sock, addr, len);
        } else {            
            if (port < 0) {PDEBUG("violation: connect: rejecting leak connect() null port\n"); errno = ECONNREFUSED; return -1;}
            if ((proxybound_allow_dns) && (is_dns_port(port))) {return true_connect(sock, addr, len);}
            PDEBUG("connect: rejecting leak connect()\n"); errno = ECONNREFUSED; return -1;
        }
    }
    
    //Proxify connect
	flags = fcntl(sock, F_GETFL, 0);
	if(flags & O_NONBLOCK) {fcntl(sock, F_SETFL, !O_NONBLOCK);}
	dest_ip.as_int = SOCKADDR(*addr);
	ret = connect_proxy_chain(sock, dest_ip, SOCKPORT(*addr), proxybound_pd, proxybound_proxy_count, proxybound_ct, proxybound_max_chain);

	fcntl(sock, F_SETFL, flags);
	if(ret != SUCCESS) errno = ECONNREFUSED;
	return ret;
}

//int connect(int sock, const struct sockaddr *addr, socklen_t len)
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    PDEBUG("bind: got a bind request ------------------------\n");
    
    if (true_bind == NULL) {
        PDEBUG("violation: bind: rejecting, unresolved symbol: bind\n");
        errno = EFAULT; return -1;
    }
    
    int socktype = 0;
    socklen_t optlen = 0;
    char ip[256];
    struct in_addr *p_addr_in;
    unsigned short port;
    size_t i;
    int remote_dns_bind = 0;
    optlen = sizeof(socktype);
    getsockopt(sockfd, SOL_SOCKET, SO_TYPE, &socktype, &optlen);
    
    if (!socktype) {
        PDEBUG("violation: bind: allowing, no socket_type\n");
        return true_bind(sockfd, addr, addrlen);
    }

    /*if ((SOCKFAMILY(*addr) < 1) && (!proxybound_allow_leak)) {
        PDEBUG("violation: bind: rejecting, unresolved, socket family\n");
        errno = EFAULT; return -1;
    }*/
        
    //Allow direct unix
    if ((SOCKFAMILY(*addr) != AF_INET) && (SOCKFAMILY(*addr) != AF_INET6) ) {
        PDEBUG("bind: allowing non inet sock bind()\n");
        return true_bind(sockfd, addr, addrlen);
    }
    
    p_addr_in = &((struct sockaddr_in *) addr)->sin_addr;
    port = ntohs(((struct sockaddr_in *) addr)->sin_port);
    //inet_ntop - convert IPv4 and IPv6 addresses from binary to text form
    inet_ntop(AF_INET, p_addr_in, ip, sizeof(ip));

    #ifdef DEBUG
    if (strlen(ip) == 0) {PDEBUG("violation: bind: null ip\n");} else {PDEBUG("bind: target: %s\n", ip);}
    if (port < 0) {PDEBUG("violation: bind: null port\n");} else {PDEBUG("bind: port: %d\n", port);}
    PDEBUG("-------------------------------------------------\n");
    #endif

    //Allow direct local 127.x.x.x
    if ((strlen(ip) == 0) && (ip[0] == '1') && (ip[1] == '2') && (ip[2] == '7') && (ip[3] == '.')) {
        PDEBUG("bind: local ip detected... ignoring\n");
        return true_bind(sockfd, addr, addrlen);
    }

	//Check if bind called from proxydns
    remote_dns_bind = (ntohl(p_addr_in->s_addr) >> 24 == remote_dns_subnet);
	for(i = 0; i < num_localnet_addr && !remote_dns_bind; i++) {
		if((localnet_addr[i].in_addr.s_addr & localnet_addr[i].netmask.s_addr) == (p_addr_in->s_addr & localnet_addr[i].netmask.s_addr)) {
			if(!localnet_addr[i].port || localnet_addr[i].port == port) {
				PDEBUG("bind: accessing localnet using true_bind\n");
				return true_bind(sockfd, addr, addrlen);
			}
		}
	}

    #ifdef DEBUG
    PDEBUG("bind() sock SOCK_STREAM = %d\n",SOCK_STREAM);
    PDEBUG("bind() sock SOCK_DGRAM = %d\n",SOCK_DGRAM);
    PDEBUG("bind() sock SOCK_SEQPACKET = %d\n",SOCK_SEQPACKET);
    PDEBUG("bind() sock SOCK_RAW = %d\n",SOCK_RAW);
    PDEBUG("bind() sock SOCK_RDM = %d\n",SOCK_RDM);
    PDEBUG("bind() sock SOCK_PACKET = %d\n",SOCK_PACKET);
    PDEBUG("-------------------------------------------------\n");
    PDEBUG("bind: requested SOCK =%d\n",socktype);
    PDEBUG("bind: requested SOCKFAMILY =%d\n",SOCKFAMILY(*addr));
    PDEBUG("-------------------------------------------------\n");
    #endif

    //Required for proxify, type raw, 0.0.0.0, MSG_PROXY
    /*if ((socktype == SOCK_RAW) && (SOCKFAMILY(*addr) == MSG_PROXY)) {
        if ((ip[0] == '0') && (ip[1] == '.') && (ip[2] == '0') && (ip[3] == '.' ) && (ip[4] == '0') && (ip[5] == '.') && (ip[6] == '0')) {
            PDEBUG("bind: bind allowing, 0.0.0.0, MSG_PROXY...\n");
            return true_bind(sockfd, addr, addrlen);
        }
    }*/

    //Block unsupported sock
    //WARNING: this block other unrelated network connect 
    if ((socktype != SOCK_STREAM) && (!proxybound_allow_leak)) {
        if (port < 0) {PDEBUG("violation: bind: rejecting un-managed protocol bind() null port\n"); errno = EFAULT; return -1;}
        if ((proxybound_allow_dns) && (is_dns_port(port))) {return true_bind(sockfd, addr, addrlen);}
        PDEBUG("bind: rejecting un-managed protocol bind()\n");
        errno = EFAULT; return -1;
    } else {
        return true_bind(sockfd, addr, addrlen);
    }

    if (proxybound_allow_leak) {
        return true_bind(sockfd, addr, addrlen);
    }
    
    PDEBUG("bind: rejecting un-managed protocol bind()\n");
    errno = EFAULT; return -1;
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    PDEBUG("sendmsg: got sendmsg request --------------------\n");
    return true_sendmsg(sockfd, msg, flags);
    
    if (true_sendmsg == NULL) {
        PDEBUG("violation: sendmsg: rejecting, unresolved symbol: sendmsg\n");
        errno = EFAULT; return -1;
    }
    
    int sock_type = -1;
    unsigned int sock_type_len = sizeof(sock_type);    
    const struct sockaddr_in *addr = (const struct sockaddr_in *)msg->msg_name; //optional adress 
    getsockopt(sockfd, SOL_SOCKET, SO_TYPE, (void *) &sock_type, &sock_type_len); //get the type of the socket
    
    if (!sock_type) {
        PDEBUG("violation: sendmsg: allowing unknown sock\n");
        return true_sendmsg(sockfd, msg, flags);
    }
    
    if (addr) {
        if (addr->sin_family) {
            PDEBUG("sendmsg: sock familly %d\n",addr->sin_family);
            if ((addr->sin_family != AF_INET) && (addr->sin_family != AF_INET6)) {
                PDEBUG("sendmsg: allowing non inet\n");
                return true_sendmsg(sockfd, msg, flags);
            }
        } else {
            //This require connect on the first place... 
            PDEBUG("violation: sendmsg: allowing unknown sin_family\n");
            return true_sendmsg(sockfd, msg, flags);
        }
    } else {
        //This require connect on the first place... 
        PDEBUG("violation: sendmsg: allowing unknown addr\n");
        return true_sendmsg(sockfd, msg, flags);
    }

    //Block unsupported sock
    //WARNING: this block other unrelated network connect
    if (sock_type == SOCK_STREAM) {
        PDEBUG("sendmsg: allowing tcp sock stream\n");
        return true_sendmsg(sockfd, msg, flags);
    } else {
        PDEBUG("sendmsg sock SOCK_STREAM = %d\n",SOCK_STREAM);
        PDEBUG("sendmsg sock SOCK_DGRAM = %d\n",SOCK_DGRAM);
        PDEBUG("sendmsg sock SOCK_SEQPACKET = %d\n",SOCK_SEQPACKET);
        PDEBUG("sendmsg sock SOCK_RAW = %d\n",SOCK_RAW);
        PDEBUG("sendmsg sock SOCK_RDM = %d\n",SOCK_RDM);
        PDEBUG("sendmsg sock SOCK_PACKET = %d\n",SOCK_PACKET);
        PDEBUG("-------------------------------------------------\n");
        PDEBUG("sendmsg: sock %d\n",sock_type);
        PDEBUG("-------------------------------------------------\n");
    }
    
    /*if (((sock_type == SOCK_SEQPACKET) || (sock_type == SOCK_SEQPACKET)) && (!addr)) {
        PDEBUG("sendmsg: allowing seqpacket stream\n");        
        return true_sendmsg(sockfd, msg, flags);
    }*/
    
    if (proxybound_allow_leak) {
        PDEBUG("sendmsg: allow leak\n");
        return true_sendmsg(sockfd, msg, flags);
    } else {
        if (addr) {
            char ip[256];
            struct in_addr *p_addr_in;        
            p_addr_in = &((struct sockaddr_in *) addr)->sin_addr;
            inet_ntop(AF_INET, p_addr_in, ip, sizeof(ip)); 
            if (strlen(ip) == 0) {PDEBUG("violation: sendmsg: rejecting null ip\n"); errno = EFAULT; return -1;}
            PDEBUG("sendmsg: ip: %s\n",ip); 
            
            unsigned short port;
            port = ntohs(((struct sockaddr_in *) addr)->sin_port);
            if (port < 0) {PDEBUG("violation: sendmsg: rejecting null port\n"); errno = EFAULT; return -1;}
            PDEBUG("sendmsg: port: %d\n", port);            
            
            //Allow local
            if ((strlen(ip) != 0) && ((ip[0] == '1') && (ip[1] == '2') && (ip[2] == '7') && (ip[3] == '.'))) {
                PDEBUG("sendmsg: allowing local 127.0.0.1\n");
                return true_sendmsg(sockfd, msg, flags);
            }
            
            if ((proxybound_allow_dns) && (is_dns_port(port))) {return true_sendmsg(sockfd, msg, flags);} 
            
            PDEBUG("sendmsg: rejecting c\n");
            errno = EFAULT; return -1;
        }
        PDEBUG("sendmsg: rejecting d\n");
        errno = EFAULT; return -1;
    }
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {  
    PDEBUG("sendto: got sendto request ----------------------\n");
    
    if (true_sendto == NULL) {
        PDEBUG("violation: sendto: rejecting, unresolved symbol: sendto\n");
        errno = EFAULT; return -1;        
    }    
    
    int sock_type = -1;
    unsigned int sock_type_len = sizeof(sock_type);
    getsockopt(sockfd, SOL_SOCKET, SO_TYPE, (void *) &sock_type, &sock_type_len); //get the type of the socket
    
    if (!sock_type) {
        PDEBUG("violation: sendto: allowing, no socket_type\n");
        return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);
    }
    
    struct sockaddr_in *connaddr;
    connaddr = (struct sockaddr_in *) dest_addr;
    
    if (!connaddr) {
        PDEBUG("violation: sendto: null dest_addr\n");        
        //send(sockfd, buf, len, flags) = sendto(sockfd, buf, len, flags, NULL, 0)
        //send require connect on the first place... 
        return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);
    }
    
    if ((connaddr->sin_family != AF_INET) && (connaddr->sin_family != AF_INET6)) {
        PDEBUG("sendto: allowing non inet socket\n");
        return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);
    }
    
    //Block unsupported sock
    //WARNING: this block other unrelated network connect
    if ((sock_type != SOCK_STREAM) && (!proxybound_allow_leak)) {    
        PDEBUG("sendto: is on a udp/unsupported stream\n");     
        PDEBUG("sendto: requested SOCK =%d\n",sock_type);
        char ip[256];
        struct in_addr *p_addr_in;
        unsigned short port;
        p_addr_in = &((struct sockaddr_in *) connaddr)->sin_addr;
        port = ntohs(((struct sockaddr_in *) connaddr)->sin_port);
        inet_ntop(AF_INET, p_addr_in, ip, sizeof(ip));
        
        if (!connaddr->sin_family) {PDEBUG("violation: sendto: rejecting null sin_family\n"); errno = EFAULT; return -1;}
        if (strlen(ip) == 0) {PDEBUG("violation: sendmsg: rejecting null ip\n"); errno = EFAULT; return -1;}
        if (port < 0) {PDEBUG("violation: sendmsg: rejecting null port\n"); errno = EFAULT; return -1;}
        
        PDEBUG("sendto: requested SOCKFAMILY =%d\n",connaddr->sin_family);
        PDEBUG("sendto: ip: %s\n",ip);
        PDEBUG("sendto: port: %d\n", port);
        
        PDEBUG("sendto: -----------------------------------------\n");        
        
        //Allow local
        if ((ip[0] == '1') && (ip[1] == '2') && (ip[2] == '7') && (ip[3] == '.')) {
            PDEBUG("sendto: allowing local 127.0.0.1\n");
            return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);
        }
        
        //Blocking the connection
        if (proxybound_allow_leak) {
            PDEBUG("sendto: allowing udp/unsupported sendto()\n"); 
             return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);
        } else {
            //if (port < 0) {PDEBUG("violation: sendto: rejecting null port\n"); errno = EFAULT; return -1;}
            //if ((proxybound_allow_dns) && (is_dns_port(port))) {return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);} 
            PDEBUG("sendto: rejecting.\n");
            errno = EFAULT; return -1;
        }
    } else {
        return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);
    }    
    
    if (proxybound_allow_leak) {
        return true_sendto(sockfd, buf, len, flags, *dest_addr, addrlen);
    }
    
    PDEBUG("sendto: rejecting.\n");
    errno = EFAULT; return -1;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    //PDEBUG("send: got send request --------------------------\n"); //avoid too talkative logs
    //The send() call may be used only when the socket is in a connected state (so that the intended recipient is known)
    //To avoid any hack this is watched for leak too
    
    /* If the real connect doesn't exist, we're stuffed */
    if (true_send == NULL) {
        PDEBUG("violation: send: rejecting, unresolved symbol: send\n");
        errno = EFAULT; return -1;        
    }
    
    //send require connect/sendto in the first place... 
    return true_send(sockfd, buf, len, flags);
    
    /*if (proxybound_allow_leak) {
        PDEBUG("send: allowing direct send()\n"); 
        return true_send(sockfd, buf, len, flags);
    } else {
        int sock_type = -1;
        unsigned int sock_type_len = sizeof(sock_type);
        getsockopt(sockfd, SOL_SOCKET, SO_TYPE, (void *) &sock_type, &sock_type_len); //Get the type of the socket
        
        if (!sock_type) {
            PDEBUG("violation: send: allowing empty sock_type send()\n");
            return true_send(sockfd, buf, len, flags);
        }
        
        //SOCK_STREAM  SOCK_DGRAM  SOCK_SEQPACKET  SOCK_RAW  SOCK_RDM  SOCK_PACKET
        if (sock_type == SOCK_STREAM) {return true_send(sockfd, buf, len, flags);}
        
        PDEBUG("send: sock SOCK_STREAM = %d\n",SOCK_STREAM);
        PDEBUG("send: sock SOCK_DGRAM = %d\n",SOCK_DGRAM);
        PDEBUG("send: sock SOCK_SEQPACKET = %d\n",SOCK_SEQPACKET);
        PDEBUG("send: sock SOCK_RAW = %d\n",SOCK_RAW);
        PDEBUG("send: sock SOCK_RDM = %d\n",SOCK_RDM);
        PDEBUG("send: sock SOCK_PACKET = %d\n",SOCK_PACKET);
        PDEBUG("-------------------------------------------------\n");
        PDEBUG("send: sock %d\n",sock_type);
        PDEBUG("-------------------------------------------------\n");
        
        PDEBUG("send: rejecting send request unsupported sock\n");
        errno = EFAULT; return -1;
    }*/
}

//TODO: DNS LEAK: OTHER RESOLVER FUNCTION
//=======================================
//realresinit = dlsym(lib, "res_init");
//realresquery = dlsym(lib, "res_query");
//realressend = dlsym(lib, "res_send");
//realresquerydomain = dlsym(lib, "res_querydomain");
//realressearch = dlsym(lib, "res_search");
//realgethostbyaddr = dlsym(lib, "gethostbyaddr"); //Needs rewrite
//realgetipnodebyname = dlsym(lib, "getipnodebyname");

//UDP & DNS LEAK
//==============
//realsendto = dlsym(lib, "sendto");
//realsendmsg = dlsym(lib, "sendmsg");

static struct gethostbyname_data ghbndata;

struct hostent *gethostbyname(const char *name) {
    PDEBUG("gethostbyname: got gethostbyname request --------\n");
    
	INIT();

	PDEBUG("gethostbyname: gethostbyname: %s\n", name);

	if(proxybound_resolver)
		return proxy_gethostbyname(name, &ghbndata);
	else
		return true_gethostbyname(name);

	return NULL;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    PDEBUG("getaddrinfo: got getaddrinfo request ------------\n");
    
	int ret = 0;

	INIT();

	PDEBUG("getaddrinfo: %s %s\n", node, service);

	if(proxybound_resolver)
		ret = proxy_getaddrinfo(node, service, hints, res);
	else
		ret = true_getaddrinfo(node, service, hints, res);

	return ret;
}

void freeaddrinfo(struct addrinfo *res) {
    PDEBUG("freeaddrinfo: got freeaddrinfo request ----------\n");
        
	INIT();

	PDEBUG("freeaddrinfo: %p \n", res);

	if(!proxybound_resolver)
		true_freeaddrinfo(res);
	else
		proxy_freeaddrinfo(res);
	return;
}

int getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags) {
    PDEBUG("getnameinfo: got getnameinfo request ------------\n");
        
	char ip_buf[16];
	int ret = 0;

	INIT();
	
	PDEBUG("getnameinfo: %s %s\n", host, serv);

	if(!proxybound_resolver) {
		ret = true_getnameinfo(sa, salen, host, hostlen, serv, servlen, flags);
	} else {
		if(hostlen) {
			pc_stringfromipv4((unsigned char*) &(SOCKADDR_2(*sa)), ip_buf);
			strncpy(host, ip_buf, hostlen);
		}
		if(servlen)
			snprintf(serv, servlen, "%d", ntohs(SOCKPORT(*sa)));
	}
	return ret;
}

struct hostent *gethostbyaddr(const void *addr, socklen_t len, int type) {    
    PDEBUG("gethostbyaddr: got gethostbyaddr request --------\n");

	static char buf[16];
	static char ipv4[4];
	static char *list[2];
	static char *aliases[1];
	static struct hostent he;

	INIT();

    //TODO: proper gethostbyaddr hook
	PDEBUG("hostent: todo: proper gethostbyaddr hook\n");

	if(!proxybound_resolver)
		return true_gethostbyaddr(addr, len, type);
	else {
		PDEBUG("hostent: len %u\n", len);
		if(len != 4)
			return NULL;
		he.h_name = buf;
		memcpy(ipv4, addr, 4);
		list[0] = ipv4;
		list[1] = NULL;
		he.h_addr_list = list;
		he.h_addrtype = AF_INET;
		aliases[0] = NULL;
		he.h_aliases = aliases;
		he.h_length = 4;
		pc_stringfromipv4((unsigned char *) addr, buf);
		return &he;
	}
	return NULL;
}
