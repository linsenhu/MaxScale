/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-01-15
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * @file config.c Configuration file processing
 */

#include <maxscale/config.hh>

#include <ctype.h>
#include <ftw.h>
#include <fcntl.h>
#include <glob.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <ini.h>

#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <unordered_set>

#include <maxbase/atomic.hh>
#include <maxbase/format.hh>
#include <maxbase/pretty_print.hh>
#include <maxscale/adminusers.hh>
#include <maxbase/alloc.h>
#include <maxscale/clock.h>
#include <maxscale/housekeeper.h>
#include <maxscale/http.hh>
#include <maxscale/json_api.hh>
#include <maxscale/limits.h>
#include <maxscale/maxscale.h>
#include <maxscale/paths.h>
#include <maxscale/pcre2.hh>
#include <maxscale/router.hh>
#include <maxscale/secrets.hh>
#include <maxscale/utils.h>
#include <maxscale/utils.hh>
#include <maxscale/version.h>

#include "internal/config.hh"
#include "internal/event.hh"
#include "internal/filter.hh"
#include "internal/listener.hh"
#include "internal/modules.hh"
#include "internal/monitor.hh"
#include "internal/monitormanager.hh"
#include "internal/servermanager.hh"
#include "internal/service.hh"

using std::set;
using std::string;
using maxscale::Monitor;
using std::chrono::milliseconds;
using std::chrono::seconds;

config::Specification MXS_CONFIG::s_specification("maxscale", config::Specification::GLOBAL);

config::ParamBool MXS_CONFIG::s_load_persisted_configs(
    &MXS_CONFIG::s_specification,
    CN_LOAD_PERSISTED_CONFIGS,
    "Specifies whether persisted configuration files should be loaded on startup.",
    true);

config::ParamInteger MXS_CONFIG::s_max_auth_errors_until_block(
    &MXS_CONFIG::s_specification,
    CN_MAX_AUTH_ERRORS_UNTIL_BLOCK,
    "The maximum number of authentication failures that are tolerated "
    "before a host is temporarily blocked.",
    DEFAULT_MAX_AUTH_ERRORS_UNTIL_BLOCK,
    0, std::numeric_limits<config::ParamInteger::value_type>::max(), // min, max
    config::Param::Modifiable::AT_RUNTIME);

config::ParamInteger MXS_CONFIG::s_rebalance_threshold(
    &MXS_CONFIG::s_specification,
    CN_REBALANCE_THRESHOLD,
    "If the difference in load between the thread with the maximum load and the thread "
    "with the minimum load is larger than the value of this parameter, then work will "
    "be moved from the former to the latter.",
    20,     // default
    5, 100, // min, max
    config::Param::Modifiable::AT_RUNTIME);

config::ParamDuration<std::chrono::milliseconds> MXS_CONFIG::s_rebalance_period(
    &MXS_CONFIG::s_specification,
    CN_REBALANCE_PERIOD,
    "How often should the load of the worker threads be checked and rebalancing be made.",
    mxs::config::NO_INTERPRETATION,
    std::chrono::milliseconds(0),
    config::Param::Modifiable::AT_RUNTIME);

config::ParamCount MXS_CONFIG::s_rebalance_window(
    &MXS_CONFIG::s_specification,
    CN_REBALANCE_WINDOW,
    "The load of how many seconds should be taken into account when rebalancing.",
    10,     // default
    1, 60,  // min, max
    config::Param::Modifiable::AT_RUNTIME);

MXS_CONFIG::MXS_CONFIG()
    : config::Configuration("maxscale", &s_specification)
    , load_persisted_configs(this, &s_load_persisted_configs)
    , max_auth_errors_until_block(this, &s_max_auth_errors_until_block)
    , rebalance_threshold(this, &s_rebalance_threshold)
    , rebalance_period(this, &s_rebalance_period)
    , rebalance_window(this, &s_rebalance_window)
{
}

void MXS_CONFIG::RebalancePeriod::do_set(const milliseconds& new_value)
{
    config::Duration<milliseconds>::do_set(new_value);

    mxb_assert(mxs::MainWorker::get());
    mxs::MainWorker::get()->start_rebalancing();
}

static bool        process_config_context(CONFIG_CONTEXT*);
static bool        process_config_update(CONFIG_CONTEXT*);
static int         handle_global_item(const char*, const char*);
static bool        check_config_objects(CONFIG_CONTEXT* context);
static int         maxscale_getline(char** dest, int* size, FILE* file);
static bool        check_first_last_char(const char* string, char expected);
static void        remove_first_last_char(char* value);
static bool        test_regex_string_validity(const char* regex_string, const char* key);
static pcre2_code* compile_regex_string(const char* regex_string,
                                        bool jit_enabled,
                                        uint32_t options,
                                        uint32_t* output_ovector_size);
static bool duration_is_valid(const char* zValue, mxs::config::DurationUnit* pUnit);
static bool get_seconds(const char* zName, const char* zValue, seconds* pSeconds);
static bool get_seconds(const char* zName, const char* zValue, time_t* pSeconds);
static bool get_milliseconds(const char* zName,
                             const char* zValue,
                             const char* zDisplay_value,
                             std::chrono::milliseconds* pMilliseconds);
static bool get_milliseconds(const char* zName,
                             const char* zValue,
                             const char* zDisplay_value,
                             time_t* pMilliseconds);
static void log_duration_suffix_warning(const char* zName, const char* zValue);

int         config_get_ifaddr(unsigned char* output);
static int  config_get_release_string(char* release);
bool        config_has_duplicate_sections(const char* config, DUPLICATE_CONTEXT* context);
int         create_new_service(CONFIG_CONTEXT* obj);
int         create_new_server(CONFIG_CONTEXT* obj);
int         create_new_monitor(CONFIG_CONTEXT* obj, std::set<std::string>& monitored_servers);
int         create_new_listener(CONFIG_CONTEXT* obj);
int         create_new_filter(CONFIG_CONTEXT* obj);
void        config_fix_param(const MXS_MODULE_PARAM* params, const string& name, string* value);
std::string closest_matching_parameter(const std::string& str,
                                       const MXS_MODULE_PARAM* base,
                                       const MXS_MODULE_PARAM* mod);

static const char* config_file = NULL;
static MXS_CONFIG gateway;
static bool is_persisted_config = false;    /**< True if a persisted configuration file is being parsed */
static CONFIG_CONTEXT config_context;

namespace
{

const char CN_ADMIN_PAM_READWRITE_SERVICE[] = "admin_pam_readwrite_service";
const char CN_ADMIN_PAM_READONLY_SERVICE[] = "admin_pam_readonly_service";
const char CN_LOCAL_ADDRESS[] = "local_address";
const char CN_USERS_REFRESH_TIME[] = "users_refresh_time";
const char CN_USERS_REFRESH_INTERVAL[] = "users_refresh_interval";
}

const MXS_MODULE_PARAM config_filter_params[] =
{
    {
        CN_TYPE, MXS_MODULE_PARAM_STRING,
        CN_FILTER,
        MXS_MODULE_OPT_REQUIRED
    },
    {
        CN_MODULE,
        MXS_MODULE_PARAM_STRING,
        NULL,
        MXS_MODULE_OPT_REQUIRED
    },
    {NULL}
};

/*
 * This is currently only used in handle_global_item() to verify that
 * all global configuration item names are valid.
 */
const char* config_pre_parse_global_params[] =
{
    CN_LOGDIR,
    CN_LIBDIR,
    CN_PIDDIR,
    CN_DATADIR,
    CN_CACHEDIR,
    CN_LANGUAGE,
    CN_EXECDIR,
    CN_CONNECTOR_PLUGINDIR,
    CN_PERSISTDIR,
    CN_MODULE_CONFIGDIR,
    CN_SYSLOG,
    CN_MAXLOG,
    CN_LOG_AUGMENTATION,
    CN_SUBSTITUTE_VARIABLES,
    NULL
};

const char* deprecated_server_params[] =
{
    CN_AUTHENTICATOR_OPTIONS,
    NULL
};

void config_finish()
{
    config_context_free(config_context.m_next);
}

bool duplicate_context_init(DUPLICATE_CONTEXT* context)
{
    bool rv = false;

    std::set<std::string>* sections = new(std::nothrow) std::set<std::string>;
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code* re = pcre2_compile((PCRE2_SPTR) "^\\s*\\[(.+)\\]\\s*$",
                                   PCRE2_ZERO_TERMINATED,
                                   0,
                                   &errcode,
                                   &erroffset,
                                   NULL);
    pcre2_match_data* mdata = NULL;

    if (sections && re && (mdata = pcre2_match_data_create_from_pattern(re, NULL)))
    {
        context->sections = sections;
        context->re = re;
        context->mdata = mdata;
        rv = true;
    }
    else
    {
        pcre2_match_data_free(mdata);
        pcre2_code_free(re);
        delete sections;
    }

    return rv;
}

void duplicate_context_finish(DUPLICATE_CONTEXT* context)
{
    pcre2_match_data_free(context->mdata);
    pcre2_code_free(context->re);
    delete context->sections;

    context->mdata = NULL;
    context->re = NULL;
    context->sections = NULL;
}


/**
 * Remove extra commas and whitespace from a string. This string is interpreted
 * as a list of string values separated by commas.
 * @param strptr String to clean
 * @return pointer to a new string or NULL if an error occurred
 */
char* config_clean_string_list(const char* str)
{
    size_t destsize = strlen(str) + 1;
    char* dest = (char*)MXS_MALLOC(destsize);

    if (dest)
    {
        pcre2_code* re;
        pcre2_match_data* data;
        int re_err;
        size_t err_offset;

        if ((re = pcre2_compile((PCRE2_SPTR) "[[:space:],]*([^,]*[^[:space:],])[[:space:],]*",
                                PCRE2_ZERO_TERMINATED,
                                0,
                                &re_err,
                                &err_offset,
                                NULL)) == NULL
            || (data = pcre2_match_data_create_from_pattern(re, NULL)) == NULL)
        {
            PCRE2_UCHAR errbuf[MXS_STRERROR_BUFLEN];
            pcre2_get_error_message(re_err, errbuf, sizeof(errbuf));
            MXS_ERROR("[%s] Regular expression compilation failed at %d: %s",
                      __FUNCTION__,
                      (int)err_offset,
                      errbuf);
            pcre2_code_free(re);
            MXS_FREE(dest);
            return NULL;
        }

        const char* replace = "$1,";
        int rval = 0;
        size_t destsize_tmp = destsize;
        while ((rval = pcre2_substitute(re,
                                        (PCRE2_SPTR) str,
                                        PCRE2_ZERO_TERMINATED,
                                        0,
                                        PCRE2_SUBSTITUTE_GLOBAL,
                                        data,
                                        NULL,
                                        (PCRE2_SPTR) replace,
                                        PCRE2_ZERO_TERMINATED,
                                        (PCRE2_UCHAR*) dest,
                                        &destsize_tmp)) == PCRE2_ERROR_NOMEMORY)
        {
            destsize_tmp = 2 * destsize;
            char* tmp = (char*)MXS_REALLOC(dest, destsize_tmp);
            if (tmp == NULL)
            {
                MXS_FREE(dest);
                dest = NULL;
                break;
            }
            dest = tmp;
            destsize = destsize_tmp;
        }

        /** Remove the trailing comma */
        if (dest && dest[strlen(dest) - 1] == ',')
        {
            dest[strlen(dest) - 1] = '\0';
        }

        pcre2_code_free(re);
        pcre2_match_data_free(data);
    }

    return dest;
}

CONFIG_CONTEXT::CONFIG_CONTEXT(const string& section)
    : m_name(section)
    , m_was_persisted(is_persisted_config)
    , m_next(nullptr)
{
}

CONFIG_CONTEXT* config_context_create(const char* section)
{
    return new CONFIG_CONTEXT(section);
}

void fix_object_name(char* name)
{
    mxb::trim(name);
}

void fix_object_name(std::string& name)
{
    char buf[name.size() + 1];
    strcpy(buf, name.c_str());
    fix_object_name(buf);
    name.assign(buf);
}

static bool is_empty_string(const char* str)
{
    for (const char* p = str; *p; p++)
    {
        if (!isspace(*p))
        {
            return false;
        }
    }

    return true;
}

static bool is_maxscale_section(const char* section)
{
    return strcasecmp(section, CN_GATEWAY) == 0 || strcasecmp(section, CN_MAXSCALE) == 0;
}

static bool is_root_config_file = true;

static int ini_global_handler(void* userdata, const char* section, const char* name, const char* value)
{
    return is_maxscale_section(section) ? handle_global_item(name, value) : 1;
}

/**
 * Config item handler for the ini file reader
 *
 * @param userdata      The config context element
 * @param section       The config file section
 * @param name          The Parameter name
 * @param value         The Parameter value
 * @return zero on error
 */
static int ini_handler(void* userdata, const char* section, const char* name, const char* value)
{
    CONFIG_CONTEXT* cntxt = (CONFIG_CONTEXT*)userdata;
    CONFIG_CONTEXT* ptr = cntxt;

    const std::set<std::string> legacy_parameters {"passwd"};

    if (is_persisted_config && legacy_parameters.count(name))
    {
        /**
         * Ignore legacy parameters in persisted configurations. Needs to be
         * done to make upgrades from pre-2.3 versions work.
         */
        return 1;
    }

    if (is_empty_string(value))
    {
        if (is_persisted_config)
        {
            /**
             * Found old-style persisted configuration. These will be automatically
             * upgraded on the next modification so we can safely ignore it.
             */
            return 1;
        }
        else
        {
            MXS_ERROR("Empty value given to parameter '%s'", name);
            return 0;
        }
    }

    if (config_get_global_options()->substitute_variables)
    {
        if (*value == '$')
        {
            char* env_value = getenv(value + 1);

            if (!env_value)
            {
                MXS_ERROR("The environment variable %s, used as value for parameter %s "
                          "in section %s, does not exist.",
                          value + 1,
                          name,
                          section);
                return 0;
            }

            value = env_value;
        }
    }

    if (strlen(section) == 0)
    {
        MXS_ERROR("Parameter '%s=%s' declared outside a section.", name, value);
        return 0;
    }

    string reason;
    if (!config_is_valid_name(section, &reason))
    {
        /* A set that holds all the section names that are invalid. As the configuration file
         * is parsed multiple times, we need to do this to prevent the same complaint from
         * being logged multiple times.
         */
        static std::set<string> warned_invalid_names;

        if (warned_invalid_names.find(reason) == warned_invalid_names.end())
        {
            MXS_ERROR("%s", reason.c_str());
            warned_invalid_names.insert(reason);
        }
        return 0;
    }

    /*
     * If we already have some parameters for the object
     * add the parameters to that object. If not create
     * a new object.
     */
    while (ptr && strcmp(ptr->name(), section) != 0)
    {
        ptr = ptr->m_next;
    }

    if (!ptr)
    {
        if ((ptr = config_context_create(section)) == NULL)
        {
            return 0;
        }

        ptr->m_next = cntxt->m_next;
        cntxt->m_next = ptr;
    }

    if (ptr && !ptr->m_was_persisted && is_persisted_config)
    {
        MXS_NOTICE("Found persisted configuration for '%s'.", ptr->name());
        ptr->m_was_persisted = true;
        ptr->m_parameters.clear();
    }

    if (ptr->m_parameters.contains(name))
    {
        /** The values in the persisted configurations are updated versions of
         * the ones in the main configuration file.  */
        if (is_persisted_config)
        {
            if (!config_replace_param(ptr, name, value))
            {
                return 0;
            }
        }
        /** Multi-line parameter */
        else if (!config_append_param(ptr, name, value))
        {
            return 0;
        }
    }
    else if (!config_add_param(ptr, name, value))
    {
        return 0;
    }

    if (is_maxscale_section(section))
    {
        if (!is_root_config_file && !is_persisted_config)
        {
            MXS_ERROR("The [maxscale] section must only be defined in the root configuration file.");
            return 0;
        }
    }

    return 1;
}

static void log_config_error(const char* file, int rval)
{
    char errorbuffer[1024 + 1];

    if (rval > 0)
    {
        snprintf(errorbuffer,
                 sizeof(errorbuffer),
                 "Failed to parse configuration file %s. Error on line %d.",
                 file,
                 rval);
    }
    else if (rval == -1)
    {
        snprintf(errorbuffer,
                 sizeof(errorbuffer),
                 "Failed to parse configuration file %s. Could not open file.",
                 file);
    }
    else
    {
        snprintf(errorbuffer,
                 sizeof(errorbuffer),
                 "Failed to parse configuration file %s. Memory allocation failed.",
                 file);
    }

    MXS_ERROR("%s", errorbuffer);
}

