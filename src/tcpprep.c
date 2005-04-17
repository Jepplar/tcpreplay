/* $Id$ */

/*
 * Copyright (c) 2001-2005 Aaron Turner.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright owners nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  Purpose:
 *  1) Remove the performance bottleneck in tcpreplay for choosing an NIC
 *  2) Seperate code to make it more manageable
 *  3) Add addtional features which require multiple passes of a pcap
 *
 *  Support:
 *  Right now we support matching source IP based upon on of the following:
 *  - Regular expression
 *  - IP address is contained in one of a list of CIDR blocks
 *  - Auto learning of CIDR block for servers (clients all other)
 */

#include "config.h"
#include "defines.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <string.h>
#include <unistd.h>

#include "tcpprep.h"
#include "portmap.h"
#include "tcpprep_opts.h"
#include "lib/tree.h"
#include "tree.h"
#include "lib/sll.h"
#include "lib/strlcpy.h"
#include "dlt.h"

/*
 * global variables
 */
#ifdef DEBUG
int debug = 0;
#endif

#ifdef HAVE_TCPDUMP
tcpdump_t tcpdump;
#endif

tcpprep_opt_t options;
int info = 0;
char *ourregex = NULL;
char *cidr = NULL;
data_tree_t treeroot;

static void init(void);
static void post_args(int, char *[]);
static void print_comment(const char *);
static void print_info(const char *);
static int check_ip_regex(const unsigned long ip);
static COUNTER process_raw_packets(pcap_t * pcap);
static int check_dst_port(ip_hdr_t *ip_hdr, int len);


/*
 *  main()
 */
int
main(int argc, char *argv[])
{
    int out_file;
    COUNTER totpackets = 0;
    char errbuf[PCAP_ERRBUF_SIZE];
    int optct = 0;
 
    init();                     /* init our globals */
    
    optct = optionProcess(&tcpprepOptions, argc, argv);
    post_args(argc, argv);

    argc -= optct;
    argv += optct;
 
  
    /* open the cache file */
    if ((out_file = open(OPT_ARG(CACHEFILE), O_WRONLY | O_CREAT | O_TRUNC,
            S_IREAD | S_IWRITE | S_IRGRP | S_IWGRP | S_IROTH)) == -1)
        errx(1, "Unable to open cache file %s for writing: %s", 
            OPT_ARG(CACHEFILE), strerror(errno));

  readpcap:
    /* open the pcap file */
    if ((options.pcap = pcap_open_offline(OPT_ARG(PCAP), errbuf)) == NULL)
        errx(1, "Error opening file: %s", errbuf);

    if ((pcap_datalink(options.pcap) != DLT_EN10MB) &&
        (pcap_datalink(options.pcap) != DLT_LINUX_SLL) &&
        (pcap_datalink(options.pcap) != DLT_RAW) &&
        (pcap_datalink(options.pcap) != DLT_C_HDLC)) {
        errx(1, "Unsupported pcap DLT type: 0x%x", pcap_datalink(options.pcap));
    }

    /* do we apply a bpf filter? */
    if (options.bpf.filter != NULL) {
        if (pcap_compile(options.pcap, &options.bpf.program, options.bpf.filter,
                         options.bpf.optimize, 0) != 0) {
            errx(1, "Error compiling BPF filter: %s", pcap_geterr(options.pcap));
        }
        pcap_setfilter(options.pcap, &options.bpf.program);
    }

    if ((totpackets = process_raw_packets(options.pcap)) == 0) {
        pcap_close(options.pcap);
        err(1, "No packets were processed.  Filter too limiting?");
    }
    pcap_close(options.pcap);


    /* we need to process the pcap file twice in HASH/AUTO mode */
    if (options.mode == AUTO_MODE) {
        options.mode = options.automode;
        if (options.mode == ROUTER_MODE) {  /* do we need to convert TREE->CIDR? */
            if (info)
                fprintf(stderr, "Building network list from pre-cache...\n");
            if (!process_tree()) {
                err(1, "Error: unable to build a valid list of servers. Aborting.");
            }
        }
        else {
            /*
             * in bridge mode we need to calculate client/sever
             * manually since this is done automatically in
             * process_tree()
             */
            tree_calculate(&treeroot);
        }

        if (info)
            fprintf(stderr, "Buliding cache file...\n");
        /* 
         * re-process files, but this time generate
         * cache 
         */
        goto readpcap;
    }
#ifdef DEBUG
    if (debug && (options.cidrdata != NULL))
        print_cidr(options.cidrdata);
#endif

    /* write cache data */
    totpackets = write_cache(options.cachedata, out_file, totpackets, 
        options.comment);
    if (info)
        notice("Done.\nCached " COUNTER_SPEC " packets.\n", totpackets);

    /* close cache file */
    close(out_file);
    return 0;

}


