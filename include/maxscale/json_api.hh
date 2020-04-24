/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-04-23
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

/**
 * @file Helper functions for creating JSON API conforming objects
 *
 * @see http://jsonapi.org/format/
 */

#include <maxscale/ccdefs.hh>
#include <maxbase/jansson.h>
#include <vector>

/** Resource endpoints */
#define MXS_JSON_API_SERVERS     "/servers/"
#define MXS_JSON_API_SERVICES    "/services/"
#define MXS_JSON_API_LISTENERS   "/listeners/"
#define MXS_JSON_API_FILTERS     "/filters/"
#define MXS_JSON_API_MONITORS    "/monitors/"
#define MXS_JSON_API_SESSIONS    "/sessions/"
#define MXS_JSON_API_MAXSCALE    "/maxscale/"
#define MXS_JSON_API_THREADS     "/maxscale/threads/"
#define MXS_JSON_API_LOGS        "/maxscale/logs/"
#define MXS_JSON_API_TASKS       "/maxscale/tasks/"
#define MXS_JSON_API_MODULES     "/maxscale/modules/"
#define MXS_JSON_API_QC_STATS    "/maxscale/qc_stats/"
#define MXS_JSON_API_QC          "/maxscale/query_classifier/"
#define MXS_JSON_API_QC_CLASSIFY "/maxscale/query_classifier/classify"
#define MXS_JSON_API_QC_CACHE    "/maxscale/query_classifier/cache"
#define MXS_JSON_API_USERS       "/users/"

/**
 * @brief Create a JSON object
 *
 * @param host Hostname of this server
 * @param self Endpoint of this resource
 * @param data The JSON data, either an array or an object, stored in
 *             the `data` field of the returned object
 *
 * @return A valid top-level JSON API object
 */
json_t* mxs_json_resource(const char* host, const char* self, json_t* data);

/**
 * Validate the JSON object
 *
 * @return An empty string if the given JSON is a valid JSON API resource object. A non-empty string with the
 *         error description if the JSON is not valid.
 */
std::string mxs_is_valid_json_resource(json_t* json);

/**
 * @brief Create a JSON metadata object
 *
 * This should be used to transport non-standard data to the client.
 *
 * @param host Hostname of this server
 * @param self Endpoint of this resource
 * @param data The JSON data, either an array or an object, stored in
 *             the `meta` field of the returned object
 *
 * @return A valid top-level JSON API object
 */
json_t* mxs_json_metadata(const char* host, const char* self, json_t* data);

/**
 * @brief Create an empty relationship object
 *
 * @param host    Hostname of this server
 * @param self    The self link to this relationship
 * @param related The related resource collection
 *
 * @return New relationship object
 */
json_t* mxs_json_relationship(const std::string& host, const std::string& self, const std::string& related);

/**
 * @brief Add an item to a relationship object
 *
 * @param rel  Relationship created with mxs_json_relationship
 * @param id   The resource identifier
 * @param type The resource type
 */
void mxs_json_add_relation(json_t* rel, const char* id, const char* type);

/**
 * @brief Create self link object
 *
 * The self link points to the object itself.
 *
 * @param host Hostname of this server
 * @param path Base path to the resource collection
 * @param id   The identified of this resource
 *
 * @return New self link object
 */
json_t* mxs_json_self_link(const char* host, const char* path, const char* id);

/**
 * @brief Return value at provided JSON Pointer
 *
 * @param json     JSON object
 * @param json_ptr JSON Pointer to object
 *
 * @return Pointed value or NULL if no value is found
 */
json_t* mxs_json_pointer(json_t* json, const char* json_ptr);

/**
 * @brief Check if the value at the provided JSON Pointer is of a certain type
 *
 * @param json     JSON object
 * @param json_ptr JSON Pointer to object
 * @param type     JSON type that is expected
 *
 * @return False if the object was found but it was not of the expected type. True in all other cases.
 */
bool mxs_json_is_type(json_t* json, const char* json_ptr, json_type type);

/**
 * @brief Return a JSON formatted error
 *
 * @param format Format string
 * @param ...    Variable argument list
 *
 * @return The error as JSON
 */
json_t* mxs_json_error(const char* format, ...) mxb_attribute((format (printf, 1, 2)));
json_t* mxs_json_error(const std::vector<std::string>& errors);

/**
 * @brief Append error to existing JSON object.
 *
 * @param object Existing json error object, or NULL.
 * @param format Format string
 * @param ...    Variable argument list
 *
 * @return The error added to 'errors' array of the JSON object.
 */
json_t* mxs_json_error_append(json_t* object, const char* format, ...) mxb_attribute((format (printf, 2, 3)));
