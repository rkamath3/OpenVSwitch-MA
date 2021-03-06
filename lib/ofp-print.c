/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012, 2013, 2014, 2015 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "ofp-print.h"

#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>

#include "bundle.h"
#include "byte-order.h"
#include "compiler.h"
#include "dynamic-string.h"
#include "flow.h"
#include "learn.h"
#include "multipath.h"
#include "meta-flow.h"
#include "netdev.h"
#include "nx-match.h"
#include "ofp-actions.h"
#include "ofpbuf.h"
#include "ofp-errors.h"
#include "ofp-msgs.h"
#include "ofp-util.h"
#include "openflow/openflow.h"
#include "openflow/nicira-ext.h"
#include "packets.h"
#include "dp-packet.h"
#include "type-props.h"
#include "unaligned.h"
#include "odp-util.h"
#include "util.h"

static void ofp_print_queue_name(struct ds *string, uint32_t port);
static void ofp_print_error(struct ds *, enum ofperr);

/* Returns a string that represents the contents of the Ethernet frame in the
 * 'len' bytes starting at 'data'.  The caller must free the returned string.*/
char *
ofp_packet_to_string(const void *data, size_t len)
{
    struct ds ds = DS_EMPTY_INITIALIZER;
    struct dp_packet buf;
    struct flow flow;
    size_t l4_size;

    dp_packet_use_const(&buf, data, len);
    flow_extract(&buf, &flow);
    flow_format(&ds, &flow);

    l4_size = dp_packet_l4_size(&buf);

    if (flow.nw_proto == IPPROTO_TCP && l4_size >= TCP_HEADER_LEN) {
        struct tcp_header *th = dp_packet_l4(&buf);
        ds_put_format(&ds, " tcp_csum:%"PRIx16, ntohs(th->tcp_csum));
    } else if (flow.nw_proto == IPPROTO_UDP && l4_size >= UDP_HEADER_LEN) {
        struct udp_header *uh = dp_packet_l4(&buf);
        ds_put_format(&ds, " udp_csum:%"PRIx16, ntohs(uh->udp_csum));
    } else if (flow.nw_proto == IPPROTO_SCTP && l4_size >= SCTP_HEADER_LEN) {
        struct sctp_header *sh = dp_packet_l4(&buf);
        ds_put_format(&ds, " sctp_csum:%"PRIx32,
                      ntohl(get_16aligned_be32(&sh->sctp_csum)));
    } else if (flow.nw_proto == IPPROTO_ICMP && l4_size >= ICMP_HEADER_LEN) {
        struct icmp_header *icmph = dp_packet_l4(&buf);
        ds_put_format(&ds, " icmp_csum:%"PRIx16,
                      ntohs(icmph->icmp_csum));
    } else if (flow.nw_proto == IPPROTO_ICMPV6 && l4_size >= ICMP6_HEADER_LEN) {
        struct icmp6_header *icmp6h = dp_packet_l4(&buf);
        ds_put_format(&ds, " icmp6_csum:%"PRIx16,
                      ntohs(icmp6h->icmp6_cksum));
    }

    ds_put_char(&ds, '\n');

    return ds_cstr(&ds);
}

static void
ofp_print_packet_in(struct ds *string, const struct ofp_header *oh,
                    int verbosity)
{
    char reasonbuf[OFPUTIL_PACKET_IN_REASON_BUFSIZE];
    struct ofputil_packet_in pin;
    int error;

    error = ofputil_decode_packet_in(&pin, oh);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    if (pin.table_id) {
        ds_put_format(string, " table_id=%"PRIu8, pin.table_id);
    }

    if (pin.cookie != OVS_BE64_MAX) {
        ds_put_format(string, " cookie=0x%"PRIx64, ntohll(pin.cookie));
    }

    ds_put_format(string, " total_len=%"PRIuSIZE" ", pin.total_len);

    match_format(&pin.flow_metadata, string, OFP_DEFAULT_PRIORITY);

    ds_put_format(string, " (via %s)",
                  ofputil_packet_in_reason_to_string(pin.reason, reasonbuf,
                                                     sizeof reasonbuf));

    ds_put_format(string, " data_len=%"PRIuSIZE, pin.packet_len);
    if (pin.buffer_id == UINT32_MAX) {
        ds_put_format(string, " (unbuffered)");
        if (pin.total_len != pin.packet_len) {
            ds_put_format(string, " (***total_len != data_len***)");
        }
    } else {
        ds_put_format(string, " buffer=0x%08"PRIx32, pin.buffer_id);
        if (pin.total_len < pin.packet_len) {
            ds_put_format(string, " (***total_len < data_len***)");
        }
    }
    ds_put_char(string, '\n');

    if (verbosity > 0) {
        char *packet = ofp_packet_to_string(pin.packet, pin.packet_len);
        ds_put_cstr(string, packet);
        free(packet);
    }
    if (verbosity > 2) {
        ds_put_hex_dump(string, pin.packet, pin.packet_len, 0, false);
    }
}

static void
ofp_print_packet_out(struct ds *string, const struct ofp_header *oh,
                     int verbosity)
{
    struct ofputil_packet_out po;
    struct ofpbuf ofpacts;
    enum ofperr error;

    ofpbuf_init(&ofpacts, 64);
    error = ofputil_decode_packet_out(&po, oh, &ofpacts);
    if (error) {
        ofpbuf_uninit(&ofpacts);
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " in_port=");
    ofputil_format_port(po.in_port, string);

    ds_put_cstr(string, " actions=");
    ofpacts_format(po.ofpacts, po.ofpacts_len, string);

    if (po.buffer_id == UINT32_MAX) {
        ds_put_format(string, " data_len=%"PRIuSIZE, po.packet_len);
        if (verbosity > 0 && po.packet_len > 0) {
            char *packet = ofp_packet_to_string(po.packet, po.packet_len);
            ds_put_char(string, '\n');
            ds_put_cstr(string, packet);
            free(packet);
        }
        if (verbosity > 2) {
            ds_put_hex_dump(string, po.packet, po.packet_len, 0, false);
        }
    } else {
        ds_put_format(string, " buffer=0x%08"PRIx32, po.buffer_id);
    }

    ofpbuf_uninit(&ofpacts);
}

/* qsort comparison function. */
static int
compare_ports(const void *a_, const void *b_)
{
    const struct ofputil_phy_port *a = a_;
    const struct ofputil_phy_port *b = b_;
    uint16_t ap = ofp_to_u16(a->port_no);
    uint16_t bp = ofp_to_u16(b->port_no);

    return ap < bp ? -1 : ap > bp;
}

static void
ofp_print_bit_names(struct ds *string, uint32_t bits,
                    const char *(*bit_to_name)(uint32_t bit),
                    char separator)
{
    int n = 0;
    int i;

    if (!bits) {
        ds_put_cstr(string, "0");
        return;
    }

    for (i = 0; i < 32; i++) {
        uint32_t bit = UINT32_C(1) << i;

        if (bits & bit) {
            const char *name = bit_to_name(bit);
            if (name) {
                if (n++) {
                    ds_put_char(string, separator);
                }
                ds_put_cstr(string, name);
                bits &= ~bit;
            }
        }
    }

    if (bits) {
        if (n) {
            ds_put_char(string, separator);
        }
        ds_put_format(string, "0x%"PRIx32, bits);
    }
}

static const char *
netdev_feature_to_name(uint32_t bit)
{
    enum netdev_features f = bit;

    switch (f) {
    case NETDEV_F_10MB_HD:    return "10MB-HD";
    case NETDEV_F_10MB_FD:    return "10MB-FD";
    case NETDEV_F_100MB_HD:   return "100MB-HD";
    case NETDEV_F_100MB_FD:   return "100MB-FD";
    case NETDEV_F_1GB_HD:     return "1GB-HD";
    case NETDEV_F_1GB_FD:     return "1GB-FD";
    case NETDEV_F_10GB_FD:    return "10GB-FD";
    case NETDEV_F_40GB_FD:    return "40GB-FD";
    case NETDEV_F_100GB_FD:   return "100GB-FD";
    case NETDEV_F_1TB_FD:     return "1TB-FD";
    case NETDEV_F_OTHER:      return "OTHER";
    case NETDEV_F_COPPER:     return "COPPER";
    case NETDEV_F_FIBER:      return "FIBER";
    case NETDEV_F_AUTONEG:    return "AUTO_NEG";
    case NETDEV_F_PAUSE:      return "AUTO_PAUSE";
    case NETDEV_F_PAUSE_ASYM: return "AUTO_PAUSE_ASYM";
    }

    return NULL;
}

static void
ofp_print_port_features(struct ds *string, enum netdev_features features)
{
    ofp_print_bit_names(string, features, netdev_feature_to_name, ' ');
    ds_put_char(string, '\n');
}

static const char *
ofputil_port_config_to_name(uint32_t bit)
{
    enum ofputil_port_config pc = bit;

    switch (pc) {
    case OFPUTIL_PC_PORT_DOWN:    return "PORT_DOWN";
    case OFPUTIL_PC_NO_STP:       return "NO_STP";
    case OFPUTIL_PC_NO_RECV:      return "NO_RECV";
    case OFPUTIL_PC_NO_RECV_STP:  return "NO_RECV_STP";
    case OFPUTIL_PC_NO_FLOOD:     return "NO_FLOOD";
    case OFPUTIL_PC_NO_FWD:       return "NO_FWD";
    case OFPUTIL_PC_NO_PACKET_IN: return "NO_PACKET_IN";
    }

    return NULL;
}

static void
ofp_print_port_config(struct ds *string, enum ofputil_port_config config)
{
    ofp_print_bit_names(string, config, ofputil_port_config_to_name, ' ');
    ds_put_char(string, '\n');
}

static const char *
ofputil_port_state_to_name(uint32_t bit)
{
    enum ofputil_port_state ps = bit;

    switch (ps) {
    case OFPUTIL_PS_LINK_DOWN: return "LINK_DOWN";
    case OFPUTIL_PS_BLOCKED:   return "BLOCKED";
    case OFPUTIL_PS_LIVE:      return "LIVE";

    case OFPUTIL_PS_STP_LISTEN:
    case OFPUTIL_PS_STP_LEARN:
    case OFPUTIL_PS_STP_FORWARD:
    case OFPUTIL_PS_STP_BLOCK:
        /* Handled elsewhere. */
        return NULL;
    }

    return NULL;
}

static void
ofp_print_port_state(struct ds *string, enum ofputil_port_state state)
{
    enum ofputil_port_state stp_state;

    /* The STP state is a 2-bit field so it doesn't fit in with the bitmask
     * pattern.  We have to special case it.
     *
     * OVS doesn't support STP, so this field will always be 0 if we are
     * talking to OVS, so we'd always print STP_LISTEN in that case.
     * Therefore, we don't print anything at all if the value is STP_LISTEN, to
     * avoid confusing users. */
    stp_state = state & OFPUTIL_PS_STP_MASK;
    if (stp_state) {
        ds_put_cstr(string,
                    (stp_state == OFPUTIL_PS_STP_LEARN ? "STP_LEARN"
                     : stp_state == OFPUTIL_PS_STP_FORWARD ? "STP_FORWARD"
                     : "STP_BLOCK"));
        state &= ~OFPUTIL_PS_STP_MASK;
        if (state) {
            ofp_print_bit_names(string, state, ofputil_port_state_to_name,
                                ' ');
        }
    } else {
        ofp_print_bit_names(string, state, ofputil_port_state_to_name, ' ');
    }
    ds_put_char(string, '\n');
}

static void
ofp_print_phy_port(struct ds *string, const struct ofputil_phy_port *port)
{
    char name[sizeof port->name];
    int j;

    memcpy(name, port->name, sizeof name);
    for (j = 0; j < sizeof name - 1; j++) {
        if (!isprint((unsigned char) name[j])) {
            break;
        }
    }
    name[j] = '\0';

    ds_put_char(string, ' ');
    ofputil_format_port(port->port_no, string);
    ds_put_format(string, "(%s): addr:"ETH_ADDR_FMT"\n",
                  name, ETH_ADDR_ARGS(port->hw_addr));

    ds_put_cstr(string, "     config:     ");
    ofp_print_port_config(string, port->config);

    ds_put_cstr(string, "     state:      ");
    ofp_print_port_state(string, port->state);

    if (port->curr) {
        ds_put_format(string, "     current:    ");
        ofp_print_port_features(string, port->curr);
    }
    if (port->advertised) {
        ds_put_format(string, "     advertised: ");
        ofp_print_port_features(string, port->advertised);
    }
    if (port->supported) {
        ds_put_format(string, "     supported:  ");
        ofp_print_port_features(string, port->supported);
    }
    if (port->peer) {
        ds_put_format(string, "     peer:       ");
        ofp_print_port_features(string, port->peer);
    }
    ds_put_format(string, "     speed: %"PRIu32" Mbps now, "
                  "%"PRIu32" Mbps max\n",
                  port->curr_speed / UINT32_C(1000),
                  port->max_speed / UINT32_C(1000));
}

/* Given a buffer 'b' that contains an array of OpenFlow ports of type
 * 'ofp_version', writes a detailed description of each port into
 * 'string'. */
static void
ofp_print_phy_ports(struct ds *string, uint8_t ofp_version,
                    struct ofpbuf *b)
{
    struct ofputil_phy_port *ports;
    size_t allocated_ports, n_ports;
    int retval;
    size_t i;

