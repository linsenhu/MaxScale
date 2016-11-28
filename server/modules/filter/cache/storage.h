#pragma once
/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2019-07-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/cdefs.h>
#include "cache_storage_api.h"

class Storage
{
public:
    virtual ~Storage();

    virtual cache_result_t get_key(const char* zDefaultDb,
                                   const GWBUF* pQuery,
                                   CACHE_KEY* pKey) = 0;

    virtual cache_result_t get_value(const CACHE_KEY& key,
                                     uint32_t flags,
                                     GWBUF** ppValue) = 0;

    virtual cache_result_t put_value(const CACHE_KEY& key,
                                     const GWBUF* pValue) = 0;

    virtual cache_result_t del_value(const CACHE_KEY& key) = 0;

protected:
    Storage();

    Storage(const Storage&);
    Storage& operator = (const Storage&);
};