bool config_load_single_file(const char* file,
                             DUPLICATE_CONTEXT* dcontext,
                             CONFIG_CONTEXT* ccontext)
{
    int rval = -1;

    // With multiple configuration files being loaded, we need to log the file
    // currently being loaded so that the context is clear in case of errors.
    MXS_NOTICE("Loading %s.", file);

    if (!config_has_duplicate_sections(file, dcontext))
    {
        if ((rval = ini_parse(file, ini_handler, ccontext)) != 0)
        {
            log_config_error(file, rval);
        }
    }

    /* Check this after reading config is finished */
    if ((gateway.writeq_high_water || gateway.writeq_low_water)
        && gateway.writeq_high_water <= gateway.writeq_low_water)
    {
        rval = -1;
        MXS_ERROR("Invaild configuration, writeq_high_water should be greater than writeq_low_water");
    }

    return rval == 0;
}

/**
 * The current parsing contexts must be managed explicitly since the ftw callback
 * can not have user data.
 */
static CONFIG_CONTEXT* current_ccontext;
static DUPLICATE_CONTEXT* current_dcontext;

/**
 * The nftw callback.
 *
 * @see man ftw
 */
int config_cb(const char* fpath, const struct stat* sb, int typeflag, struct FTW* ftwbuf)
{
    int rval = 0;

    if (typeflag == FTW_SL)     // A symbolic link; let's see what it points to.
    {
        struct stat sb;

        if (stat(fpath, &sb) == 0)
        {
            int file_type = (sb.st_mode & S_IFMT);

            switch (file_type)
            {
            case S_IFREG:
                // Points to a file; we'll handle that regardless of where the file resides.
                typeflag = FTW_F;
                break;

            case S_IFDIR:
                // Points to a directory; we'll ignore that.
                MXS_WARNING("Symbolic link %s in configuration directory points to a "
                            "directory; it will be ignored.",
                            fpath);
                break;

            default:
                // Points to something else; we'll silently ignore.
                ;
            }
        }
        else
        {
            MXS_WARNING("Could not get information about the symbolic link %s; "
                        "it will be ignored.",
                        fpath);
        }
    }

    if (typeflag == FTW_F)      // We are only interested in files,
    {
        const char* filename = fpath + ftwbuf->base;
        const char* dot = strrchr(filename, '.');

        if (dot && *filename != '.')    // that have a suffix and are not hidden,
        {
            const char* suffix = dot + 1;

            if (strcmp(suffix, "cnf") == 0)     // that is ".cnf".
            {
                mxb_assert(current_dcontext);
                mxb_assert(current_ccontext);

                if (!config_load_single_file(fpath, current_dcontext, current_ccontext))
                {
                    rval = -1;
                }
            }
        }
    }

    return rval;
}

/**
 * Loads all configuration files in a directory hierarchy.
 *
 * Only files with the suffix ".cnf" are considered to be configuration files.
 *
 * @param dir      The directory.
 * @param dcontext The duplicate section context.
 * @param ccontext The configuration context.
 *
 * @return True, if all configuration files in the directory hierarchy could be loaded,
 *         otherwise false.
 */
static bool config_load_dir(const char* dir, DUPLICATE_CONTEXT* dcontext, CONFIG_CONTEXT* ccontext)
{
    // Since there is no way to pass userdata to the callback, we need to store
    // the current context into a static variable. Consequently, we need lock.
    // Should not matter since config_load() is called once at startup.
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);

    int nopenfd = 5;    // Maximum concurrently opened directory descriptors

    current_dcontext = dcontext;
    current_ccontext = ccontext;
    int rv = nftw(dir, config_cb, nopenfd, FTW_PHYS);
    current_ccontext = NULL;
    current_dcontext = NULL;

    return rv == 0;
}

/**
 * Check if a directory exists
 *
 * This function also logs warnings if the directory cannot be accessed or if
 * the file is not a directory.
 * @param dir Directory to check
 * @return True if the file is an existing directory
 */
static bool is_directory(const char* dir)
{
    bool rval = false;
    struct stat st;
    if (stat(dir, &st) == -1)
    {
        if (errno == ENOENT)
        {
            MXS_NOTICE("%s does not exist, not reading.", dir);
        }
        else
        {
            MXS_WARNING("Could not access %s, not reading: %s",
                        dir,
                        mxs_strerror(errno));
        }
    }
    else
    {
        if (S_ISDIR(st.st_mode))
        {
            rval = true;
        }
        else
        {
            MXS_WARNING("%s exists, but it is not a directory. Ignoring.", dir);
        }
    }

    return rval;
}

/**
 * @brief Check if a directory contains .cnf files
 *
 * @param path Path to a directory
 * @return True if the directory contained one or more .cnf files
 */
static bool contains_cnf_files(const char* path)
{
    bool rval = false;
    glob_t matches;
    const char suffix[] = "/*.cnf";
    char pattern[strlen(path) + sizeof(suffix)];

    strcpy(pattern, path);
    strcat(pattern, suffix);
    int rc = glob(pattern, GLOB_NOSORT, NULL, &matches);

    switch (rc)
    {
    case 0:
        rval = true;
        break;

    case GLOB_NOSPACE:
        MXS_OOM();
        break;

    case GLOB_ABORTED:
        MXS_ERROR("Failed to read directory '%s'", path);
        break;

    default:
        mxb_assert(rc == GLOB_NOMATCH);
        break;
    }

    globfree(&matches);

    return rval;
}

bool export_config_file(const char* filename)
{
    bool rval = true;
    std::vector<CONFIG_CONTEXT*> contexts;

    // The config objects are stored in reverse order so first convert it back
    // to the correct order
    for (CONFIG_CONTEXT* ctx = config_context.m_next; ctx; ctx = ctx->m_next)
    {
        contexts.push_back(ctx);
    }

    std::ofstream file(filename);

    if (file)
    {
        file << "# Generated by MaxScale " << MAXSCALE_VERSION << '\n';
        file << "# Documentation: https://mariadb.com/kb/en/mariadb-enterprise/maxscale/ \n\n";

        for (auto it = contexts.rbegin(); it != contexts.rend(); it++)
        {
            CONFIG_CONTEXT* ctx = *it;

            file << '[' << ctx->m_name << "]\n";
            for (const auto& elem : ctx->m_parameters)
            {
                file << elem.first << '=' << elem.second << '\n';
            }
            file << '\n';
        }
    }
    else
    {
        MXS_ERROR("Failed to open configuration export file '%s': %d, %s",
                  filename,
                  errno,
                  mxs_strerror(errno));
        rval = false;
    }

    return rval;
}

/**
 * @brief Load the specified configuration file for MaxScale
 *
 * This function will parse the configuration file, check for duplicate sections,
 * validate the module parameters and finally turn it into a set of objects.
 *
 * @param filename        The filename of the configuration file
 * @param process_config  The function using which the successfully loaded
 *                        configuration should be processed.
 *
 * @return True on success, false on fatal error
 */