    ports = NULL;
    allocated_ports = 0;
    for (n_ports = 0; ; n_ports++) {
        if (n_ports >= allocated_ports) {
            ports = x2nrealloc(ports, &allocated_ports, sizeof *ports);
        }

        retval = ofputil_pull_phy_port(ofp_version, b, &ports[n_ports]);
        if (retval) {
            break;
        }
    }

    qsort(ports, n_ports, sizeof *ports, compare_ports);
    for (i = 0; i < n_ports; i++) {
        ofp_print_phy_port(string, &ports[i]);
    }
    free(ports);

    if (retval != EOF) {
        ofp_print_error(string, retval);
    }
}

static const char *
ofputil_capabilities_to_name(uint32_t bit)
{
    enum ofputil_capabilities capabilities = bit;

    switch (capabilities) {
    case OFPUTIL_C_FLOW_STATS:   return "FLOW_STATS";
    case OFPUTIL_C_TABLE_STATS:  return "TABLE_STATS";
    case OFPUTIL_C_PORT_STATS:   return "PORT_STATS";
    case OFPUTIL_C_IP_REASM:     return "IP_REASM";
    case OFPUTIL_C_QUEUE_STATS:  return "QUEUE_STATS";
    case OFPUTIL_C_ARP_MATCH_IP: return "ARP_MATCH_IP";
    case OFPUTIL_C_STP:          return "STP";
    case OFPUTIL_C_GROUP_STATS:  return "GROUP_STATS";
    case OFPUTIL_C_PORT_BLOCKED: return "PORT_BLOCKED";
    }

    return NULL;
}

static void
ofp_print_switch_features(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_switch_features features;
    enum ofperr error;
    struct ofpbuf b;

    error = ofputil_decode_switch_features(oh, &features, &b);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_format(string, " dpid:%016"PRIx64"\n", features.datapath_id);

    ds_put_format(string, "n_tables:%"PRIu8", n_buffers:%"PRIu32,
                  features.n_tables, features.n_buffers);
    if (features.auxiliary_id) {
        ds_put_format(string, ", auxiliary_id:%"PRIu8, features.auxiliary_id);
    }
    ds_put_char(string, '\n');

    ds_put_cstr(string, "capabilities: ");
    ofp_print_bit_names(string, features.capabilities,
                        ofputil_capabilities_to_name, ' ');
    ds_put_char(string, '\n');

    switch ((enum ofp_version)oh->version) {
    case OFP10_VERSION:
        ds_put_cstr(string, "actions: ");
        ofpact_bitmap_format(features.ofpacts, string);
        ds_put_char(string, '\n');
        break;
    case OFP11_VERSION:
    case OFP12_VERSION:
        break;
    case OFP13_VERSION:
    case OFP14_VERSION:
    case OFP15_VERSION:
        return; /* no ports in ofp13_switch_features */
    default:
        OVS_NOT_REACHED();
    }

    ofp_print_phy_ports(string, oh->version, &b);
}

static void
ofp_print_switch_config(struct ds *string, const struct ofp_switch_config *osc)
{
    enum ofp_config_flags flags;

    flags = ntohs(osc->flags);

    ds_put_format(string, " frags=%s", ofputil_frag_handling_to_string(flags));
    flags &= ~OFPC_FRAG_MASK;

    if (flags & OFPC_INVALID_TTL_TO_CONTROLLER) {
        ds_put_format(string, " invalid_ttl_to_controller");
        flags &= ~OFPC_INVALID_TTL_TO_CONTROLLER;
    }

    if (flags) {
        ds_put_format(string, " ***unknown flags 0x%04"PRIx16"***", flags);
    }

    ds_put_format(string, " miss_send_len=%"PRIu16"\n", ntohs(osc->miss_send_len));
}

static void print_wild(struct ds *string, const char *leader, int is_wild,
            int verbosity, const char *format, ...)
            OVS_PRINTF_FORMAT(5, 6);

static void print_wild(struct ds *string, const char *leader, int is_wild,
                       int verbosity, const char *format, ...)
{
    if (is_wild && verbosity < 2) {
        return;
    }
    ds_put_cstr(string, leader);
    if (!is_wild) {
        va_list args;

        va_start(args, format);
        ds_put_format_valist(string, format, args);
        va_end(args);
    } else {
        ds_put_char(string, '*');
    }
    ds_put_char(string, ',');
}

static void
print_wild_port(struct ds *string, const char *leader, int is_wild,
                int verbosity, ofp_port_t port)
{
    if (is_wild && verbosity < 2) {
        return;
    }
    ds_put_cstr(string, leader);
    if (!is_wild) {
        ofputil_format_port(port, string);
    } else {
        ds_put_char(string, '*');
    }
    ds_put_char(string, ',');
}

static void
print_ip_netmask(struct ds *string, const char *leader, ovs_be32 ip,
                 uint32_t wild_bits, int verbosity)
{
    if (wild_bits >= 32 && verbosity < 2) {
        return;
    }
    ds_put_cstr(string, leader);
    if (wild_bits < 32) {
        ds_put_format(string, IP_FMT, IP_ARGS(ip));
        if (wild_bits) {
            ds_put_format(string, "/%d", 32 - wild_bits);
        }
    } else {
        ds_put_char(string, '*');
    }
    ds_put_char(string, ',');
}

void
ofp10_match_print(struct ds *f, const struct ofp10_match *om, int verbosity)
{
    char *s = ofp10_match_to_string(om, verbosity);
    ds_put_cstr(f, s);
    free(s);
}

char *
ofp10_match_to_string(const struct ofp10_match *om, int verbosity)
{
    struct ds f = DS_EMPTY_INITIALIZER;
    uint32_t w = ntohl(om->wildcards);
    bool skip_type = false;
    bool skip_proto = false;

    if (!(w & OFPFW10_DL_TYPE)) {
        skip_type = true;
        if (om->dl_type == htons(ETH_TYPE_IP)) {
            if (!(w & OFPFW10_NW_PROTO)) {
                skip_proto = true;
                if (om->nw_proto == IPPROTO_ICMP) {
                    ds_put_cstr(&f, "icmp,");
                } else if (om->nw_proto == IPPROTO_TCP) {
                    ds_put_cstr(&f, "tcp,");
                } else if (om->nw_proto == IPPROTO_UDP) {
                    ds_put_cstr(&f, "udp,");
                } else if (om->nw_proto == IPPROTO_SCTP) {
                    ds_put_cstr(&f, "sctp,");
                } else {
                    ds_put_cstr(&f, "ip,");
                    skip_proto = false;
                }
            } else {
                ds_put_cstr(&f, "ip,");
            }
        } else if (om->dl_type == htons(ETH_TYPE_ARP)) {
            ds_put_cstr(&f, "arp,");
        } else if (om->dl_type == htons(ETH_TYPE_RARP)){
            ds_put_cstr(&f, "rarp,");
        } else if (om->dl_type == htons(ETH_TYPE_MPLS)) {
            ds_put_cstr(&f, "mpls,");
        } else if (om->dl_type == htons(ETH_TYPE_MPLS_MCAST)) {
            ds_put_cstr(&f, "mplsm,");
        } else {
            skip_type = false;
        }
    }
    print_wild_port(&f, "in_port=", w & OFPFW10_IN_PORT, verbosity,
                    u16_to_ofp(ntohs(om->in_port)));
    print_wild(&f, "dl_vlan=", w & OFPFW10_DL_VLAN, verbosity,
               "%d", ntohs(om->dl_vlan));
    print_wild(&f, "dl_vlan_pcp=", w & OFPFW10_DL_VLAN_PCP, verbosity,
               "%d", om->dl_vlan_pcp);
    print_wild(&f, "dl_src=", w & OFPFW10_DL_SRC, verbosity,
               ETH_ADDR_FMT, ETH_ADDR_ARGS(om->dl_src));
    print_wild(&f, "dl_dst=", w & OFPFW10_DL_DST, verbosity,
               ETH_ADDR_FMT, ETH_ADDR_ARGS(om->dl_dst));
    if (!skip_type) {
        print_wild(&f, "dl_type=", w & OFPFW10_DL_TYPE, verbosity,
                   "0x%04x", ntohs(om->dl_type));
    }
    print_ip_netmask(&f, "nw_src=", om->nw_src,
                     (w & OFPFW10_NW_SRC_MASK) >> OFPFW10_NW_SRC_SHIFT,
                     verbosity);
    print_ip_netmask(&f, "nw_dst=", om->nw_dst,
                     (w & OFPFW10_NW_DST_MASK) >> OFPFW10_NW_DST_SHIFT,
                     verbosity);
    if (!skip_proto) {
        if (om->dl_type == htons(ETH_TYPE_ARP) ||
            om->dl_type == htons(ETH_TYPE_RARP)) {
            print_wild(&f, "arp_op=", w & OFPFW10_NW_PROTO, verbosity,
                       "%u", om->nw_proto);
        } else {
            print_wild(&f, "nw_proto=", w & OFPFW10_NW_PROTO, verbosity,
                       "%u", om->nw_proto);
        }
    }
    print_wild(&f, "nw_tos=", w & OFPFW10_NW_TOS, verbosity,
               "%u", om->nw_tos);
    if (om->nw_proto == IPPROTO_ICMP) {
        print_wild(&f, "icmp_type=", w & OFPFW10_ICMP_TYPE, verbosity,
                   "%d", ntohs(om->tp_src));
        print_wild(&f, "icmp_code=", w & OFPFW10_ICMP_CODE, verbosity,
                   "%d", ntohs(om->tp_dst));
    } else {
        print_wild(&f, "tp_src=", w & OFPFW10_TP_SRC, verbosity,
                   "%d", ntohs(om->tp_src));
        print_wild(&f, "tp_dst=", w & OFPFW10_TP_DST, verbosity,
                   "%d", ntohs(om->tp_dst));
    }
    ds_chomp(&f, ',');
    return ds_cstr(&f);
}

static void
ofp_print_flow_flags(struct ds *s, enum ofputil_flow_mod_flags flags)
{
    if (flags & OFPUTIL_FF_SEND_FLOW_REM) {
        ds_put_cstr(s, "send_flow_rem ");
    }
    if (flags & OFPUTIL_FF_CHECK_OVERLAP) {
        ds_put_cstr(s, "check_overlap ");
    }
    if (flags & OFPUTIL_FF_RESET_COUNTS) {
        ds_put_cstr(s, "reset_counts ");
    }
    if (flags & OFPUTIL_FF_NO_PKT_COUNTS) {
        ds_put_cstr(s, "no_packet_counts ");
    }
    if (flags & OFPUTIL_FF_NO_BYT_COUNTS) {
        ds_put_cstr(s, "no_byte_counts ");
    }
    if (flags & OFPUTIL_FF_HIDDEN_FIELDS) {
        ds_put_cstr(s, "allow_hidden_fields ");
    }
    if (flags & OFPUTIL_FF_NO_READONLY) {
        ds_put_cstr(s, "no_readonly_table ");
    }
}

static void
ofp_print_flow_mod(struct ds *s, const struct ofp_header *oh, int verbosity)
{
    struct ofputil_flow_mod fm;
    struct ofpbuf ofpacts;
    bool need_priority;
    enum ofperr error;
    enum ofpraw raw;
    enum ofputil_protocol protocol;

    protocol = ofputil_protocol_from_ofp_version(oh->version);
    protocol = ofputil_protocol_set_tid(protocol, true);

    ofpbuf_init(&ofpacts, 64);
    error = ofputil_decode_flow_mod(&fm, oh, protocol, &ofpacts,
                                    OFPP_MAX, 255);
    if (error) {
        ofpbuf_uninit(&ofpacts);
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, ' ');
    switch (fm.command) {
    case OFPFC_ADD:
        ds_put_cstr(s, "ADD");
        break;
    case OFPFC_MODIFY:
        ds_put_cstr(s, "MOD");
        break;
    case OFPFC_MODIFY_STRICT:
        ds_put_cstr(s, "MOD_STRICT");
        break;
    case OFPFC_DELETE:
        ds_put_cstr(s, "DEL");
        break;
    case OFPFC_DELETE_STRICT:
        ds_put_cstr(s, "DEL_STRICT");
        break;
    default:
        ds_put_format(s, "cmd:%d", fm.command);
    }
    if (fm.table_id != 0) {
        ds_put_format(s, " table:%d", fm.table_id);
    }

    ds_put_char(s, ' ');
    ofpraw_decode(&raw, oh);
    if (verbosity >= 3 && raw == OFPRAW_OFPT10_FLOW_MOD) {
        const struct ofp10_flow_mod *ofm = ofpmsg_body(oh);
        ofp10_match_print(s, &ofm->match, verbosity);

        /* ofp_print_match() doesn't print priority. */
        need_priority = true;
    } else if (verbosity >= 3 && raw == OFPRAW_NXT_FLOW_MOD) {
        const struct nx_flow_mod *nfm = ofpmsg_body(oh);
        const void *nxm = nfm + 1;
        char *nxm_s;

        nxm_s = nx_match_to_string(nxm, ntohs(nfm->match_len));
        ds_put_cstr(s, nxm_s);
        free(nxm_s);

        /* nx_match_to_string() doesn't print priority. */
        need_priority = true;
    } else {
        match_format(&fm.match, s, fm.priority);

        /* match_format() does print priority. */
        need_priority = false;
    }

    if (ds_last(s) != ' ') {
        ds_put_char(s, ' ');
    }
    if (fm.new_cookie != htonll(0) && fm.new_cookie != OVS_BE64_MAX) {
        ds_put_format(s, "cookie:0x%"PRIx64" ", ntohll(fm.new_cookie));
    }
    if (fm.cookie_mask != htonll(0)) {
        ds_put_format(s, "cookie:0x%"PRIx64"/0x%"PRIx64" ",
                ntohll(fm.cookie), ntohll(fm.cookie_mask));
    }
    if (fm.idle_timeout != OFP_FLOW_PERMANENT) {
        ds_put_format(s, "idle:%"PRIu16" ", fm.idle_timeout);
    }
    if (fm.hard_timeout != OFP_FLOW_PERMANENT) {
        ds_put_format(s, "hard:%"PRIu16" ", fm.hard_timeout);
    }
    if (fm.importance != 0) {
        ds_put_format(s, "importance:%"PRIu16" ", fm.importance);
    }
    if (fm.priority != OFP_DEFAULT_PRIORITY && need_priority) {
        ds_put_format(s, "pri:%"PRIu16" ", fm.priority);
    }
    if (fm.buffer_id != UINT32_MAX) {
        ds_put_format(s, "buf:0x%"PRIx32" ", fm.buffer_id);
    }
    if (fm.out_port != OFPP_ANY) {
        ds_put_format(s, "out_port:");
        ofputil_format_port(fm.out_port, s);
        ds_put_char(s, ' ');
    }

    if (oh->version == OFP10_VERSION || oh->version == OFP11_VERSION) {
        /* Don't print the reset_counts flag for OF1.0 and OF1.1 because those
         * versions don't really have such a flag and printing one is likely to
         * confuse people. */
        fm.flags &= ~OFPUTIL_FF_RESET_COUNTS;
    }
    ofp_print_flow_flags(s, fm.flags);

    ds_put_cstr(s, "actions=");
    ofpacts_format(fm.ofpacts, fm.ofpacts_len, s);
    ofpbuf_uninit(&ofpacts);
}

