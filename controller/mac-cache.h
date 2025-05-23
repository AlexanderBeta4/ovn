/* Copyright (c) 2023, Red Hat, Inc.
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

#ifndef OVN_MAC_CACHE_H
#define OVN_MAC_CACHE_H

#include <stdint.h>

#include "dp-packet.h"
#include "openvswitch/hmap.h"
#include "openvswitch/hmap.h"
#include "openvswitch/list.h"
#include "openvswitch/ofpbuf.h"
#include "openvswitch/ofp-flow.h"
#include "openvswitch/ofp-packet.h"
#include "ovn-sb-idl.h"

struct ovsdb_idl_index;
struct vector;

struct mac_cache_data {
    /* 'struct mac_cache_threshold' by datapath's tunnel_key. */
    struct hmap thresholds;
    /* 'struct mac_cache_mac_binding' by 'struct mac_cache_mb_data' that are
     * local and have threshold > 0. */
    struct hmap mac_bindings;
    /* 'struct mac_cache_fdb' by 'struct mac_cache_fdb_data' that are
     * local and have threshold > 0. */
    struct hmap fdbs;
};

struct mac_cache_threshold {
    struct hmap_node hmap_node;
    /* Datapath tunnel key. */
    uint32_t dp_key;
    /* Aging threshold in ms. */
    uint64_t value;
    /* Statistics dump period. */
    uint64_t dump_period;
    /* How long to wait between two database updates. */
    uint64_t cooldown_period;
};

struct mac_binding_data {
    /* Keys. */
    uint64_t cookie;
    uint32_t port_key;
    uint32_t dp_key;
    struct in6_addr ip;
    /* Value. */
    struct eth_addr mac;
};

struct mac_binding {
    struct hmap_node hmap_node;
    /* Common data to identify MAC binding. */
    struct mac_binding_data data;
    /* Reference to the SB MAC binding record (Might be NULL). */
    const struct sbrec_mac_binding *sbrec;
    /* User specified timestamp (in ms) */
    long long timestamp;
};

struct fdb_data {
    /* Keys. */
    uint32_t dp_key;
    struct eth_addr mac;
    /* Value. */
    uint32_t port_key;
};

struct fdb {
    struct hmap_node hmap_node;
    /* Common data to identify FDB. */
    struct fdb_data data;
    /* Reference to the SB FDB record. */
    const struct sbrec_fdb *sbrec_fdb;
    long long timestamp;
};

struct mac_cache_stats {
    int64_t idle_age_ms;

    union {
        /* Common data to identify MAC binding. */
        struct mac_binding_data mb;
        /* Common data to identify FDB. */
        struct fdb_data fdb;
    } data;
};

struct bp_packet_data {
    struct ofpbuf *continuation;
    struct ofputil_packet_in pin;
};

struct buffered_packets {
    struct hmap_node hmap_node;

    struct mac_binding_data mb_data;

    /* Queue of packet_data associated with this struct. */
    struct vector queue;

    /* Timestamp in ms when the buffered packet should expire. */
    long long int expire_at_ms;

    /* Timestamp in ms when the buffered packet should do full SB lookup.*/
    long long int lookup_at_ms;
};

struct buffered_packets_ctx {
    /* Map of all buffered packets waiting for the MAC address. */
    struct hmap buffered_packets;
    /* List of packet data that are ready to be sent. */
    struct vector ready_packets_data;
};

/* Thresholds. */
void mac_cache_threshold_add(struct mac_cache_data *data,
                             const struct sbrec_datapath_binding *dp);
void mac_cache_threshold_replace(struct mac_cache_data *data,
                                 const struct sbrec_datapath_binding *dp,
                                 const struct hmap *local_datapaths);
struct mac_cache_threshold *
mac_cache_threshold_find(struct mac_cache_data *data, uint32_t dp_key);
void mac_cache_thresholds_sync(struct mac_cache_data *data,
                               const struct hmap *local_datapaths);
void mac_cache_thresholds_clear(struct mac_cache_data *data);

/* MAC binding. */