static bool config_load_and_process(const char* filename, bool (* process_config)(CONFIG_CONTEXT*))
{
    bool rval = false;
    DUPLICATE_CONTEXT dcontext;
    bool have_persisted_configs = false;

    if (duplicate_context_init(&dcontext))
    {
        if (config_load_single_file(filename, &dcontext, &config_context))
        {
            is_root_config_file = false;
            const char DIR_SUFFIX[] = ".d";

            char dir[strlen(filename) + sizeof(DIR_SUFFIX)];
            strcpy(dir, filename);
            strcat(dir, DIR_SUFFIX);

            rval = true;

            if (is_directory(dir))
            {
                rval = config_load_dir(dir, &dcontext, &config_context);
            }

            /** Create the persisted configuration directory if it doesn't exist */
            const char* persist_cnf = get_config_persistdir();
            mxs_mkdir_all(persist_cnf, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

            if (config_get_global_options()->load_persisted_configs
                && is_directory(persist_cnf) && contains_cnf_files(persist_cnf))
            {
                /**
                 * Set the global flag that we are processing a persisted configuration.
                 * This will tell the modules whether it is OK to completely overwrite
                 * the persisted configuration when changes are made.
                 *
                 * TODO: Figure out a cleaner way to do this
                 */
                is_persisted_config = true;
                have_persisted_configs = true;

                MXS_NOTICE("Runtime configuration changes have been done to MaxScale. Loading persisted "
                           "configuration files and applying them on top of the main configuration file. "
                           "These changes can override the values of the main configuration file: "
                           "To revert them, remove all the files in '%s'.", persist_cnf);
                DUPLICATE_CONTEXT p_dcontext;
                /**
                 * We need to initialize a second duplicate context for the
                 * generated configuration files as the monitors and services will
                 * have duplicate sections. The duplicate sections are used to
                 * store changes to the list of servers the services and monitors
                 * use, and thus should not be treated as errors.
                 */
                if (duplicate_context_init(&p_dcontext))
                {
                    rval = config_load_dir(persist_cnf, &p_dcontext, &config_context);
                    duplicate_context_finish(&p_dcontext);
                }
                else
                {
                    rval = false;
                }
                is_persisted_config = false;
            }

            if (rval)
            {
                if (!check_config_objects(config_context.m_next) || !process_config(config_context.m_next))
                {
                    rval = false;
                    if (have_persisted_configs)
                    {
                        MXS_WARNING("Persisted configuration files generated by runtime configuration "
                                    "changes were found at '%s' and at least one configuration error was "
                                    "encountered. If the errors relate to any of the persisted configuration "
                                    "files, remove the offending files and restart MaxScale.", persist_cnf);
                    }
                }
            }
        }

        duplicate_context_finish(&dcontext);
    }
    return rval;
}

bool config_load_global(const char* filename)
{
    int rval;

    if ((rval = ini_parse(filename, ini_global_handler, NULL)) != 0)
    {
        log_config_error(filename, rval);
    }
    else if (gateway.qc_cache_properties.max_size == -1)
    {
        gateway.qc_cache_properties.max_size = 0;
        MXS_WARNING("Failed to automatically detect available system memory: disabling the query classifier "
                    "cache. To enable it, add '%s' to the configuration file.",
                    CN_QUERY_CLASSIFIER_CACHE_SIZE);
    }
    else if (gateway.qc_cache_properties.max_size == 0)
    {
        MXS_NOTICE("Query classifier cache is disabled");
    }
    else
    {
        MXS_NOTICE("Using up to %s of memory for query classifier cache",
                   mxb::pretty_size(gateway.qc_cache_properties.max_size).c_str());
    }

    return rval == 0;
}

/**
 * @brief Load the configuration file for the MaxScale
 *
 * @param filename The filename of the configuration file
 * @return True on success, false on fatal error
 */
bool config_load(const char* filename)
{
    mxb_assert(!config_file);

    config_file = filename;
    bool rval = config_load_and_process(filename, process_config_context);

    return rval;
}

bool valid_object_type(std::string type)
{
    std::set<std::string> types {CN_SERVICE, CN_LISTENER, CN_SERVER, CN_MONITOR, CN_FILTER};
    return types.count(type);
}

const char* get_missing_module_parameter_name(const CONFIG_CONTEXT* obj)
{
    std::string type = obj->m_parameters.get_string(CN_TYPE);

    if (type == CN_SERVICE && !obj->m_parameters.contains(CN_ROUTER))
    {
        return CN_ROUTER;
    }
    else if (type == CN_LISTENER && !obj->m_parameters.contains(CN_PROTOCOL))
    {
        return CN_PROTOCOL;
    }
    else if ((type == CN_MONITOR || type == CN_FILTER) && !obj->m_parameters.contains(CN_MODULE))
    {
        return CN_MODULE;
    }
    return nullptr;
}

std::pair<const MXS_MODULE_PARAM*, const MXS_MODULE*> get_module_details(const CONFIG_CONTEXT* obj)
{
    std::string type = obj->m_parameters.get_string(CN_TYPE);

    if (type == CN_SERVICE)
    {
        auto name = obj->m_parameters.get_string(CN_ROUTER);
        return {common_service_params(), get_module(name.c_str(), MODULE_ROUTER)};
    }
    else if (type == CN_LISTENER)
    {
        auto name = obj->m_parameters.get_string(CN_PROTOCOL);
        return {common_listener_params(), get_module(name.c_str(), MODULE_PROTOCOL)};
    }
    else if (type == CN_SERVER)
    {
        // Servers do not have an associated module.
        return {common_server_params(), nullptr};
    }
    else if (type == CN_MONITOR)
    {
        auto name = obj->m_parameters.get_string(CN_MODULE);
        return {common_monitor_params(), get_module(name.c_str(), MODULE_MONITOR)};
    }
    else if (type == CN_FILTER)
    {
        auto name = obj->m_parameters.get_string(CN_MODULE);
        return {config_filter_params, get_module(name.c_str(), MODULE_FILTER)};
    }

    mxb_assert(!true);
    return {nullptr, nullptr};
}

CONFIG_CONTEXT* name_to_object(const std::vector<CONFIG_CONTEXT*>& objects,
                               const CONFIG_CONTEXT* obj,
                               std::string name)
{
    CONFIG_CONTEXT* rval = nullptr;

    fix_object_name(name);

    auto equal_name = [&](CONFIG_CONTEXT* c) {
            std::string s = c->m_name;
            fix_object_name(s);
            return s == name;
        };

    auto it = std::find_if(objects.begin(), objects.end(), equal_name);

    if (it == objects.end())
    {
        MXS_ERROR("Could not find object '%s' that '%s' depends on. "
                  "Check that the configuration object exists.",
                  name.c_str(),
                  obj->name());
    }
    else
    {
        rval = *it;
    }

    return rval;
}

std::unordered_set<CONFIG_CONTEXT*> get_dependencies(const std::vector<CONFIG_CONTEXT*>& objects,
                                                     const CONFIG_CONTEXT* obj)
{
    std::unordered_set<CONFIG_CONTEXT*> rval;
    const MXS_MODULE_PARAM* common_params = nullptr;
    const MXS_MODULE* module;
    std::tie(common_params, module) = get_module_details(obj);

    std::string type = obj->m_parameters.get_string(CN_TYPE);
    bool is_server = (type == CN_SERVER);
    const MXS_MODULE_PARAM* module_params = nullptr;
    if (!is_server)
    {
        module_params = module->parameters;
    }

    for (const auto* p : {common_params, module_params})
    {
        if (p)
        {
            for (int i = 0; p[i].name; i++)
            {
                if (obj->m_parameters.contains(p[i].name))
                {
                    if (p[i].type == MXS_MODULE_PARAM_SERVICE
                        || p[i].type == MXS_MODULE_PARAM_SERVER
                        || p[i].type == MXS_MODULE_PARAM_TARGET)
                    {
                        std::string v = obj->m_parameters.get_string(p[i].name);
                        rval.insert(name_to_object(objects, obj, v));
                    }
                }
            }
        }
    }

    if (type == CN_SERVICE && obj->m_parameters.contains(CN_FILTERS))
    {
        for (std::string name : mxs::strtok(obj->m_parameters.get_string(CN_FILTERS), "|"))
        {
            rval.insert(name_to_object(objects, obj, name));
        }
    }

    if (type == CN_SERVICE && obj->m_parameters.contains(CN_TARGETS))
    {
        for (auto name : mxs::strtok(obj->m_parameters.get_string(CN_TARGETS), ","))
        {
            rval.insert(name_to_object(objects, obj, name));
        }
    }

    if (type == CN_SERVICE && obj->m_parameters.contains(CN_CLUSTER))
    {
        rval.insert(name_to_object(objects, obj, obj->m_parameters.get_string(CN_CLUSTER)));
    }

    if ((type == CN_MONITOR || type == CN_SERVICE) && obj->m_parameters.contains(CN_SERVERS))
    {
        for (std::string name : mxs::strtok(obj->m_parameters.get_string(CN_SERVERS), ","))
        {
            rval.insert(name_to_object(objects, obj, name));
        }
    }

    return rval;
}

namespace
{

// Represents a node in a graph
template<class T>
struct Node
{
    static const int NOT_VISITED = 0;

    T    value;
    int  index;
    int  lowlink;
    bool on_stack;

    Node(T value)
        : value(value)
        , index(NOT_VISITED)
        , lowlink(NOT_VISITED)
        , on_stack(false)
    {
    }
};

template<class T>
using Container = std::unordered_map<T, std::unordered_set<T>>;
template<class T>
using Groups = std::vector<std::vector<T>>;
template<class T>
using Graph = std::unordered_multimap<Node<T>*, Node<T>*>;

/**
 * Calculate strongly connected components (i.e. cycles) of a graph
 *
 * @see https://en.wikipedia.org/wiki/Tarjan%27s_strongly_connected_components_algorithm
 *
 * @param graph An std::unordered_multimap where the keys represent nodes and
 *              the set of values for that key as the edges from that node
 *
 * @return A list of groups where each group is an ordered list of values
 */
template<class T>
Groups<T> get_graph_cycles(Container<T> graph)
{
    using namespace std::placeholders;

    std::vector<Node<T>> nodes;

    auto find_node = [&](T target, const Node<T>& n) {
            return n.value == target;
        };

    // Iterate over all values and place unique values in the vector.
    for (auto&& a : graph)
    {
        nodes.emplace_back(a.first);
    }

    Graph<T> node_graph;

    for (auto&& a : graph)
    {
        auto first = std::find_if(nodes.begin(), nodes.end(), std::bind(find_node, a.first, _1));

        for (auto&& b : a.second)
        {
            auto second = std::find_if(nodes.begin(), nodes.end(), std::bind(find_node, b, _1));
            node_graph.emplace(&(*first), &(*second));
        }
    }

    std::vector<Node<T>*> stack;
    Groups<T> groups;

    std::function<void(Node<T>*)> visit_node = [&](Node<T>* n) {
            static int s_index = 1;
            n->index = s_index++;
            n->lowlink = n->index;
            stack.push_back(n);
            n->on_stack = true;
            auto range = node_graph.equal_range(n);

            for (auto it = range.first; it != range.second; it++)
            {
                Node<T>* s = it->second;

                if (s->index == Node<T>::NOT_VISITED)
                {
                    visit_node(s);
                    n->lowlink = std::min(n->lowlink, s->lowlink);
                }
                else if (n == s)
                {
                    // This isn't strictly according to the algorithm but this is a convenient spot where we
                    // can easily spot cycles of size one. Adding an extra group with the two nodes in it
                    // causes it to be reported correctly.
                    groups.push_back({n->value, s->value});
                }
                else if (s->on_stack)
                {
                    n->lowlink = std::min(n->lowlink, s->index);
                }
            }

            if (n->index == n->lowlink)
            {
                // Start a new group
                groups.emplace_back();

                Node<T>* c;

                do
                {
                    c = stack.back();
                    stack.pop_back();
                    c->on_stack = false;
                    groups.back().push_back(c->value);
                }
                while (c != n);
            }
        };

    for (auto n = nodes.begin(); n != nodes.end(); n++)
    {
        if (n->index == Node<T>::NOT_VISITED)
        {
            visit_node((Node<T>*) & (*n));
        }
    }

    return groups;
}
}

/**
 * Resolve dependencies in the configuration and validate them
 *
 * @param objects List of objects, sorted so that dependencies are constructed first
 *
 * @return True if the configuration has bad dependencies
 */
bool resolve_dependencies(std::vector<CONFIG_CONTEXT*>& objects)
{
    int errors = 0;
    std::unordered_map<CONFIG_CONTEXT*, std::unordered_set<CONFIG_CONTEXT*>> g;

    for (const auto& obj : objects)
    {
        auto deps = get_dependencies(objects, obj);

        if (deps.count(nullptr))
        {
            // a missing reference, reported in get_dependencies
            errors++;
        }
        else
        {
            g.insert(std::make_pair(obj, deps));
        }
    }

    if (errors == 0)
    {
        std::vector<CONFIG_CONTEXT*> result;

        for (const auto& group : get_graph_cycles<CONFIG_CONTEXT*>(g))
        {
            if (group.size() > 1)
            {
                auto join = [](std::string total, CONFIG_CONTEXT* c) {
                        return total + " -> " + c->m_name;
                    };

                std::string first = group[0]->m_name;
                std::string str_group = std::accumulate(std::next(group.begin()), group.end(), first, join);
                str_group += " -> " + first;
                MXS_ERROR("A circular dependency chain was found in the configuration: %s",
                          str_group.c_str());
                errors++;
            }
            else
            {
                mxb_assert(!group.empty());
                /** Due to the algorithm that was used, the strongly connected
                 * components are always identified before the nodes that depend
                 * on them. This means that the result is sorted at the same
                 * time the circular dependencies are resolved. */
                result.push_back(group[0]);
            }
        }

        // The end result should contain the same set of nodes we started with
        mxb_assert(std::set<CONFIG_CONTEXT*>(result.begin(), result.end())
                   == std::set<CONFIG_CONTEXT*>(objects.begin(), objects.end())
                   || errors > 0);

        objects = std::move(result);
    }

    return errors > 0;
}

/**
 * @brief Process a configuration context and turn it into the set of objects
 *
 * @param context The parsed configuration context
 * @return False on fatal error, true on success
 */
static bool process_config_context(CONFIG_CONTEXT* context)
{
    std::vector<CONFIG_CONTEXT*> objects;

    for (CONFIG_CONTEXT* obj = context; obj; obj = obj->m_next)
    {
        if (!is_maxscale_section(obj->name()))
        {
            objects.push_back(obj);
        }
    }

    int error_count = 0;

    /**
     * Build the servers first to keep them in configuration file order. As
     * servers can't have references, this is safe to do as the first step.
     */
    for (CONFIG_CONTEXT* obj : objects)
    {
        std::string type = obj->m_parameters.get_string(CN_TYPE);
        mxb_assert(!type.empty());

        if (type == CN_SERVER)
        {
            error_count += create_new_server(obj);
        }
    }

    // Resolve any remaining dependencies between the objects
    if (resolve_dependencies(objects) || error_count)
    {
        return false;
    }

    std::set<std::string> monitored_servers;

    /**
     * Process the data and create the services defined in the data.
     */
    for (CONFIG_CONTEXT* obj : objects)
    {
        std::string type = obj->m_parameters.get_string(CN_TYPE);
        mxb_assert(!type.empty());

        if (type == CN_SERVICE)
        {
            error_count += create_new_service(obj);
        }
        else if (type == CN_FILTER)
        {
            error_count += create_new_filter(obj);
        }
        else if (type == CN_LISTENER)
        {
            error_count += create_new_listener(obj);
        }
        else if (type == CN_MONITOR)
        {
            error_count += create_new_monitor(obj, monitored_servers);
        }

        if (error_count)
        {
            /**
             * We need to stop creating objects after the first error since
             * any objects that depend on the object that failed would fail in
             * a very confusing manner.
             */
            break;
        }
    }

    if (error_count == 0)
    {
        MonitorManager::populate_services();
    }
    else
    {
        MXS_ERROR("%d errors were encountered while processing the configuration "
                  "file '%s'.",
                  error_count,
                  config_file);
    }

    return error_count == 0;
}

bool mxs::ConfigParameters::get_bool(const std::string& key) const
{
    string param_value = get_string(key);
    return param_value.empty() ? false : config_truth_value(param_value.c_str());
}

uint64_t mxs::ConfigParameters::get_size(const std::string& key) const
{
    string param_value = get_string(key);
    uint64_t intval = 0;
    MXB_AT_DEBUG(bool rval = ) get_suffixed_size(param_value.c_str(), &intval);
    mxb_assert(rval);
    return intval;
}

milliseconds mxs::ConfigParameters::get_duration_in_ms(const std::string& key,
                                                      mxs::config::DurationInterpretation interpretation)
const
{
    string value = get_string(key);
    milliseconds duration {0};
    MXB_AT_DEBUG(bool rval = ) get_suffixed_duration(value.c_str(), interpretation, &duration);
    // When this function is called, the validity of the value should have been checked.
    mxb_assert_message(rval, "Invalid value for '%s': %s", key.c_str(), value.c_str());
    return duration;
}

int64_t mxs::ConfigParameters::get_enum(const std::string& key, const MXS_ENUM_VALUE* enum_mapping) const
{
    int64_t rv = 0;

    for (const auto& tok : mxs::strtok(get_string(key), ", \t"))
    {
        auto value = config_enum_to_value(tok, enum_mapping);

        if (value == MXS_UNKNOWN_ENUM_VALUE)
        {
            rv = MXS_UNKNOWN_ENUM_VALUE;
            break;
        }

        rv |= value;
    }

    return rv;
}

SERVICE* mxs::ConfigParameters::get_service(const std::string& key) const
{
    string param_value = get_string(key);
    return service_find(param_value.c_str());
}

SERVER* mxs::ConfigParameters::get_server(const std::string& key) const
{
    string param_value = get_string(key);
    return ServerManager::find_by_unique_name(param_value.c_str());
}

bool mxs::ConfigParameters::contains(const string& key) const
{
    // Because of how the parameters are used, this method can be called through a null pointer.
    // Handle this here for now. TODO: Refactor away.
    auto can_be_null = this;
    return can_be_null ? m_contents.count(key) > 0 : false;
}

std::vector<SERVER*> mxs::ConfigParameters::get_server_list(const string& key, string* name_error_out) const
{
    auto names_list = get_string(key);
    auto server_names = config_break_list_string(names_list);
    std::vector<SERVER*> server_arr = SERVER::server_find_by_unique_names(server_names);
    for (size_t i = 0; i < server_arr.size(); i++)
    {
        if (server_arr[i] == nullptr)
        {
            if (name_error_out)
            {
                *name_error_out = server_names[i];
            }
            // If even one server name was not found, the parameter is in error.
            server_arr.clear();
            break;
        }
    }
    return server_arr;
}

mxs::Target* mxs::ConfigParameters::get_target(const string& key) const
{
    return mxs::Target::find(get_string(key));
}

std::vector<mxs::Target*> mxs::ConfigParameters::get_target_list(const string& key) const
{
    std::vector<mxs::Target*> targets;

    for (auto t : mxb::strtok(get_string(key), ", "))
    {
        targets.push_back(mxs::Target::find(t));
        mxb_assert(targets.back());
    }

    return targets;
}

char* mxs::ConfigParameters::get_c_str_copy(const string& key) const
{
    string value = get_string(key);
    char* rval = NULL;
    if (!value.empty())
    {
        rval = MXS_STRDUP_A(value.c_str());
    }
    return rval;
}

std::unique_ptr<pcre2_code> mxs::ConfigParameters::get_compiled_regex(const string& key, uint32_t options,
                                                                     uint32_t* output_ovec_size) const
{
    auto regex_string = get_string(key);
    std::unique_ptr<pcre2_code> code;

    if (!regex_string.empty())
    {
        uint32_t jit_available = 0;
        pcre2_config(PCRE2_CONFIG_JIT, &jit_available);
        code.reset(compile_regex_string(regex_string.c_str(), jit_available, options, output_ovec_size));
    }

    return code;
}

std::vector<std::unique_ptr<pcre2_code>> mxs::ConfigParameters::get_compiled_regexes(
    const std::vector<string>& keys,
    uint32_t options,
    uint32_t* ovec_size_out,
    bool* compile_error_out)
{
    std::vector<std::unique_ptr<pcre2_code>> rval;
    bool compile_error = false;
    uint32_t max_ovec_size = 0;
    uint32_t ovec_size_temp = 0;
    for (auto& key : keys)
    {
        std::unique_ptr<pcre2_code> code;
        /* get_compiled_regex() returns null if the config setting didn't exist. */
        if (contains(key))
        {
            code = get_compiled_regex(key, options, &ovec_size_temp);
            if (code)
            {
                if (ovec_size_temp > max_ovec_size)
                {
                    max_ovec_size = ovec_size_temp;
                }
            }
            else
            {
                compile_error = true;
            }
        }
        rval.push_back(std::move(code));
    }

    if (ovec_size_out)
    {
        *ovec_size_out = max_ovec_size;
    }
    if (compile_error_out)
    {
        *compile_error_out = compile_error;
    }
    return rval;
}

string mxs::ConfigParameters::get_string(const std::string& key) const
{
    string rval;
    auto iter = m_contents.find(key);
    if (iter != m_contents.end())
    {
        rval = iter->second;
    }
    return rval;
}

int64_t mxs::ConfigParameters::get_integer(const std::string& key) const
{
    string value = get_string(key);
    return value.empty() ? 0 : strtoll(value.c_str(), NULL, 10);
}

void config_free_one_param(mxs::ConfigParameters* p1)
{
    if (p1)
    {
        delete p1;
    }
}

void config_context_free(CONFIG_CONTEXT* context)
{
    CONFIG_CONTEXT* obj;
    while (context)
    {
        obj = context->m_next;
        delete context;
        context = obj;
    }
}

bool config_add_param(CONFIG_CONTEXT* obj, const char* key, const char* value)
{
    mxb_assert(!obj->m_parameters.contains(key));
    obj->m_parameters.set(key, value);
    return true;
}

bool config_append_param(CONFIG_CONTEXT* obj, const char* key, const char* value)
{
    mxb_assert(obj->m_parameters.contains(key));
    auto old_val = obj->m_parameters.get_string(key);
    string new_val = old_val + "," + value;
    char* new_val_z = config_clean_string_list(new_val.c_str());

    bool rval = false;
    if (new_val_z)
    {
        obj->m_parameters.set(key, new_val_z);
        MXS_FREE(new_val_z);
        rval = true;
    }
    return rval;
}

void mxs::ConfigParameters::set(const std::string& key, const std::string& value)
{
    m_contents[key] = value;
}

void mxs::ConfigParameters::set_multiple(const mxs::ConfigParameters& source)
{
    for (const auto& elem : source)
    {
        set(elem.first, elem.second);
    }
}

void mxs::ConfigParameters::set_from_list(std::vector<std::pair<std::string, std::string>> list,
                                         const MXS_MODULE_PARAM* module_params)
{
    // Add custom values.
    for (const auto& a : list)
    {
        set(a.first, a.second);
    }

    if (module_params)
    {
        // Add default values for the rest of the parameters.
        for (auto module_param = module_params; module_param->name; module_param++)
        {
            if (module_param->default_value && !contains(module_param->name))
            {
                set(module_param->name, module_param->default_value);
            }
        }
    }
}

void mxs::ConfigParameters::remove(const string& key)
{
    m_contents.erase(key);
}

void mxs::ConfigParameters::clear()
{
    m_contents.clear();
}

bool mxs::ConfigParameters::empty() const
{
    return m_contents.empty();
}

mxs::ConfigParameters::ContainerType::const_iterator mxs::ConfigParameters::begin() const
{
    return m_contents.begin();
}

mxs::ConfigParameters::ContainerType::const_iterator mxs::ConfigParameters::end() const
{
    return m_contents.end();
}

bool config_replace_param(CONFIG_CONTEXT* obj, const char* key, const char* value)
{
    obj->m_parameters.set(key, value);
    return true;
}

void config_remove_param(CONFIG_CONTEXT* obj, const char* name)
{
    obj->m_parameters.remove(name);
}

/**
 * Return the number of configured threads
 *
 * @return The number of threads configured in the config file
 */
int config_threadcount()
{
    return gateway.n_threads;
}

size_t config_thread_stack_size()
{
    return gateway.thread_stack_size;
}

uint32_t config_writeq_high_water()
{
    return mxb::atomic::load(&gateway.writeq_high_water, mxb::atomic::RELAXED);
}

bool config_set_writeq_high_water(uint32_t size)
{
    bool rval = false;

    if (size >= MIN_WRITEQ_HIGH_WATER)
    {
        mxb::atomic::store(&gateway.writeq_high_water, size, mxb::atomic::RELAXED);
        rval = true;
    }

    return rval;
}

uint32_t config_writeq_low_water()
{
    return mxb::atomic::load(&gateway.writeq_low_water, mxb::atomic::RELAXED);
}

bool config_set_writeq_low_water(uint32_t size)
{
    bool rval = false;

    if (size >= MIN_WRITEQ_LOW_WATER)
    {
        mxb::atomic::store(&gateway.writeq_low_water, size, mxb::atomic::RELAXED);
        rval = true;
    }

    return rval;
}

static struct
{
    const char* name;
    int         priority;
    const char* replacement;
} lognames[] =
{
    {"log_messages", LOG_NOTICE,
     "log_notice"},     //
    // Deprecated
    {"log_trace",    LOG_INFO,
     "log_info"},   //
    // Deprecated
    {"log_debug",    LOG_DEBUG,
     NULL},
    {"log_warning",  LOG_WARNING,
     NULL},
    {"log_notice",   LOG_NOTICE,
     NULL},
    {"log_info",     LOG_INFO,
     NULL},
    {NULL,           0}
};

/**
 * Configuration handler for items in the global [MaxScale] section
 *
 * @param name  The item name
 * @param value The item value
 * @return 0 on error
 */
static int handle_global_item(const char* name, const char* value)
{
    bool processed = true;      // assume 'name' is valid

    config::Type* item = nullptr;
    int i;
    if (strcmp(name, CN_THREADS) == 0)
    {
        if (strcmp(value, CN_AUTO) == 0)
        {
            gateway.n_threads = get_processor_count();
        }
        else
        {
            int thrcount = atoi(value);
            if (thrcount > 0)
            {
                gateway.n_threads = thrcount;

                int processor_count = get_processor_count();
                if (thrcount > processor_count)
                {
                    MXS_WARNING("Number of threads set to %d, which is greater than "
                                "the number of processors available: %d",
                                thrcount,
                                processor_count);
                }
            }
            else
            {
                MXS_ERROR("Invalid value for 'threads': %s.", value);
                return 0;
            }
        }

        if (gateway.n_threads > MXS_MAX_ROUTING_THREADS)
        {
            MXS_WARNING("Number of threads set to %d, which is greater than the "
                        "hard maximum of %d. Number of threads adjusted down "
                        "accordingly.",
                        gateway.n_threads,
                        MXS_MAX_ROUTING_THREADS);
            gateway.n_threads = MXS_MAX_ROUTING_THREADS;
        }
    }
    else if (strcmp(name, CN_THREAD_STACK_SIZE) == 0)
    {
        // DEPRECATED in 2.3, remove in 2.4
        MXS_WARNING("%s is ignored and has been deprecated. If you need to explicitly "
                    "set the stack size, do so with 'ulimit -s' before starting MaxScale.",
                    CN_THREAD_STACK_SIZE);
    }
    else if (strcmp(name, CN_MS_TIMESTAMP) == 0)
    {
        mxs_log_set_highprecision_enabled(config_truth_value((char*)value));
    }
    else if (strcmp(name, CN_SKIP_PERMISSION_CHECKS) == 0)
    {
        gateway.skip_permission_checks = config_truth_value((char*)value);
    }
    else if (strcmp(name, CN_AUTH_CONNECT_TIMEOUT) == 0)
    {
        if (!get_seconds(name, value, &gateway.auth_conn_timeout))
        {
            return 0;
        }
    }
    else if (strcmp(name, CN_AUTH_READ_TIMEOUT) == 0)
    {
        if (!get_seconds(name, value, &gateway.auth_read_timeout))
        {
            return 0;
        }
    }
    else if (strcmp(name, CN_AUTH_WRITE_TIMEOUT) == 0)
    {
        if (!get_seconds(name, value, &gateway.auth_write_timeout))
        {
            return 0;
        }
    }
    else if (strcmp(name, CN_QUERY_CLASSIFIER) == 0)
    {
        int len = strlen(value);
        int max_len = sizeof(gateway.qc_name) - 1;

        if (len <= max_len)
        {
            strcpy(gateway.qc_name, value);
        }
        else
        {
            MXS_ERROR("The length of '%s' is %d, while the maximum length is %d.", value, len, max_len);
            return 0;
        }
    }
    else if (strcmp(name, CN_QUERY_CLASSIFIER_ARGS) == 0)
    {
        gateway.qc_args = MXS_STRDUP_A(value);
    }
    else if (strcmp(name, CN_QUERY_CLASSIFIER_CACHE_SIZE) == 0)
    {
        uint64_t int_value;

        if (!get_suffixed_size(value, &int_value))
        {
            MXS_ERROR("Invalid value for %s: %s", CN_QUERY_CLASSIFIER_CACHE_SIZE, value);
            return 0;
        }

        decltype(gateway.qc_cache_properties.max_size) max_size = int_value;

        if (max_size >= 0)
        {
            gateway.qc_cache_properties.max_size = max_size;
        }
        else
        {
            MXS_ERROR("Value too large for %s: %s", CN_QUERY_CLASSIFIER_CACHE_SIZE, value);
            return 0;
        }
    }
    else if (strcmp(name, "sql_mode") == 0)
    {
        if (strcasecmp(value, "default") == 0)
        {
            gateway.qc_sql_mode = QC_SQL_MODE_DEFAULT;
        }
        else if (strcasecmp(value, "oracle") == 0)
        {
            gateway.qc_sql_mode = QC_SQL_MODE_ORACLE;
        }
        else
        {
            MXS_ERROR("'%s' is not a valid value for '%s'. Allowed values are 'DEFAULT' and 'ORACLE'.",
                      value, name);
            return 0;
        }
    }
    else if (strcmp(name, CN_QUERY_RETRIES) == 0)
    {
        char* endptr;
        int intval = strtol(value, &endptr, 0);
        if (*endptr == '\0' && intval >= 0)
        {
            gateway.query_retries = intval;
        }
        else
        {
            MXS_ERROR("Invalid timeout value for '%s': %s", CN_QUERY_RETRIES, value);
            return 0;
        }
    }
    else if (strcmp(name, CN_QUERY_RETRY_TIMEOUT) == 0)
    {
        if (!get_seconds(name, value, &gateway.query_retry_timeout))
        {
            return 0;
        }
    }
    else if (strcmp(name, CN_LOG_THROTTLING) == 0)
    {
        if (*value == 0)
        {
            MXS_LOG_THROTTLING throttling = {0, 0, 0};

            mxs_log_set_throttling(&throttling);
        }
        else
        {
            char* v = MXS_STRDUP_A(value);

            char* count = v;
            char* window_ms = NULL;
            char* suppress_ms = NULL;

            window_ms = strchr(count, ',');
            if (window_ms)
            {
                *window_ms = 0;
                ++window_ms;

                suppress_ms = strchr(window_ms, ',');
                if (suppress_ms)
                {
                    *suppress_ms = 0;
                    ++suppress_ms;
                }
            }

            if (!count || !window_ms || !suppress_ms)
            {
                MXS_ERROR("Invalid value for the `log_throttling` configuration entry: '%s'. "
                          "The format of the value for `log_throttling` is 'X, Y, Z', where "
                          "X is the maximum number of times a particular error can be logged "
                          "in the time window of Y milliseconds, before the logging is suppressed "
                          "for Z milliseconds.", value);
                return 0;
            }
            else
            {
                int c = atoi(count);
                time_t w;
                time_t s;

                if (c >= 0
                    && get_milliseconds(name, window_ms, value, &w)
                    && get_milliseconds(name, suppress_ms, value, &s))
                {
                    MXS_LOG_THROTTLING throttling;
                    throttling.count = c;
                    throttling.window_ms = w;
                    throttling.suppress_ms = s;

                    mxs_log_set_throttling(&throttling);
                }
                else
                {
                    MXS_ERROR("Invalid value for the `log_throttling` configuration entry: '%s'. "
                              "The configuration entry `log_throttling` requires as value one zero or "
                              "positive integer and two durations.", value);
                    return 0;
                }
            }

            MXS_FREE(v);
        }
    }
    else if (strcmp(name, CN_ADMIN_PORT) == 0)
    {
        gateway.admin_port = atoi(value);
    }
    else if (strcmp(name, CN_ADMIN_HOST) == 0)
    {
        strcpy(gateway.admin_host, value);
    }
    else if (strcmp(name, CN_ADMIN_SSL_KEY) == 0)
    {
        strcpy(gateway.admin_ssl_key, value);
    }
    else if (strcmp(name, CN_ADMIN_SSL_CERT) == 0)
    {
        strcpy(gateway.admin_ssl_cert, value);
    }
    else if (strcmp(name, CN_ADMIN_SSL_CA_CERT) == 0)
    {
        strcpy(gateway.admin_ssl_ca_cert, value);
    }
    else if (strcmp(name, CN_ADMIN_AUTH) == 0)
    {
        gateway.admin_auth = config_truth_value(value);
    }
    else if (strcmp(name, CN_ADMIN_ENABLED) == 0)
    {
        gateway.admin_enabled = config_truth_value(value);
    }
    else if (strcmp(name, CN_ADMIN_LOG_AUTH_FAILURES) == 0)
    {
        gateway.admin_log_auth_failures = config_truth_value(value);
    }
    else if (strcmp(name, CN_ADMIN_PAM_READWRITE_SERVICE) == 0)
    {
        gateway.admin_pam_rw_service = value;
    }
    else if (strcmp(name, CN_ADMIN_PAM_READONLY_SERVICE) == 0)
    {
        gateway.admin_pam_ro_service = value;
    }
    else if (strcmp(name, CN_PASSIVE) == 0)
    {
        gateway.passive = config_truth_value((char*)value);
    }
    else if (strcmp(name, CN_LOCAL_ADDRESS) == 0)
    {
        gateway.local_address = MXS_STRDUP_A(value);
    }
    else if (strcmp(name, CN_USERS_REFRESH_TIME) == 0)
    {
        char* endptr;
        time_t users_refresh_time = strtol(value, &endptr, 0);

        if (*endptr == '\0' && users_refresh_time < 0)
        {
            MXS_NOTICE("Value of '%s' is less than 0, users will "
                       "not be automatically refreshed.",
                       CN_USERS_REFRESH_TIME);
            // Strictly speaking they will be refreshed once every 68 years,
            // but I just don't beleave the uptime will be that long.
            users_refresh_time = INT32_MAX;
        }
        else
        {
            // Have to "parse" the value anew in case a suffix has been used.
            if (!get_seconds(name, value, &users_refresh_time))
            {
                return 0;
            }

            if (users_refresh_time > INT32_MAX)
            {
                // To ensure that there will be no overflows when
                // we later do arithmetic.
                users_refresh_time = INT32_MAX;
            }
        }

        gateway.users_refresh_time = users_refresh_time;
    }
    else if (strcmp(name, CN_USERS_REFRESH_INTERVAL) == 0)
    {
        if (!get_seconds(name, value, &gateway.users_refresh_interval))
        {
            return 0;
        }
    }
    else if (strcmp(name, CN_WRITEQ_HIGH_WATER) == 0)
    {
        if (!get_suffixed_size(value, &gateway.writeq_high_water))
        {
            MXS_ERROR("Invalid value for %s: %s", CN_WRITEQ_HIGH_WATER, value);
            return 0;
        }

        if (gateway.writeq_high_water < MIN_WRITEQ_HIGH_WATER)
        {
            MXS_WARNING("The specified writeq high water mark %lu, is smaller "
                        "than the minimum allowed size %lu. Changing to minimum.",
                        gateway.writeq_high_water,
                        MIN_WRITEQ_HIGH_WATER);
            gateway.writeq_high_water = MIN_WRITEQ_HIGH_WATER;
        }
        MXS_NOTICE("Writeq high water mark set to: %lu", gateway.writeq_high_water);
    }
    else if (strcmp(name, CN_WRITEQ_LOW_WATER) == 0)
    {
        if (!get_suffixed_size(value, &gateway.writeq_low_water))
        {
            MXS_ERROR("Invalid value for %s: %s", CN_WRITEQ_LOW_WATER, value);
            return 0;
        }

        if (gateway.writeq_low_water < MIN_WRITEQ_LOW_WATER)
        {
            MXS_WARNING("The specified writeq low water mark %lu, is smaller "
                        "than the minimum allowed size %lu. Changing to minimum.",
                        gateway.writeq_low_water,
                        MIN_WRITEQ_LOW_WATER);
            gateway.writeq_low_water = MIN_WRITEQ_LOW_WATER;
        }
        MXS_NOTICE("Writeq low water mark set to: %lu", gateway.writeq_low_water);
    }
    else if (strcmp(name, CN_RETAIN_LAST_STATEMENTS) == 0)
    {
        char* endptr;
        int intval = strtol(value, &endptr, 0);
        if (*endptr == '\0' && intval >= 0)
        {
            session_set_retain_last_statements(intval);
        }
        else
        {
            MXS_ERROR("Invalid value for '%s': %s", CN_RETAIN_LAST_STATEMENTS, value);
            return 0;
        }
    }
    else if (strcmp(name, CN_DUMP_LAST_STATEMENTS) == 0)
    {
        if (strcmp(value, "on_close") == 0)
        {
            session_set_dump_statements(SESSION_DUMP_STATEMENTS_ON_CLOSE);
        }
        else if (strcmp(value, "on_error") == 0)
        {
            session_set_dump_statements(SESSION_DUMP_STATEMENTS_ON_ERROR);
        }
        else if (strcmp(value, "never") == 0)
        {
            session_set_dump_statements(SESSION_DUMP_STATEMENTS_NEVER);
        }
        else
        {
            MXS_ERROR("%s can have the values 'never', 'on_close' or 'on_error'.",
                      CN_DUMP_LAST_STATEMENTS);
            return 0;
        }
    }
    else if (strcmp(name, CN_SESSION_TRACE) == 0)
    {
        char* endptr;
        int intval = strtol(value, &endptr, 0);
        if (*endptr == '\0' && intval >= 0)
        {
            session_set_session_trace(intval);
            mxb_log_set_session_trace(true);
        }
        else
        {
            MXS_ERROR("Invalid value for '%s': %s", CN_SESSION_TRACE, value);
            return 0;
        }
    }
    else if ((item = gateway.find_value(name)) != nullptr)
    {
        if (!item->set(value))
        {
            MXS_ERROR("Invalid value for '%s': %s", item->parameter().name().c_str(), value);
            return 0;
        }
    }
    else
    {
        bool found = false;
#ifndef SS_DEBUG
        if (strcmp(name, "log_debug") == 0)
        {
            MXS_WARNING("The 'log_debug' option has no effect in release mode.");
            found = true;
        }
        else
#endif
        {
            maxscale::event::result_t result = maxscale::event::configure(name, value);

            switch (result)
            {
            case maxscale::event::ACCEPTED:
                found = true;
                break;

            case maxscale::event::IGNORED:
                for (i = 0; lognames[i].name; i++)
                {
                    if (strcasecmp(name, lognames[i].name) == 0)
                    {
                        found = true;
                        if (lognames[i].replacement)
                        {
                            MXS_WARNING("In the configuration file the use of '%s' is deprecated, "
                                        "use '%s' instead.",
                                        lognames[i].name,
                                        lognames[i].replacement);
                        }

                        mxs_log_set_priority_enabled(lognames[i].priority, config_truth_value(value));
                    }
                }
                break;

            case maxscale::event::INVALID:
                return 0;
            }
        }


        if (!found)
        {
            for (int i = 0; !found && config_pre_parse_global_params[i]; ++i)
            {
                found = strcmp(name, config_pre_parse_global_params[i]) == 0;
            }
        }
        processed = found;
    }

    if (!processed)
    {
        MXS_ERROR("Unknown global parameter '%s'.", name);
    }

    return processed ? 1 : 0;
}

bool config_can_modify_at_runtime(const char* name)
{
    for (int i = 0; config_pre_parse_global_params[i]; ++i)
    {
        if (strcmp(name, config_pre_parse_global_params[i]) == 0)
        {
            return true;
        }
    }
    std::unordered_set<std::string> static_params
    {
        CN_USERS_REFRESH_INTERVAL,
        CN_USERS_REFRESH_TIME,
        CN_LOCAL_ADDRESS,
        CN_ADMIN_ENABLED,
        CN_ADMIN_SSL_CA_CERT,
        CN_ADMIN_SSL_CERT,
        CN_ADMIN_SSL_KEY,
        CN_ADMIN_HOST,
        CN_ADMIN_PORT,
        CN_ADMIN_PAM_READWRITE_SERVICE,
        CN_ADMIN_PAM_READONLY_SERVICE,
        CN_LOG_THROTTLING,
        "sql_mode",
        CN_QUERY_CLASSIFIER_ARGS,
        CN_QUERY_CLASSIFIER,
        CN_THREAD_STACK_SIZE,
        CN_THREADS
    };

    return static_params.count(name);
}

void config_set_global_defaults()
{
    uint8_t mac_addr[6] = "";
    struct utsname uname_data;
    gateway.config_check = false;
    gateway.n_threads = DEFAULT_NTHREADS;
    gateway.auth_conn_timeout = DEFAULT_AUTH_CONNECT_TIMEOUT;
    gateway.auth_read_timeout = DEFAULT_AUTH_READ_TIMEOUT;
    gateway.auth_write_timeout = DEFAULT_AUTH_WRITE_TIMEOUT;
    gateway.skip_permission_checks = false;
    gateway.syslog = 1;
    gateway.maxlog = 1;
    gateway.admin_port = DEFAULT_ADMIN_HTTP_PORT;
    gateway.admin_auth = true;
    gateway.admin_log_auth_failures = true;
    gateway.admin_enabled = true;
    strcpy(gateway.admin_host, DEFAULT_ADMIN_HOST);
    gateway.admin_ssl_key[0] = '\0';
    gateway.admin_ssl_cert[0] = '\0';
    gateway.admin_ssl_ca_cert[0] = '\0';
    gateway.query_retries = DEFAULT_QUERY_RETRIES;
    gateway.query_retry_timeout = DEFAULT_QUERY_RETRY_TIMEOUT;
    gateway.passive = false;
    gateway.promoted_at = 0;
    gateway.users_refresh_time = USERS_REFRESH_TIME_DEFAULT;
    gateway.users_refresh_interval = 0;

    gateway.log_target = MXB_LOG_TARGET_DEFAULT;

    gateway.qc_cache_properties.max_size = get_total_memory() * 0.15;

    if (gateway.qc_cache_properties.max_size == 0)
    {
        // Set to -1 so that we know the auto-sizing failed.
        gateway.qc_cache_properties.max_size = -1;
    }

    gateway.thread_stack_size = 0;
    gateway.writeq_high_water = 0;
    gateway.writeq_low_water = 0;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0)
    {
        size_t thread_stack_size;
        if (pthread_attr_getstacksize(&attr, &thread_stack_size) == 0)
        {
            gateway.thread_stack_size = thread_stack_size;
        }
    }

    /* get release string */
    if (!config_get_release_string(gateway.release_string))
    {
        sprintf(gateway.release_string, "undefined");
    }

    /* get first mac_address in SHA1 */
    if (config_get_ifaddr(mac_addr))
    {
        gw_sha1_str(mac_addr, 6, gateway.mac_sha1);
    }
    else
    {
        memset(gateway.mac_sha1, '\0', sizeof(gateway.mac_sha1));
        memcpy(gateway.mac_sha1, "MAC-undef", 9);
    }

    /* get uname info */
    if (uname(&uname_data))
    {
        strcpy(gateway.sysname, "undefined");
    }
    else
    {
        strcpy(gateway.sysname, uname_data.sysname);
    }

    /* query_classifier */
    memset(gateway.qc_name, 0, sizeof(gateway.qc_name));
    gateway.qc_args = NULL;
    gateway.qc_sql_mode = QC_SQL_MODE_DEFAULT;
}