static void
ofp_print_duration(struct ds *string, unsigned int sec, unsigned int nsec)
{
    ds_put_format(string, "%u", sec);

    /* If there are no fractional seconds, don't print any decimals.
     *
     * If the fractional seconds can be expressed exactly as milliseconds,
     * print 3 decimals.  Open vSwitch provides millisecond precision for most
     * time measurements, so printing 3 decimals every time makes it easier to
     * spot real changes in flow dumps that refresh themselves quickly.
     *
     * If the fractional seconds are more precise than milliseconds, print the
     * number of decimals needed to express them exactly.
     */
    if (nsec > 0) {
        unsigned int msec = nsec / 1000000;
        if (msec * 1000000 == nsec) {
            ds_put_format(string, ".%03u", msec);
        } else {
            ds_put_format(string, ".%09u", nsec);
            while (string->string[string->length - 1] == '0') {
                string->length--;
            }
        }
    }
    ds_put_char(string, 's');
}

/* Returns a string form of 'reason'.  The return value is either a statically
 * allocated constant string or the 'bufsize'-byte buffer 'reasonbuf'.
 * 'bufsize' should be at least OFP_FLOW_REMOVED_REASON_BUFSIZE. */
#define OFP_FLOW_REMOVED_REASON_BUFSIZE (INT_STRLEN(int) + 1)
static const char *
ofp_flow_removed_reason_to_string(enum ofp_flow_removed_reason reason,
                                  char *reasonbuf, size_t bufsize)
{
    switch (reason) {
    case OFPRR_IDLE_TIMEOUT:
        return "idle";
    case OFPRR_HARD_TIMEOUT:
        return "hard";
    case OFPRR_DELETE:
        return "delete";
    case OFPRR_GROUP_DELETE:
        return "group_delete";
    case OFPRR_EVICTION:
        return "eviction";
    case OFPRR_METER_DELETE:
        return "meter_delete";
    case OVS_OFPRR_NONE:
    default:
        snprintf(reasonbuf, bufsize, "%d", (int) reason);
        return reasonbuf;
    }
}

static void
ofp_print_flow_removed(struct ds *string, const struct ofp_header *oh)
{
    char reasonbuf[OFP_FLOW_REMOVED_REASON_BUFSIZE];
    struct ofputil_flow_removed fr;
    enum ofperr error;

    error = ofputil_decode_flow_removed(&fr, oh);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_char(string, ' ');
    match_format(&fr.match, string, fr.priority);

    ds_put_format(string, " reason=%s",
                  ofp_flow_removed_reason_to_string(fr.reason, reasonbuf,
                                                    sizeof reasonbuf));

    if (fr.table_id != 255) {
        ds_put_format(string, " table_id=%"PRIu8, fr.table_id);
    }

    if (fr.cookie != htonll(0)) {
        ds_put_format(string, " cookie:0x%"PRIx64, ntohll(fr.cookie));
    }
    ds_put_cstr(string, " duration");
    ofp_print_duration(string, fr.duration_sec, fr.duration_nsec);
    ds_put_format(string, " idle%"PRIu16, fr.idle_timeout);
    if (fr.hard_timeout) {
        /* The hard timeout was only added in OF1.2, so only print it if it is
         * actually in use to avoid gratuitous change to the formatting. */
        ds_put_format(string, " hard%"PRIu16, fr.hard_timeout);
    }
    ds_put_format(string, " pkts%"PRIu64" bytes%"PRIu64"\n",
                  fr.packet_count, fr.byte_count);
}

static void
ofp_print_port_mod(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_port_mod pm;
    enum ofperr error;

    error = ofputil_decode_port_mod(oh, &pm, true);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, "port: ");
    ofputil_format_port(pm.port_no, string);
    ds_put_format(string, ": addr:"ETH_ADDR_FMT"\n",
                  ETH_ADDR_ARGS(pm.hw_addr));

    ds_put_cstr(string, "     config: ");
    ofp_print_port_config(string, pm.config);

    ds_put_cstr(string, "     mask:   ");
    ofp_print_port_config(string, pm.mask);

    ds_put_cstr(string, "     advertise: ");
    if (pm.advertise) {
        ofp_print_port_features(string, pm.advertise);
    } else {
        ds_put_cstr(string, "UNCHANGED\n");
    }
}

static const char *
ofputil_table_miss_to_string(enum ofputil_table_miss miss)
{
    switch (miss) {
    case OFPUTIL_TABLE_MISS_DEFAULT: return "default";
    case OFPUTIL_TABLE_MISS_CONTROLLER: return "controller";
    case OFPUTIL_TABLE_MISS_CONTINUE: return "continue";
    case OFPUTIL_TABLE_MISS_DROP: return "drop";
    default: return "***error***";
    }
}

static const char *
ofputil_table_eviction_to_string(enum ofputil_table_eviction eviction)
{
    switch (eviction) {
    case OFPUTIL_TABLE_EVICTION_DEFAULT: return "default";
    case OFPUTIL_TABLE_EVICTION_ON: return "on";
    case OFPUTIL_TABLE_EVICTION_OFF: return "off";
    default: return "***error***";
    }

}

static const char *
ofputil_eviction_flag_to_string(uint32_t bit)
{
    enum ofp14_table_mod_prop_eviction_flag eviction_flag = bit;

    switch (eviction_flag) {
    case OFPTMPEF14_OTHER:      return "OTHER";
    case OFPTMPEF14_IMPORTANCE: return "IMPORTANCE";
    case OFPTMPEF14_LIFETIME:   return "LIFETIME";
    }

    return NULL;
}

/* Appends to 'string' a description of the bitmap of OFPTMPEF14_* values in
 * 'eviction_flags'. */
static void
ofputil_put_eviction_flags(struct ds *string, uint32_t eviction_flags)
{
    if (eviction_flags != UINT32_MAX) {
        ofp_print_bit_names(string, eviction_flags,
                            ofputil_eviction_flag_to_string, '|');
    } else {
        ds_put_cstr(string, "(default)");
    }
}

static void
ofp_print_table_mod(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_table_mod pm;
    enum ofperr error;

    error = ofputil_decode_table_mod(oh, &pm);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    if (pm.table_id == 0xff) {
        ds_put_cstr(string, " table_id: ALL_TABLES");
    } else {
        ds_put_format(string, " table_id=%"PRIu8, pm.table_id);
    }

    if (pm.miss != OFPUTIL_TABLE_MISS_DEFAULT) {
        ds_put_format(string, ", flow_miss_config=%s",
                      ofputil_table_miss_to_string(pm.miss));
    }
    if (pm.eviction != OFPUTIL_TABLE_EVICTION_DEFAULT) {
        ds_put_format(string, ", eviction=%s",
                      ofputil_table_eviction_to_string(pm.eviction));
    }
    if (pm.eviction_flags != UINT32_MAX) {
        ds_put_cstr(string, "eviction_flags=");
        ofputil_put_eviction_flags(string, pm.eviction_flags);
    }
}

/* This function will print the Table description properties. */
static void
ofp_print_table_desc(struct ds *string, const struct ofputil_table_desc *td)
{
    ds_put_format(string, "\n  table %"PRIu8, td->table_id);
    ds_put_cstr(string, ":\n");
    ds_put_format(string, "   eviction=%s eviction_flags=",
                  ofputil_table_eviction_to_string(td->eviction));
    ofputil_put_eviction_flags(string, td->eviction_flags);
    ds_put_char(string, '\n');
}

static void
ofp_print_queue_get_config_request(struct ds *string,
                                   const struct ofp_header *oh)
{
    enum ofperr error;
    ofp_port_t port;

    error = ofputil_decode_queue_get_config_request(oh, &port);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " port=");
    ofputil_format_port(port, string);
}

static void
print_queue_rate(struct ds *string, const char *name, unsigned int rate)
{
    if (rate <= 1000) {
        ds_put_format(string, " %s:%u.%u%%", name, rate / 10, rate % 10);
    } else if (rate < UINT16_MAX) {
        ds_put_format(string, " %s:(disabled)", name);
    }
}

static void
ofp_print_queue_get_config_reply(struct ds *string,
                                 const struct ofp_header *oh)
{
    enum ofperr error;
    struct ofpbuf b;
    ofp_port_t port;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    error = ofputil_decode_queue_get_config_reply(&b, &port);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " port=");
    ofputil_format_port(port, string);
    ds_put_char(string, '\n');

    for (;;) {
        struct ofputil_queue_config queue;
        int retval;

        retval = ofputil_pull_queue_get_config_reply(&b, &queue);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(string, retval);
            }
            break;
        }

        ds_put_format(string, "queue %"PRIu32":", queue.queue_id);
        print_queue_rate(string, "min_rate", queue.min_rate);
        print_queue_rate(string, "max_rate", queue.max_rate);
        ds_put_char(string, '\n');
    }
}

static void
ofp_print_meter_flags(struct ds *s, uint16_t flags)
{
    if (flags & OFPMF13_KBPS) {
        ds_put_cstr(s, "kbps ");
    }
    if (flags & OFPMF13_PKTPS) {
        ds_put_cstr(s, "pktps ");
    }
    if (flags & OFPMF13_BURST) {
        ds_put_cstr(s, "burst ");
    }
    if (flags & OFPMF13_STATS) {
        ds_put_cstr(s, "stats ");
    }

    flags &= ~(OFPMF13_KBPS | OFPMF13_PKTPS | OFPMF13_BURST | OFPMF13_STATS);
    if (flags) {
        ds_put_format(s, "flags:0x%"PRIx16" ", flags);
    }
}

static void
ofp_print_meter_band(struct ds *s, uint16_t flags,
                     const struct ofputil_meter_band *mb)
{
    ds_put_cstr(s, "\ntype=");
    switch (mb->type) {
    case OFPMBT13_DROP:
        ds_put_cstr(s, "drop");
        break;
    case OFPMBT13_DSCP_REMARK:
        ds_put_cstr(s, "dscp_remark");
        break;
    default:
        ds_put_format(s, "%u", mb->type);
    }

    ds_put_format(s, " rate=%"PRIu32, mb->rate);

    if (flags & OFPMF13_BURST) {
        ds_put_format(s, " burst_size=%"PRIu32, mb->burst_size);
    }
    if (mb->type == OFPMBT13_DSCP_REMARK) {
        ds_put_format(s, " prec_level=%"PRIu8, mb->prec_level);
    }
}

static void
ofp_print_meter_stats(struct ds *s, const struct ofputil_meter_stats *ms)
{
    uint16_t i;

    ds_put_format(s, "meter:%"PRIu32" ", ms->meter_id);
    ds_put_format(s, "flow_count:%"PRIu32" ", ms->flow_count);
    ds_put_format(s, "packet_in_count:%"PRIu64" ", ms->packet_in_count);
    ds_put_format(s, "byte_in_count:%"PRIu64" ", ms->byte_in_count);
    ds_put_cstr(s, "duration:");
    ofp_print_duration(s, ms->duration_sec, ms->duration_nsec);
    ds_put_char(s, ' ');

    ds_put_cstr(s, "bands:\n");
    for (i = 0; i < ms->n_bands; ++i) {
        ds_put_format(s, "%d: ", i);
        ds_put_format(s, "packet_count:%"PRIu64" ", ms->bands[i].packet_count);
        ds_put_format(s, "byte_count:%"PRIu64"\n", ms->bands[i].byte_count);
    }
}

static void
ofp_print_meter_config(struct ds *s, const struct ofputil_meter_config *mc)
{
    uint16_t i;

    ds_put_format(s, "meter=%"PRIu32" ", mc->meter_id);

    ofp_print_meter_flags(s, mc->flags);

    ds_put_cstr(s, "bands=");
    for (i = 0; i < mc->n_bands; ++i) {
        ofp_print_meter_band(s, mc->flags, &mc->bands[i]);
    }
    ds_put_char(s, '\n');
}

