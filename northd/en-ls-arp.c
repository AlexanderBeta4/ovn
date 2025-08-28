/*
  * Copyright (c) 2024, Red Hat, Inc.
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

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>

/* OVS includes */
#include "include/openvswitch/hmap.h"
#include "lib/bitmap.h"
#include "lib/socket-util.h"
#include "lib/uuidset.h"
#include "openvswitch/util.h"
#include "openvswitch/vlog.h"
#include "stopwatch.h"

/* OVN includes */
#include "en-lr-nat.h"
#include "en-ls-arp.h"
#include "lib/inc-proc-eng.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-util.h"
#include "lib/stopwatch-names.h"
#include "lflow-mgr.h"
#include "northd.h"

VLOG_DEFINE_THIS_MODULE(en_ls_arp);

/* static functions. */

struct ls_arp_input {
    const struct ovn_datapaths *ls_datapaths;
    const struct hmap *lr_nats;
};

static struct ls_arp_input
ls_arp_get_input_data(struct engine_node *node)
{
    const struct northd_data *northd_data =
        engine_get_input_data("northd", node);
    struct ed_type_lr_nat_data *lr_nat_data =
        engine_get_input_data("lr_nat", node);

    return (struct ls_arp_input) {
        .ls_datapaths = &northd_data->ls_datapaths,
        .lr_nats = &lr_nat_data->lr_nats.entries,
    };
}

static void
ls_arp_table_clear(struct ls_arp_table *table)
{
    struct ls_arp_record *ar;
    HMAP_FOR_EACH_POP (ar, key_node, &table->entries) {
        lflow_ref_destroy(ar->lflow_ref);
        hmapx_destroy(&ar->nat_records);
        free(ar);
    }
}

/*
    Return list of datapaths, assumed lswitch, that are gateways for given nat
*/
static void
nat_odmap_create(struct lr_nat_record *nr,
                 struct hmapx *odmap)
{
    for (int i = 0; i < nr->n_nat_entries; i++) {
        struct ovn_nat *ent = &nr->nat_entries[i];

        if (ent->is_valid
            && ent->l3dgw_port
            && ent->l3dgw_port->peer
            && ent->l3dgw_port->peer->od
            && !ent->is_distributed) {
            hmapx_add(odmap, ent->l3dgw_port->peer->od);
        }
    }
}

static bool
find_ods_by_index(struct hmapx *odmap, size_t index)
{
    struct hmapx_node *hmapx_node;
    HMAPX_FOR_EACH (hmapx_node, odmap) {
        struct ovn_datapath *od = hmapx_node->data;

        if (od->index == index) {
            return true;
        }
    }

    return false;
}

static struct ls_arp_record*
find_ars_by_index(const struct ls_arp_table *table, size_t index)
{
    struct ls_arp_record *ar;
    HMAP_FOR_EACH (ar, key_node, &table->entries) {
        if (ar->ls_index == index) {
            return ar;
        }
    }

    return NULL;
}

static void
ls_arp_record_set_nats(struct ls_arp_record *ar,
                       const struct hmap *nats)
{
    struct lr_nat_record *nr;
    HMAP_FOR_EACH (nr, key_node, nats) {
        struct hmapx B = HMAPX_INITIALIZER(&B);

        nat_odmap_create(nr, &B);

        if (find_ods_by_index(&B, ar->ls_index)) {
            hmapx_add(&ar->nat_records, nr);
        }

        hmapx_destroy(&B);
    }
}

static struct ls_arp_record *
ls_arp_record_create(struct ls_arp_table *table,
                     const struct ovn_datapath *od,
                     const struct hmap *lr_nats)
{
    struct ls_arp_record *ar = xzalloc(sizeof *ar);

    ar->ls_index = od->index;
    hmapx_init(&ar->nat_records);
    ls_arp_record_set_nats(ar, lr_nats);
    ar->lflow_ref = lflow_ref_create();

    hmap_insert(&table->entries, &ar->key_node,
                uuid_hash(&od->nbs->header_.uuid));

    return ar;
}