bool missing_required_parameters(const MXS_MODULE_PARAM* mod_params,
                                 const mxs::ConfigParameters& params,
                                 const char* name)
{
    bool rval = false;

    if (mod_params)
    {
        for (int i = 0; mod_params[i].name; i++)
        {
            if ((mod_params[i].options & MXS_MODULE_OPT_REQUIRED) && !params.contains(mod_params[i].name))
            {
                MXS_ERROR("Mandatory parameter '%s' is not defined for '%s'.",
                          mod_params[i].name,
                          name);
                rval = true;
            }
        }
    }

    return rval;
}

static bool is_path_parameter(const MXS_MODULE_PARAM* params, const char* name)
{
    bool rval = false;

    if (params)
    {
        for (int i = 0; params[i].name; i++)
        {
            if (strcmp(params[i].name, name) == 0 && params[i].type == MXS_MODULE_PARAM_PATH)
            {
                rval = true;
                break;
            }
        }
    }

    return rval;
}

static void process_path_parameter(std::string* param)
{
    if (param->empty() || (*param)[0] != '/')
    {
        const char* mod_dir = get_module_configdir();
        size_t size = param->length() + strlen(mod_dir) + 3;
        char new_value[size];

        sprintf(new_value, "/%s/%s", mod_dir, param->c_str());
        clean_up_pathname(new_value);
        param->assign(new_value);
    }
}