static void
ofp_print_meter_mod__(struct ds *s, const struct ofputil_meter_mod *mm)
{
    switch (mm->command) {
    case OFPMC13_ADD:
        ds_put_cstr(s, " ADD ");
        break;
    case OFPMC13_MODIFY:
        ds_put_cstr(s, " MOD ");
        break;
    case OFPMC13_DELETE:
        ds_put_cstr(s, " DEL ");
        break;
    default:
        ds_put_format(s, " cmd:%d ", mm->command);
    }

    ofp_print_meter_config(s, &mm->meter);
}

static void
ofp_print_meter_mod(struct ds *s, const struct ofp_header *oh)
{
    struct ofputil_meter_mod mm;
    struct ofpbuf bands;
    enum ofperr error;

    ofpbuf_init(&bands, 64);
    error = ofputil_decode_meter_mod(oh, &mm, &bands);
    if (error) {
        ofp_print_error(s, error);
    } else {
        ofp_print_meter_mod__(s, &mm);
    }
    ofpbuf_uninit(&bands);
}

static void
ofp_print_meter_stats_request(struct ds *s, const struct ofp_header *oh)
{
    uint32_t meter_id;

    ofputil_decode_meter_request(oh, &meter_id);

    ds_put_format(s, " meter=%"PRIu32, meter_id);
}

static const char *
ofputil_meter_capabilities_to_name(uint32_t bit)
{
    enum ofp13_meter_flags flag = bit;

    switch (flag) {
    case OFPMF13_KBPS:    return "kbps";
    case OFPMF13_PKTPS:   return "pktps";
    case OFPMF13_BURST:   return "burst";
    case OFPMF13_STATS:   return "stats";
    }

    return NULL;
}

static const char *
ofputil_meter_band_types_to_name(uint32_t bit)
{
    switch (bit) {
    case 1 << OFPMBT13_DROP:          return "drop";
    case 1 << OFPMBT13_DSCP_REMARK:   return "dscp_remark";
    }

    return NULL;
}

static void
ofp_print_meter_features_reply(struct ds *s, const struct ofp_header *oh)
{
    struct ofputil_meter_features mf;

    ofputil_decode_meter_features(oh, &mf);

    ds_put_format(s, "\nmax_meter:%"PRIu32, mf.max_meters);
    ds_put_format(s, " max_bands:%"PRIu8, mf.max_bands);
    ds_put_format(s, " max_color:%"PRIu8"\n", mf.max_color);

    ds_put_cstr(s, "band_types: ");
    ofp_print_bit_names(s, mf.band_types,
                        ofputil_meter_band_types_to_name, ' ');
    ds_put_char(s, '\n');

    ds_put_cstr(s, "capabilities: ");
    ofp_print_bit_names(s, mf.capabilities,
                        ofputil_meter_capabilities_to_name, ' ');
    ds_put_char(s, '\n');
}

static void
ofp_print_meter_config_reply(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf bands;
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpbuf_init(&bands, 64);
    for (;;) {
        struct ofputil_meter_config mc;
        int retval;

        retval = ofputil_decode_meter_config(&b, &mc, &bands);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(s, retval);
            }
            break;
        }
        ds_put_char(s, '\n');
        ofp_print_meter_config(s, &mc);
    }
    ofpbuf_uninit(&bands);
}

static void
ofp_print_meter_stats_reply(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf bands;
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpbuf_init(&bands, 64);
    for (;;) {
        struct ofputil_meter_stats ms;
        int retval;

        retval = ofputil_decode_meter_stats(&b, &ms, &bands);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(s, retval);
            }
            break;
        }
        ds_put_char(s, '\n');
        ofp_print_meter_stats(s, &ms);
    }
    ofpbuf_uninit(&bands);
}

static void
ofp_print_error(struct ds *string, enum ofperr error)
{
    if (string->length) {
        ds_put_char(string, ' ');
    }
    ds_put_format(string, "***decode error: %s***\n", ofperr_get_name(error));
}

static void
ofp_print_hello(struct ds *string, const struct ofp_header *oh)
{
    uint32_t allowed_versions;
    bool ok;

    ok = ofputil_decode_hello(oh, &allowed_versions);

    ds_put_cstr(string, "\n version bitmap: ");
    ofputil_format_version_bitmap(string, allowed_versions);

    if (!ok) {
        ds_put_cstr(string, "\n unknown data in hello:\n");
        ds_put_hex_dump(string, oh, ntohs(oh->length), 0, true);
    }
}

static void
ofp_print_error_msg(struct ds *string, const struct ofp_header *oh)
{
    size_t len = ntohs(oh->length);
    struct ofpbuf payload;
    enum ofperr error;
    char *s;

    error = ofperr_decode_msg(oh, &payload);
    if (!error) {
        ds_put_cstr(string, "***decode error***");
        ds_put_hex_dump(string, oh + 1, len - sizeof *oh, 0, true);
        return;
    }

    ds_put_format(string, " %s\n", ofperr_get_name(error));

    if (error == OFPERR_OFPHFC_INCOMPATIBLE || error == OFPERR_OFPHFC_EPERM) {
        ds_put_printable(string, payload.data, payload.size);
    } else {
        s = ofp_to_string(payload.data, payload.size, 1);
        ds_put_cstr(string, s);
        free(s);
    }
    ofpbuf_uninit(&payload);
}

static void
ofp_print_port_status(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_port_status ps;
    enum ofperr error;

    error = ofputil_decode_port_status(oh, &ps);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    if (ps.reason == OFPPR_ADD) {
        ds_put_format(string, " ADD:");
    } else if (ps.reason == OFPPR_DELETE) {
        ds_put_format(string, " DEL:");
    } else if (ps.reason == OFPPR_MODIFY) {
        ds_put_format(string, " MOD:");
    }

    ofp_print_phy_port(string, &ps.desc);
}

static void
ofp_print_ofpst_desc_reply(struct ds *string, const struct ofp_header *oh)
{
    const struct ofp_desc_stats *ods = ofpmsg_body(oh);

    ds_put_char(string, '\n');
    ds_put_format(string, "Manufacturer: %.*s\n",
            (int) sizeof ods->mfr_desc, ods->mfr_desc);
    ds_put_format(string, "Hardware: %.*s\n",
            (int) sizeof ods->hw_desc, ods->hw_desc);
    ds_put_format(string, "Software: %.*s\n",
            (int) sizeof ods->sw_desc, ods->sw_desc);
    ds_put_format(string, "Serial Num: %.*s\n",
            (int) sizeof ods->serial_num, ods->serial_num);
    ds_put_format(string, "DP Description: %.*s\n",
            (int) sizeof ods->dp_desc, ods->dp_desc);
}

static void
ofp_print_flow_stats_request(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_flow_stats_request fsr;
    enum ofperr error;

    error = ofputil_decode_flow_stats_request(&fsr, oh);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    if (fsr.table_id != 0xff) {
        ds_put_format(string, " table=%"PRIu8, fsr.table_id);
    }

    if (fsr.out_port != OFPP_ANY) {
        ds_put_cstr(string, " out_port=");
        ofputil_format_port(fsr.out_port, string);
    }

    ds_put_char(string, ' ');
    match_format(&fsr.match, string, OFP_DEFAULT_PRIORITY);
}

void
ofp_print_flow_stats(struct ds *string, struct ofputil_flow_stats *fs)
{
    ds_put_format(string, " cookie=0x%"PRIx64", duration=",
                  ntohll(fs->cookie));

    ofp_print_duration(string, fs->duration_sec, fs->duration_nsec);
    ds_put_format(string, ", table=%"PRIu8", ", fs->table_id);
    ds_put_format(string, "n_packets=%"PRIu64", ", fs->packet_count);
    ds_put_format(string, "n_bytes=%"PRIu64", ", fs->byte_count);
    if (fs->idle_timeout != OFP_FLOW_PERMANENT) {
        ds_put_format(string, "idle_timeout=%"PRIu16", ", fs->idle_timeout);
    }
    if (fs->hard_timeout != OFP_FLOW_PERMANENT) {
        ds_put_format(string, "hard_timeout=%"PRIu16", ", fs->hard_timeout);
    }
    if (fs->flags) {
        ofp_print_flow_flags(string, fs->flags);
    }
    if (fs->importance != 0) {
        ds_put_format(string, "importance=%"PRIu16", ", fs->importance);
    }
    if (fs->idle_age >= 0) {
        ds_put_format(string, "idle_age=%d, ", fs->idle_age);
    }
    if (fs->hard_age >= 0 && fs->hard_age != fs->duration_sec) {
        ds_put_format(string, "hard_age=%d, ", fs->hard_age);
    }

    match_format(&fs->match, string, fs->priority);
    if (string->string[string->length - 1] != ' ') {
        ds_put_char(string, ' ');
    }

    ds_put_cstr(string, "actions=");
    ofpacts_format(fs->ofpacts, fs->ofpacts_len, string);
}

static void
ofp_print_flow_stats_reply(struct ds *string, const struct ofp_header *oh)
{
    struct ofpbuf ofpacts;
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpbuf_init(&ofpacts, 64);
    for (;;) {
        struct ofputil_flow_stats fs;
        int retval;

        retval = ofputil_decode_flow_stats_reply(&fs, &b, true, &ofpacts);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(string, " ***parse error***");
            }
            break;
        }
        ds_put_char(string, '\n');
        ofp_print_flow_stats(string, &fs);
     }
    ofpbuf_uninit(&ofpacts);
}

static void
ofp_print_aggregate_stats_reply(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_aggregate_stats as;
    enum ofperr error;

    error = ofputil_decode_aggregate_stats_reply(&as, oh);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_format(string, " packet_count=%"PRIu64, as.packet_count);
    ds_put_format(string, " byte_count=%"PRIu64, as.byte_count);
    ds_put_format(string, " flow_count=%"PRIu32, as.flow_count);
}

static void
print_port_stat(struct ds *string, const char *leader, uint64_t stat, int more)
{
    ds_put_cstr(string, leader);
    if (stat != UINT64_MAX) {
        ds_put_format(string, "%"PRIu64, stat);
    } else {
        ds_put_char(string, '?');
    }
    if (more) {
        ds_put_cstr(string, ", ");
    } else {
        ds_put_cstr(string, "\n");
    }
}

static void
ofp_print_ofpst_port_request(struct ds *string, const struct ofp_header *oh)
{
    ofp_port_t ofp10_port;
    enum ofperr error;

    error = ofputil_decode_port_stats_request(oh, &ofp10_port);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " port_no=");
    ofputil_format_port(ofp10_port, string);
}

static void
ofp_print_ofpst_port_reply(struct ds *string, const struct ofp_header *oh,
                           int verbosity)
{
    struct ofpbuf b;

    ds_put_format(string, " %"PRIuSIZE" ports\n", ofputil_count_port_stats(oh));
    if (verbosity < 1) {
        return;
    }

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_port_stats ps;
        int retval;

        retval = ofputil_decode_port_stats(&ps, &b);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(string, " ***parse error***");
            }
            return;
        }

        ds_put_cstr(string, "  port ");
        if (ofp_to_u16(ps.port_no) < 10) {
            ds_put_char(string, ' ');
        }
        ofputil_format_port(ps.port_no, string);

        ds_put_cstr(string, ": rx ");
        print_port_stat(string, "pkts=", ps.stats.rx_packets, 1);
        print_port_stat(string, "bytes=", ps.stats.rx_bytes, 1);
        print_port_stat(string, "drop=", ps.stats.rx_dropped, 1);
        print_port_stat(string, "errs=", ps.stats.rx_errors, 1);
        print_port_stat(string, "frame=", ps.stats.rx_frame_errors, 1);
        print_port_stat(string, "over=", ps.stats.rx_over_errors, 1);
        print_port_stat(string, "crc=", ps.stats.rx_crc_errors, 0);

        ds_put_cstr(string, "           tx ");
        print_port_stat(string, "pkts=", ps.stats.tx_packets, 1);
        print_port_stat(string, "bytes=", ps.stats.tx_bytes, 1);
        print_port_stat(string, "drop=", ps.stats.tx_dropped, 1);
        print_port_stat(string, "errs=", ps.stats.tx_errors, 1);
        print_port_stat(string, "coll=", ps.stats.collisions, 0);

        if (ps.duration_sec != UINT32_MAX) {
            ds_put_cstr(string, "           duration=");
            ofp_print_duration(string, ps.duration_sec, ps.duration_nsec);
            ds_put_char(string, '\n');
        }
    }
}

static void
ofp_print_table_stats_reply(struct ds *string, const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpraw_pull_assert(&b);

    struct ofputil_table_features prev_features;
    struct ofputil_table_stats prev_stats;
    for (int i = 0;; i++) {
        struct ofputil_table_features features;
        struct ofputil_table_stats stats;
        int retval;

        retval = ofputil_decode_table_stats_reply(&b, &stats, &features);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(string, retval);
            }
            return;
        }

        ds_put_char(string, '\n');
        ofp_print_table_features(string,
                                 &features, i ? &prev_features : NULL,
                                 &stats, i ? &prev_stats : NULL);
        prev_features = features;
        prev_stats = stats;
    }
}

static void
ofp_print_queue_name(struct ds *string, uint32_t queue_id)
{
    if (queue_id == OFPQ_ALL) {
        ds_put_cstr(string, "ALL");
    } else {
        ds_put_format(string, "%"PRIu32, queue_id);
    }
}

static void
ofp_print_ofpst_queue_request(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_queue_stats_request oqsr;
    enum ofperr error;

    error = ofputil_decode_queue_stats_request(oh, &oqsr);
    if (error) {
        ds_put_format(string, "***decode error: %s***\n", ofperr_get_name(error));
        return;
    }

    ds_put_cstr(string, "port=");
    ofputil_format_port(oqsr.port_no, string);

    ds_put_cstr(string, " queue=");
    ofp_print_queue_name(string, oqsr.queue_id);
}