/*
 * checks the dst port to see if this is destined for a server port.
 * returns 1 for true, 0 for false
 */
static int 
check_dst_port(ip_hdr_t *ip_hdr, int len)
{
    tcp_hdr_t *tcp_hdr = NULL;
    udp_hdr_t *udp_hdr = NULL;

    dbg(3, "Checking the destination port...");

    if (ip_hdr->ip_p == IPPROTO_TCP) {
        tcp_hdr = (tcp_hdr_t *)get_layer4(ip_hdr);

        /* is a service? */
        if (options.services.tcp[ntohs(tcp_hdr->th_dport)]) {
            dbg(1, "TCP packet is destined for a server port: %d", ntohs(tcp_hdr->th_dport));
            return 1;
        }

        /* nope */
        dbg(1, "TCP packet is NOT destined for a server port: %d", ntohs(tcp_hdr->th_dport));
        return 0;
    } else if (ip_hdr->ip_p == IPPROTO_UDP) {
        udp_hdr = (udp_hdr_t *)get_layer4(ip_hdr);

        /* is a service? */
        if (options.services.udp[ntohs(udp_hdr->uh_dport)]) {
            dbg(1, "UDP packet is destined for a server port: %d", ntohs(udp_hdr->uh_dport));
            return 1;
        }

        /* nope */
        dbg(1, "UDP packet is NOT destined for a server port: %d", ntohs(udp_hdr->uh_dport));
        return 0;
    }

    
    /* not a TCP or UDP packet... return as non_ip */
    dbg(1, "Packet isn't a UDP or TCP packet... no port to process.");
    return options.nonip;
}


/*
 * checks to see if an ip address matches a regex.  Returns 1 for true
 * 0 for false
 */
static int
check_ip_regex(const unsigned long ip)
{
    int eflags = 0;
    u_char src_ip[16];
    size_t nmatch = 0;
    regmatch_t *pmatch = NULL;

    memset(src_ip, '\0', 16);
    strlcpy((char *)src_ip, (char *)libnet_addr2name4(ip, LIBNET_DONT_RESOLVE),
            sizeof(src_ip));
    if (regexec(&options.preg, (char *)src_ip, nmatch, pmatch, eflags) == 0) {
        return (1);
    }
    else {
        return (0);
    }

}

/*
 * uses libpcap library to parse the packets and build
 * the cache file.
 */