static bool param_is_deprecated(const MXS_MODULE_PARAM* params, const char* name, const char* modname)
{
    bool rval = false;

    for (int i = 0; params[i].name; i++)
    {
        if (strcmp(params[i].name, name) == 0)
        {
            if (params[i].options & MXS_MODULE_OPT_DEPRECATED)
            {
                MXS_WARNING("Parameter '%s' for module '%s' is deprecated and "
                            "will be ignored.",
                            name,
                            modname);
                rval = true;
            }
            break;
        }
    }

    return rval;
}

static bool param_in_set(const MXS_MODULE_PARAM* params, const char* name)
{
    bool found = false;

    for (int i = 0; params[i].name; i++)
    {
        if (strcmp(params[i].name, name) == 0)
        {
            found = true;
            break;
        }
    }

    return found;
}

const char* param_type_to_str(const MXS_MODULE_PARAM* params, const char* name)
{

    for (int i = 0; params[i].name; i++)
    {
        if (strcmp(params[i].name, name) == 0)
        {
            switch (params[i].type)
            {
            case MXS_MODULE_PARAM_COUNT:
                return "a non-negative integer";

            case MXS_MODULE_PARAM_INT:
                return "an integer";

            case MXS_MODULE_PARAM_SIZE:
                return "a size in bytes (e.g. 1M)";

            case MXS_MODULE_PARAM_BOOL:
                return "a boolean value";

            case MXS_MODULE_PARAM_STRING:
                return "a string";

            case MXS_MODULE_PARAM_PASSWORD:
                return "a password string";

            case MXS_MODULE_PARAM_QUOTEDSTRING:
                return "a quoted string";

            case MXS_MODULE_PARAM_REGEX:
                return "a regular expression";

            case MXS_MODULE_PARAM_ENUM:
                return "an enumeration value";

            case MXS_MODULE_PARAM_SERVICE:
                return "a service name";

            case MXS_MODULE_PARAM_SERVER:
                return "a server name";

            case MXS_MODULE_PARAM_TARGET:
                return "a target name";

            case MXS_MODULE_PARAM_SERVERLIST:
                return "a comma-separated list of server names";

            case MXS_MODULE_PARAM_TARGETLIST:
                return "a comma-separated list of target names";

            case MXS_MODULE_PARAM_PATH:
                return "a path to a file";

            default:
                mxb_assert_message(!true, "Unknown parameter type");
                return "<unknown parameter type>";
            }
        }
    }

    mxb_assert_message(!true, "Unknown parameter name");
    return "<unknown parameter name>";
}

/**
 * @brief Check that the configuration objects have valid parameters
 *
 * @param context Configuration context
 * @return True if the configuration is OK, false if errors were detected
 */
static bool check_config_objects(CONFIG_CONTEXT* context)
{
    bool rval = true;

    for (CONFIG_CONTEXT* obj = context; obj; obj = obj->m_next)
    {
        if (is_maxscale_section(obj->name()))
        {
            continue;
        }

        std::string type = obj->m_parameters.get_string(CN_TYPE);

        if (!valid_object_type(type))
        {
            MXS_ERROR("Unknown module type for object '%s': %s", obj->name(), type.c_str());
            rval = false;
            continue;
        }

        const char* no_module_defined = get_missing_module_parameter_name(obj);

        if (no_module_defined)
        {
            MXS_ERROR("'%s' is missing the required parameter '%s'", obj->name(), no_module_defined);
            rval = false;
            continue;
        }

        const MXS_MODULE_PARAM* param_set = nullptr;
        const MXS_MODULE* mod = nullptr;
        std::tie(param_set, mod) = get_module_details(obj);

        // Servers are a special case as they don't have a module.
        bool is_server = (type == CN_SERVER);
        if (!is_server && !mod)     // Error is logged in load_module
        {
            rval = false;
            continue;
        }

        mxb_assert(param_set);
        std::vector<std::string> to_be_removed;

        for (auto iter = obj->m_parameters.begin(); iter != obj->m_parameters.end(); ++iter)
        {
            const char* param_namez = iter->first.c_str();
            const MXS_MODULE_PARAM* fix_params;

            if (param_in_set(param_set, param_namez))
            {
                fix_params = param_set;
            }
            else if (!is_server && param_in_set(mod->parameters, param_namez))
            {
                fix_params = mod->parameters;
            }
            else
            {
                // Server's "need" to ignore any unknown parameters as they could
                // be used as weighting parameters
                if (!is_server)
                {
                    MXS_ERROR("Unknown parameter '%s' for object '%s' of type '%s'. %s",
                              param_namez, obj->name(), type.c_str(),
                              closest_matching_parameter(param_namez, param_set, mod->parameters).c_str());
                    rval = false;
                }
                continue;
            }

            const string param_value = iter->second;
            if (config_param_is_valid(fix_params, param_namez, param_value.c_str(), context))
            {
                auto temp = param_value;
                if (is_path_parameter(fix_params, param_namez))
                {
                    process_path_parameter(&temp);
                }
                else    // Fix old-style object names
                {
                    config_fix_param(fix_params, param_namez, &temp);
                }
                obj->m_parameters.set(param_namez, temp);

                if (param_is_deprecated(fix_params, param_namez, obj->name()))
                {
                    to_be_removed.push_back(param_namez);
                }
            }
            else
            {
                MXS_ERROR("Invalid value '%s' for parameter '%s' for object '%s' "
                          "of type '%s' (was expecting %s)",
                          param_value.c_str(), param_namez, obj->name(),
                          type.c_str(),
                          param_type_to_str(fix_params, param_namez));
                rval = false;
            }
        }

        for (const auto& a : to_be_removed)
        {
            config_remove_param(obj, a.c_str());
        }

        if (missing_required_parameters(param_set, obj->m_parameters, obj->name())
            || (!is_server && missing_required_parameters(mod->parameters, obj->m_parameters, obj->name())))
        {
            rval = false;
        }
    }

    return rval;
}

int config_truth_value(const char* str)
{
    if (strcasecmp(str, "true") == 0 || strcasecmp(str, "on") == 0
        || strcasecmp(str, "yes") == 0 || strcasecmp(str, "1") == 0)
    {
        return 1;
    }
    if (strcasecmp(str, "false") == 0 || strcasecmp(str, "off") == 0
        || strcasecmp(str, "no") == 0 || strcasecmp(str, "0") == 0)
    {
        return 0;
    }

    return -1;
}

/**
 * Get the MAC address of first network interface
 *
 * and fill the provided allocated buffer with SHA1 encoding
 * @param output        Allocated 6 bytes buffer
 * @return 1 on success, 0 on failure
 *
 */
int config_get_ifaddr(unsigned char* output)
{
    struct ifreq ifr;
    struct ifconf ifc;
    char buf[1024];
    struct ifreq* it;
    struct ifreq* end;
    int success = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1)
    {
        return 0;
    }

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) == -1)
    {
        close(sock);
        return 0;
    }

    it = ifc.ifc_req;
    end = it + (ifc.ifc_len / sizeof(struct ifreq));

    for (; it != end; ++it)
    {
        strcpy(ifr.ifr_name, it->ifr_name);

        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0)
        {
            if (!(ifr.ifr_flags & IFF_LOOPBACK))
            {
                /* don't count loopback */
                if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0)
                {
                    success = 1;
                    break;
                }
            }
        }
        else
        {
            close(sock);
            return 0;
        }
    }

    if (success)
    {
        memcpy(output, ifr.ifr_hwaddr.sa_data, 6);
    }
    close(sock);

    return success;
}

/**
 * Get the linux distribution info
 *
 * @param release The buffer where the found distribution is copied.
 *                Assumed to be at least _RELEASE_STR_LENGTH bytes.
 *
 * @return 1 on success, 0 on failure
 */