static void
ofp_print_ofpst_queue_reply(struct ds *string, const struct ofp_header *oh,
                            int verbosity)
{
    struct ofpbuf b;

    ds_put_format(string, " %"PRIuSIZE" queues\n", ofputil_count_queue_stats(oh));
    if (verbosity < 1) {
        return;
    }

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_queue_stats qs;
        int retval;

        retval = ofputil_decode_queue_stats(&qs, &b);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(string, " ***parse error***");
            }
            return;
        }

        ds_put_cstr(string, "  port ");
        ofputil_format_port(qs.port_no, string);
        ds_put_cstr(string, " queue ");
        ofp_print_queue_name(string, qs.queue_id);
        ds_put_cstr(string, ": ");

        print_port_stat(string, "bytes=", qs.tx_bytes, 1);
        print_port_stat(string, "pkts=", qs.tx_packets, 1);
        print_port_stat(string, "errors=", qs.tx_errors, 1);

        ds_put_cstr(string, "duration=");
        if (qs.duration_sec != UINT32_MAX) {
            ofp_print_duration(string, qs.duration_sec, qs.duration_nsec);
        } else {
            ds_put_char(string, '?');
        }
        ds_put_char(string, '\n');
    }
}

static void
ofp_print_ofpst_port_desc_request(struct ds *string,
                                  const struct ofp_header *oh)
{
    enum ofperr error;
    ofp_port_t port;

    error = ofputil_decode_port_desc_stats_request(oh, &port);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " port=");
    ofputil_format_port(port, string);
}

static void
ofp_print_ofpst_port_desc_reply(struct ds *string,
                                const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpraw_pull_assert(&b);
    ds_put_char(string, '\n');
    ofp_print_phy_ports(string, oh->version, &b);
}

static void
ofp_print_stats(struct ds *string, const struct ofp_header *oh)
{
    uint16_t flags = ofpmp_flags(oh);

    if (flags) {
        ds_put_cstr(string, " flags=");
        if ((!ofpmsg_is_stat_request(oh) || oh->version >= OFP13_VERSION)
            && (flags & OFPSF_REPLY_MORE)) {
            ds_put_cstr(string, "[more]");
            flags &= ~OFPSF_REPLY_MORE;
        }
        if (flags) {
            ds_put_format(string, "[***unknown flags 0x%04"PRIx16"***]",
                          flags);
        }
    }
}

static void
ofp_print_echo(struct ds *string, const struct ofp_header *oh, int verbosity)
{
    size_t len = ntohs(oh->length);

    ds_put_format(string, " %"PRIuSIZE" bytes of payload\n", len - sizeof *oh);
    if (verbosity > 1) {
        ds_put_hex_dump(string, oh + 1, len - sizeof *oh, 0, true);
    }
}

static void
ofp_print_role_generic(struct ds *string, enum ofp12_controller_role role,
                       uint64_t generation_id)
{
    ds_put_cstr(string, " role=");

    switch (role) {
    case OFPCR12_ROLE_NOCHANGE:
        ds_put_cstr(string, "nochange");
        break;
    case OFPCR12_ROLE_EQUAL:
        ds_put_cstr(string, "equal"); /* OF 1.2 wording */
        break;
    case OFPCR12_ROLE_MASTER:
        ds_put_cstr(string, "master");
        break;
    case OFPCR12_ROLE_SLAVE:
        ds_put_cstr(string, "slave");
        break;
    default:
        OVS_NOT_REACHED();
    }

    if (generation_id != UINT64_MAX) {
        ds_put_format(string, " generation_id=%"PRIu64, generation_id);
    }
}

static void
ofp_print_role_message(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_role_request rr;
    enum ofperr error;

    error = ofputil_decode_role_message(oh, &rr);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ofp_print_role_generic(string, rr.role, rr.have_generation_id ? rr.generation_id : UINT64_MAX);
}

static void
ofp_print_role_status_message(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_role_status rs;
    enum ofperr error;

    error = ofputil_decode_role_status(oh, &rs);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ofp_print_role_generic(string, rs.role, rs.generation_id);

    ds_put_cstr(string, " reason=");

    switch (rs.reason) {
    case OFPCRR_MASTER_REQUEST:
        ds_put_cstr(string, "master_request");
        break;
    case OFPCRR_CONFIG:
        ds_put_cstr(string, "configuration_changed");
        break;
    case OFPCRR_EXPERIMENTER:
        ds_put_cstr(string, "experimenter_data_changed");
        break;
    default:
        OVS_NOT_REACHED();
    }
}

static void
ofp_print_nxt_flow_mod_table_id(struct ds *string,
                                const struct nx_flow_mod_table_id *nfmti)
{
    ds_put_format(string, " %s", nfmti->set ? "enable" : "disable");
}

static void
ofp_print_nxt_set_flow_format(struct ds *string,
                              const struct nx_set_flow_format *nsff)
{
    uint32_t format = ntohl(nsff->format);

    ds_put_cstr(string, " format=");
    if (ofputil_nx_flow_format_is_valid(format)) {
        ds_put_cstr(string, ofputil_nx_flow_format_to_string(format));
    } else {
        ds_put_format(string, "%"PRIu32, format);
    }
}

static void
ofp_print_nxt_set_packet_in_format(struct ds *string,
                                   const struct nx_set_packet_in_format *nspf)
{
    uint32_t format = ntohl(nspf->format);

    ds_put_cstr(string, " format=");
    if (ofputil_packet_in_format_is_valid(format)) {
        ds_put_cstr(string, ofputil_packet_in_format_to_string(format));
    } else {
        ds_put_format(string, "%"PRIu32, format);
    }
}

/* Returns a string form of 'reason'.  The return value is either a statically
 * allocated constant string or the 'bufsize'-byte buffer 'reasonbuf'.
 * 'bufsize' should be at least OFP_PORT_REASON_BUFSIZE. */
#define OFP_PORT_REASON_BUFSIZE (INT_STRLEN(int) + 1)
static const char *
ofp_port_reason_to_string(enum ofp_port_reason reason,
                          char *reasonbuf, size_t bufsize)
{
    switch (reason) {
    case OFPPR_ADD:
        return "add";

    case OFPPR_DELETE:
        return "delete";

    case OFPPR_MODIFY:
        return "modify";

    default:
        snprintf(reasonbuf, bufsize, "%d", (int) reason);
        return reasonbuf;
    }
}

/* Returns a string form of 'reason'.  The return value is either a statically
 * allocated constant string or the 'bufsize'-byte buffer 'reasonbuf'.
 * 'bufsize' should be at least OFP_ASYNC_CONFIG_REASON_BUFSIZE. */
static const char*
ofp_role_reason_to_string(enum ofp14_controller_role_reason reason,
                          char *reasonbuf, size_t bufsize)
{
    switch (reason) {
    case OFPCRR_MASTER_REQUEST:
        return "master_request";

    case OFPCRR_CONFIG:
        return "configuration_changed";

    case OFPCRR_EXPERIMENTER:
        return "experimenter_data_changed";

    default:
        snprintf(reasonbuf, bufsize, "%d", (int) reason);
        return reasonbuf;
    }
}

/* Returns a string form of 'reason'.  The return value is either a statically
 * allocated constant string or the 'bufsize'-byte buffer 'reasonbuf'.
 * 'bufsize' should be at least OFP_ASYNC_CONFIG_REASON_BUFSIZE. */
static const char*
ofp_table_reason_to_string(enum ofp14_table_reason reason,
                           char *reasonbuf, size_t bufsize)
{
    switch (reason) {
    case OFPTR_VACANCY_DOWN:
        return "vacancy_down";

    case OFPTR_VACANCY_UP:
        return "vacancy_up";

    default:
        snprintf(reasonbuf, bufsize, "%d", (int) reason);
        return reasonbuf;
    }
}

/* Returns a string form of 'reason'.  The return value is either a statically
 * allocated constant string or the 'bufsize'-byte buffer 'reasonbuf'.
 * 'bufsize' should be at least OFP_ASYNC_CONFIG_REASON_BUFSIZE. */
static const char*
ofp_requestforward_reason_to_string(enum ofp14_requestforward_reason reason,
                                    char *reasonbuf, size_t bufsize)
{
    switch (reason) {
    case OFPRFR_GROUP_MOD:
        return "group_mod_request";

    case OFPRFR_METER_MOD:
        return "meter_mod_request";

    default:
        snprintf(reasonbuf, bufsize, "%d", (int) reason);
        return reasonbuf;
    }
}

static const char *
ofp_async_config_reason_to_string(uint32_t reason,
                                  enum ofputil_async_msg_type type,
                                  char *reasonbuf, size_t bufsize)
{
    switch (type) {
    case OAM_PACKET_IN:
        return ofputil_packet_in_reason_to_string(reason, reasonbuf, bufsize);

    case OAM_PORT_STATUS:
        return ofp_port_reason_to_string(reason, reasonbuf, bufsize);

    case OAM_FLOW_REMOVED:
        return ofp_flow_removed_reason_to_string(reason, reasonbuf, bufsize);

    case OAM_ROLE_STATUS:
        return ofp_role_reason_to_string(reason, reasonbuf, bufsize);

    case OAM_TABLE_STATUS:
        return ofp_table_reason_to_string(reason, reasonbuf, bufsize);

    case OAM_REQUESTFORWARD:
        return ofp_requestforward_reason_to_string(reason, reasonbuf, bufsize);

    case OAM_N_TYPES:
    default:
        return "Unknown asynchronous configuration message type";
    }
}


#define OFP_ASYNC_CONFIG_REASON_BUFSIZE (INT_STRLEN(int) + 1)
static void
ofp_print_nxt_set_async_config(struct ds *string,
                               const struct ofp_header *oh)
{
    int i, j;
    enum ofpraw raw;

    ofpraw_decode(&raw, oh);

    if (raw == OFPRAW_OFPT13_SET_ASYNC ||
        raw == OFPRAW_NXT_SET_ASYNC_CONFIG ||
        raw == OFPRAW_OFPT13_GET_ASYNC_REPLY) {
        const struct nx_async_config *nac = ofpmsg_body(oh);

        for (i = 0; i < 2; i++) {

            ds_put_format(string, "\n %s:\n", i == 0 ? "master" : "slave");

            ds_put_cstr(string, "       PACKET_IN:");
            for (j = 0; j < 32; j++) {
                if (nac->packet_in_mask[i] & htonl(1u << j)) {
                    char reasonbuf[OFPUTIL_PACKET_IN_REASON_BUFSIZE];
                    const char *reason;

                    reason = ofputil_packet_in_reason_to_string(j, reasonbuf,
                                                            sizeof reasonbuf);
                    ds_put_format(string, " %s", reason);
                }
            }
            if (!nac->packet_in_mask[i]) {
                ds_put_cstr(string, " (off)");
            }
            ds_put_char(string, '\n');

            ds_put_cstr(string, "     PORT_STATUS:");
            for (j = 0; j < 32; j++) {
                if (nac->port_status_mask[i] & htonl(1u << j)) {
                    char reasonbuf[OFP_PORT_REASON_BUFSIZE];
                    const char *reason;

                    reason = ofp_port_reason_to_string(j, reasonbuf,
                                                       sizeof reasonbuf);
                    ds_put_format(string, " %s", reason);
                }
            }
            if (!nac->port_status_mask[i]) {
                ds_put_cstr(string, " (off)");
            }
            ds_put_char(string, '\n');

            ds_put_cstr(string, "    FLOW_REMOVED:");
            for (j = 0; j < 32; j++) {
                if (nac->flow_removed_mask[i] & htonl(1u << j)) {
                    char reasonbuf[OFP_FLOW_REMOVED_REASON_BUFSIZE];
                    const char *reason;

                    reason = ofp_flow_removed_reason_to_string(j, reasonbuf,
                                                           sizeof reasonbuf);
                    ds_put_format(string, " %s", reason);
                }
            }
            if (!nac->flow_removed_mask[i]) {
                ds_put_cstr(string, " (off)");
            }
            ds_put_char(string, '\n');
        }
    } else if (raw == OFPRAW_OFPT14_SET_ASYNC ||
               raw == OFPRAW_OFPT14_GET_ASYNC_REPLY) {
        uint32_t role[2][OAM_N_TYPES] = {{0}};
        uint32_t type;

        ofputil_decode_set_async_config(oh, role[0], role[1], true);
        for (i = 0; i < 2; i++) {

            ds_put_format(string, "\n %s:\n", i == 0 ? "master" : "slave");
            for (type = 0; type < OAM_N_TYPES; type++) {
                switch (type) {
                case OAM_PACKET_IN:
                    ds_put_cstr(string, "       PACKET_IN:");
                    break;

                case OAM_PORT_STATUS:
                    ds_put_cstr(string, "     PORT_STATUS:");
                    break;

                case OAM_FLOW_REMOVED:
                    ds_put_cstr(string, "    FLOW_REMOVED:");
                    break;

                case OAM_ROLE_STATUS:
                    ds_put_cstr(string, "     ROLE_STATUS:");
                    break;

                case OAM_TABLE_STATUS:
                    ds_put_cstr(string, "    TABLE_STATUS:");
                    break;

                case OAM_REQUESTFORWARD:
                    ds_put_cstr(string, "  REQUESTFORWARD:");
                    break;
                }

                for (j = 0; j < 32; j++) {
                    if (role[i][type] & (1u << j)) {
                        char reasonbuf[OFP_ASYNC_CONFIG_REASON_BUFSIZE];
                        const char *reason;

                        reason = ofp_async_config_reason_to_string(j, type,
                                                                   reasonbuf,
                                                           sizeof reasonbuf);
                        ds_put_format(string, " %s", reason);
                    }
                }
                if (!role[i][type]) {
                    ds_put_cstr(string, " (off)");
                }
                ds_put_char(string, '\n');
            }
        }
    }
}