static COUNTER
process_raw_packets(pcap_t * pcap)
{
    ip_hdr_t *ip_hdr = NULL;
    struct pcap_pkthdr pkthdr;
    const u_char *pktdata = NULL;
    COUNTER packetnum = 0;
    int l2len, cache_result = 0;
    u_char ipbuff[MAXPACKET], *buffptr;
#ifdef HAVE_TCPDUMP
    struct pollfd poller[1];
    
    poller[0].fd = tcpdump.outfd;
    poller[0].events = POLLIN;
    poller[0].revents = 0;
#endif
    
    while ((pktdata = pcap_next(pcap, &pkthdr)) != NULL) {
        packetnum++;

        dbg(1, "Packet " COUNTER_SPEC, packetnum);

        /* look for include or exclude LIST match */
        if (options.xX.list != NULL) {
            if (options.xX.mode < xXExclude) {
                if (!check_list(options.xX.list, packetnum)) {
                    add_cache(&options.cachedata, DONT_SEND, 0);
                    continue;
                }
            }
            else if (check_list(options.xX.list, packetnum)) {
                add_cache(&options.cachedata, DONT_SEND, 0);
                continue;
            }
        }

        /* get the IP header (if any) */
        buffptr = ipbuff;
        ip_hdr = (ip_hdr_t *)get_ipv4(pktdata, pkthdr.caplen, pcap_datalink(pcap), &buffptr);
        
        if (ip_hdr == NULL) {
            dbg(2, "Packet isn't IP");

            /* we don't want to cache these packets twice */
            if (options.mode != AUTO_MODE)
                add_cache(&options.cachedata, SEND, options.nonip);

            continue;
        }

        l2len = get_l2len(pktdata, pkthdr.caplen, pcap_datalink(pcap));

        /* look for include or exclude CIDR match */
        if (options.xX.cidr != NULL) {
            if (!process_xX_by_cidr(options.xX.mode, options.xX.cidr, ip_hdr)) {
                add_cache(&options.cachedata, DONT_SEND, 0);
                continue;
            }
        }

        switch (options.mode) {
        case REGEX_MODE:
            cache_result = add_cache(&options.cachedata, SEND, 
                check_ip_regex(ip_hdr->ip_src.s_addr));
            break;
        case CIDR_MODE:
            cache_result = add_cache(&options.cachedata, SEND,
                      check_ip_cidr(options.cidrdata, ip_hdr->ip_src.s_addr));
            break;
        case AUTO_MODE:
            /* first run through in auto mode: create tree */
            add_tree(ip_hdr->ip_src.s_addr, pktdata);
            break;
        case ROUTER_MODE:
            cache_result = add_cache(&options.cachedata, SEND,
                      check_ip_cidr(options.cidrdata, ip_hdr->ip_src.s_addr));
            break;
        case BRIDGE_MODE:
            /*
             * second run through in auto mode: create bridge
             * based cache
             */
            cache_result = add_cache(&options.cachedata, SEND,
                      check_ip_tree(UNKNOWN, ip_hdr->ip_src.s_addr));
            break;
        case SERVER_MODE:
            /* 
             * second run through in auto mode: create bridge
             * where unknowns are servers
             */
            cache_result = add_cache(&options.cachedata, SEND,
                      check_ip_tree(SERVER, ip_hdr->ip_src.s_addr));
            break;
        case CLIENT_MODE:
            /* 
             * second run through in auto mode: create bridge
             * where unknowns are clients
             */
            cache_result = add_cache(&options.cachedata, SEND,
                      check_ip_tree(CLIENT, ip_hdr->ip_src.s_addr));
            break;
        case PORT_MODE:
            /*
             * process ports based on their destination port
             */
            cache_result = add_cache(&options.cachedata, SEND, 
                      check_dst_port(ip_hdr, (pkthdr.caplen - l2len)));
            break;
        }
#ifdef HAVE_TCPDUMP
        if (options.verbose)
            tcpdump_print(&tcpdump, &pkthdr, pktdata);
#endif
    }

    return packetnum;
}

/*
 * init our options
 */
void 
init(void)
{
    int i;

    memset(&options, '\0', sizeof(options));
    options.bpf.optimize = BPF_OPTIMIZE;

    for (i = DEFAULT_LOW_SERVER_PORT; i <= DEFAULT_HIGH_SERVER_PORT; i++) {
        options.services.tcp[i] = 1;
        options.services.udp[i] = 1;
    }

    options.max_mask = DEF_MAX_MASK;
    options.min_mask = DEF_MIN_MASK;
    options.ratio = DEF_RATIO;

}

/* 
 * post process args
 */
