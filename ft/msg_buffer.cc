/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:

/*
COPYING CONDITIONS NOTICE:

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation, and provided that the
  following conditions are met:

      * Redistributions of source code must retain this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below).

      * Redistributions in binary form must reproduce this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below) in the documentation and/or other materials
        provided with the distribution.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301, USA.

COPYRIGHT NOTICE:

  TokuDB, Tokutek Fractal Tree Indexing Library.
  Copyright (C) 2014 Tokutek, Inc.

DISCLAIMER:

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

UNIVERSITY PATENT NOTICE:

  The technology is licensed by the Massachusetts Institute of
  Technology, Rutgers State University of New Jersey, and the Research
  Foundation of State University of New York at Stony Brook under
  United States of America Serial No. 11/760379 and to the patents
  and/or patent applications resulting from it.

PATENT MARKING NOTICE:

  This software is covered by US Patent No. 8,185,551.
  This software is covered by US Patent No. 8,489,638.

PATENT RIGHTS GRANT:

  "THIS IMPLEMENTATION" means the copyrightable works distributed by
  Tokutek as part of the Fractal Tree project.

  "PATENT CLAIMS" means the claims of patents that are owned or
  licensable by Tokutek, both currently or in the future; and that in
  the absence of this license would be infringed by THIS
  IMPLEMENTATION or by using or running THIS IMPLEMENTATION.

  "PATENT CHALLENGE" shall mean a challenge to the validity,
  patentability, enforceability and/or non-infringement of any of the
  PATENT CLAIMS or otherwise opposing any of the PATENT CLAIMS.

  Tokutek hereby grants to you, for the term and geographical scope of
  the PATENT CLAIMS, a non-exclusive, no-charge, royalty-free,
  irrevocable (except as stated in this section) patent license to
  make, have made, use, offer to sell, sell, import, transfer, and
  otherwise run, modify, and propagate the contents of THIS
  IMPLEMENTATION, where such license applies only to the PATENT
  CLAIMS.  This grant does not include claims that would be infringed
  only as a consequence of further modifications of THIS
  IMPLEMENTATION.  If you or your agent or licensee institute or order
  or agree to the institution of patent litigation against any entity
  (including a cross-claim or counterclaim in a lawsuit) alleging that
  THIS IMPLEMENTATION constitutes direct or contributory patent
  infringement, or inducement of patent infringement, then any rights
  granted to you under this License shall terminate as of the date
  such litigation is filed.  If you or your agent or exclusive
  licensee institute or order or agree to the institution of a PATENT
  CHALLENGE, then Tokutek may terminate any rights granted to you
  under this License.
*/

#include "ft/msg_buffer.h"
#include "ft/ybt.h"

void message_buffer::create() {
    _num_entries = 0;
    _memory = nullptr;
    _memory_size = 0;
    _memory_used = 0;
}

void message_buffer::clone(message_buffer *src) {
    _num_entries = src->_num_entries;
    _memory_used = src->_memory_used;
    _memory_size = src->_memory_size;
    XMALLOC_N(_memory_size, _memory);
    memcpy(_memory, src->_memory, _memory_size);
}

void message_buffer::destroy() {
    if (_memory != nullptr) {
        toku_free(_memory);
    }
}

void message_buffer::resize(size_t new_size) {
    XREALLOC_N(new_size, _memory);
    _memory_size = new_size;
}

static int next_power_of_two (int n) {
    int r = 4096;
    while (r < n) {
        r*=2;
        assert(r>0);
    }
    return r;
}

struct message_buffer::buffer_entry *message_buffer::get_buffer_entry(int32_t offset) const {
    return (struct buffer_entry *) (_memory + offset);
}

void message_buffer::enqueue(FT_MSG msg, bool is_fresh, int32_t *offset) {
    ITEMLEN keylen = ft_msg_get_keylen(msg);
    ITEMLEN datalen = ft_msg_get_vallen(msg);
    XIDS xids = ft_msg_get_xids(msg);
    int need_space_here = sizeof(struct buffer_entry)
                          + keylen + datalen
                          + xids_get_size(xids)
                          - sizeof(XIDS_S); //Prevent double counting
    int need_space_total = _memory_used + need_space_here;
    if (_memory == nullptr || need_space_total > _memory_size) {
        // resize the buffer to the next power of 2 greater than the needed space
        int next_2 = next_power_of_two(need_space_total);
        resize(next_2);
    }
    struct buffer_entry *entry = get_buffer_entry(_memory_used);
    entry->type = (unsigned char) ft_msg_get_type(msg);
    entry->msn = msg->msn;
    xids_cpy(&entry->xids_s, xids);
    entry->is_fresh = is_fresh;
    unsigned char *e_key = xids_get_end_of_array(&entry->xids_s);
    entry->keylen = keylen;
    memcpy(e_key, ft_msg_get_key(msg), keylen);
    entry->vallen = datalen;
    memcpy(e_key + keylen, ft_msg_get_val(msg), datalen);
    if (offset) {
        *offset = _memory_used;
    }
    _num_entries++;
    _memory_used += need_space_here;
}

void message_buffer::set_freshness(int32_t offset, bool is_fresh) {
    struct buffer_entry *entry = get_buffer_entry(offset);
    entry->is_fresh = is_fresh;
}

bool message_buffer::get_freshness(int32_t offset) const {
    struct buffer_entry *entry = get_buffer_entry(offset);
    return entry->is_fresh;
}

FT_MSG_S message_buffer::get_message(int32_t offset, DBT *keydbt, DBT *valdbt) const {
    struct buffer_entry *entry = get_buffer_entry(offset);
    ITEMLEN keylen = entry->keylen;
    ITEMLEN vallen = entry->vallen;
    enum ft_msg_type type = (enum ft_msg_type) entry->type;
    MSN msn = entry->msn;
    const XIDS xids = (XIDS) &entry->xids_s;
    bytevec key = xids_get_end_of_array(xids);
    bytevec val = (uint8_t *) key + entry->keylen;
    FT_MSG_S msg = {
        type, msn, xids,
        .u = { .id = { toku_fill_dbt(keydbt, key, keylen), toku_fill_dbt(valdbt, val, vallen) } }
    };
    return msg;
}

void message_buffer::get_message_key_msn(int32_t offset, DBT *key, MSN *msn) const {
    struct buffer_entry *entry = get_buffer_entry(offset);
    if (key != nullptr) {
        toku_fill_dbt(key, xids_get_end_of_array((XIDS) &entry->xids_s), entry->keylen);
    }
    if (msn != nullptr) {
        *msn = entry->msn;
    }
}

int message_buffer::num_entries() const {
    return _num_entries;
}

size_t message_buffer::buffer_size_in_use() const {
    return _memory_used;
}

size_t message_buffer::memory_size_in_use() const {
    return sizeof(*this) + _memory_used;
}

size_t message_buffer::memory_footprint() const {
    return sizeof(*this) + toku_memory_footprint(_memory, _memory_used);
}

bool message_buffer::equals(message_buffer *other) const {
    return (_memory_used == other->_memory_used &&
            memcmp(_memory, other->_memory, _memory_used) == 0);
}

size_t message_buffer::msg_memsize_in_buffer(FT_MSG msg) {
    return sizeof(struct buffer_entry)
        + msg->u.id.key->size + msg->u.id.val->size
        + xids_get_size(msg->xids)
        - sizeof(XIDS_S);
}