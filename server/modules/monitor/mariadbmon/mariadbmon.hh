/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-08-24
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once
#include "mariadbmon_common.hh"

#include <condition_variable>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <maxbase/stopwatch.hh>
#include <maxscale/monitor.hh>
#include "mariadbserver.hh"

// Used by multiple source files.
extern const char* const CN_AUTO_FAILOVER;
extern const char* const CN_SWITCHOVER_ON_LOW_DISK_SPACE;
extern const char* const CN_PROMOTION_SQL_FILE;
extern const char* const CN_DEMOTION_SQL_FILE;

// Map of server id:s to MariaDBServer. Useful when constructing the replication graph.
typedef std::unordered_map<int64_t, MariaDBServer*> IdToServerMap;
// Map of cycle number to cycle members. The elements should be ordered for predictability when iterating.
typedef std::map<int, ServerArray> CycleMap;

// MariaDB Monitor instance data
class MariaDBMonitor : public maxscale::MonitorWorker
{
private:
    MariaDBMonitor(const MariaDBMonitor&) = delete;
    MariaDBMonitor& operator=(const MariaDBMonitor&) = delete;
public:
    // Helper class used for testing.
    class Test;
    friend class Test;

    /**
     * Create the monitor instance and return the instance data.
     *
     * @param name Monitor config name
     * @param module Module name
     * @return MariaDBMonitor instance
     */
    static MariaDBMonitor* create(const std::string& name, const std::string& module);

    ~MariaDBMonitor();

    /**
     * Print diagnostics.
     *
     * @param dcb DCB to print to
     */
    void diagnostics(DCB* dcb) const;

    /**
     * Print diagnostics to json object.
     *
     * @return Diagnostics messages
     */
    json_t* diagnostics_json() const;

    /**
     * Perform user-activated switchover.
     *
     * @param new_master      The specified new master. If NULL, monitor will autoselect.
     * @param current_master  The specified current master. If NULL, monitor will autoselect.
     * @param error_out       Json error output
     * @return True if switchover was performed
     */
    bool run_manual_switchover(SERVER* new_master, SERVER* current_master, json_t** error_out);

    /**
     * Perform user-activated failover.
     *
     * @param error_out Json error output
     * @return True if failover was performed
     */
    bool run_manual_failover(json_t** error_out);

    /**
     * Perform user-activated rejoin
     *
     * @param rejoin_server Server to join
     * @param error_out Json error output
     * @return True if rejoin was performed
     */
    bool run_manual_rejoin(SERVER* rejoin_server, json_t** error_out);

    /**
     * Perform user-activated reset-replication
     *
     * @param master_server The server to promote. If NULL, monitor will select the current master.
     * @param error_out Error output
     * @return True if operation completed successfully
     */
    bool run_manual_reset_replication(SERVER* master_server, json_t** error_out);

protected:
    void pre_loop() override;
    void tick() override;
    void process_state_changes() override;

private:
    // Some methods need a log on/off setting.
    enum class Log
    {
        OFF,
        ON
    };

    enum class RequireRunning
    {
        REQUIRED,
        OPTIONAL
    };

    class SwitchoverParams
    {
    public:
        ServerOperation promotion;
        ServerOperation demotion;
        GeneralOpData   general;

        SwitchoverParams(const ServerOperation& promotion, const ServerOperation& demotion,
                         const GeneralOpData& general);
    };

    class FailoverParams
    {
    public:
        ServerOperation            promotion;   // Required by MariaDBServer->promote()
        const MariaDBServer* const demotion_target;
        GeneralOpData              general;

        FailoverParams(const ServerOperation& promotion, const MariaDBServer* demotion_target,
                       const GeneralOpData& general);
    };

    // Information about a multimaster group (replication cycle)
    struct CycleInfo
    {
        int         cycle_id = NodeData::CYCLE_NONE;
        ServerArray cycle_members;
    };

    /* Structure used to communicate commands and results between the MaxAdmin and monitor threads.
     * The monitor can only process one manual command at a time, which is already enforced by
     * the admin thread. */
    struct ManualCommand
    {
    public:
        std::mutex                mutex;        /* Mutex used by the condition variables */
        std::condition_variable   has_command;  /* Notified when a command is waiting execution */
        std::condition_variable   has_result;   /* Notified when the command has ran */
        std::function<void(void)> method;       /* The method to run when executing the command */

        bool command_waiting_exec = false;  /* Guard variable for has_command */
        bool result_waiting = false;        /* Guard variable for has_result */
    };

    class DNSResolver
    {
    public:
        using StringSet = std::unordered_set<std::string>;
        StringSet resolve_server(const std::string& host);