static int config_get_release_string(char* release)
{
    const char* masks[] =
    {
        "/etc/*-version", "/etc/*-release",
        "/etc/*_version", "/etc/*_release"
    };

    bool have_distribution;
    char distribution[RELEASE_STR_LENGTH] = "";
    int fd;

    have_distribution = false;

    /* get data from lsb-release first */
    if ((fd = open("/etc/lsb-release", O_RDONLY)) != -1)
    {
        /* LSB-compliant distribution! */
        size_t len = read(fd, (char*)distribution, sizeof(distribution) - 1);
        close(fd);

        if (len != (size_t) -1)
        {
            distribution[len] = 0;

            char* found = strstr(distribution, "DISTRIB_DESCRIPTION=");

            if (found)
            {
                have_distribution = true;
                char* end = strstr(found, "\n");
                if (end == NULL)
                {
                    end = distribution + len;
                }
                found += 20;    // strlen("DISTRIB_DESCRIPTION=")

                if (*found == '"' && end[-1] == '"')
                {
                    found++;
                    end--;
                }
                *end = 0;

                char* to = strcpy(distribution, "lsb: ");
                memmove(to, found, end - found + 1 < INT_MAX ? end - found + 1 : INT_MAX);

                strcpy(release, to);

                return 1;
            }
        }
    }

    /* if not an LSB-compliant distribution */
    for (int i = 0; !have_distribution && i < 4; i++)
    {
        glob_t found;
        char* new_to;

        if (glob(masks[i], GLOB_NOSORT, NULL, &found) == 0)
        {
            int fd;
            size_t k = 0;
            int skipindex = 0;
            int startindex = 0;

            for (k = 0; k < found.gl_pathc; k++)
            {
                if (strcmp(found.gl_pathv[k], "/etc/lsb-release") == 0)
                {
                    skipindex = k;
                }
            }

            if (skipindex == 0)
            {
                startindex++;
            }

            if ((fd = open(found.gl_pathv[startindex], O_RDONLY)) != -1)
            {
                /*
                 +5 and -8 below cut the file name part out of the
                 *  full pathname that corresponds to the mask as above.
                 */
                new_to = strncpy(distribution, found.gl_pathv[0] + 5, RELEASE_STR_LENGTH - 1);
                new_to += 8;
                *new_to++ = ':';
                *new_to++ = ' ';

                size_t to_len = distribution + sizeof(distribution) - 1 - new_to;
                size_t len = read(fd, (char*)new_to, to_len);

                close(fd);

                if (len != (size_t) -1)
                {
                    new_to[len] = 0;
                    char* end = strstr(new_to, "\n");
                    if (end)
                    {
                        *end = 0;
                    }

                    have_distribution = true;
                    strncpy(release, new_to, RELEASE_STR_LENGTH - 1);
                    release[RELEASE_STR_LENGTH - 1] = '\0';
                }
            }
        }
        globfree(&found);
    }

    if (have_distribution)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

MXS_CONFIG* config_get_global_options()
{
    return &gateway;
}

/**
 * Check if sections are defined multiple times in the configuration file.
 *
 * @param filename Path to the configuration file
 * @param context  The context object used for tracking the duplication
 *                 section information.
 *
 * @return True if duplicate sections were found or an error occurred
 */
bool config_has_duplicate_sections(const char* filename, DUPLICATE_CONTEXT* context)
{
    bool rval = false;

    int size = 1024;
    char* buffer = (char*)MXS_MALLOC(size * sizeof(char));

    if (buffer)
    {
        FILE* file = fopen(filename, "r");

        if (file)
        {
            while (maxscale_getline(&buffer, &size, file) > 0)
            {
                if (pcre2_match(context->re,
                                (PCRE2_SPTR) buffer,
                                PCRE2_ZERO_TERMINATED,
                                0,
                                0,
                                context->mdata,
                                NULL) > 0)
                {
                    /**
                     * Neither of the PCRE2 calls will fail since we know the pattern
                     * beforehand and we allocate enough memory from the stack
                     */
                    PCRE2_SIZE len;
                    pcre2_substring_length_bynumber(context->mdata, 1, &len);
                    len += 1;   /** one for the null terminator */
                    PCRE2_UCHAR section[len];
                    pcre2_substring_copy_bynumber(context->mdata, 1, section, &len);

                    string key(reinterpret_cast<char*>(section), len);
                    if (context->sections->insert(key).second == false)
                    {
                        MXS_ERROR("Duplicate section found: %s", section);
                        rval = true;
                    }
                }
            }
            fclose(file);
        }
        else
        {
            MXS_ERROR("Failed to open file '%s': %s", filename, mxs_strerror(errno));
            rval = true;
        }
    }
    else
    {
        MXS_OOM_MESSAGE("Failed to allocate enough memory when checking"
                        " for duplicate sections in configuration file.");
        rval = true;
    }

    MXS_FREE(buffer);
    return rval;
}


/**
 * Read from a FILE pointer until a newline character or the end of the file is found.
 * The provided buffer will be reallocated if it is too small to store the whole
 * line. The size after the reallocation will be stored in @c size. The read line
 * will be stored in @c dest and it will always be null terminated. The newline
 * character will not be copied into the buffer.
 * @param dest Pointer to a buffer of at least @c size bytes
 * @param size Size of the buffer
 * @param file A valid file stream
 * @return When a complete line was successfully read the function returns 1. If
 * the end of the file was reached before any characters were read the return value
 * will be 0. If the provided buffer could not be reallocated to store the complete
 * line the original size will be retained, everything read up to this point
 * will be stored in it as a null terminated string and -1 will be returned.
 */
int maxscale_getline(char** dest, int* size, FILE* file)
{
    char* destptr = *dest;
    int offset = 0;

    if (feof(file) || ferror(file))
    {
        return 0;
    }

    while (true)
    {
        if (*size <= offset)
        {
            char* tmp = (char*)MXS_REALLOC(destptr, *size * 2);
            if (tmp)
            {
                destptr = tmp;
                *size *= 2;
            }
            else
            {
                destptr[offset - 1] = '\0';
                *dest = destptr;
                return -1;
            }
        }

        int c = fgetc(file);

        if ((c == '\n') || (c == EOF))
        {
            destptr[offset] = '\0';
            break;
        }
        else
        {
            destptr[offset] = c;
        }

        offset++;
    }

    *dest = destptr;
    return 1;
}

void config_add_defaults(mxs::ConfigParameters* dest, const MXS_MODULE_PARAM* params)
{
    if (params)
    {
        for (int i = 0; params[i].name; i++)
        {
            if (params[i].default_value && !dest->contains(params[i].name))
            {
                std::string key = params[i].name;
                std::string value = params[i].default_value;
                config_fix_param(params, key, &value);
                dest->set(key, value);
            }
        }
    }
}

template<class T>
inline int64_t duration_to_int(const string& value)
{
    T duration;
    get_suffixed_duration(value.c_str(), &duration);
    return duration.count();
}

/**
 * Convert a config value to a json object.
 *
 * @param param_info Type information for the parameter
 * @return Json integer, boolean or string
 */
static
json_t* param_value_to_json(const MXS_MODULE_PARAM* param_info, const string& name, const string& value)
{
    mxb_assert(name == param_info->name);
    json_t* rval = NULL;

    switch (param_info->type)
    {
    case MXS_MODULE_PARAM_COUNT:
    case MXS_MODULE_PARAM_INT:
        rval = json_integer(strtol(value.c_str(), NULL, 10));
        break;

    case MXS_MODULE_PARAM_DURATION:
        rval = json_integer((param_info->options & MXS_MODULE_OPT_DURATION_S) ?
                            duration_to_int<seconds>(value) :
                            duration_to_int<milliseconds>(value));
        break;


    case MXS_MODULE_PARAM_BOOL:
        rval = json_boolean(config_truth_value(value.c_str()));
        break;

    case MXS_MODULE_PARAM_PASSWORD:
        rval = json_string("*****");
        break;

    default:
        rval = json_string(value.c_str());
        break;
    }

    return rval;
}

void config_add_module_params_json(const mxs::ConfigParameters* parameters,
                                   const std::unordered_set<std::string>& ignored_params,
                                   const MXS_MODULE_PARAM* basic_params,
                                   const MXS_MODULE_PARAM* module_params,
                                   json_t* output)
{
    for (const auto* param_info : {basic_params, module_params})
    {
        if (param_info)
        {
            for (int i = 0; param_info[i].name; i++)
            {
                const string param_name = param_info[i].name;
                if (ignored_params.count(param_name) == 0 && !json_object_get(output, param_name.c_str()))
                {
                    if (parameters->contains(param_name))
                    {
                        const string value = parameters->get_string(param_name);
                        json_object_set_new(output, param_name.c_str(),
                                            param_value_to_json(&param_info[i], param_name, value));
                    }
                    else
                    {
                        // The parameter was not set in config and does not have a default value.
                        // Print a null value.
                        json_object_set_new(output, param_name.c_str(), json_null());
                    }
                }
            }
        }
    }
}

void log_exclusive_param_error(CONFIG_CONTEXT* obj)
{
    std::vector<std::string> types;

    for (auto a : {CN_SERVERS, CN_TARGETS, CN_CLUSTER})
    {
        if (obj->m_parameters.contains(a))
        {
            types.push_back("'" + std::string(a) + "'");
        }
    }

    MXS_ERROR("Service '%s' is configured with mutually exclusive parameters (%s). "
              "Only one of them is allowed.", obj->name(), mxb::join(types, ", ").c_str());
}

/**
 * Create a new router for a service
 * @param obj Service configuration context
 * @return True if configuration was successful, false if an error occurred.
 */
int create_new_service(CONFIG_CONTEXT* obj)
{
    auto router = obj->m_parameters.get_string(CN_ROUTER);
    mxb_assert(!router.empty());

    const string servers = obj->m_parameters.get_string(CN_SERVERS);
    const string targets = obj->m_parameters.get_string(CN_TARGETS);
    const string cluster = obj->m_parameters.get_string(CN_CLUSTER);

    if (!servers.empty() + !cluster.empty() + !targets.empty() > 1)
    {
        log_exclusive_param_error(obj);
        return 1;
    }

    string user = obj->m_parameters.get_string(CN_USER);
    string auth = obj->m_parameters.get_string(CN_PASSWORD);
    const MXS_MODULE* module = get_module(router.c_str(), MODULE_ROUTER);
    mxb_assert(module);

    if ((user.empty() || auth.empty())
        && !rcap_type_required(module->module_capabilities, RCAP_TYPE_NO_AUTH))
    {
        MXS_ERROR("Service '%s' is missing %s%s%s.",
                  obj->name(),
                  !user.empty() ? "" : "the 'user' parameter",
                  user.empty() && auth.empty() ? " and " : "",
                  !auth.empty() ? "" : "the 'password' parameter");
        return 1;
    }

    config_add_defaults(&obj->m_parameters, common_service_params());
    config_add_defaults(&obj->m_parameters, module->parameters);

    int error_count = 0;
    Service* service = Service::create(obj->name(), router.c_str(), &obj->m_parameters);

    if (service)
    {
        if (!servers.empty())
        {
            for (auto& a : mxs::strtok(servers, ","))
            {
                if (auto s = ServerManager::find_by_unique_name(mxb::trimmed_copy(a)))
                {
                    service->add_target(s);
                }
                else
                {
                    MXS_ERROR("Unable to find server '%s' that is configured as part of service '%s'.",
                              a.c_str(), obj->name());
                    error_count++;
                }
            }
        }
        else if (!targets.empty())
        {
            for (auto& a : mxs::strtok(targets, ","))
            {
                if (auto s = ServerManager::find_by_unique_name(mxb::trimmed_copy(a)))
                {
                    service->add_target(s);
                }
                else if (auto s = service_find(mxb::trimmed_copy(a).c_str()))
                {
                    service->add_target(s);
                }
                else
                {
                    MXS_ERROR("Unable to find target '%s' that is configured as part of service '%s'.",
                              a.c_str(), obj->name());
                    error_count++;
                }
            }
        }

        string filters = obj->m_parameters.get_string(CN_FILTERS);
        if (!filters.empty())
        {
            auto flist = mxs::strtok(filters, "|");

            if (!service->set_filters(flist))
            {
                error_count++;
            }
        }

        if (!cluster.empty())
        {
            Monitor* pMonitor = MonitorManager::find_monitor(cluster.c_str());

            if (pMonitor)
            {
                service->m_monitor = pMonitor;
            }
            else
            {
                MXS_ERROR("Unable to find monitor '%s' that defines the cluster used by "
                          "service '%s'.", cluster.c_str(), obj->name());
                error_count++;
            }
        }
    }
    else
    {
        MXS_ERROR("Service '%s' creation failed.", obj->name());
        error_count++;
    }

    return error_count;
}

/**
 * Check if a parameter is a default server parameter.
 * @param param Parameter name
 * @return True if it is one of the standard server parameters
 */
bool is_normal_server_parameter(const char* param)
{
    auto server_params = common_server_params();
    for (int i = 0; server_params[i].name; i++)
    {
        if (strcmp(param, server_params[i].name) == 0)
        {
            return true;
        }
    }
    // Check if parameter is deprecated
    for (int i = 0; deprecated_server_params[i]; i++)
    {
        if (strcmp(param, deprecated_server_params[i]) == 0)
        {
            MXS_WARNING("Server parameter '%s' is deprecated and will be ignored.", param);
            return true;
        }
    }
    return false;
}

/**
 * Create a new server
 * @param obj Server configuration context
 * @return Number of errors
 */
int create_new_server(CONFIG_CONTEXT* obj)
{
    bool error = false;

    config_add_defaults(&obj->m_parameters, common_server_params());

    bool have_address = obj->m_parameters.contains(CN_ADDRESS);
    bool have_socket = obj->m_parameters.contains(CN_SOCKET);

    if (have_socket && have_address)
    {
        MXS_ERROR("Both '%s' and '%s' defined for server '%s': only one of the parameters can be defined",
                  CN_ADDRESS, CN_SOCKET, obj->name());
        return 1;
    }
    else if (!have_address && !have_socket)
    {
        MXS_ERROR("Server '%s' is missing a required parameter: either '%s' or '%s' must be defined",
                  obj->name(), CN_ADDRESS, CN_SOCKET);
        return 1;
    }
    else if (have_address && obj->m_parameters.get_string(CN_ADDRESS)[0] == '/')
    {
        MXS_ERROR("The '%s' parameter for '%s' is not a valid IP or hostname", CN_ADDRESS, obj->name());
        return 1;
    }

    if (Server* server = ServerManager::create_server(obj->name(), obj->m_parameters))
    {
        auto disk_space_threshold = obj->m_parameters.get_string(CN_DISK_SPACE_THRESHOLD);
        if (!server->set_disk_space_threshold(disk_space_threshold))
        {
            MXS_ERROR("Invalid value for '%s' for server %s: %s",
                      CN_DISK_SPACE_THRESHOLD,
                      server->name(),
                      disk_space_threshold.c_str());
            error = true;
        }
    }
    else
    {
        MXS_ERROR("Failed to create a new server, memory allocation failed.");
        error = true;
    }

    return error;
}

/**
 * Create a new monitor
 *
 * @param obj               Monitor configuration context
 * @return 0 on success
 */
int create_new_monitor(CONFIG_CONTEXT* obj, std::set<std::string>& monitored_servers)
{
    auto module = obj->m_parameters.get_string(CN_MODULE);
    mxb_assert(!module.empty());
    int rval = 1;
    Monitor* monitor = MonitorManager::create_monitor(obj->name(), module, &obj->m_parameters);
    if (monitor)
    {
        rval = 0;
    }
    else
    {
        MXS_ERROR("Failed to create monitor '%s'.", obj->name());
    }

    return rval;
}

/**
 * Create a new listener for a service
 *
 * @param obj Listener configuration context
 *
 * @return Number of errors
 */
int create_new_listener(CONFIG_CONTEXT* obj)
{
    auto protocol = obj->m_parameters.get_string(CN_PROTOCOL);
    mxb_assert(!protocol.empty());

    if (const MXS_MODULE* mod = get_module(protocol.c_str(), MODULE_PROTOCOL))
    {
        config_add_defaults(&obj->m_parameters, common_listener_params());
        config_add_defaults(&obj->m_parameters, mod->parameters);
    }
    else
    {
        MXS_ERROR("Unable to load protocol module '%s'.", protocol.c_str());
        return 1;
    }

    return Listener::create(obj->name(), protocol, obj->m_parameters) ? 0 : 1;
}

/**
 * Create a new filter
 * @param obj Filter configuration context
 * @return Number of errors
 */
int create_new_filter(CONFIG_CONTEXT* obj)
{
    int error_count = 0;
    auto module_str = obj->m_parameters.get_string(CN_MODULE);
    mxb_assert(!module_str.empty());
    const char* module = module_str.c_str();

    if (const MXS_MODULE* mod = get_module(module, MODULE_FILTER))
    {
        config_add_defaults(&obj->m_parameters, mod->parameters);

        if (!filter_alloc(obj->name(), module, &obj->m_parameters))
        {
            MXS_ERROR("Failed to create filter '%s'. Memory allocation failed.",
                      obj->name());
            error_count++;
        }
    }
    else
    {
        MXS_ERROR("Failed to load filter module '%s'", module);
        error_count++;
    }

    return error_count;
}

bool config_is_ssl_parameter(const char* key)
{
    const char* ssl_params[] =
    {
        CN_SSL_CERT,
        CN_SSL_CA_CERT,
        CN_SSL,
        CN_SSL_KEY,
        CN_SSL_VERSION,
        CN_SSL_CERT_VERIFY_DEPTH,
        CN_SSL_VERIFY_PEER_CERTIFICATE,
        CN_SSL_VERIFY_PEER_HOST,
        NULL
    };

    for (int i = 0; ssl_params[i]; i++)
    {
        if (strcmp(key, ssl_params[i]) == 0)
        {
            return true;
        }
    }

    return false;
}

// TEMPORARILY EXPOSED
/*static*/ bool check_path_parameter(const MXS_MODULE_PARAM* params, const char* value)
{
    bool valid = false;

    if (params->options & (MXS_MODULE_OPT_PATH_W_OK
                           | MXS_MODULE_OPT_PATH_R_OK
                           | MXS_MODULE_OPT_PATH_X_OK
                           | MXS_MODULE_OPT_PATH_F_OK))
    {
        char buf[strlen(get_module_configdir()) + strlen(value) + 3];

        if (*value != '/')
        {
            sprintf(buf, "/%s/%s", get_module_configdir(), value);
            clean_up_pathname(buf);
        }
        else
        {
            strcpy(buf, value);
        }

        int mode = F_OK;
        int mask = 0;

        if (params->options & MXS_MODULE_OPT_PATH_W_OK)
        {
            mask |= S_IWUSR | S_IWGRP;
            mode |= W_OK;
        }
        if (params->options & MXS_MODULE_OPT_PATH_R_OK)
        {
            mask |= S_IRUSR | S_IRGRP;
            mode |= R_OK;
        }
        if (params->options & MXS_MODULE_OPT_PATH_X_OK)
        {
            mask |= S_IXUSR | S_IXGRP;
            mode |= X_OK;
        }

        if (access(buf, mode) == 0)
        {
            valid = true;
        }
        else
        {
            /** Save errno as we do a second call to `accept` */
            int er = errno;

            if (access(buf, F_OK) == 0 || (params->options & MXS_MODULE_OPT_PATH_CREAT) == 0)
            {
                /**
                 * Path already exists and it doesn't have the requested access
                 * right or the module doesn't want the directory to be created
                 * if it doesn't exist.
                 */
                MXS_ERROR("Bad path parameter '%s' (absolute path '%s'): %d, %s",
                          value,
                          buf,
                          er,
                          mxs_strerror(er));
            }
            else if (mxs_mkdir_all(buf, mask))
            {
                /** Successfully created path */
                valid = true;
            }
            else
            {
                /** Failed to create the directory, errno is set in `mxs_mkdir_all` */
                MXS_ERROR("Can't create path '%s' (absolute path '%s'): %d, %s",
                          value,
                          buf,
                          errno,
                          mxs_strerror(errno));
            }
        }
    }
    else
    {
        /** No checks for the path are required */
        valid = true;
    }

    return valid;
}

static bool config_contains_type(const CONFIG_CONTEXT* ctx, const char* name,
                                 std::set<std::string> types)
{
    while (ctx)
    {
        if (strcmp(ctx->name(), name) == 0 && types.count(ctx->m_parameters.get_string(CN_TYPE)))
        {
            return true;
        }

        ctx = ctx->m_next;
    }

    return false;
}

void fix_serverlist(char* value)
{
    string dest;
    char* end;
    char* start = strtok_r(value, ",", &end);
    const char* sep = "";

    while (start)
    {
        fix_object_name(start);
        dest += sep;
        dest += start;
        sep = ",";
        start = strtok_r(NULL, ",", &end);
    }

    /** The value will always be smaller than the original one or of equal size */
    strcpy(value, dest.c_str());
}

void config_fix_param(const MXS_MODULE_PARAM* params, const string& name, string* value)
{
    // A char* is needed for C-style functions.
    char temp_value[value->length() + 1];
    strcpy(temp_value, value->c_str());

    for (int i = 0; params[i].name; i++)
    {
        if (params[i].name == name)
        {
            switch (params[i].type)
            {
            case MXS_MODULE_PARAM_SERVER:
            case MXS_MODULE_PARAM_SERVICE:
            case MXS_MODULE_PARAM_TARGET:
                fix_object_name(temp_value);
                break;

            case MXS_MODULE_PARAM_SERVERLIST:
            case MXS_MODULE_PARAM_TARGETLIST:
                fix_serverlist(temp_value);
                break;

            case MXS_MODULE_PARAM_QUOTEDSTRING:
                // Remove *if* once '" .. "' is no longer optional
                if (check_first_last_char(temp_value, '"'))
                {
                    remove_first_last_char(temp_value);
                }
                break;

            case MXS_MODULE_PARAM_REGEX:
                // Remove *if* once '/ .. /' is no longer optional
                if (check_first_last_char(temp_value, '/'))
                {
                    remove_first_last_char(temp_value);
                }
                break;

            default:
                break;
            }

            break;
        }
    }
    value->assign(temp_value);
}

bool config_param_is_valid(const MXS_MODULE_PARAM* params,
                           const char* key,
                           const char* value,
                           const CONFIG_CONTEXT* context)
{
    bool valid = false;
    char fixed_value[strlen(value) + 1];
    strcpy(fixed_value, value);
    fix_object_name(fixed_value);

    for (int i = 0; params[i].name && !valid; i++)
    {
        if (strcmp(params[i].name, key) == 0)
        {
            char* endptr;

            switch (params[i].type)
            {
            case MXS_MODULE_PARAM_COUNT:
                if ((strtol(value, &endptr, 10)) >= 0 && endptr != value && *endptr == '\0')
                {
                    valid = true;
                }
                break;

            case MXS_MODULE_PARAM_INT:
                {
                    errno = 0;
                    long int v = strtol(value, &endptr, 10);
                    (void)v;    // error: ignoring return value of 'strtol'
                    if ((errno == 0) && (endptr != value) && (*endptr == '\0'))
                    {
                        valid = true;
                    }
                }
                break;

            case MXS_MODULE_PARAM_SIZE:
                {
                    errno = 0;
                    long long int v = strtoll(value, &endptr, 10);
                    (void)v;    // error: ignoring return value of 'strtoll'
                    if (errno == 0)
                    {
                        if (endptr != value)
                        {
                            switch (*endptr)
                            {
                            case 'T':
                            case 't':
                            case 'G':
                            case 'g':
                            case 'M':
                            case 'm':
                            case 'K':
                            case 'k':
                                if (*(endptr + 1) == '\0'
                                    || ((*(endptr + 1) == 'i' || *(endptr + 1) == 'I')
                                        && *(endptr + 2) == '\0'))
                                {
                                    valid = true;
                                }
                                break;

                            case '\0':
                                valid = true;
                                break;

                            default:
                                break;
                            }
                        }
                    }
                }
                break;

            case MXS_MODULE_PARAM_DURATION:
                {
                    mxs::config::DurationUnit unit;

                    if (duration_is_valid(value, &unit))
                    {
                        valid = true;

                        switch (unit)
                        {
                        case mxs::config::DURATION_IN_MILLISECONDS:
                            if (params[i].options & MXS_MODULE_OPT_DURATION_S)
                            {
                                MXS_ERROR("Currently the granularity of '%s' is seconds. The value "
                                          "cannot be specified in milliseconds.", params[i].name);
                                valid = false;
                            }
                            break;

                        case mxs::config::DURATION_IN_DEFAULT:
                            log_duration_suffix_warning(key, value);
                            break;

                        default:
                            break;
                        }
                    }
                }
                break;

            case MXS_MODULE_PARAM_BOOL:
                if (config_truth_value(value) != -1)
                {
                    valid = true;
                }
                break;

            case MXS_MODULE_PARAM_STRING:
            case MXS_MODULE_PARAM_PASSWORD:
                if (*value)
                {
                    valid = true;
                }
                break;

            case MXS_MODULE_PARAM_QUOTEDSTRING:
                if (*value)
                {
                    valid = true;
                    if (!check_first_last_char(value, '"'))
                    {
                        // Change warning to valid=false once quotes are no longer optional
                        MXS_WARNING("Missing quotes (\") around a quoted string is deprecated: '%s=%s'.",
                                    key,
                                    value);
                    }
                }
                break;

            case MXS_MODULE_PARAM_REGEX:
                valid = test_regex_string_validity(value, key);
                break;

            case MXS_MODULE_PARAM_ENUM:
                if (params[i].accepted_values)
                {
                    char* endptr;
                    const char* delim = ", \t";
                    char buf[strlen(value) + 1];
                    strcpy(buf, value);
                    char* tok = strtok_r(buf, delim, &endptr);

                    while (tok)
                    {
                        valid = false;

                        for (int j = 0; params[i].accepted_values[j].name; j++)
                        {
                            if (strcmp(params[i].accepted_values[j].name, tok) == 0)
                            {
                                valid = true;
                                break;
                            }
                        }

                        tok = strtok_r(NULL, delim, &endptr);

                        if ((params[i].options & MXS_MODULE_OPT_ENUM_UNIQUE) && (tok || !valid))
                        {
                            /** Either the only defined enum value is not valid
                             * or multiple values were defined */
                            valid = false;
                            break;
                        }
                    }
                }
                break;

            case MXS_MODULE_PARAM_SERVICE:
                if ((context && config_contains_type(context, fixed_value, {CN_SERVICE}))
                    || (!context && service_find(fixed_value)))
                {
                    valid = true;
                }
                break;

            case MXS_MODULE_PARAM_SERVER:
                if ((context && config_contains_type(context, fixed_value, {CN_SERVER}))
                    || (!context && ServerManager::find_by_unique_name(fixed_value)))
                {
                    valid = true;
                }
                break;

            case MXS_MODULE_PARAM_TARGET:
                if ((context && config_contains_type(context, fixed_value, {CN_SERVER, CN_SERVICE}))
                    || (!context && mxs::Target::find(fixed_value)))
                {
                    valid = true;
                }
                break;

            case MXS_MODULE_PARAM_SERVERLIST:
                {
                    auto names = config_break_list_string(value);
                    valid = !names.empty();

                    for (const auto& elem : names)
                    {
                        if ((context && !config_contains_type(context, elem.c_str(), {CN_SERVER}))
                            || (!context && !ServerManager::find_by_unique_name(elem)))
                        {
                            valid = false;
                            break;
                        }
                    }
                }
                break;

            case MXS_MODULE_PARAM_TARGETLIST:
                {
                    auto names = config_break_list_string(value);
                    valid = !names.empty();

                    for (const auto& elem : names)
                    {
                        if ((context && !config_contains_type(context, elem.c_str(), {CN_SERVER, CN_SERVICE}))
                            || (!context && !mxs::Target::find(elem)))
                        {
                            valid = false;
                            break;
                        }
                    }
                }
                break;

            case MXS_MODULE_PARAM_PATH:
                valid = check_path_parameter(&params[i], value);
                break;

            default:
                MXS_ERROR("Unexpected module parameter type: %d", params[i].type);
                mxb_assert(false);
                break;
            }
        }
    }

    return valid;
}

std::vector<string> config_break_list_string(const string& list_string)
{
    string copy = list_string;
    /* Parse the elements from the list. They are separated by ',' and are trimmed of whitespace. */
    std::vector<string> tokenized = mxs::strtok(copy, ",");
    for (auto& elem : tokenized)
    {
        fix_object_name(elem);
    }
    return tokenized;
}

json_t* config_maxscale_to_json(const char* host)
{
    json_t* param = json_object();
    json_object_set_new(param, "libdir", json_string(get_libdir()));
    json_object_set_new(param, "datadir", json_string(get_datadir()));
    json_object_set_new(param, "process_datadir", json_string(get_process_datadir()));
    json_object_set_new(param, "cachedir", json_string(get_cachedir()));
    json_object_set_new(param, "configdir", json_string(get_configdir()));
    json_object_set_new(param, "config_persistdir", json_string(get_config_persistdir()));
    json_object_set_new(param, "module_configdir", json_string(get_module_configdir()));
    json_object_set_new(param, "piddir", json_string(get_piddir()));
    json_object_set_new(param, "logdir", json_string(get_logdir()));
    json_object_set_new(param, "langdir", json_string(get_langdir()));
    json_object_set_new(param, "execdir", json_string(get_execdir()));
    json_object_set_new(param, "connector_plugindir", json_string(get_connector_plugindir()));
    json_object_set_new(param, CN_THREADS, json_integer(config_threadcount()));
    json_object_set_new(param, CN_THREAD_STACK_SIZE, json_integer(config_thread_stack_size()));
    json_object_set_new(param, CN_WRITEQ_HIGH_WATER, json_integer(config_writeq_high_water()));
    json_object_set_new(param, CN_WRITEQ_LOW_WATER, json_integer(config_writeq_low_water()));

    MXS_CONFIG* cnf = config_get_global_options();

    json_object_set_new(param, CN_AUTH_CONNECT_TIMEOUT, json_integer(cnf->auth_conn_timeout));
    json_object_set_new(param, CN_AUTH_READ_TIMEOUT, json_integer(cnf->auth_read_timeout));
    json_object_set_new(param, CN_AUTH_WRITE_TIMEOUT, json_integer(cnf->auth_write_timeout));
    json_object_set_new(param, CN_SKIP_PERMISSION_CHECKS, json_boolean(cnf->skip_permission_checks));
    json_object_set_new(param, CN_ADMIN_AUTH, json_boolean(cnf->admin_auth));
    json_object_set_new(param, CN_ADMIN_ENABLED, json_boolean(cnf->admin_enabled));
    json_object_set_new(param, CN_ADMIN_LOG_AUTH_FAILURES, json_boolean(cnf->admin_log_auth_failures));
    json_object_set_new(param, CN_ADMIN_HOST, json_string(cnf->admin_host));
    json_object_set_new(param, CN_ADMIN_PORT, json_integer(cnf->admin_port));
    json_object_set_new(param, CN_ADMIN_SSL_KEY, json_string(cnf->admin_ssl_key));
    json_object_set_new(param, CN_ADMIN_SSL_CERT, json_string(cnf->admin_ssl_cert));
    json_object_set_new(param, CN_ADMIN_SSL_CA_CERT, json_string(cnf->admin_ssl_ca_cert));
    json_object_set_new(param, CN_ADMIN_PAM_READWRITE_SERVICE,
                        json_string(cnf->admin_pam_rw_service.c_str()));
    json_object_set_new(param, CN_ADMIN_PAM_READONLY_SERVICE,
                        json_string(cnf->admin_pam_ro_service.c_str()));

    json_object_set_new(param, CN_PASSIVE, json_boolean(cnf->passive));

    json_object_set_new(param, CN_QUERY_CLASSIFIER, json_string(cnf->qc_name));

    if (cnf->qc_args)
    {
        json_object_set_new(param, CN_QUERY_CLASSIFIER_ARGS, json_string(cnf->qc_args));
    }

    json_object_set_new(param,
                        CN_QUERY_CLASSIFIER_CACHE_SIZE,
                        json_integer(cnf->qc_cache_properties.max_size));

    json_object_set_new(param, CN_RETAIN_LAST_STATEMENTS, json_integer(session_get_retain_last_statements()));
    json_object_set_new(param, CN_DUMP_LAST_STATEMENTS, json_string(session_get_dump_statements_str()));
    json_object_set_new(param, CN_SESSION_TRACE, json_integer(session_get_session_trace()));

    // This will dump all parameters defined using the new configuration mechanism.
    cnf->fill(param);

    json_t* attr = json_object();
    time_t started = maxscale_started();
    time_t activated = started + MXS_CLOCK_TO_SEC(cnf->promoted_at);
    json_object_set_new(attr, CN_PARAMETERS, param);
    json_object_set_new(attr, "version", json_string(MAXSCALE_VERSION));
    json_object_set_new(attr, "commit", json_string(MAXSCALE_COMMIT));
    json_object_set_new(attr, "started_at", json_string(http_to_date(started).c_str()));
    json_object_set_new(attr, "activated_at", json_string(http_to_date(activated).c_str()));
    json_object_set_new(attr, "uptime", json_integer(maxscale_uptime()));

    json_t* obj = json_object();
    json_object_set_new(obj, CN_ATTRIBUTES, attr);
    json_object_set_new(obj, CN_ID, json_string(CN_MAXSCALE));
    json_object_set_new(obj, CN_TYPE, json_string(CN_MAXSCALE));

    return mxs_json_resource(host, MXS_JSON_API_MAXSCALE, obj);
}

/**
 * Creates a global configuration at the location pointed by @c filename
 *
 * @param filename Filename where configuration is written
 * @return True on success, false on error
 */
static bool create_global_config(const char* filename)
{
    int file = open(filename, O_EXCL | O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (file == -1)
    {
        MXS_ERROR("Failed to open file '%s' when serializing global configuration: %d, %s",
                  filename,
                  errno,
                  mxs_strerror(errno));
        return false;
    }

    dprintf(file, "[maxscale]\n");
    dprintf(file, "%s=%ld\n", CN_AUTH_CONNECT_TIMEOUT, gateway.auth_conn_timeout);
    dprintf(file, "%s=%ld\n", CN_AUTH_READ_TIMEOUT, gateway.auth_read_timeout);
    dprintf(file, "%s=%ld\n", CN_AUTH_WRITE_TIMEOUT, gateway.auth_write_timeout);
    dprintf(file, "%s=%s\n", CN_ADMIN_AUTH, gateway.admin_auth ? "true" : "false");
    dprintf(file, "%s=%u\n", CN_PASSIVE, gateway.passive);
    dprintf(file, "%s=%s\n", CN_REBALANCE_PERIOD, gateway.rebalance_period.to_string().c_str());
    dprintf(file, "%s=%s\n", CN_REBALANCE_THRESHOLD, gateway.rebalance_threshold.to_string().c_str());

    close(file);

    return true;
}

bool config_global_serialize()
{
    static const char* GLOBAL_CONFIG_NAME = "global-options";
    bool rval = false;
    char filename[PATH_MAX];

    snprintf(filename,
             sizeof(filename),
             "%s/%s.cnf.tmp",
             get_config_persistdir(),
             GLOBAL_CONFIG_NAME);

    if (unlink(filename) == -1 && errno != ENOENT)
    {
        MXS_ERROR("Failed to remove temporary global configuration at '%s': %d, %s",
                  filename,
                  errno,
                  mxs_strerror(errno));
    }
    else if (create_global_config(filename))
    {
        char final_filename[PATH_MAX];
        strcpy(final_filename, filename);

        char* dot = strrchr(final_filename, '.');
        mxb_assert(dot);
        *dot = '\0';

        if (rename(filename, final_filename) == 0)
        {
            rval = true;
        }
        else
        {
            MXS_ERROR("Failed to rename temporary server configuration at '%s': %d, %s",
                      filename,
                      errno,
                      mxs_strerror(errno));
        }
    }

    return rval;
}

/**
 * Test if first and last char in the string are as expected.
 *
 * @param string Input string
 * @param expected Required character
 * @return True, if string has at least two chars and both first and last char
 * equal @c expected
 */
static bool check_first_last_char(const char* string, char expected)
{
    bool valid = false;
    {
        size_t len = strlen(string);
        if ((len >= 2) && (string[0] == expected) && (string[len - 1] == expected))
        {
            valid = true;
        }
    }
    return valid;
}

/**
 * Chop a char off from both ends of the string.
 *
 * @param value Input string
 */
static void remove_first_last_char(char* value)
{
    size_t len = strlen(value);
    value[len - 1] = '\0';
    memmove(value, value + 1, len - 1);
}

/**
 * Compile a regex string using PCRE2 using the settings provided.
 *
 * @param regex_string The string to compile
 * @param jit_enabled Enable JIT compilation. If true but JIT is not available,
 * a warning is printed.
 * @param options PCRE2 compilation options
 * @param output_ovector_size Output for the match data ovector size. On error,
 * nothing is written. If NULL, the parameter is ignored.
 * @return Compiled regex code on success, NULL otherwise
 */
static pcre2_code* compile_regex_string(const char* regex_string,
                                        bool jit_enabled,
                                        uint32_t options,
                                        uint32_t* output_ovector_size)
{
    bool success = true;
    int errorcode = -1;
    PCRE2_SIZE error_offset = -1;
    uint32_t capcount = 0;
    pcre2_code* machine =
        pcre2_compile((PCRE2_SPTR) regex_string,
                      PCRE2_ZERO_TERMINATED,
                      options,
                      &errorcode,
                      &error_offset,
                      NULL);
    if (machine)
    {
        if (jit_enabled)
        {
            // Try to compile even further for faster matching
            if (pcre2_jit_compile(machine, PCRE2_JIT_COMPLETE) < 0)
            {
                MXS_WARNING("PCRE2 JIT compilation of pattern '%s' failed, "
                            "falling back to normal compilation.",
                            regex_string);
            }
        }
        /* Check what is the required match_data size for this pattern. */
        int ret_info = pcre2_pattern_info(machine, PCRE2_INFO_CAPTURECOUNT, &capcount);
        if (ret_info != 0)
        {
            MXS_PCRE2_PRINT_ERROR(ret_info);
            success = false;
        }
    }
    else
    {
        MXS_ERROR("Invalid PCRE2 regular expression '%s' (position '%zu').",
                  regex_string,
                  error_offset);
        MXS_PCRE2_PRINT_ERROR(errorcode);
        success = false;
    }

    if (!success)
    {
        pcre2_code_free(machine);
        machine = NULL;
    }
    else if (output_ovector_size)
    {
        *output_ovector_size = capcount + 1;
    }
    return machine;
}

/**
 * Test if the given string is a valid MaxScale regular expression and can be
 * compiled to a regex machine using PCRE2.
 *
 * @param regex_string The input string
 * @return True if compilation succeeded, false if string is invalid or cannot
 * be compiled.
 */
static bool test_regex_string_validity(const char* regex_string, const char* key)
{
    if (*regex_string == '\0')
    {
        return false;
    }
    char regex_copy[strlen(regex_string) + 1];
    strcpy(regex_copy, regex_string);
    if (!check_first_last_char(regex_string, '/'))
    {
        // return false; // Uncomment this line once '/ .. /' is no longer optional
        MXS_WARNING("Missing slashes (/) around a regular expression is deprecated: '%s=%s'.",
                    key,
                    regex_string);
    }
    else
    {
        remove_first_last_char(regex_copy);
    }

    pcre2_code* code = compile_regex_string(regex_copy, false, 0, NULL);
    bool rval = (code != NULL);
    pcre2_code_free(code);
    return rval;
}

bool get_suffixed_size(const char* value, uint64_t* dest)
{
    if (!isdigit(*value))
    {
        // This will also catch negative values
        return false;
    }

    bool rval = false;
    char* end;
    uint64_t size = strtoll(value, &end, 10);

    switch (*end)
    {
    case 'T':
    case 't':
        if ((*(end + 1) == 'i') || (*(end + 1) == 'I'))
        {
            size *= 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        }
        else
        {
            size *= 1000ULL * 1000ULL * 1000ULL * 1000ULL;
        }
        break;

    case 'G':
    case 'g':
        if ((*(end + 1) == 'i') || (*(end + 1) == 'I'))
        {
            size *= 1024ULL * 1024ULL * 1024ULL;
        }
        else
        {
            size *= 1000ULL * 1000ULL * 1000ULL;
        }
        break;

    case 'M':
    case 'm':
        if ((*(end + 1) == 'i') || (*(end + 1) == 'I'))
        {
            size *= 1024ULL * 1024ULL;
        }
        else
        {
            size *= 1000ULL * 1000ULL;
        }
        break;

    case 'K':
    case 'k':
        if ((*(end + 1) == 'i') || (*(end + 1) == 'I'))
        {
            size *= 1024ULL;
        }
        else
        {
            size *= 1000ULL;
        }
        break;

    default:
        break;
    }

    const std::set<char> first {'T', 't', 'G', 'g', 'M', 'm', 'K', 'k'};
    const std::set<char> second {'I', 'i'};

    if (end[0] == '\0')
    {
        rval = true;
    }
    else if (end[1] == '\0')
    {
        // First character must be valid
        rval = first.count(end[0]);
    }
    else if (end[2] == '\0')
    {
        // Both characters have to be valid
        rval = first.count(end[0]) && second.count(end[1]);
    }

    if (dest)
    {
        *dest = size;
    }

    return rval;
}

bool get_suffixed_duration(const char* zValue,
                           mxs::config::DurationInterpretation interpretation,
                           milliseconds* pDuration,
                           mxs::config::DurationUnit* pUnit)
{
    if (!isdigit(*zValue))
    {
        // This will also catch negative values
        return false;
    }

    bool rval = true;
    char* zEnd;
    uint64_t value = strtoll(zValue, &zEnd, 10);

    milliseconds duration;
    mxs::config::DurationUnit unit = mxs::config::DURATION_IN_DEFAULT;

    switch (*zEnd)
    {
    case 'H':
    case 'h':
        unit = mxs::config::DURATION_IN_HOURS;
        duration = std::chrono::duration_cast<milliseconds>(std::chrono::hours(value));
        ++zEnd;
        break;

    case 'M':
    case 'm':
        if (*(zEnd + 1) == 's' || *(zEnd + 1) == 'S')
        {
            unit = mxs::config::DURATION_IN_MILLISECONDS;
            duration = milliseconds(value);
            ++zEnd;
        }
        else
        {
            unit = mxs::config::DURATION_IN_MINUTES;
            duration = std::chrono::duration_cast<milliseconds>(std::chrono::minutes(value));
        }
        ++zEnd;
        break;

    case 'S':
    case 's':
        unit = mxs::config::DURATION_IN_SECONDS;
        duration = std::chrono::duration_cast<milliseconds>(seconds(value));
        ++zEnd;
        break;

    case 0:
        switch (interpretation)
        {
        case mxs::config::INTERPRET_AS_SECONDS:
            duration = std::chrono::duration_cast<milliseconds>(seconds(value));
            break;

        case mxs::config::INTERPRET_AS_MILLISECONDS:
            duration = milliseconds(value);
            break;

        case mxs::config::NO_INTERPRETATION:
            // A suffix is required.
            rval = false;
            break;
        }
        break;

    default:
        break;
    }

    if (rval)
    {
        if (*zEnd == 0)
        {
            if (pDuration)
            {
                *pDuration = duration;
            }

            if (pUnit)
            {
                *pUnit = unit;
            }
        }
        else
        {
            rval = false;
        }
    }

    return rval;
}

static bool duration_is_valid(const char* zValue, mxs::config::DurationUnit* pUnit)
{
    // When the validity is checked, it does not matter how the value
    // should be interpreted, so any mxs::config::DurationInterpretation is fine.
    milliseconds duration;
    mxs::config::DurationUnit unit;
    bool valid = get_suffixed_duration(zValue, mxs::config::INTERPRET_AS_SECONDS, &duration, &unit);

    if (valid)
    {
        if (unit == mxs::config::DURATION_IN_DEFAULT)
        {
            // "0" is a special case, as it means the same regardless of the
            // unit and the presence of a unit.
            if (duration.count() == 0)
            {
                // To prevent unnecessary complaints, we claim it was specified in
                // seconds which is acceptable in all cases.
                unit = mxs::config::DURATION_IN_SECONDS;
            }
        }

        *pUnit = unit;
    }

    return valid;
}

static void log_duration_suffix_warning(const char* zName, const char* zValue)
{
    MXS_INFO("Specifying durations without a suffix denoting the unit "
             "is strongly discouraged as it will be deprecated in the "
             "future: %s=%s. Use the suffixes 'h' (hour), 'm' (minute), "
             "'s' (second) or 'ms' (milliseconds).", zName, zValue);
}

static bool get_seconds(const char* zName, const char* zValue, seconds* pSeconds)
{
    bool valid = false;

    mxs::config::DurationUnit unit;
    seconds seconds;
    if (get_suffixed_duration(zValue, &seconds, &unit))
    {
        switch (unit)
        {
        case mxs::config::DURATION_IN_MILLISECONDS:
            MXS_ERROR("Currently the granularity of `%s` is seconds. The value cannot "
                      "be specified in milliseconds.", zName);
            valid = false;
            break;

        case mxs::config::DURATION_IN_DEFAULT:
            log_duration_suffix_warning(zName, zValue);

        default:
            *pSeconds = seconds;
            valid = true;
        }
    }
    else
    {
        MXS_ERROR("Invalid duration %s: %s=%s", zValue, zName, zValue);
    }

    return valid;
}

static bool get_seconds(const char* zName, const char* zValue, time_t* pSeconds)
{
    seconds seconds;

    bool valid = get_seconds(zName, zValue, &seconds);

    if (valid)
    {
        *pSeconds = seconds.count();
    }

    return valid;
}

static bool get_milliseconds(const char* zName,
                             const char* zValue,
                             const char* zDisplay_value,
                             milliseconds* pMilliseconds)
{
    bool valid = false;

    if (!zDisplay_value)
    {
        zDisplay_value = zValue;
    }

    mxs::config::DurationUnit unit;
    milliseconds milliseconds;
    if (get_suffixed_duration(zValue, &milliseconds, &unit))
    {
        if (unit == mxs::config::DURATION_IN_DEFAULT)
        {
            log_duration_suffix_warning(zName, zDisplay_value);
        }

        *pMilliseconds = milliseconds;
        valid = true;
    }
    else
    {
        MXS_ERROR("Invalid duration %s: %s=%s.", zName, zValue, zDisplay_value);
    }

    return valid;
}

static bool get_milliseconds(const char* zName,
                             const char* zValue,
                             const char* zDisplay_value,
                             time_t* pMilliseconds)
{
    milliseconds milliseconds;

    bool valid = get_milliseconds(zName, zValue, zDisplay_value, &milliseconds);

    if (valid)
    {
        *pMilliseconds = milliseconds.count();
    }

    return valid;
}

bool config_parse_disk_space_threshold(SERVER::DiskSpaceLimits* pDisk_space_threshold,
                                       const char* zDisk_space_threshold)
{
    mxb_assert(pDisk_space_threshold);
    mxb_assert(zDisk_space_threshold);

    bool success = true;

    using namespace std;

    SERVER::DiskSpaceLimits disk_space_threshold;
    string s(zDisk_space_threshold);

    // Somewhat simplified, this is what we expect: [^:]+:[:digit:]+(,[^:]+:[:digit:]+)*
    // So, e.g. the following are fine "/data:20", "/data1:50,/data2:60", "*:80".

    while (success && !s.empty())
    {
        size_t i = s.find_first_of(',');
        string entry = s.substr(0, i);

        s.erase(0, i != string::npos ? i + 1 : i);

        size_t j = entry.find_first_of(':');

        if (j != string::npos)
        {
            string path = entry.substr(0, j);
            string tail = entry.substr(j + 1);

            mxb::trim(path);
            mxb::trim(tail);

            if (!path.empty() && !tail.empty())
            {
                char* end;
                int32_t percentage = strtol(tail.c_str(), &end, 0);

                if ((*end == 0) && (percentage >= 0) && (percentage <= 100))
                {
                    disk_space_threshold[path] = percentage;
                }
                else
                {
                    MXS_ERROR("The value following the ':' must be a percentage: %s",
                              entry.c_str());
                    success = false;
                }
            }
            else
            {
                MXS_ERROR("The %s parameter '%s' contains an invalid entry: '%s'",
                          CN_DISK_SPACE_THRESHOLD,
                          zDisk_space_threshold,
                          entry.c_str());
                success = false;
            }
        }
        else
        {
            MXS_ERROR("The %s parameter '%s' contains an invalid entry: '%s'",
                      CN_DISK_SPACE_THRESHOLD,
                      zDisk_space_threshold,
                      entry.c_str());
            success = false;
        }
    }

    if (success)
    {
        pDisk_space_threshold->swap(disk_space_threshold);
    }

    return success;
}

std::string generate_config_string(const std::string& instance_name, const mxs::ConfigParameters& parameters,
                                   const MXS_MODULE_PARAM* common_param_defs,
                                   const MXS_MODULE_PARAM* module_param_defs)
{
    string output = "[" + instance_name + "]\n";
    // Common params and module params are null-terminated arrays. Loop over both and print parameter
    // names and values.
    for (auto param_set : {common_param_defs, module_param_defs})
    {
        if (param_set)
        {
            for (int i = 0; param_set[i].name; i++)
            {
                auto param_info = param_set + i;
                // Do not print deprecated parameters.
                if ((param_info->options & MXS_MODULE_OPT_DEPRECATED) == 0)
                {
                    string param_name = param_info->name;
                    if (parameters.contains(param_name))
                    {
                        // Parameter value in the container can be an empty string and still be printed.
                        string param_value = parameters.get_string(param_name);
                        output += param_name + "=" + param_value + "\n";
                    }
                }
            }
        }
    }
    return output;
}

/**
 * Optimal string alignment distance of two strings
 *
 * @see https://en.wikipedia.org/wiki/Damerau%E2%80%93Levenshtein_distance
 *
 * @param a First string
 * @param b Second string
 *
 * @return The distance between the two strings
 */
int string_distance(const std::string& a, const std::string& b)
{
    char d[a.length() + 1][b.length() + 1];

    for (size_t i = 0; i <= a.length(); i++)
    {
        d[i][0] = i;
    }

    for (size_t i = 0; i <= b.length(); i++)
    {
        d[0][i] = i;
    }

    for (size_t i = 1; i <= a.length(); i++)
    {
        for (size_t j = 1; j <= b.length(); j++)
        {
            char cost = a[i - 1] == b[j - 1] ? 0 : 1;
            // Remove, add or substitute a character
            d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});

            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
            {
                // Transpose the characters
                d[i][j] = std::min({d[i][j], (char)(d[i - 2][j - 2] + cost)});
            }
        }
    }

    return d[a.length()][b.length()];
}