static void
ofp_print_nxt_set_controller_id(struct ds *string,
                                const struct nx_controller_id *nci)
{
    ds_put_format(string, " id=%"PRIu16, ntohs(nci->controller_id));
}

static void
ofp_print_nxt_flow_monitor_cancel(struct ds *string,
                                  const struct ofp_header *oh)
{
    ds_put_format(string, " id=%"PRIu32,
                  ofputil_decode_flow_monitor_cancel(oh));
}

static const char *
nx_flow_monitor_flags_to_name(uint32_t bit)
{
    enum nx_flow_monitor_flags fmf = bit;

    switch (fmf) {
    case NXFMF_INITIAL: return "initial";
    case NXFMF_ADD: return "add";
    case NXFMF_DELETE: return "delete";
    case NXFMF_MODIFY: return "modify";
    case NXFMF_ACTIONS: return "actions";
    case NXFMF_OWN: return "own";
    }

    return NULL;
}

static void
ofp_print_nxst_flow_monitor_request(struct ds *string,
                                    const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_flow_monitor_request request;
        int retval;

        retval = ofputil_decode_flow_monitor_request(&request, &b);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(string, retval);
            }
            return;
        }

        ds_put_format(string, "\n id=%"PRIu32" flags=", request.id);
        ofp_print_bit_names(string, request.flags,
                            nx_flow_monitor_flags_to_name, ',');

        if (request.out_port != OFPP_NONE) {
            ds_put_cstr(string, " out_port=");
            ofputil_format_port(request.out_port, string);
        }

        if (request.table_id != 0xff) {
            ds_put_format(string, " table=%"PRIu8, request.table_id);
        }

        ds_put_char(string, ' ');
        match_format(&request.match, string, OFP_DEFAULT_PRIORITY);
        ds_chomp(string, ' ');
    }
}

static void
ofp_print_nxst_flow_monitor_reply(struct ds *string,
                                  const struct ofp_header *oh)
{
    uint64_t ofpacts_stub[1024 / 8];
    struct ofpbuf ofpacts;
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    ofpbuf_use_stub(&ofpacts, ofpacts_stub, sizeof ofpacts_stub);
    for (;;) {
        char reasonbuf[OFP_FLOW_REMOVED_REASON_BUFSIZE];
        struct ofputil_flow_update update;
        struct match match;
        int retval;

        update.match = &match;
        retval = ofputil_decode_flow_update(&update, &b, &ofpacts);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(string, retval);
            }
            ofpbuf_uninit(&ofpacts);
            return;
        }

        ds_put_cstr(string, "\n event=");
        switch (update.event) {
        case NXFME_ADDED:
            ds_put_cstr(string, "ADDED");
            break;

        case NXFME_DELETED:
            ds_put_format(string, "DELETED reason=%s",
                          ofp_flow_removed_reason_to_string(update.reason,
                                                            reasonbuf,
                                                            sizeof reasonbuf));
            break;

        case NXFME_MODIFIED:
            ds_put_cstr(string, "MODIFIED");
            break;

        case NXFME_ABBREV:
            ds_put_format(string, "ABBREV xid=0x%"PRIx32, ntohl(update.xid));
            continue;
        }

        ds_put_format(string, " table=%"PRIu8, update.table_id);
        if (update.idle_timeout != OFP_FLOW_PERMANENT) {
            ds_put_format(string, " idle_timeout=%"PRIu16,
                          update.idle_timeout);
        }
        if (update.hard_timeout != OFP_FLOW_PERMANENT) {
            ds_put_format(string, " hard_timeout=%"PRIu16,
                          update.hard_timeout);
        }
        ds_put_format(string, " cookie=%#"PRIx64, ntohll(update.cookie));

        ds_put_char(string, ' ');
        match_format(update.match, string, OFP_DEFAULT_PRIORITY);

        if (update.ofpacts_len) {
            if (string->string[string->length - 1] != ' ') {
                ds_put_char(string, ' ');
            }
            ds_put_cstr(string, "actions=");
            ofpacts_format(update.ofpacts, update.ofpacts_len, string);
        }
    }
}

void
ofp_print_version(const struct ofp_header *oh,
                  struct ds *string)
{
    switch (oh->version) {
    case OFP10_VERSION:
        break;
    case OFP11_VERSION:
        ds_put_cstr(string, " (OF1.1)");
        break;
    case OFP12_VERSION:
        ds_put_cstr(string, " (OF1.2)");
        break;
    case OFP13_VERSION:
        ds_put_cstr(string, " (OF1.3)");
        break;
    case OFP14_VERSION:
        ds_put_cstr(string, " (OF1.4)");
        break;
    case OFP15_VERSION:
        ds_put_cstr(string, " (OF1.5)");
        break;
    default:
        ds_put_format(string, " (OF 0x%02"PRIx8")", oh->version);
        break;
    }
    ds_put_format(string, " (xid=0x%"PRIx32"):", ntohl(oh->xid));
}

static void
ofp_header_to_string__(const struct ofp_header *oh, enum ofpraw raw,
                       struct ds *string)
{
    ds_put_cstr(string, ofpraw_get_name(raw));
    ofp_print_version(oh, string);
}

static void
ofp_print_bucket_id(struct ds *s, const char *label, uint32_t bucket_id,
                    enum ofp_version ofp_version)
{
    if (ofp_version < OFP15_VERSION) {
        return;
    }

    ds_put_cstr(s, label);

    switch (bucket_id) {
    case OFPG15_BUCKET_FIRST:
        ds_put_cstr(s, "first");
        break;
    case OFPG15_BUCKET_LAST:
        ds_put_cstr(s, "last");
        break;
    case OFPG15_BUCKET_ALL:
        ds_put_cstr(s, "all");
        break;
    default:
        ds_put_format(s, "%"PRIu32, bucket_id);
        break;
    }

    ds_put_char(s, ',');
}

static void
ofp_print_group(struct ds *s, uint32_t group_id, uint8_t type,
                const struct ovs_list *p_buckets,
                const struct ofputil_group_props *props,
                enum ofp_version ofp_version, bool suppress_type)
{
    struct ofputil_bucket *bucket;

    ds_put_format(s, "group_id=%"PRIu32, group_id);

    if (!suppress_type) {
        static const char *type_str[] = { "all", "select", "indirect",
                                          "ff", "unknown" };
        ds_put_format(s, ",type=%s", type_str[type > 4 ? 4 : type]);
    }

    if (props->selection_method[0]) {
        ds_put_format(s, ",selection_method=%s", props->selection_method);
        if (props->selection_method_param) {
            ds_put_format(s, ",selection_method_param=%"PRIu64,
                          props->selection_method_param);
        }

        size_t n = bitmap_count1(props->fields.used.bm, MFF_N_IDS);
        if (n == 1) {
            ds_put_cstr(s, ",fields=");
            oxm_format_field_array(s, &props->fields);
        } else if (n > 1) {
            ds_put_cstr(s, ",fields(");
            oxm_format_field_array(s, &props->fields);
            ds_put_char(s, ')');
        }
    }

    if (!p_buckets) {
        return;
    }

    ds_put_char(s, ',');

    LIST_FOR_EACH (bucket, list_node, p_buckets) {
        ds_put_cstr(s, "bucket=");

        ofp_print_bucket_id(s, "bucket_id:", bucket->bucket_id, ofp_version);
        if (bucket->weight != (type == OFPGT11_SELECT ? 1 : 0)) {
            ds_put_format(s, "weight:%"PRIu16",", bucket->weight);
        }
        if (bucket->watch_port != OFPP_NONE) {
            ds_put_format(s, "watch_port:%"PRIu32",", bucket->watch_port);
        }
        if (bucket->watch_group != OFPG11_ANY) {
            ds_put_format(s, "watch_group:%"PRIu32",", bucket->watch_group);
        }

        ds_put_cstr(s, "actions=");
        ofpacts_format(bucket->ofpacts, bucket->ofpacts_len, s);
        ds_put_char(s, ',');
    }

    ds_chomp(s, ',');
}

static void
ofp_print_ofpst_group_desc_request(struct ds *string,
                                   const struct ofp_header *oh)
{
    uint32_t group_id = ofputil_decode_group_desc_request(oh);
    ds_put_cstr(string, " group_id=");
    ofputil_format_group(group_id, string);
}

static void
ofp_print_group_desc(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));
    for (;;) {
        struct ofputil_group_desc gd;
        int retval;

        retval = ofputil_decode_group_desc_reply(&gd, &b, oh->version);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(s, " ***parse error***");
            }
            break;
        }

        ds_put_char(s, '\n');
        ds_put_char(s, ' ');
        ofp_print_group(s, gd.group_id, gd.type, &gd.buckets, &gd.props,
                        oh->version, false);
        ofputil_bucket_list_destroy(&gd.buckets);
     }
}

static void
ofp_print_ofpst_group_request(struct ds *string, const struct ofp_header *oh)
{
    enum ofperr error;
    uint32_t group_id;

    error = ofputil_decode_group_stats_request(oh, &group_id);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " group_id=");
    ofputil_format_group(group_id, string);
}

static void
ofp_print_group_stats(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf b;
    uint32_t bucket_i;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));

    for (;;) {
        struct ofputil_group_stats gs;
        int retval;

        retval = ofputil_decode_group_stats_reply(&b, &gs);
        if (retval) {
            if (retval != EOF) {
                ds_put_cstr(s, " ***parse error***");
            }
            break;
        }

        ds_put_char(s, '\n');

        ds_put_char(s, ' ');
        ds_put_format(s, "group_id=%"PRIu32",", gs.group_id);

        if (gs.duration_sec != UINT32_MAX) {
            ds_put_cstr(s, "duration=");
            ofp_print_duration(s, gs.duration_sec, gs.duration_nsec);
            ds_put_char(s, ',');
        }
        ds_put_format(s, "ref_count=%"PRIu32",", gs.ref_count);
        ds_put_format(s, "packet_count=%"PRIu64",", gs.packet_count);
        ds_put_format(s, "byte_count=%"PRIu64"", gs.byte_count);

        for (bucket_i = 0; bucket_i < gs.n_buckets; bucket_i++) {
            if (gs.bucket_stats[bucket_i].packet_count != UINT64_MAX) {
                ds_put_format(s, ",bucket%"PRIu32":", bucket_i);
                ds_put_format(s, "packet_count=%"PRIu64",", gs.bucket_stats[bucket_i].packet_count);
                ds_put_format(s, "byte_count=%"PRIu64"", gs.bucket_stats[bucket_i].byte_count);
            }
        }

        free(gs.bucket_stats);
     }
}

static const char *
group_type_to_string(enum ofp11_group_type type)
{
    switch (type) {
    case OFPGT11_ALL: return "all";
    case OFPGT11_SELECT: return "select";
    case OFPGT11_INDIRECT: return "indirect";
    case OFPGT11_FF: return "fast failover";
    default: OVS_NOT_REACHED();
    }
}

static void
ofp_print_group_features(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_group_features features;
    int i;

    ofputil_decode_group_features_reply(oh, &features);

    ds_put_format(string, "\n Group table:\n");
    ds_put_format(string, "    Types:  0x%"PRIx32"\n", features.types);
    ds_put_format(string, "    Capabilities:  0x%"PRIx32"\n",
                  features.capabilities);

    for (i = 0; i < OFPGT12_N_TYPES; i++) {
        if (features.types & (1u << i)) {
            ds_put_format(string, "    %s group:\n", group_type_to_string(i));
            ds_put_format(string, "       max_groups=%#"PRIx32"\n",
                          features.max_groups[i]);
            ds_put_format(string, "       actions: ");
            ofpact_bitmap_format(features.ofpacts[i], string);
            ds_put_char(string, '\n');
        }
    }
}

static void
ofp_print_group_mod__(struct ds *s, enum ofp_version ofp_version,
                      const struct ofputil_group_mod *gm)
{
    bool bucket_command = false;

    ds_put_char(s, '\n');

    ds_put_char(s, ' ');
    switch (gm->command) {
    case OFPGC11_ADD:
        ds_put_cstr(s, "ADD");
        break;

    case OFPGC11_MODIFY:
        ds_put_cstr(s, "MOD");
        break;

    case OFPGC11_DELETE:
        ds_put_cstr(s, "DEL");
        break;

    case OFPGC15_INSERT_BUCKET:
        ds_put_cstr(s, "INSERT_BUCKET");
        bucket_command = true;
        break;

    case OFPGC15_REMOVE_BUCKET:
        ds_put_cstr(s, "REMOVE_BUCKET");
        bucket_command = true;
        break;

    default:
        ds_put_format(s, "cmd:%"PRIu16"", gm->command);
    }
    ds_put_char(s, ' ');

    if (bucket_command) {
        ofp_print_bucket_id(s, "command_bucket_id:",
                            gm->command_bucket_id, ofp_version);
    }

    ofp_print_group(s, gm->group_id, gm->type, &gm->buckets, &gm->props,
                    ofp_version, bucket_command);
}

static void
ofp_print_group_mod(struct ds *s, const struct ofp_header *oh)
{
    struct ofputil_group_mod gm;
    int error;

    error = ofputil_decode_group_mod(oh, &gm);
    if (error) {
        ofp_print_error(s, error);
        return;
    }
    ofp_print_group_mod__(s, oh->version, &gm);
    ofputil_bucket_list_destroy(&gm.buckets);
}