    private:
        struct MapElement
        {
            StringSet   addresses; // A hostname can map to multiple addresses
            mxb::TimePoint timestamp;
        };

        std::unordered_map<std::string, MapElement> m_mapping; // hostname -> address cache
    };

    ManualCommand m_manual_cmd;     /* Communicates manual commands and results */

    // Server containers, mostly constant.
    ServerArray   m_servers;        /* Servers of the monitor */
    IdToServerMap m_servers_by_id;  /* Map from server id:s to MariaDBServer */

    // Topology related fields
    MariaDBServer* m_master = NULL;         /* The most "master-like" server in the cluster. Is the only
                                             * server which can get the Master status. */
    MariaDBServer* m_next_master = NULL;    /* When a cluster operation changes the master, the new master is
                                             * written here so the next monitor tick picks it up. */
    bool m_cluster_topology_changed = true; /* Has cluster topology changed since last monitor loop?
                                             * Causes a topology rebuild on the current tick. */
    bool m_cluster_modified = false;        /* Has a cluster operation been performed this loop? Prevents
                                             * other operations during this tick. */

    DNSResolver m_resolver;                 /* DNS-resolver with cache */

    /* Counter for temporary automatic cluster operation disabling. */
    int cluster_operation_disable_timer = 0;

    CycleMap  m_cycles;                     /* Map from cycle number to cycle member servers */
    CycleInfo m_master_cycle_status;        /* Info about master server cycle from previous round */

    // Miscellaneous info
    int64_t m_master_gtid_domain = GTID_DOMAIN_UNKNOWN;     /* gtid_domain_id most recently seen on
                                                             * the master */

    // Fields controlling logging of various events. TODO: Check these
    bool m_log_no_master {true};                /* Should it be logged that there is no master? */
    bool m_warn_current_master_invalid {true};  /* Print warning if current master is not valid? */
    bool m_warn_cannot_find_master {true};      /* Print warning if a master cannot be found? */
    bool m_warn_master_down {true};             /* Print warning that failover may happen soon? */
    bool m_warn_failover_precond {true};        /* Print failover preconditions error message? */
    bool m_warn_switchover_precond {true};      /* Print switchover preconditions error message? */
    bool m_warn_cannot_rejoin {true};           /* Print warning if auto_rejoin fails because of invalid
                                                 * gtid:s? */

    // MariaDB-Monitor specific settings. These are only written to when configuring the monitor.
    class Settings
    {
    public:
        /* The default setting values given here may not be the actual defaults given by
         * the module configuration. */

        // Replication topology detection settings.

        bool detect_stale_master {true};      /* Allow stale masters. TODO: Remove this */
        bool detect_stale_slave {true};       /* Allow stale slaves: a running slave behind a downed
                                               * master/relay is still a valid slave */
        bool detect_standalone_master {true}; /* Allow writes to a master without any slaves.
                                               * TODO: think about removing */
        bool ignore_external_masters {false}; /* Ignore masters outside of the monitor configuration.
                                               * TODO: requires work */
        bool assume_unique_hostnames {true};  /* Are server hostnames consistent between MaxScale and
                                               * servers */

        int failcount {1};  /* Number of ticks master must be down before it's considered
                             * totally down, allowing failover or master change. */

        // Cluster operations activation settings

        bool auto_failover {false};                 /* Automatic master failover enabled? */
        bool auto_rejoin {false};                   /* Automatic rejoin enabled? */
        bool switchover_on_low_disk_space {false};  /* Automatically switch over a master low on disk space */
        bool maintenance_on_low_disk_space {false}; /* Automatically set slave and unreplicating servers low
                                                     * on disk space to maintenance. */
        bool enforce_read_only_slaves {false};      /* If true, the monitor checks and enforces every tick
                                                     * that all slaves are in read-only-mode. */
        bool enforce_simple_topology {false};       /* Can the monitor assume and enforce a simple, 1-master
                                                     * and N slaves topology? Also allows unsafe failover */

        // Cluster operations additional settings
        int  failover_timeout {10};            /* Time limit in seconds for failover */
        int  switchover_timeout {10};          /* Time limit in seconds for switchover */
        bool verify_master_failure {true};     /* Is master failure is verified via slaves? */
        int  master_failure_timeout {10};      /* Master failure verification (via slaves) time in seconds */

        ServerArray excluded_servers;          /* Servers which cannot be autoselected when deciding which
                                                * slave to promote during failover switchover. */

        SharedSettings shared;                 /* Settings required by MariaDBServer objects */
    };

    Settings m_settings;

    // Base methods
    MariaDBMonitor(const std::string& name, const std::string& module);
    bool configure(const MXS_CONFIG_PARAMETER* params) override;
    bool set_replication_credentials(const MXS_CONFIG_PARAMETER* params);
    void reset_server_info();