static void
post_args(int argc, char *argv[])
{
    char myargs[MYARGS_LEN];
    int i, bufsize;
    char *tempstr;

    memset(myargs, 0, MYARGS_LEN);

#ifdef DEBUG
    if (HAVE_OPT(DBUG))
        debug = OPT_VALUE_DBUG;
#endif

    /* print_comment and print_info don't return */
    if (HAVE_OPT(PRINT_COMMENT))
        print_comment(OPT_ARG(PRINT_COMMENT));

    if (HAVE_OPT(PRINT_INFO))
        print_info(OPT_ARG(PRINT_INFO));

    if (! HAVE_OPT(CACHEFILE) && ! HAVE_OPT(PCAP))
        err(1, "Must specify an output cachefile (-o) and input pcap (-i)");
    
    if (! options.mode)
        err(1, "Must specify a processing mode: -a, -c, -r, -p");

    /* 
     * if we are to include the cli args, then prep it for the
     * cache file header
     */
    if (! options.nocomment) {
        /* copy all of our args to myargs */
        for (i = 1; i < argc; i ++) {
            /* skip the -C <comment> */
            if (strcmp(argv[i], "-C") == 0) {
                i += 2;
                continue;
            }
            
            strlcat(myargs, argv[i], MYARGS_LEN);
            strlcat(myargs, " ", MYARGS_LEN);
        }

        /* remove trailing space */
        myargs[strlen(myargs) - 1] = 0;

        dbg(1, "Comment args length: %d", strlen(myargs));
    }

    /* setup or options.comment buffer so that that we get args\ncomment */
    if (options.comment != NULL) {
        strlcat(myargs, "\n", MYARGS_LEN);
        bufsize = strlen(options.comment) + strlen(myargs) + 1;
        options.comment = (char *)safe_realloc(options.comment, 
            bufsize);
        
        tempstr = strdup(options.comment);
        strlcpy(options.comment, myargs, bufsize);
        strlcat(options.comment, tempstr, bufsize);
    } else {
        bufsize = strlen(myargs) + 1;
        options.comment = (char *)safe_malloc(bufsize);
        strlcpy(options.comment, myargs, bufsize);
    }
        
    dbg(1, "Final comment length: %d", strlen(options.comment));

    /* copy over our min/max mask */
    if (HAVE_OPT(MINMASK))
        options.min_mask = OPT_VALUE_MINMASK;
    
    if (HAVE_OPT(MAXMASK))
        options.max_mask = OPT_VALUE_MAXMASK;
    
    if (! options.min_mask > options.max_mask)
        errx(1, "Min network mask len (%d) must be less then max network mask len (%d)",
        options.min_mask, options.max_mask);
    
    if (options.ratio < 0)
        err(1, "Ratio must be a non-negative number.");
}

/*
 * print the tcpprep cache file comment
 */
static void
print_comment(const char *file)
{
    char *cachedata = NULL;
    char *comment = NULL;
    COUNTER count = 0;

    count = read_cache(&cachedata, file, &comment);
    printf("tcpprep args: %s\n", comment);
    printf("Cache contains data for " COUNTER_SPEC " packets\n", count);

    exit(0);
}

/*
 * prints out the cache file details
 */
static void
print_info(const char *file)
{
    char *cachedata = NULL;
    char *comment = NULL;
    COUNTER count = 0, i;

    count = read_cache(&cachedata, file, &comment);
    for (i = 1; i <= count; i ++) {
        
        switch (check_cache(cachedata, i)) {
        case CACHE_PRIMARY:
            printf("Packet " COUNTER_SPEC " -> Primary\n", i);
            break;
        case CACHE_SECONDARY:
            printf("Packet " COUNTER_SPEC " -> Secondary\n", i);
            break;
        case CACHE_NOSEND:
            printf("Packet " COUNTER_SPEC " -> Don't Send\n", i);
            break;
        default:
            err(1, "Invalid cachedata value!");
            break;
        }

    }
    exit(0);
}

/*
 Local Variables:
 mode:c
 indent-tabs-mode:nil
 c-basic-offset:4
 End:
*/