static void
print_table_action_features(struct ds *s,
                            const struct ofputil_table_action_features *taf)
{
    if (taf->ofpacts) {
        ds_put_cstr(s, "        actions: ");
        ofpact_bitmap_format(taf->ofpacts, s);
        ds_put_char(s, '\n');
    }

    if (!bitmap_is_all_zeros(taf->set_fields.bm, MFF_N_IDS)) {
        int i;

        ds_put_cstr(s, "        supported on Set-Field:");
        BITMAP_FOR_EACH_1 (i, MFF_N_IDS, taf->set_fields.bm) {
            ds_put_format(s, " %s", mf_from_id(i)->name);
        }
        ds_put_char(s, '\n');
    }
}

static bool
table_action_features_equal(const struct ofputil_table_action_features *a,
                            const struct ofputil_table_action_features *b)
{
    return (a->ofpacts == b->ofpacts
            && bitmap_equal(a->set_fields.bm, b->set_fields.bm, MFF_N_IDS));
}

static bool
table_action_features_empty(const struct ofputil_table_action_features *taf)
{
    return !taf->ofpacts && bitmap_is_all_zeros(taf->set_fields.bm, MFF_N_IDS);
}

static void
print_table_instruction_features(
    struct ds *s,
    const struct ofputil_table_instruction_features *tif,
    const struct ofputil_table_instruction_features *prev_tif)
{
    int start, end;

    if (!bitmap_is_all_zeros(tif->next, 255)) {
        ds_put_cstr(s, "      next tables: ");
        for (start = bitmap_scan(tif->next, 1, 0, 255); start < 255;
             start = bitmap_scan(tif->next, 1, end, 255)) {
            end = bitmap_scan(tif->next, 0, start + 1, 255);
            if (end == start + 1) {
                ds_put_format(s, "%d,", start);
            } else {
                ds_put_format(s, "%d-%d,", start, end - 1);
            }
        }
        ds_chomp(s, ',');
        if (ds_last(s) == ' ') {
            ds_put_cstr(s, "none");
        }
        ds_put_char(s, '\n');
    }

    if (tif->instructions) {
        if (prev_tif && tif->instructions == prev_tif->instructions) {
            ds_put_cstr(s, "      (same instructions)\n");
        } else {
            ds_put_cstr(s, "      instructions: ");
            int i;

            for (i = 0; i < 32; i++) {
                if (tif->instructions & (1u << i)) {
                    ds_put_format(s, "%s,", ovs_instruction_name_from_type(i));
                }
            }
            ds_chomp(s, ',');
            ds_put_char(s, '\n');
        }
    }

    if (prev_tif
        && table_action_features_equal(&tif->write, &prev_tif->write)
        && table_action_features_equal(&tif->apply, &prev_tif->apply)
        && !bitmap_is_all_zeros(tif->write.set_fields.bm, MFF_N_IDS)) {
        ds_put_cstr(s, "      (same actions)\n");
    } else if (!table_action_features_equal(&tif->write, &tif->apply)) {
        ds_put_cstr(s, "      Write-Actions features:\n");
        print_table_action_features(s, &tif->write);
        ds_put_cstr(s, "      Apply-Actions features:\n");
        print_table_action_features(s, &tif->apply);
    } else if (tif->write.ofpacts
               || !bitmap_is_all_zeros(tif->write.set_fields.bm, MFF_N_IDS)) {
        ds_put_cstr(s, "      Write-Actions and Apply-Actions features:\n");
        print_table_action_features(s, &tif->write);
    }
}

static bool
table_instruction_features_equal(
    const struct ofputil_table_instruction_features *a,
    const struct ofputil_table_instruction_features *b)
{
    return (bitmap_equal(a->next, b->next, 255)
            && a->instructions == b->instructions
            && table_action_features_equal(&a->write, &b->write)
            && table_action_features_equal(&a->apply, &b->apply));
}

static bool
table_instruction_features_empty(
    const struct ofputil_table_instruction_features *tif)
{
    return (bitmap_is_all_zeros(tif->next, 255)
            && !tif->instructions
            && table_action_features_empty(&tif->write)
            && table_action_features_empty(&tif->apply));
}

static bool
table_features_equal(const struct ofputil_table_features *a,
                     const struct ofputil_table_features *b)
{
    return (a->metadata_match == b->metadata_match
            && a->metadata_write == b->metadata_write
            && a->miss_config == b->miss_config
            && a->supports_eviction == b->supports_eviction
            && a->supports_vacancy_events == b->supports_vacancy_events
            && a->max_entries == b->max_entries
            && table_instruction_features_equal(&a->nonmiss, &b->nonmiss)
            && table_instruction_features_equal(&a->miss, &b->miss)
            && bitmap_equal(a->match.bm, b->match.bm, MFF_N_IDS));
}

static bool
table_features_empty(const struct ofputil_table_features *tf)
{
    return (!tf->metadata_match
            && !tf->metadata_write
            && tf->miss_config == OFPUTIL_TABLE_MISS_DEFAULT
            && tf->supports_eviction < 0
            && tf->supports_vacancy_events < 0
            && !tf->max_entries
            && table_instruction_features_empty(&tf->nonmiss)
            && table_instruction_features_empty(&tf->miss)
            && bitmap_is_all_zeros(tf->match.bm, MFF_N_IDS));
}

static bool
table_stats_equal(const struct ofputil_table_stats *a,
                  const struct ofputil_table_stats *b)
{
    return (a->active_count == b->active_count
            && a->lookup_count == b->lookup_count
            && a->matched_count == b->matched_count);
}

void
ofp_print_table_features(struct ds *s,
                         const struct ofputil_table_features *features,
                         const struct ofputil_table_features *prev_features,
                         const struct ofputil_table_stats *stats,
                         const struct ofputil_table_stats *prev_stats)
{
    int i;

    ds_put_format(s, "  table %"PRIu8, features->table_id);
    if (features->name[0]) {
        ds_put_format(s, " (\"%s\")", features->name);
    }
    ds_put_char(s, ':');

    bool same_stats = prev_stats && table_stats_equal(stats, prev_stats);
    bool same_features = prev_features && table_features_equal(features,
                                                               prev_features);
    if ((!stats || same_stats) && same_features) {
        ds_put_cstr(s, " ditto");
        return;
    }
    ds_put_char(s, '\n');
    if (stats) {
        ds_put_format(s, "    active=%"PRIu32", ", stats->active_count);
        ds_put_format(s, "lookup=%"PRIu64", ", stats->lookup_count);
        ds_put_format(s, "matched=%"PRIu64"\n", stats->matched_count);
    }
    if (same_features) {
        if (!table_features_empty(features)) {
            ds_put_cstr(s, "    (same features)\n");
        }
        return;
    }
    if (features->metadata_match || features->metadata_write) {
        ds_put_format(s, "    metadata: match=%#"PRIx64" write=%#"PRIx64"\n",
                      ntohll(features->metadata_match),
                      ntohll(features->metadata_write));
    }

    if (features->miss_config != OFPUTIL_TABLE_MISS_DEFAULT) {
        ds_put_format(s, "    config=%s\n",
                      ofputil_table_miss_to_string(features->miss_config));
    }

    if (features->supports_eviction >= 0) {
        ds_put_format(s, "    eviction: %ssupported\n",
                      features->supports_eviction ? "" : "not ");

    }
    if (features->supports_vacancy_events >= 0) {
        ds_put_format(s, "    vacancy events: %ssupported\n",
                      features->supports_vacancy_events ? "" : "not ");

    }

    if (features->max_entries) {
        ds_put_format(s, "    max_entries=%"PRIu32"\n", features->max_entries);
    }

    const struct ofputil_table_instruction_features *prev_nonmiss
        = prev_features ? &prev_features->nonmiss : NULL;
    const struct ofputil_table_instruction_features *prev_miss
        = prev_features ? &prev_features->miss : NULL;
    if (prev_features
        && table_instruction_features_equal(&features->nonmiss, prev_nonmiss)
        && table_instruction_features_equal(&features->miss, prev_miss)) {
        if (!table_instruction_features_empty(&features->nonmiss)) {
            ds_put_cstr(s, "    (same instructions)\n");
        }
    } else if (!table_instruction_features_equal(&features->nonmiss,
                                                 &features->miss)) {
        ds_put_cstr(s, "    instructions (other than table miss):\n");
        print_table_instruction_features(s, &features->nonmiss, prev_nonmiss);
        ds_put_cstr(s, "    instructions (table miss):\n");
        print_table_instruction_features(s, &features->miss, prev_miss);
    } else if (!table_instruction_features_empty(&features->nonmiss)) {
        ds_put_cstr(s, "    instructions (table miss and others):\n");
        print_table_instruction_features(s, &features->nonmiss, prev_nonmiss);
    }

    if (!bitmap_is_all_zeros(features->match.bm, MFF_N_IDS)) {
        if (prev_features
            && bitmap_equal(features->match.bm, prev_features->match.bm,
                            MFF_N_IDS)) {
            ds_put_cstr(s, "    (same matching)\n");
        } else {
            ds_put_cstr(s, "    matching:\n");
            BITMAP_FOR_EACH_1 (i, MFF_N_IDS, features->match.bm) {
                const struct mf_field *f = mf_from_id(i);
                bool mask = bitmap_is_set(features->mask.bm, i);
                bool wildcard = bitmap_is_set(features->wildcard.bm, i);

                ds_put_format(s, "      %s: %s\n",
                              f->name,
                              (mask ? "arbitrary mask"
                               : wildcard ? "exact match or wildcard"
                               : "must exact match"));
            }
        }
    }
}

static void
ofp_print_table_features_reply(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));

    struct ofputil_table_features prev;
    for (int i = 0; ; i++) {
        struct ofputil_table_features tf;
        int retval;

        retval = ofputil_decode_table_features(&b, &tf, true);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(s, retval);
            }
            return;
        }

        ds_put_char(s, '\n');
        ofp_print_table_features(s, &tf, i ? &prev : NULL, NULL, NULL);
        prev = tf;
    }
}

static void
ofp_print_table_desc_reply(struct ds *s, const struct ofp_header *oh)
{
    struct ofpbuf b;

    ofpbuf_use_const(&b, oh, ntohs(oh->length));

    for (;;) {
        struct ofputil_table_desc td;
        int retval;

        retval = ofputil_decode_table_desc(&b, &td, oh->version);
        if (retval) {
            if (retval != EOF) {
                ofp_print_error(s, retval);
            }
            return;
        }
        ofp_print_table_desc(s, &td);
    }
}

static const char *
bundle_flags_to_name(uint32_t bit)
{
    switch (bit) {
    case OFPBF_ATOMIC:
        return "atomic";
    case OFPBF_ORDERED:
        return "ordered";
    default:
        return NULL;
    }
}

static void
ofp_print_bundle_ctrl(struct ds *s, const struct ofp_header *oh)
{
    int error;
    struct ofputil_bundle_ctrl_msg bctrl;

    error = ofputil_decode_bundle_ctrl(oh, &bctrl);
    if (error) {
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, '\n');

    ds_put_format(s, " bundle_id=%#"PRIx32" type=",  bctrl.bundle_id);
    switch (bctrl.type) {
    case OFPBCT_OPEN_REQUEST:
        ds_put_cstr(s, "OPEN_REQUEST");
        break;
    case OFPBCT_OPEN_REPLY:
        ds_put_cstr(s, "OPEN_REPLY");
        break;
    case OFPBCT_CLOSE_REQUEST:
        ds_put_cstr(s, "CLOSE_REQUEST");
        break;
    case OFPBCT_CLOSE_REPLY:
        ds_put_cstr(s, "CLOSE_REPLY");
        break;
    case OFPBCT_COMMIT_REQUEST:
        ds_put_cstr(s, "COMMIT_REQUEST");
        break;
    case OFPBCT_COMMIT_REPLY:
        ds_put_cstr(s, "COMMIT_REPLY");
        break;
    case OFPBCT_DISCARD_REQUEST:
        ds_put_cstr(s, "DISCARD_REQUEST");
        break;
    case OFPBCT_DISCARD_REPLY:
        ds_put_cstr(s, "DISCARD_REPLY");
        break;
    }

    ds_put_cstr(s, " flags=");
    ofp_print_bit_names(s, bctrl.flags, bundle_flags_to_name, ' ');
}

static void
ofp_print_bundle_add(struct ds *s, const struct ofp_header *oh, int verbosity)
{
    int error;
    struct ofputil_bundle_add_msg badd;
    char *msg;

    error = ofputil_decode_bundle_add(oh, &badd, NULL);
    if (error) {
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, '\n');
    ds_put_format(s, " bundle_id=%#"PRIx32,  badd.bundle_id);
    ds_put_cstr(s, " flags=");
    ofp_print_bit_names(s, badd.flags, bundle_flags_to_name, ' ');

    ds_put_char(s, '\n');
    msg = ofp_to_string(badd.msg, ntohs(badd.msg->length), verbosity);
    if (msg) {
        ds_put_cstr(s, msg);
    }
}

static void
print_geneve_table(struct ds *s, struct ovs_list *mappings)
{
    struct ofputil_geneve_map *map;

    ds_put_cstr(s, " mapping table:\n");
    ds_put_cstr(s, " class\ttype\tlength\tmatch field\n");
    ds_put_cstr(s, " -----\t----\t------\t-----------");

    LIST_FOR_EACH (map, list_node, mappings) {
        ds_put_char(s, '\n');
        ds_put_format(s, " 0x%"PRIx16"\t0x%"PRIx8"\t%"PRIu8"\ttun_metadata%"PRIu16,
                      map->option_class, map->option_type, map->option_len,
                      map->index);
    }
}