    void reset_node_index_info();
    bool execute_manual_command(std::function<void ()> command, json_t** error_out);
    bool immediate_tick_required() const;

    std::string diagnostics_to_string() const;
    json_t*     to_json() const;

    MariaDBServer* get_server(const EndPoint& search_ep);
    MariaDBServer* get_server(int64_t id);
    MariaDBServer* get_server(mxs::MonitorServer* mon_server);
    MariaDBServer* get_server(SERVER* server);

    // Cluster discovery and status assignment methods, top levels
    void update_topology();
    void build_replication_graph();
    void update_master();
    void assign_new_master(MariaDBServer* new_master);
    void find_graph_cycles();
    bool master_is_valid(std::string* reason_out);
    void assign_server_roles();
    void assign_slave_and_relay_master(MariaDBServer* start_node);
    void check_cluster_operations_support();

    MariaDBServer* find_topology_master_server(RequireRunning req_running, std::string* msg_out = nullptr);
    MariaDBServer* find_best_reach_server(const ServerArray& candidates);

    // Cluster discovery and status assignment methods, low level
    void tarjan_scc_visit_node(MariaDBServer* node, ServerArray* stack, int* index, int* cycle);
    void calculate_node_reach(MariaDBServer* search_root);
    int  running_slaves(MariaDBServer* search_root);
    bool cycle_has_master_server(ServerArray& cycle_servers);
    void update_gtid_domain();

    void update_master_cycle_info();
    bool is_candidate_valid(MariaDBServer* cand, RequireRunning req_running, std::string* why_not = nullptr);

    // Cluster operation launchers
    bool manual_switchover(SERVER* new_master, SERVER* current_master, json_t** error_out);
    bool manual_failover(json_t** output);
    bool manual_rejoin(SERVER* rejoin_cand_srv, json_t** output);
    void handle_low_disk_space_master();
    void handle_auto_failover();
    void handle_auto_rejoin();

    const MariaDBServer* slave_receiving_events(const MariaDBServer* demotion_target,
                                                maxbase::Duration* event_age_out,
                                                maxbase::Duration* delay_out);
    std::unique_ptr<SwitchoverParams> switchover_prepare(SERVER* new_master, SERVER* current_master,
                                                         Log log_mode, json_t** error_out);
    std::unique_ptr<FailoverParams> failover_prepare(Log log_mode, json_t** error_out);

    bool switchover_perform(SwitchoverParams& operation);
    bool failover_perform(FailoverParams& op);

    void delay_auto_cluster_ops();
    bool can_perform_cluster_ops();

    // Methods used by failover/switchover/rejoin
    MariaDBServer* select_promotion_target(MariaDBServer* demotion_target, OperationType op, Log log_mode,
                                           int64_t* gtid_domain_out, json_t** error_out);
    bool is_candidate_better(const MariaDBServer* candidate, const MariaDBServer* current_best,
                             const MariaDBServer* demotion_target, uint32_t gtid_domain,
                             std::string* reason_out = NULL);
    bool server_is_excluded(const MariaDBServer* server);
    bool check_gtid_replication(Log log_mode, const MariaDBServer* demotion_target,
                                int64_t cluster_gtid_domain, json_t** error_out);
    int64_t guess_gtid_domain(MariaDBServer* demotion_target, const ServerArray& candidates,
                              int* id_missing_out) const;

    ServerArray get_redirectables(const MariaDBServer* old_master, const MariaDBServer* ignored_slave);

    int redirect_slaves_ex(GeneralOpData& op,
                           OperationType type,
                           const MariaDBServer* promotion_target,
                           const MariaDBServer* demotion_target,
                           ServerArray* redirected_to_promo,
                           ServerArray* redirected_to_demo);

    void        wait_cluster_stabilization(GeneralOpData& op, const ServerArray& slaves,
                                           const MariaDBServer* new_master);

    // Rejoin methods
    bool     cluster_can_be_joined();
    bool     get_joinable_servers(ServerArray* output);
    bool     server_is_rejoin_suspect(MariaDBServer* rejoin_cand, json_t** output);
    uint32_t do_rejoin(const ServerArray& joinable_servers, json_t** output);

    bool check_sql_files();
    void enforce_read_only_on_slaves();
    void log_master_changes();
    void set_low_disk_slaves_maintenance();
    bool manual_reset_replication(SERVER* master_server, json_t** error_out);
};

/**
 * Generates a list of server names separated by ', '
 *
 * @param servers The servers
 * @return Server names
 */
std::string monitored_servers_to_string(const ServerArray& servers);