static bool
ls_arp_has_tracked_data(struct ls_arp_tracked_data *trk_data) {
    return !hmapx_is_empty(&trk_data->crupdated);
}

/* public functions. */
void*
en_ls_arp_init(struct engine_node *node OVS_UNUSED,
               struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_ls_arp *data = xzalloc(sizeof *data);

    hmap_init(&data->table.entries);
    hmapx_init(&data->trk_data.crupdated);

    return data;
}

void
en_ls_arp_cleanup(void *data_)
{
return;
    struct ed_type_ls_arp *data = data_;

    ls_arp_table_clear(&data->table);
    hmap_destroy (&data->table.entries);
    hmapx_destroy(&data->trk_data.crupdated);
}

void
en_ls_arp_clear_tracked_data(void *data_)
{
return;
    struct ed_type_ls_arp *data = data_;
    hmapx_clear(&data->trk_data.crupdated);
}

enum engine_node_state
en_ls_arp_run(struct engine_node *node, void *data_)

{
    return EN_UPDATED;

    struct ls_arp_input input_data = ls_arp_get_input_data(node);
    struct ed_type_ls_arp *data = data_;

    stopwatch_start(LS_ARP_RUN_STOPWATCH_NAME, time_msec());

    ls_arp_table_clear(&data->table);

    const struct ovn_datapath *od;
    HMAP_FOR_EACH (od, key_node, &input_data.ls_datapaths->datapaths) {
        ls_arp_record_create(&data->table, od, input_data.lr_nats);
    }

    stopwatch_stop(LS_ARP_RUN_STOPWATCH_NAME, time_msec());

    return EN_UPDATED;
}

/* Handler functions. */

enum engine_input_handler_result
ls_arp_northd_handler(struct engine_node *node, void*)
{
    struct northd_data *northd_data = engine_get_input_data("northd", node);
    if (!northd_has_tracked_data(&northd_data->trk_data)) {
        return EN_UNHANDLED;
    }

    return EN_HANDLED_UNCHANGED;
}

enum engine_input_handler_result
ls_arp_lr_nat_handler(struct engine_node *node, void *data_)
{
    struct ed_type_lr_nat_data *lr_nat_data =
        engine_get_input_data("lr_nat", node);

    if (!lr_nat_has_tracked_data(&lr_nat_data->trk_data)) {
        return EN_UNHANDLED;
    }

    return EN_HANDLED_UNCHANGED;

    struct ed_type_ls_arp *data = data_;
    struct hmapx_node *hmapx_node;

    HMAPX_FOR_EACH (hmapx_node, &lr_nat_data->trk_data.crupdated) {
        struct hmapx B = HMAPX_INITIALIZER(&B);
        struct lr_nat_record *nr_cur = hmapx_node->data;

        //  Find nr in nat_records
        //  If found then A := nat_records[i].odmap
        //    Create B := new_odmap from nr
        //    reviewMap = A\B + B\A
        //    Trigger ls from reviewMap
        //  Else
        //    Trigger ls from B
        //  End
        //  Save nr + B -> nat_records

        nat_odmap_create(nr_cur, &B);

        struct ls_arp_record *ar;
        LS_ARP_TABLE_FOR_EACH (ar, &data->table) {
            struct hmapx_node *nr_node = hmapx_find(&ar->nat_records, nr_cur);

            if (nr_node) {
                hmapx_add(&data->trk_data.crupdated, ar);

                if (!find_ods_by_index(&B, ar->ls_index)) {
                    hmapx_delete(&ar->nat_records, nr_node);
                }
            }
        }

        struct hmapx_node *hmapx_node2;
        HMAPX_FOR_EACH (hmapx_node2, &B) {
            struct ovn_datapath *od = hmapx_node2->data;

            ar = find_ars_by_index(&data->table, od->index);
            ovs_assert(ar);

            hmapx_add(&data->trk_data.crupdated, ar);
            hmapx_add(&ar->nat_records, nr_cur);
        }

        hmapx_destroy(&B);
    }

    if (ls_arp_has_tracked_data(&data->trk_data)) {
        return EN_HANDLED_UPDATED;
    }

    return EN_HANDLED_UNCHANGED;
}