/**
 * Returns a suggestion with the parameter name closest to @c str
 *
 * @param str  String to match against
 * @param base Module type parameters
 * @param mod  Module implementation parameters
 *
 * @return A suggestion with the parameter name closest to @c str or an empty string if
 *         the string is not close enough to any of the parameters.
 */
std::string closest_matching_parameter(const std::string& str,
                                       const MXS_MODULE_PARAM* base,
                                       const MXS_MODULE_PARAM* mod)
{
    std::string name;
    int lowest = 99999;     // Nobody can come up with a parameter name this long

    for (auto params : {base, mod})
    {
        for (int i = 0; params[i].name; i++)
        {
            int dist = string_distance(str, params[i].name);

            if (dist < lowest)
            {
                name = params[i].name;
                lowest = dist;
            }
        }
    }

    std::string rval;
    const int min_dist = 4;

    if (lowest <= min_dist)
    {
        rval = "Did you mean '" + name + "'?";
        name.clear();
    }

    return rval;
}

bool config_is_valid_name(const char* zName, std::string* pReason)
{
    bool is_valid = true;

    for (const char* z = zName; is_valid && *z; z++)
    {
        if (isspace(*z))
        {
            is_valid = false;

            if (pReason)
            {
                *pReason = "The name '";
                *pReason += zName;
                *pReason += "' contains whitespace.";
            }
        }
    }

    if (is_valid)
    {
        if (strncmp(zName, "@@", 2) == 0)
        {
            is_valid = false;

            if (pReason)
            {
                *pReason = "The name '";
                *pReason += zName;
                *pReason += "' starts with '@@', which is a prefix reserved for MaxScale.";
            }
        }
    }

    return is_valid;
}