static inline void
mac_binding_data_init(struct mac_binding_data *data,
                      uint32_t dp_key, uint32_t port_key,
                      struct in6_addr ip, struct eth_addr mac)
{
    *data = (struct mac_binding_data) {
        .cookie = 0,
        .dp_key = (dp_key),
        .port_key = (port_key),
        .ip = (ip),
        .mac = (mac),
    };
}

void mac_binding_add(struct hmap *map, struct mac_binding_data mb_data,
                     const struct sbrec_mac_binding *, long long timestamp);

void mac_binding_remove(struct hmap *map, struct mac_binding *mb);

struct mac_binding *mac_binding_find(const struct hmap *map,
                                     const struct mac_binding_data *mb_data);

bool mac_binding_data_parse(struct mac_binding_data *data,
                            uint32_t dp_key, uint32_t port_key,
                            const char *ip_str, const char *mac_str);
bool mac_binding_data_from_sbrec(struct mac_binding_data *data,
                                 const struct sbrec_mac_binding *mb);

void mac_bindings_clear(struct hmap *map);
void mac_bindings_to_string(const struct hmap *map, struct ds *out_data);

/* FDB. */
struct fdb *fdb_add(struct hmap *map, struct fdb_data fdb_data,
                    long long timestamp);

void fdb_remove(struct hmap *map, struct fdb *fdb);

bool fdb_data_from_sbrec(struct fdb_data *data, const struct sbrec_fdb *fdb);

struct fdb *fdb_find(const struct hmap *map, const struct fdb_data *fdb_data);

void fdbs_clear(struct hmap *map);

/* MAC binding stat processing. */
void
mac_binding_stats_process_flow_stats(struct vector *stats_vec,
                                     struct ofputil_flow_stats *ofp_stats);

void mac_binding_stats_run(
        struct rconn *swconn OVS_UNUSED,
        struct ovsdb_idl_index *sbrec_port_binding_by_name OVS_UNUSED,
        struct vector *stats_vec, uint64_t *req_delay, void *data);

/* FDB stat processing. */
void fdb_stats_process_flow_stats(struct vector *stats_vec,
                                  struct ofputil_flow_stats *ofp_stats);

void fdb_stats_run(
        struct rconn *swconn OVS_UNUSED,
        struct ovsdb_idl_index *sbrec_port_binding_by_name OVS_UNUSED,
        struct vector *stats_vec, uint64_t *req_delay, void *data);

/* Packet buffering. */
void bp_packet_data_destroy(struct bp_packet_data *pd);

struct buffered_packets *
buffered_packets_add(struct buffered_packets_ctx *ctx,
                     struct mac_binding_data mb_data);

void buffered_packets_packet_data_enqueue(struct buffered_packets *bp,
                                          const struct ofputil_packet_in *pin,
                                          const struct ofpbuf *continuation);

void buffered_packets_ctx_run(struct buffered_packets_ctx *ctx,
                              const struct hmap *recent_mbs,
                              struct ovsdb_idl_index *sbrec_pb_by_key,
                              struct ovsdb_idl_index *sbrec_dp_by_key,
                              struct ovsdb_idl_index *sbrec_pb_by_name,
                              struct ovsdb_idl_index *sbrec_mb_by_lport_ip);

void buffered_packets_ctx_init(struct buffered_packets_ctx *ctx);

void buffered_packets_ctx_destroy(struct buffered_packets_ctx *ctx);

bool buffered_packets_ctx_is_ready_to_send(struct buffered_packets_ctx *ctx);

bool buffered_packets_ctx_has_packets(struct buffered_packets_ctx *ctx);

void mac_binding_probe_stats_process_flow_stats(
        struct vector *stats_vec,
        struct ofputil_flow_stats *ofp_stats);

void mac_binding_probe_stats_run(
        struct rconn *swconn,
        struct ovsdb_idl_index *sbrec_port_binding_by_name,
        struct vector *stats_vec, uint64_t *req_delay, void *data);

#endif /* controller/mac-cache.h */