static void
ofp_print_geneve_table_mod(struct ds *s, const struct ofp_header *oh)
{
    int error;
    struct ofputil_geneve_table_mod gtm;

    error = ofputil_decode_geneve_table_mod(oh, &gtm);
    if (error) {
        ofp_print_error(s, error);
        return;
    }

    ds_put_cstr(s, "\n ");

    switch (gtm.command) {
    case NXGTMC_ADD:
        ds_put_cstr(s, "ADD");
        break;
    case NXGTMC_DELETE:
        ds_put_cstr(s, "DEL");
        break;
    case NXGTMC_CLEAR:
        ds_put_cstr(s, "CLEAR");
        break;
    }

    if (gtm.command != NXGTMC_CLEAR) {
        print_geneve_table(s, &gtm.mappings);
    }

    ofputil_uninit_geneve_table(&gtm.mappings);
}

static void
ofp_print_geneve_table_reply(struct ds *s, const struct ofp_header *oh)
{
    int error;
    struct ofputil_geneve_table_reply gtr;
    struct ofputil_geneve_map *map;
    int allocated_space = 0;

    error = ofputil_decode_geneve_table_reply(oh, &gtr);
    if (error) {
        ofp_print_error(s, error);
        return;
    }

    ds_put_char(s, '\n');

    LIST_FOR_EACH (map, list_node, &gtr.mappings) {
        allocated_space += map->option_len;
    }

    ds_put_format(s, " max option space=%"PRIu32" max fields=%"PRIu16"\n",
                  gtr.max_option_space, gtr.max_fields);
    ds_put_format(s, " allocated option space=%d\n", allocated_space);
    ds_put_char(s, '\n');
    print_geneve_table(s, &gtr.mappings);

    ofputil_uninit_geneve_table(&gtr.mappings);
}

/* This function will print the request forward message. The reason for
 * request forward is taken from rf.request.type */
static void
ofp_print_requestforward(struct ds *string, const struct ofp_header *oh)
{
    struct ofputil_requestforward rf;
    enum ofperr error;

    error = ofputil_decode_requestforward(oh, &rf);
    if (error) {
        ofp_print_error(string, error);
        return;
    }

    ds_put_cstr(string, " reason=");

    switch (rf.reason) {
    case OFPRFR_GROUP_MOD:
        ds_put_cstr(string, "group_mod");
        ofp_print_group_mod__(string, oh->version, rf.group_mod);
        break;

    case OFPRFR_METER_MOD:
        ds_put_cstr(string, "meter_mod");
        ofp_print_meter_mod__(string, rf.meter_mod);
        break;
    }
    ofputil_destroy_requestforward(&rf);
}

static void
ofp_to_string__(const struct ofp_header *oh, enum ofpraw raw,
                struct ds *string, int verbosity)
{
    const void *msg = oh;

    ofp_header_to_string__(oh, raw, string);
    switch (ofptype_from_ofpraw(raw)) {

    case OFPTYPE_GROUP_STATS_REQUEST:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_group_request(string, oh);
        break;

    case OFPTYPE_GROUP_STATS_REPLY:
        ofp_print_group_stats(string, oh);
        break;

    case OFPTYPE_GROUP_DESC_STATS_REQUEST:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_group_desc_request(string, oh);
        break;

    case OFPTYPE_GROUP_DESC_STATS_REPLY:
        ofp_print_group_desc(string, oh);
        break;

    case OFPTYPE_GROUP_FEATURES_STATS_REQUEST:
        ofp_print_stats(string, oh);
        break;

    case OFPTYPE_GROUP_FEATURES_STATS_REPLY:
        ofp_print_group_features(string, oh);
        break;

    case OFPTYPE_GROUP_MOD:
        ofp_print_group_mod(string, oh);
        break;

    case OFPTYPE_TABLE_FEATURES_STATS_REQUEST:
    case OFPTYPE_TABLE_FEATURES_STATS_REPLY:
        ofp_print_table_features_reply(string, oh);
        break;

    case OFPTYPE_TABLE_DESC_REQUEST:
    case OFPTYPE_TABLE_DESC_REPLY:
        ofp_print_table_desc_reply(string, oh);
        break;

    case OFPTYPE_HELLO:
        ofp_print_hello(string, oh);
        break;

    case OFPTYPE_ERROR:
        ofp_print_error_msg(string, oh);
        break;

    case OFPTYPE_ECHO_REQUEST:
    case OFPTYPE_ECHO_REPLY:
        ofp_print_echo(string, oh, verbosity);
        break;

    case OFPTYPE_FEATURES_REQUEST:
        break;

    case OFPTYPE_FEATURES_REPLY:
        ofp_print_switch_features(string, oh);
        break;

    case OFPTYPE_GET_CONFIG_REQUEST:
        break;

    case OFPTYPE_GET_CONFIG_REPLY:
    case OFPTYPE_SET_CONFIG:
        ofp_print_switch_config(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_PACKET_IN:
        ofp_print_packet_in(string, oh, verbosity);
        break;

    case OFPTYPE_FLOW_REMOVED:
        ofp_print_flow_removed(string, oh);
        break;

    case OFPTYPE_PORT_STATUS:
        ofp_print_port_status(string, oh);
        break;

    case OFPTYPE_PACKET_OUT:
        ofp_print_packet_out(string, oh, verbosity);
        break;

    case OFPTYPE_FLOW_MOD:
        ofp_print_flow_mod(string, oh, verbosity);
        break;

    case OFPTYPE_PORT_MOD:
        ofp_print_port_mod(string, oh);
        break;

    case OFPTYPE_TABLE_MOD:
        ofp_print_table_mod(string, oh);
        break;

    case OFPTYPE_METER_MOD:
        ofp_print_meter_mod(string, oh);
        break;

    case OFPTYPE_BARRIER_REQUEST:
    case OFPTYPE_BARRIER_REPLY:
        break;

    case OFPTYPE_QUEUE_GET_CONFIG_REQUEST:
        ofp_print_queue_get_config_request(string, oh);
        break;

    case OFPTYPE_QUEUE_GET_CONFIG_REPLY:
        ofp_print_queue_get_config_reply(string, oh);
        break;

    case OFPTYPE_ROLE_REQUEST:
    case OFPTYPE_ROLE_REPLY:
        ofp_print_role_message(string, oh);
        break;
    case OFPTYPE_ROLE_STATUS:
        ofp_print_role_status_message(string, oh);
        break;

    case OFPTYPE_REQUESTFORWARD:
        ofp_print_requestforward(string, oh);
        break;

    case OFPTYPE_METER_STATS_REQUEST:
    case OFPTYPE_METER_CONFIG_STATS_REQUEST:
        ofp_print_stats(string, oh);
        ofp_print_meter_stats_request(string, oh);
        break;

    case OFPTYPE_METER_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_meter_stats_reply(string, oh);
        break;

    case OFPTYPE_METER_CONFIG_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_meter_config_reply(string, oh);
        break;

    case OFPTYPE_METER_FEATURES_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_meter_features_reply(string, oh);
        break;

    case OFPTYPE_DESC_STATS_REQUEST:
    case OFPTYPE_METER_FEATURES_STATS_REQUEST:
        ofp_print_stats(string, oh);
        break;

    case OFPTYPE_FLOW_STATS_REQUEST:
    case OFPTYPE_AGGREGATE_STATS_REQUEST:
        ofp_print_stats(string, oh);
        ofp_print_flow_stats_request(string, oh);
        break;

    case OFPTYPE_TABLE_STATS_REQUEST:
        ofp_print_stats(string, oh);
        break;

    case OFPTYPE_PORT_STATS_REQUEST:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_port_request(string, oh);
        break;

    case OFPTYPE_QUEUE_STATS_REQUEST:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_queue_request(string, oh);
        break;

    case OFPTYPE_DESC_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_desc_reply(string, oh);
        break;

    case OFPTYPE_FLOW_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_flow_stats_reply(string, oh);
        break;

    case OFPTYPE_QUEUE_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_queue_reply(string, oh, verbosity);
        break;

    case OFPTYPE_PORT_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_port_reply(string, oh, verbosity);
        break;

    case OFPTYPE_TABLE_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_table_stats_reply(string, oh);
        break;

    case OFPTYPE_AGGREGATE_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_aggregate_stats_reply(string, oh);
        break;

    case OFPTYPE_PORT_DESC_STATS_REQUEST:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_port_desc_request(string, oh);
        break;

    case OFPTYPE_PORT_DESC_STATS_REPLY:
        ofp_print_stats(string, oh);
        ofp_print_ofpst_port_desc_reply(string, oh);
        break;

    case OFPTYPE_FLOW_MOD_TABLE_ID:
        ofp_print_nxt_flow_mod_table_id(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_SET_FLOW_FORMAT:
        ofp_print_nxt_set_flow_format(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_SET_PACKET_IN_FORMAT:
        ofp_print_nxt_set_packet_in_format(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_FLOW_AGE:
        break;

    case OFPTYPE_SET_CONTROLLER_ID:
        ofp_print_nxt_set_controller_id(string, ofpmsg_body(oh));
        break;

    case OFPTYPE_GET_ASYNC_REPLY:
    case OFPTYPE_SET_ASYNC_CONFIG:
        ofp_print_nxt_set_async_config(string, oh);
        break;
    case OFPTYPE_GET_ASYNC_REQUEST:
        break;
    case OFPTYPE_FLOW_MONITOR_CANCEL:
        ofp_print_nxt_flow_monitor_cancel(string, msg);
        break;

    case OFPTYPE_FLOW_MONITOR_PAUSED:
    case OFPTYPE_FLOW_MONITOR_RESUMED:
        break;

    case OFPTYPE_FLOW_MONITOR_STATS_REQUEST:
        ofp_print_nxst_flow_monitor_request(string, msg);
        break;

    case OFPTYPE_FLOW_MONITOR_STATS_REPLY:
        ofp_print_nxst_flow_monitor_reply(string, msg);
        break;

    case OFPTYPE_BUNDLE_CONTROL:
        ofp_print_bundle_ctrl(string, msg);
        break;

    case OFPTYPE_BUNDLE_ADD_MESSAGE:
        ofp_print_bundle_add(string, msg, verbosity);
        break;

    case OFPTYPE_NXT_GENEVE_TABLE_MOD:
        ofp_print_geneve_table_mod(string, msg);
        break;

    case OFPTYPE_NXT_GENEVE_TABLE_REQUEST:
        break;

    case OFPTYPE_NXT_GENEVE_TABLE_REPLY:
        ofp_print_geneve_table_reply(string, msg);
        break;

    }
}

/* Composes and returns a string representing the OpenFlow packet of 'len'
 * bytes at 'oh' at the given 'verbosity' level.  0 is a minimal amount of
 * verbosity and higher numbers increase verbosity.  The caller is responsible
 * for freeing the string. */
char *
ofp_to_string(const void *oh_, size_t len, int verbosity)
{
    struct ds string = DS_EMPTY_INITIALIZER;
    const struct ofp_header *oh = oh_;

    if (!len) {
        ds_put_cstr(&string, "OpenFlow message is empty\n");
    } else if (len < sizeof(struct ofp_header)) {
        ds_put_format(&string, "OpenFlow packet too short (only %"PRIuSIZE" bytes):\n",
                      len);
    } else if (ntohs(oh->length) > len) {
        enum ofperr error;
        enum ofpraw raw;

        error = ofpraw_decode_partial(&raw, oh, len);
        if (!error) {
            ofp_header_to_string__(oh, raw, &string);
            ds_put_char(&string, '\n');
        }

        ds_put_format(&string,
                      "(***truncated to %"PRIuSIZE" bytes from %"PRIu16"***)\n",
                      len, ntohs(oh->length));
    } else if (ntohs(oh->length) < len) {
        ds_put_format(&string,
                      "(***only uses %"PRIu16" bytes out of %"PRIuSIZE"***)\n",
                      ntohs(oh->length), len);
    } else {
        enum ofperr error;
        enum ofpraw raw;

        error = ofpraw_decode(&raw, oh);
        if (!error) {
            ofp_to_string__(oh, raw, &string, verbosity);
            if (verbosity >= 5) {
                if (ds_last(&string) != '\n') {
                    ds_put_char(&string, '\n');
                }
                ds_put_hex_dump(&string, oh, len, 0, true);
            }
            if (ds_last(&string) != '\n') {
                ds_put_char(&string, '\n');
            }
            return ds_steal_cstr(&string);
        }

        ofp_print_error(&string, error);
    }
    ds_put_hex_dump(&string, oh, len, 0, true);
    return ds_steal_cstr(&string);
}

static void
print_and_free(FILE *stream, char *string)
{
    fputs(string, stream);
    free(string);
}

/* Pretty-print the OpenFlow packet of 'len' bytes at 'oh' to 'stream' at the
 * given 'verbosity' level.  0 is a minimal amount of verbosity and higher
 * numbers increase verbosity. */
void
ofp_print(FILE *stream, const void *oh, size_t len, int verbosity)
{
    print_and_free(stream, ofp_to_string(oh, len, verbosity));
}

/* Dumps the contents of the Ethernet frame in the 'len' bytes starting at
 * 'data' to 'stream'. */
void
ofp_print_packet(FILE *stream, const void *data, size_t len)
{
    print_and_free(stream, ofp_packet_to_string(data, len));
}