int64_t config_enum_to_value(const std::string& value, const MXS_ENUM_VALUE* values)
{
    for (auto v = values; v->name; ++v)
    {
        if (value == v->name)
        {
            return v->enum_value;
        }
    }

    return MXS_UNKNOWN_ENUM_VALUE;
}

bool validate_param(const MXS_MODULE_PARAM* basic, const MXS_MODULE_PARAM* module,
                    const string& key, const string& value, string* error_out)
{
    bool success = false;
    string error_msg;
    if (!param_is_known(basic, module, key.c_str()))
    {
        error_msg = mxb::string_printf("Unknown parameter: %s", key.c_str());
    }
    else if (!value[0])
    {
        error_msg = mxb::string_printf("Empty value for parameter: %s", key.c_str());
    }
    else if (!param_is_valid(basic, module, key.c_str(), value.c_str()))
    {
        error_msg = mxb::string_printf("Invalid parameter value for '%s': %s", key.c_str(), value.c_str());
    }
    else
    {
        success = true;
    }

    if (!success)
    {
        *error_out = error_msg;
    }
    return success;
}

bool param_is_known(const MXS_MODULE_PARAM* basic, const MXS_MODULE_PARAM* module, const char* key)
{
    std::unordered_set<std::string> names;

    for (auto param : {basic, module})
    {
        for (int i = 0; param[i].name; i++)
        {
            names.insert(param[i].name);
        }
    }

    return names.count(key);
}

bool param_is_valid(const MXS_MODULE_PARAM* basic, const MXS_MODULE_PARAM* module,
                    const char* key, const char* value)
{
    return config_param_is_valid(basic, key, value, NULL)
           || (module && config_param_is_valid(module, key, value, NULL));
}

bool config_set_rebalance_threshold(const char* value)
{
    bool rv = false;

    char* endptr;
    int intval = strtol(value, &endptr, 0);
    if (*endptr == '\0' && intval >= 0 && intval <= 100)
    {
        gateway.rebalance_threshold = intval;
        rv = true;
    }
    else
    {
        MXS_ERROR("Invalid value (percentage expected) for '%s': %s", CN_REBALANCE_THRESHOLD, value);
    }

    return rv;
}
