/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-07-07
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#define MXS_MODULE_NAME "mariadbbackend"

#include <maxscale/ccdefs.hh>
#include <maxbase/alloc.h>
#include <maxscale/limits.h>
#include <maxscale/modinfo.h>
#include <maxscale/modutil.hh>
#include <maxscale/poll.hh>
#include <maxscale/protocol.hh>
#include <maxscale/protocol/mysql.hh>
#include <maxscale/router.hh>
#include <maxscale/server.hh>
#include <maxscale/utils.h>
// For setting server status through monitor
#include "../../../../core/internal/monitormanager.hh"

#include <sstream>

/*
 * MySQL Protocol module for handling the protocol between the gateway
 * and the backend MySQL database.
 */

static int         gw_create_backend_connection(DCB* backend, SERVER* server, MXS_SESSION* in_session);
static int         gw_read_backend_event(DCB* dcb);
static int         gw_write_backend_event(DCB* dcb);
static int         gw_MySQLWrite_backend(DCB* dcb, GWBUF* queue);
static int         gw_error_backend_event(DCB* dcb);
static int         gw_backend_close(DCB* dcb);
static int         gw_backend_hangup(DCB* dcb);
static int         backend_write_delayqueue(DCB* dcb, GWBUF* buffer);
static void        backend_set_delayqueue(DCB* dcb, GWBUF* queue);
static int         gw_change_user(DCB* backend_dcb, SERVER* server, MXS_SESSION* in_session, GWBUF* queue);
static char*       gw_backend_default_auth();
static GWBUF*      process_response_data(DCB* dcb, GWBUF** readbuf, int nbytes_to_process);
static bool        sescmd_response_complete(DCB* dcb);
static void        gw_reply_on_error(DCB* dcb, mxs_auth_state_t state);
static int         gw_read_and_write(DCB* dcb);
static int         gw_do_connect_to_backend(char* host, int port, int* fd);
static void inline close_socket(int socket);
static GWBUF*      gw_create_change_user_packet(MYSQL_session* mses,
                                                MySQLProtocol* protocol);
static int gw_send_change_user_to_backend(char* dbname,
                                          char* user,
                                          uint8_t* passwd,
                                          MySQLProtocol* conn);
static bool gw_send_proxy_protocol_header(DCB* backend_dcb);

struct AddressInfo
{
    bool        success {false};
    char        addr[INET6_ADDRSTRLEN] {};
    in_port_t   port {0};
    std::string error_msg;
};
static AddressInfo get_ip_string_and_port(const sockaddr_storage* sa);
static bool        gw_connection_established(DCB* dcb);
static bool gw_auth_is_complete(DCB* dcb);
json_t*     gw_json_diagnostics(DCB* dcb);

extern "C"
{
/*
 * The module entry point routine. It is this routine that
 * must populate the structure that is referred to as the
 * "module object", this is a structure with the set of
 * external entry points for this module.
 *
 * @return The module object
 */
MXS_MODULE* MXS_CREATE_MODULE()
{
    static MXS_PROTOCOL MyObject =
    {
        gw_read_backend_event,              /* Read - EPOLLIN handler        */
        gw_MySQLWrite_backend,              /* Write - data from gateway     */
        gw_write_backend_event,             /* WriteReady - EPOLLOUT handler */
        gw_error_backend_event,             /* Error - EPOLLERR handler      */
        gw_backend_hangup,                  /* HangUp - EPOLLHUP handler     */
        NULL,                               /* Accept                        */
        gw_create_backend_connection,       /* Connect                     */
        gw_backend_close,                   /* Close                         */
        gw_change_user,                     /* Authentication                */
        gw_backend_default_auth,            /* Default authenticator         */
        NULL,                               /* Connection limit reached      */
        gw_connection_established,
        gw_json_diagnostics,
        NULL,
        gw_auth_is_complete,
    };

    static MXS_MODULE info =
    {
        MXS_MODULE_API_PROTOCOL,
        MXS_MODULE_GA,
        MXS_PROTOCOL_VERSION,
        "The MySQL to backend server protocol",
        "V2.0.0",
        MXS_NO_MODULE_CAPABILITIES,
        &MyObject,
        NULL,       /* Process init. */
        NULL,       /* Process finish. */
        NULL,       /* Thread init. */
        NULL,       /* Thread finish. */
        {
            {MXS_END_MODULE_PARAMS}
        }
    };

    return &info;
}
}

/**
 * The default authenticator name for this protocol
 *
 * This is not used for a backend protocol, it is for client authentication.
 *
 * @return name of authenticator
 */
static char* gw_backend_default_auth()
{
    return const_cast<char*>("mariadbbackendauth");
}
/*lint +e14 */

/*******************************************************************************
 *******************************************************************************
 *
 * API Entry Point - Connect
 *
 * This is the first entry point that will be called in the life of a backend
 * (database) connection. It creates a protocol data structure and attempts
 * to open a non-blocking socket to the database. If it succeeds, the
 * protocol_auth_state will become MYSQL_CONNECTED.
 *
 *******************************************************************************
 ******************************************************************************/

/*
 * Create a new backend connection.
 *
 * This routine will connect to a backend server and it is called by dbc_connect
 * in router->newSession
 *
 * @param backend_dcb, in, out, use - backend DCB allocated from dcb_connect
 * @param server, in, use - server to connect to
 * @param session, in use - current session from client DCB
 * @return 0/1 on Success and -1 on Failure.
 * If succesful, returns positive fd to socket which is connected to
 *  backend server. Positive fd is copied to protocol and to dcb.
 * If fails, fd == -1 and socket is closed.
 */
static int gw_create_backend_connection(DCB* backend_dcb,
                                        SERVER* server,
                                        MXS_SESSION* session)
{
    MySQLProtocol* protocol = NULL;
    int rv = -1;
    int fd = -1;

    protocol = mysql_protocol_init(backend_dcb, -1);
    mxb_assert(protocol != NULL);

    if (protocol == NULL)
    {
        MXS_ERROR("Failed to create protocol object for backend connection.");
        goto return_fd;
    }

    /** Copy client flags to backend protocol */
    if (backend_dcb->session->client_dcb->protocol)
    {
        MySQLProtocol* client = (MySQLProtocol*)backend_dcb->session->client_dcb->protocol;
        protocol->client_capabilities = client->client_capabilities;
        protocol->charset = client->charset;
        protocol->extra_capabilities = client->extra_capabilities;
    }
    else
    {
        protocol->client_capabilities = (int)GW_MYSQL_CAPABILITIES_CLIENT;
        protocol->charset = 0x08;
    }

    /*
     *< if succeed, fd > 0, -1 otherwise
     * TODO: Better if function returned a protocol auth state
     */
    rv = gw_do_connect_to_backend(server->address, server->port, &fd);
    /*< Assign protocol with backend_dcb */
    backend_dcb->protocol = protocol;

    /*< Set protocol state */
    switch (rv)
    {
    case 0:
        mxb_assert(fd != DCBFD_CLOSED);
        protocol->fd = fd;
        protocol->protocol_auth_state = MXS_AUTH_STATE_CONNECTED;
        MXS_DEBUG("Established "
                  "connection to %s:%i, protocol fd %d client "
                  "fd %d.",
                  server->address,
                  server->port,
                  protocol->fd,
                  session->client_dcb->fd);

        if (server->proxy_protocol)
        {
            gw_send_proxy_protocol_header(backend_dcb);
        }
        break;

    case 1:
        /*
         * The state MYSQL_PENDING_CONNECT is likely to be transitory,
         * as it means the calls have been successful but the connection
         * has not yet completed and the calls are non-blocking.
         */
        mxb_assert(fd != DCBFD_CLOSED);
        protocol->protocol_auth_state = MXS_AUTH_STATE_PENDING_CONNECT;
        protocol->fd = fd;
        MXS_DEBUG("Connection "
                  "pending to %s:%i, protocol fd %d client fd %d.",
                  server->address,
                  server->port,
                  protocol->fd,
                  session->client_dcb->fd);
        break;

    default:
        /* Failure - the state reverts to its initial value */
        mxb_assert(fd == -1);
        mxb_assert(protocol->protocol_auth_state == MXS_AUTH_STATE_INIT);
        break;
    }   /*< switch */

return_fd:
    return fd;
}

/**
 * gw_do_connect_to_backend
 *
 * This routine creates socket and connects to a backend server.
 * Connect it non-blocking operation. If connect fails, socket is closed.
 *
 * @param host The host to connect to
 * @param port The host TCP/IP port
 * @param *fd where connected fd is copied
 * @return 0/1 on success and -1 on failure
 * If successful, fd has file descriptor to socket which is connected to
 * backend server. In failure, fd == -1 and socket is closed.
 *
 */
static int gw_do_connect_to_backend(char* host, int port, int* fd)
{
    struct sockaddr_storage serv_addr = {};
    int rv = -1;

    /* prepare for connect */
    int so;
    size_t sz;

    if (host[0] == '/')
    {
        so = open_unix_socket(MXS_SOCKET_NETWORK, (struct sockaddr_un*)&serv_addr, host);
        sz = sizeof(sockaddr_un);
    }
    else
    {
        so = open_network_socket(MXS_SOCKET_NETWORK, &serv_addr, host, port);
        sz = sizeof(sockaddr_storage);
    }

    if (so == -1)
    {
        MXS_ERROR("Establishing connection to backend server [%s]:%d failed.", host, port);
        return rv;
    }

    rv = connect(so, (struct sockaddr*)&serv_addr, sz);

    if (rv != 0)
    {
        if (errno == EINPROGRESS)
        {
            rv = 1;
        }
        else
        {
            MXS_ERROR("Failed to connect backend server [%s]:%d due to: %d, %s.",
                      host,
                      port,
                      errno,
                      mxs_strerror(errno));
            close(so);
            return rv;
        }
    }

    *fd = so;
    MXS_DEBUG("Connected to backend server [%s]:%d, fd %d.", host, port, so);

    return rv;
}

/**
 * @brief Check if the response contain an error
 *
 * @param buffer Buffer with a complete response
 * @return True if the reponse contains an MySQL error packet
 */
bool is_error_response(GWBUF* buffer)
{
    uint8_t cmd;
    return gwbuf_copy_data(buffer, MYSQL_HEADER_LEN, 1, &cmd) && cmd == MYSQL_REPLY_ERR;
}

/**
 * @brief Log handshake failure
 *
 * @param dcb Backend DCB where authentication failed
 * @param buffer Buffer containing the response from the backend
 */
static void handle_error_response(DCB* dcb, GWBUF* buffer)
{
    uint8_t* data = (uint8_t*)GWBUF_DATA(buffer);
    size_t len = MYSQL_GET_PAYLOAD_LEN(data);
    uint16_t errcode = MYSQL_GET_ERRCODE(data);
    char bufstr[len];
    memcpy(bufstr, data + 7, len - 3);
    bufstr[len - 3] = '\0';

    MXS_ERROR("Invalid authentication message from backend '%s'. Error code: %d, "
              "Msg : %s",
              dcb->server->name(),
              errcode,
              bufstr);

    /** If the error is ER_HOST_IS_BLOCKED put the server into maintenance mode.
     * This will prevent repeated authentication failures. */
    if (errcode == ER_HOST_IS_BLOCKED)
    {
        auto main_worker = mxs::RoutingWorker::get(mxs::RoutingWorker::MAIN);
        auto target_server = dcb->server;
        main_worker->execute([target_server]() {
                                 MonitorManager::set_server_status(target_server, SERVER_MAINT);
                             }, mxb::Worker::EXECUTE_AUTO);

        MXS_ERROR("Server %s has been put into maintenance mode due to the server blocking connections "
                  "from MaxScale. Run 'mysqladmin -h %s -P %d flush-hosts' on this server before taking "
                  "this server out of maintenance mode. To avoid this problem in the future, set "
                  "'max_connect_errors' to a larger value in the backend server.",
                  dcb->server->name(),
                  dcb->server->address,
                  dcb->server->port);
    }
    else if (errcode == ER_ACCESS_DENIED_ERROR
             || errcode == ER_DBACCESS_DENIED_ERROR
             || errcode == ER_ACCESS_DENIED_NO_PASSWORD_ERROR)
    {
        // Authentication failed, reload users
        service_refresh_users(dcb->service);
    }
}

/**
 * @brief Handle the server's response packet
 *
 * This function reads the server's response packet and does the final step of
 * the authentication.
 *
 * @param dcb Backend DCB
 * @param buffer Buffer containing the server's complete handshake
 * @return MXS_AUTH_STATE_HANDSHAKE_FAILED on failure.
 */
mxs_auth_state_t handle_server_response(DCB* dcb, GWBUF* buffer)
{
    MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol;
    mxs_auth_state_t rval = proto->protocol_auth_state == MXS_AUTH_STATE_CONNECTED ?
        MXS_AUTH_STATE_HANDSHAKE_FAILED : MXS_AUTH_STATE_FAILED;

    if (dcb->authfunc.extract(dcb, buffer))
    {
        switch (dcb->authfunc.authenticate(dcb))
        {
        case MXS_AUTH_INCOMPLETE:
        case MXS_AUTH_SSL_INCOMPLETE:
            rval = MXS_AUTH_STATE_RESPONSE_SENT;
            break;

        case MXS_AUTH_SUCCEEDED:
            rval = MXS_AUTH_STATE_COMPLETE;

        default:
            break;
        }
    }

    return rval;
}

/**
 * @brief Prepare protocol for a write
 *
 * This prepares both the buffer and the protocol itself for writing a query
 * to the backend.
 *
 * @param dcb    The backend DCB to write to
 * @param buffer Buffer that will be written
 */
static inline void prepare_for_write(DCB* dcb, GWBUF* buffer)
{
    MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol;

    // The DCB's session is set to null when it is put into the persistent connection pool.
    if (dcb->session)
    {
        uint64_t capabilities = service_get_capabilities(dcb->session->service);

        /**
         * Copy the current command being executed to this backend. For statement
         * based routers, this is tracked by using the current command being executed.
         * For routers that stream data, the client protocol command tracking data
         * is used which does not guarantee that the correct command is tracked if
         * something queues commands internally.
         */
        if (rcap_type_required(capabilities, RCAP_TYPE_STMT_INPUT))
        {
            uint8_t* data = GWBUF_DATA(buffer);

            if (!proto->large_query && !session_is_load_active(dcb->session))
            {
                proto->current_command = (mxs_mysql_cmd_t)MYSQL_GET_COMMAND(data);
            }

            /**
             * If the buffer contains a large query, we have to skip the command
             * byte extraction for the next packet. This way current_command always
             * contains the latest command executed on this backend.
             */
            proto->large_query = MYSQL_GET_PAYLOAD_LEN(data) == MYSQL_PACKET_LENGTH_MAX;
        }
        else if (dcb->session->client_dcb && dcb->session->client_dcb->protocol)
        {
            MySQLProtocol* client_proto = (MySQLProtocol*)dcb->session->client_dcb->protocol;
            proto->current_command = client_proto->current_command;
        }
    }

    if (GWBUF_SHOULD_COLLECT_RESULT(buffer))
    {
        proto->collect_result = true;
    }

    proto->track_state = GWBUF_SHOULD_TRACK_STATE(buffer);
}

/*******************************************************************************
 *******************************************************************************
 *
 * API Entry Point - Read
 *
 * When the polling mechanism finds that new incoming data is available for
 * a backend connection, it will call this entry point, passing the relevant
 * DCB.
 *
 * The first time through, it is expected that protocol_auth_state will be
 * MYSQL_CONNECTED and an attempt will be made to send authentication data
 * to the backend server. The state may progress to MYSQL_AUTH_REC although
 * for an SSL connection this will not happen straight away, and the state
 * will remain MYSQL_CONNECTED.
 *
 * When the connection is fully established, it is expected that the state
 * will be MYSQL_IDLE and the information read from the backend will be
 * transferred to the client (front end).
 *
 *******************************************************************************
 ******************************************************************************/

/**
 * Backend Read Event for EPOLLIN on the MySQL backend protocol module
 * @param dcb   The backend Descriptor Control Block
 * @return 1 on operation, 0 for no action
 */
static int gw_read_backend_event(DCB* dcb)
{
    if (dcb->persistentstart)
    {
        /** If a DCB gets a read event when it's in the persistent pool, it is
         * treated as if it were an error. */
        poll_fake_hangup_event(dcb);
        return 0;
    }

    mxb_assert(dcb->session);

    MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol;

    MXS_DEBUG("Read dcb %p fd %d protocol state %d, %s.",
              dcb,
              dcb->fd,
              proto->protocol_auth_state,
              mxs::to_string(proto->protocol_auth_state));

    int rc = 0;
    if (proto->protocol_auth_state == MXS_AUTH_STATE_COMPLETE)
    {
        rc = gw_read_and_write(dcb);
    }
    else
    {
        GWBUF* readbuf = NULL;

        if (!read_complete_packet(dcb, &readbuf))
        {
            proto->protocol_auth_state = MXS_AUTH_STATE_FAILED;
            gw_reply_on_error(dcb, proto->protocol_auth_state);
        }
        else if (readbuf)
        {
            /*
            ** We have a complete response from the server
            ** TODO: add support for non-contiguous responses
            */
            readbuf = gwbuf_make_contiguous(readbuf);
            MXS_ABORT_IF_NULL(readbuf);

            if (is_error_response(readbuf))
            {
                /** The server responded with an error */
                proto->protocol_auth_state = MXS_AUTH_STATE_FAILED;
                handle_error_response(dcb, readbuf);
            }

            if (proto->protocol_auth_state == MXS_AUTH_STATE_CONNECTED)
            {
                mxs_auth_state_t state = MXS_AUTH_STATE_FAILED;

                /** Read the server handshake and send the standard response */
                if (gw_read_backend_handshake(dcb, readbuf))
                {
                    state = gw_send_backend_auth(dcb);
                }

                proto->protocol_auth_state = state;
            }
            else if (proto->protocol_auth_state == MXS_AUTH_STATE_RESPONSE_SENT)
            {
                /** Read the message from the server. This will be the first
                 * packet that can contain authenticator specific data from the
                 * backend server. For 'mysql_native_password' it'll be an OK
                 * packet */
                proto->protocol_auth_state = handle_server_response(dcb, readbuf);
            }

            if (proto->protocol_auth_state == MXS_AUTH_STATE_COMPLETE)
            {
                /** Authentication completed successfully */
                GWBUF* localq = dcb->delayq;
                dcb->delayq = NULL;

                if (localq)
                {
                    /** Send the queued commands to the backend */
                    prepare_for_write(dcb, localq);
                    rc = backend_write_delayqueue(dcb, localq);
                }
            }
            else if (proto->protocol_auth_state == MXS_AUTH_STATE_FAILED
                     || proto->protocol_auth_state == MXS_AUTH_STATE_HANDSHAKE_FAILED)
            {
                /** Authentication failed */
                gw_reply_on_error(dcb, proto->protocol_auth_state);
            }

            gwbuf_free(readbuf);
        }
        else if (proto->protocol_auth_state == MXS_AUTH_STATE_CONNECTED
                 && dcb->ssl_state == SSL_ESTABLISHED)
        {
            proto->protocol_auth_state = gw_send_backend_auth(dcb);
        }
    }

    return rc;
}

static std::string get_detailed_error(DCB* dcb)
{
    std::ostringstream ss;

    if (int err = gw_getsockerrno(dcb->fd))
    {
        ss << " (" << err << ", " << mxs_strerror(err) << ")";
    }
    else if (dcb->is_fake_event)
    {
        // Fake events should not have TCP socket errors
        ss << " (Generated event)";
    }

    return ss.str();
}

static void do_handle_error(DCB* dcb, mxs_error_action_t action, const char* errmsg)
{
    bool succp = true;
    MXS_SESSION* session = dcb->session;

    mxb_assert(!dcb->dcb_errhandle_called);

    GWBUF* errbuf = mysql_create_custom_error(1, 0, (errmsg + get_detailed_error(dcb)).c_str());
    MXS_ROUTER_SESSION* rsession = static_cast<MXS_ROUTER_SESSION*>(session->router_session);
    MXS_ROUTER_OBJECT* router = session->service->router;
    MXS_ROUTER* router_instance = session->service->router_instance;

    router->handleError(router_instance, rsession, errbuf, dcb, action, &succp);

    gwbuf_free(errbuf);
    /**
     * If error handler fails it means that routing session can't continue
     * and it must be closed. In success, only this DCB is closed.
     */
    if (!succp)
    {
        session->close_reason = SESSION_CLOSE_HANDLEERROR_FAILED;
        poll_fake_hangup_event(session->client_dcb);
    }
}

/**
 * @brief Authentication of backend - read the reply, or handle an error
 *
 * @param dcb               Descriptor control block for backend server
 * @param local_session     The current MySQL session data structure
 * @return
 */
static void gw_reply_on_error(DCB* dcb, mxs_auth_state_t state)
{
    MXS_SESSION* session = dcb->session;

    do_handle_error(dcb,
                    ERRACT_REPLY_CLIENT,
                    "Authentication with backend failed. Session will be closed.");
}

/**
 * @brief Check if a reply can be routed to the client
 *
 * @param Backend DCB
 * @return True if session is ready for reply routing
 */
static inline bool session_ok_to_route(DCB* dcb)
{
    bool rval = false;

    if (dcb->session->state == SESSION_STATE_STARTED
        && dcb->session->client_dcb != NULL
        && dcb->session->client_dcb->state == DCB_STATE_POLLING
        && (dcb->session->router_session
            || service_get_capabilities(dcb->session->service) & RCAP_TYPE_NO_RSESSION))
    {
        MySQLProtocol* client_protocol = (MySQLProtocol*)dcb->session->client_dcb->protocol;

        if (client_protocol)
        {
            if (client_protocol->protocol_auth_state == MXS_AUTH_STATE_COMPLETE)
            {
                rval = true;
            }
        }
        else if (dcb->session->client_dcb->role == DCB::Role::INTERNAL)
        {
            rval = true;
        }
    }

    return rval;
}

static inline bool expecting_text_result(MySQLProtocol* proto)
{
    return proto->current_command == MXS_COM_QUERY
           || proto->current_command == MXS_COM_STMT_EXECUTE
           ||   /**
                 * The addition of COM_STMT_FETCH to the list of commands that produce
                 * result sets is slightly wrong. The command can generate complete
                 * result sets but it can also generate incomplete ones if cursors
                 * are used. The use of cursors most likely needs to be detected on
                 * an upper level and the use of this function avoided in those cases.
                 */
           proto->current_command == MXS_COM_STMT_FETCH;
}

static inline bool expecting_ps_response(MySQLProtocol* proto)
{
    return proto->current_command == MXS_COM_STMT_PREPARE;
}

static inline bool complete_ps_response(GWBUF* buffer)
{
    mxb_assert(GWBUF_IS_CONTIGUOUS(buffer));
    MXS_PS_RESPONSE resp;
    bool rval = false;

    if (mxs_mysql_extract_ps_response(buffer, &resp))
    {
        int expected_packets = 1;

        if (resp.columns > 0)
        {
            // Column definition packets plus one for the EOF
            expected_packets += resp.columns + 1;
        }

        if (resp.parameters > 0)
        {
            // Parameter definition packets plus one for the EOF
            expected_packets += resp.parameters + 1;
        }

        int n_packets = modutil_count_packets(buffer);

        MXS_DEBUG("Expecting %u packets, have %u", n_packets, expected_packets);

        rval = n_packets == expected_packets;
    }

    return rval;
}

static inline bool collecting_resultset(MySQLProtocol* proto, uint64_t capabilities)
{
    return rcap_type_required(capabilities, RCAP_TYPE_RESULTSET_OUTPUT)
           || proto->collect_result;
}

/**
 * Helpers for checking OK and ERR packets specific to COM_CHANGE_USER
 */
static inline bool not_ok_packet(const GWBUF* buffer)
{
    const uint8_t* data = GWBUF_DATA(buffer);

    return data[4] != MYSQL_REPLY_OK
           ||   // Should be more than 7 bytes of payload
           gw_mysql_get_byte3(data) < MYSQL_OK_PACKET_MIN_LEN - MYSQL_HEADER_LEN
           ||   // Should have no affected rows
           data[5] != 0
           ||   // Should not generate an insert ID
           data[6] != 0;
}

static inline bool not_err_packet(const GWBUF* buffer)
{
    return GWBUF_DATA(buffer)[4] != MYSQL_REPLY_ERR;
}

static inline bool auth_change_requested(GWBUF* buf)
{
    return mxs_mysql_get_command(buf) == MYSQL_REPLY_AUTHSWITCHREQUEST
           && gwbuf_length(buf) > MYSQL_EOF_PACKET_LEN;
}

static bool handle_auth_change_response(GWBUF* reply, MySQLProtocol* proto, DCB* dcb)
{
    bool rval = false;

    if (strcmp((char*)GWBUF_DATA(reply) + 5, DEFAULT_MYSQL_AUTH_PLUGIN) == 0)
    {
        /**
         * The server requested a change of authentication methods.
         * If we're changing the authentication method to the same one we
         * are using now, it means that the server is simply generating
         * a new scramble for the re-authentication process.
         */

        rval = send_mysql_native_password_response(dcb, reply);
    }

    return rval;
}

/**
 * @brief With authentication completed, read new data and write to backend
 *
 * @param dcb           Descriptor control block for backend server
 * @param local_session Current MySQL session data structure
 * @return 0 is fail, 1 is success
 */
static int gw_read_and_write(DCB* dcb)
{
    GWBUF* read_buffer = NULL;
    MXS_SESSION* session = dcb->session;
    int nbytes_read = 0;
    int return_code = 0;

    /* read available backend data */
    return_code = dcb_read(dcb, &read_buffer, 0);

    if (return_code < 0)
    {
        do_handle_error(dcb, ERRACT_NEW_CONNECTION, "Read from backend failed");
        return 0;
    }

    if (read_buffer)
    {
        nbytes_read = gwbuf_length(read_buffer);
    }

    if (nbytes_read == 0)
    {
        mxb_assert(read_buffer == NULL);
        return return_code;
    }
    else
    {
        mxb_assert(read_buffer != NULL);
    }

    /** Ask what type of output the router/filter chain expects */
    uint64_t capabilities = service_get_capabilities(session->service);
    bool result_collected = false;
    MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol;

    if (rcap_type_required(capabilities, RCAP_TYPE_PACKET_OUTPUT)
        || rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT)
        || rcap_type_required(capabilities, RCAP_TYPE_CONTIGUOUS_OUTPUT)
        || proto->collect_result
        || proto->ignore_replies != 0)
    {
        GWBUF* tmp = modutil_get_complete_packets(&read_buffer);
        /* Put any residue into the read queue */

        dcb_readq_set(dcb, read_buffer);

        if (tmp == NULL)
        {
            /** No complete packets */
            return 0;
        }

        /** Get sesion track info from ok packet and save it to gwbuf properties.
         *
         * The OK packets sent in response to COM_STMT_PREPARE are of a different
         * format so we need to detect and skip them. */
        if (rcap_type_required(capabilities, RCAP_TYPE_SESSION_STATE_TRACKING)
            && !expecting_ps_response(proto)
            && proto->track_state)
        {
            mxs_mysql_get_session_track_info(tmp, proto);
        }

        read_buffer = tmp;

        if (rcap_type_required(capabilities, RCAP_TYPE_CONTIGUOUS_OUTPUT)
            || proto->collect_result
            || proto->ignore_replies != 0)
        {
            if ((tmp = gwbuf_make_contiguous(read_buffer)))
            {
                read_buffer = tmp;
            }
            else
            {
                /** Failed to make the buffer contiguous */
                gwbuf_free(read_buffer);
                poll_fake_hangup_event(dcb);
                return 0;
            }

            if (collecting_resultset(proto, capabilities))
            {
                if (expecting_text_result(proto))
                {
                    if (mxs_mysql_is_result_set(read_buffer))
                    {
                        bool more = false;
                        int eof_cnt = modutil_count_signal_packets(read_buffer, 0, &more, NULL);
                        if (more || eof_cnt % 2 != 0)
                        {
                            dcb_readq_prepend(dcb, read_buffer);
                            return 0;
                        }
                    }

                    // Collected the complete result
                    proto->collect_result = false;
                    result_collected = true;
                }
                else if (expecting_ps_response(proto)
                         && mxs_mysql_is_prep_stmt_ok(read_buffer)
                         && !complete_ps_response(read_buffer))
                {
                    dcb_readq_prepend(dcb, read_buffer);
                    return 0;
                }
                else
                {
                    // Collected the complete result
                    proto->collect_result = false;
                    result_collected = true;
                }
            }
        }
    }

    if (proto->changing_user)
    {
        if (auth_change_requested(read_buffer)
            && handle_auth_change_response(read_buffer, proto, dcb))
        {
            gwbuf_free(read_buffer);
            return 0;
        }
        else
        {
            /**
             * The client protocol always requests an authentication method
             * switch to the same plugin to be compatible with most connectors.
             *
             * To prevent packet sequence number mismatch, always return a sequence
             * of 3 for the final response to a COM_CHANGE_USER.
             */
            GWBUF_DATA(read_buffer)[3] = 0x3;
            proto->changing_user = false;

            auto s = (MYSQL_session*)session->client_dcb->data;
            s->changing_user = false;
        }
    }

    if (proto->ignore_replies > 0)
    {
        /** The reply to a COM_CHANGE_USER is in packet */
        GWBUF* query = proto->stored_query;
        proto->stored_query = NULL;
        proto->ignore_replies--;
        mxb_assert(proto->ignore_replies >= 0);
        GWBUF* reply = modutil_get_next_MySQL_packet(&read_buffer);

        while (read_buffer)
        {
            /** Skip to the last packet if we get more than one */
            gwbuf_free(reply);
            reply = modutil_get_next_MySQL_packet(&read_buffer);
        }

        mxb_assert(reply);
        mxb_assert(!read_buffer);
        uint8_t result = MYSQL_GET_COMMAND(GWBUF_DATA(reply));
        int rval = 0;

        if (result == MYSQL_REPLY_OK)
        {
            MXS_INFO("Response to COM_CHANGE_USER is OK, writing stored query");
            rval = query ? dcb->func.write(dcb, query) : 1;
        }
        else if (auth_change_requested(reply))
        {
            if (handle_auth_change_response(reply, proto, dcb))
            {
                /** Store the query until we know the result of the authentication
                 * method switch. */
                proto->stored_query = query;
                proto->ignore_replies++;

                gwbuf_free(reply);
                return rval;
            }
            else
            {
                /** The server requested a change to something other than
                 * the default auth plugin */
                gwbuf_free(query);
                poll_fake_hangup_event(dcb);

                // TODO: Use the authenticators to handle COM_CHANGE_USER responses
                MXS_ERROR("Received AuthSwitchRequest to '%s' when '%s' was expected",
                          (char*)GWBUF_DATA(reply) + 5,
                          DEFAULT_MYSQL_AUTH_PLUGIN);
            }
        }
        else
        {
            /**
             * The ignorable command failed when we had a queued query from the
             * client. Generate a fake hangup event to close the DCB and send
             * an error to the client.
             */
            if (result == MYSQL_REPLY_ERR)
            {
                /** The COM_CHANGE USER failed, generate a fake hangup event to
                 * close the DCB and send an error to the client. */
                handle_error_response(dcb, reply);
            }
            else
            {
                /** This should never happen */
                MXS_ERROR("Unknown response to COM_CHANGE_USER (0x%02hhx), "
                          "closing connection",
                          result);
            }

            gwbuf_free(query);
            poll_fake_hangup_event(dcb);
        }

        gwbuf_free(reply);
        return rval;
    }

    do
    {
        GWBUF* stmt = NULL;

        if (result_collected)
        {
            /** The result set or PS response was collected, we know it's complete */
            stmt = read_buffer;
            read_buffer = NULL;
            gwbuf_set_type(stmt, GWBUF_TYPE_RESULT);
        }
        else if (rcap_type_required(capabilities, RCAP_TYPE_STMT_OUTPUT)
                 && !rcap_type_required(capabilities, RCAP_TYPE_RESULTSET_OUTPUT))
        {
            stmt = modutil_get_next_MySQL_packet(&read_buffer);
            mxb_assert_message(stmt, "There should be only complete packets in read_buffer");

            if (stmt && !GWBUF_IS_CONTIGUOUS(stmt))
            {
                // Make sure the buffer is contiguous
                stmt = gwbuf_make_contiguous(stmt);
            }
        }
        else
        {
            stmt = read_buffer;
            read_buffer = NULL;
        }

        if (session_ok_to_route(dcb))
        {
            if (result_collected)
            {
                // Mark that this is a buffer containing a collected result
                gwbuf_set_type(stmt, GWBUF_TYPE_RESULT);
            }

            session->service->router->clientReply(session->service->router_instance,
                                                  session->router_session,
                                                  stmt,
                                                  dcb);
            return_code = 1;
        }
        else    /*< session is closing; replying to client isn't possible */
        {
            gwbuf_free(stmt);
        }
    }
    while (read_buffer);

    return return_code;
}

/*
 * EPOLLOUT handler for the MySQL Backend protocol module.
 *
 * @param dcb   The descriptor control block
 * @return      1 in success, 0 in case of failure,
 */
static int gw_write_backend_event(DCB* dcb)
{
    int rc = 1;

    if (dcb->state != DCB_STATE_POLLING)
    {
        /** Don't write to backend if backend_dcb is not in poll set anymore */
        uint8_t* data = NULL;
        bool com_quit = false;

        if (dcb->writeq)
        {
            data = (uint8_t*) GWBUF_DATA(dcb->writeq);
            com_quit = MYSQL_IS_COM_QUIT(data);
        }

        if (data)
        {
            rc = 0;

            if (!com_quit)
            {
                mysql_send_custom_error(dcb->session->client_dcb,
                                        1,
                                        0,
                                        "Writing to backend failed due invalid Maxscale state.");
                MXS_ERROR("Attempt to write buffered data to backend "
                          "failed due internal inconsistent state: %s",
                          STRDCBSTATE(dcb->state));
            }
        }
        else
        {
            MXS_DEBUG("Dcb %p in state %s but there's nothing to write either.",
                      dcb,
                      STRDCBSTATE(dcb->state));
        }
    }
    else
    {
        MySQLProtocol* backend_protocol = (MySQLProtocol*)dcb->protocol;

        if (backend_protocol->protocol_auth_state == MXS_AUTH_STATE_PENDING_CONNECT)
        {
            backend_protocol->protocol_auth_state = MXS_AUTH_STATE_CONNECTED;
            if (dcb->server->proxy_protocol && !gw_send_proxy_protocol_header(dcb))
            {
                rc = 0;
            }
        }
        else
        {
            dcb_drain_writeq(dcb);
        }

        MXS_DEBUG("wrote to dcb %p fd %d, return %d", dcb, dcb->fd, rc);
    }

    return rc;
}

/*
 * Write function for backend DCB. Store command to protocol.
 *
 * @param dcb   The DCB of the backend
 * @param queue Queue of buffers to write
 * @return      0 on failure, 1 on success
 */
static int gw_MySQLWrite_backend(DCB* dcb, GWBUF* queue)
{
    MySQLProtocol* backend_protocol = static_cast<MySQLProtocol*>(dcb->protocol);
    int rc = 0;

    if (dcb->was_persistent)
    {
        mxb_assert(!dcb->fakeq);
        mxb_assert(!dcb->readq);
        mxb_assert(!dcb->delayq);
        mxb_assert(!dcb->writeq);
        mxb_assert(dcb->persistentstart == 0);
        dcb->was_persistent = false;
        mxb_assert(backend_protocol->ignore_replies >= 0);
        backend_protocol->ignore_replies = 0;

        if (dcb->state != DCB_STATE_POLLING
            || backend_protocol->protocol_auth_state != MXS_AUTH_STATE_COMPLETE)
        {
            MXS_INFO("DCB and protocol state do not qualify for pooling: %s, %s",
                     STRDCBSTATE(dcb->state),
                     mxs::to_string(backend_protocol->protocol_auth_state));
            gwbuf_free(queue);
            return 0;
        }


        /**
         * This is a DCB that was just taken out of the persistent connection pool.
         * We need to sent a COM_CHANGE_USER query to the backend to reset the
         * session state.
         */
        if (backend_protocol->stored_query)
        {
            /** It is possible that the client DCB is closed before the COM_CHANGE_USER
             * response is received. */
            gwbuf_free(backend_protocol->stored_query);
        }

        if (MYSQL_IS_COM_QUIT(GWBUF_DATA(queue)))
        {
            /** The connection is being closed before the first write to this
             * backend was done. The COM_QUIT is ignored and the DCB will be put
             * back into the pool once it's closed. */
            MXS_INFO("COM_QUIT received as the first write, ignoring and "
                     "sending the DCB back to the pool.");
            gwbuf_free(queue);
            return 1;
        }

        GWBUF* buf = gw_create_change_user_packet(static_cast<MYSQL_session*>(dcb->session->client_dcb->data),
                                                  static_cast<MySQLProtocol*>(dcb->protocol));
        int rc = 0;

        if (dcb_write(dcb, buf))
        {
            MXS_INFO("Sent COM_CHANGE_USER");
            backend_protocol->ignore_replies++;
            backend_protocol->stored_query = queue;
            rc = 1;
        }
        else
        {
            gwbuf_free(queue);
        }

        return rc;
    }
    else if (backend_protocol->ignore_replies > 0)
    {
        if (MYSQL_IS_COM_QUIT((uint8_t*)GWBUF_DATA(queue)))
        {
            /** The COM_CHANGE_USER was already sent but the session is already
             * closing. */
            MXS_INFO("COM_QUIT received while COM_CHANGE_USER is in progress, closing pooled connection");
            gwbuf_free(queue);
            poll_fake_hangup_event(dcb);
            rc = 0;
        }
        else
        {
            /**
             * We're still waiting on the reply to the COM_CHANGE_USER, append the
             * buffer to the stored query. This is possible if the client sends
             * BLOB data on the first command or is sending multiple COM_QUERY
             * packets at one time.
             */
            MXS_INFO("COM_CHANGE_USER in progress, appending query to queue");
            backend_protocol->stored_query = gwbuf_append(backend_protocol->stored_query, queue);
            rc = 1;
        }
        return rc;
    }

    /**
     * Pick action according to state of protocol.
     * If auth failed, return value is 0, write and buffered write
     * return 1.
     */
    switch (backend_protocol->protocol_auth_state)
    {
    case MXS_AUTH_STATE_HANDSHAKE_FAILED:
    case MXS_AUTH_STATE_FAILED:
        if (dcb->session->state != SESSION_STATE_STOPPING)
        {
            MXS_ERROR("Unable to write to backend '%s' due to "
                      "%s failure. Server in state %s.",
                      dcb->server->name(),
                      backend_protocol->protocol_auth_state == MXS_AUTH_STATE_HANDSHAKE_FAILED ?
                      "handshake" : "authentication",
                      dcb->server->status_string().c_str());
        }

        gwbuf_free(queue);
        rc = 0;

        break;

    case MXS_AUTH_STATE_COMPLETE:
        {
            uint8_t* ptr = GWBUF_DATA(queue);
            mxs_mysql_cmd_t cmd = static_cast<mxs_mysql_cmd_t>(mxs_mysql_get_command(queue));

            MXS_DEBUG("write to dcb %p fd %d protocol state %s.",
                      dcb,
                      dcb->fd,
                      mxs::to_string(backend_protocol->protocol_auth_state));

            prepare_for_write(dcb, queue);

            if (cmd == MXS_COM_QUIT && dcb->server->persistent_conns_enabled())
            {
                /** We need to keep the pooled connections alive so we just ignore the COM_QUIT packet */
                gwbuf_free(queue);
                rc = 1;
            }
            else
            {
                if (GWBUF_IS_IGNORABLE(queue))
                {
                    /** The response to this command should be ignored */
                    backend_protocol->ignore_replies++;
                    mxb_assert(backend_protocol->ignore_replies > 0);
                }

                /** Write to backend */
                rc = dcb_write(dcb, queue);
            }
        }
        break;

    default:
        {
            MXS_DEBUG("delayed write to dcb %p fd %d protocol state %s.",
                      dcb,
                      dcb->fd,
                      mxs::to_string(backend_protocol->protocol_auth_state));

            /** Store data until authentication is complete */
            prepare_for_write(dcb, queue);
            backend_set_delayqueue(dcb, queue);
            rc = 1;
        }
        break;
    }
    return rc;
}

/**
 * Error event handler.
 * Create error message, pass it to router's error handler and if error
 * handler fails in providing enough backend servers, mark session being
 * closed and call DCB close function which triggers closing router session
 * and related backends (if any exists.
 */
static int gw_error_backend_event(DCB* dcb)
{
    MXS_SESSION* session = dcb->session;

    if (!session)
    {
        if (dcb->persistentstart == 0)
        {
            /** Not a persistent connection, something is wrong. */
            MXS_ERROR("EPOLLERR event on a non-persistent DCB with no session. Closing connection.");
        }
        dcb_close(dcb);
    }
    else if (dcb->state != DCB_STATE_POLLING || session->state != SESSION_STATE_STARTED)
    {
        if (auto err = gw_getsockerrno(dcb->fd))
        {
            MXS_ERROR("DCB in state %s got error '%s'.", STRDCBSTATE(dcb->state), mxs_strerror(err));
        }
    }
    else
    {
        do_handle_error(dcb, ERRACT_NEW_CONNECTION, "Lost connection to backend server: network error");
    }

    return 1;
}

/**
 * Error event handler.
 * Create error message, pass it to router's error handler and if error
 * handler fails in providing enough backend servers, mark session being
 * closed and call DCB close function which triggers closing router session
 * and related backends (if any exists.
 *
 * @param dcb The current Backend DCB
 * @return 1 always
 */
static int gw_backend_hangup(DCB* dcb)
{
    mxb_assert(dcb->n_close == 0);
    MXS_SESSION* session = dcb->session;

    if (!dcb->persistentstart)
    {
        if (session->state != SESSION_STATE_STARTED)
        {
            if (session->state != SESSION_STATE_STOPPING)
            {
                if (auto err = gw_getsockerrno(dcb->fd))
                {
                    MXS_ERROR("Hangup in session that is not ready for routing: %s", mxs_strerror(err));
                }
            }
        }
        else
        {
            do_handle_error(dcb, ERRACT_NEW_CONNECTION,
                            "Lost connection to backend server: connection closed by peer");
        }
    }

    return 1;
}

/**
 * Send COM_QUIT to backend so that it can be closed.
 * @param dcb The current Backend DCB
 * @return 1 always
 */
static int gw_backend_close(DCB* dcb)
{
    mxb_assert(dcb->session || dcb->persistentstart);
    MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol;

    // Always send a COM_QUIT to the backend being closed. This causes the connection to be closed faster.
    dcb->silence_write_errors = true;
    dcb_write(dcb, mysql_create_com_quit(NULL, 0));

    /** Free protocol data */
    mysql_protocol_done(dcb);

    return 1;
}

/**
 * This routine put into the delay queue the input queue
 * The input is what backend DCB is receiving
 * The routine is called from func.write() when mysql backend connection
 * is not yet complete buu there are inout data from client
 *
 * @param dcb   The current backend DCB
 * @param queue Input data in the GWBUF struct
 */
static void backend_set_delayqueue(DCB* dcb, GWBUF* queue)
{
    /* Append data */
    dcb->delayq = gwbuf_append(dcb->delayq, queue);
}

/**
 * This routine writes the delayq via dcb_write
 * The dcb->delayq contains data received from the client before
 * mysql backend authentication succeded
 *
 * @param dcb The current backend DCB
 * @return The dcb_write status
 */
static int backend_write_delayqueue(DCB* dcb, GWBUF* buffer)
{
    mxb_assert(buffer);
    mxb_assert(dcb->persistentstart == 0);
    mxb_assert(!dcb->was_persistent);

    if (MYSQL_IS_CHANGE_USER(((uint8_t*)GWBUF_DATA(buffer))))
    {
        /** Recreate the COM_CHANGE_USER packet with the scramble the backend sent to us */
        MYSQL_session mses;
        gw_get_shared_session_auth_info(dcb, &mses);
        gwbuf_free(buffer);
        buffer = gw_create_change_user_packet(&mses, static_cast<MySQLProtocol*>(dcb->protocol));
    }

    int rc = 1;

    if (MYSQL_IS_COM_QUIT(((uint8_t*)GWBUF_DATA(buffer))) && dcb->server->persistent_conns_enabled())
    {
        /** We need to keep the pooled connections alive so we just ignore the COM_QUIT packet */
        gwbuf_free(buffer);
        rc = 1;
    }
    else
    {
        rc = dcb_write(dcb, buffer);
    }

    if (rc == 0)
    {
        do_handle_error(dcb, ERRACT_NEW_CONNECTION,
                        "Lost connection to backend server while writing delay queue.");
    }

    return rc;
}

/**
 * This routine handles the COM_CHANGE_USER command
 *
 * TODO: Move this into the authenticators
 *
 * @param dcb           The current backend DCB
 * @param server        The backend server pointer
 * @param in_session    The current session data (MYSQL_session)
 * @param queue         The GWBUF containing the COM_CHANGE_USER receveid
 * @return 1 on success and 0 on failure
 */
static int gw_change_user(DCB* backend,
                          SERVER* server,
                          MXS_SESSION* in_session,
                          GWBUF* queue)
{
    MYSQL_session* current_session = NULL;
    MySQLProtocol* backend_protocol = NULL;
    MySQLProtocol* client_protocol = NULL;
    char username[MYSQL_USER_MAXLEN + 1] = "";
    char database[MYSQL_DATABASE_MAXLEN + 1] = "";
    char current_database[MYSQL_DATABASE_MAXLEN + 1] = "";
    uint8_t client_sha1[MYSQL_SCRAMBLE_LEN] = "";
    uint8_t* client_auth_packet = GWBUF_DATA(queue);
    unsigned int auth_token_len = 0;
    uint8_t* auth_token = NULL;
    int rv = -1;
    int auth_ret = 1;

    current_session = (MYSQL_session*)in_session->client_dcb->data;
    backend_protocol = static_cast<MySQLProtocol*>(backend->protocol);
    client_protocol = static_cast<MySQLProtocol*>(in_session->client_dcb->protocol);

    /* now get the user, after 4 bytes header and 1 byte command */
    client_auth_packet += 5;
    size_t len = strlen((char*)client_auth_packet);
    if (len > MYSQL_USER_MAXLEN)
    {
        MXS_ERROR("Client sent user name \"%s\",which is %lu characters long, "
                  "while a maximum length of %d is allowed. Cutting trailing "
                  "characters.",
                  (char*)client_auth_packet,
                  len,
                  MYSQL_USER_MAXLEN);
    }
    strncpy(username, (char*)client_auth_packet, MYSQL_USER_MAXLEN);
    username[MYSQL_USER_MAXLEN] = 0;

    client_auth_packet += (len + 1);

    /* get the auth token len */
    memcpy(&auth_token_len, client_auth_packet, 1);

    client_auth_packet++;

    /* allocate memory for token only if auth_token_len > 0 */
    if (auth_token_len > 0)
    {
        auth_token = (uint8_t*)MXS_MALLOC(auth_token_len);
        mxb_assert(auth_token != NULL);

        if (auth_token == NULL)
        {
            return rv;
        }
        memcpy(auth_token, client_auth_packet, auth_token_len);
        client_auth_packet += auth_token_len;
    }

    /* get new database name */
    len = strlen((char*)client_auth_packet);
    if (len > MYSQL_DATABASE_MAXLEN)
    {
        MXS_ERROR("Client sent database name \"%s\", which is %lu characters long, "
                  "while a maximum length of %d is allowed. Cutting trailing "
                  "characters.",
                  (char*)client_auth_packet,
                  len,
                  MYSQL_DATABASE_MAXLEN);
    }
    strncpy(database, (char*)client_auth_packet, MYSQL_DATABASE_MAXLEN);
    database[MYSQL_DATABASE_MAXLEN] = 0;

    client_auth_packet += (len + 1);

    if (*client_auth_packet)
    {
        memcpy(&backend_protocol->charset, client_auth_packet, sizeof(int));
    }

    /* save current_database name */
    strcpy(current_database, current_session->db);

    /*
     * Now clear database name in dcb as we don't do local authentication on db name for change user.
     * Local authentication only for user@host and if successful the database name change is sent to backend.
     */
    *current_session->db = 0;

    /*
     * Decode the token and check the password.
     * Note: if auth_token_len == 0 && auth_token == NULL, user is without password
     */
    DCB* dcb = backend->session->client_dcb;

    if (dcb->authfunc.reauthenticate == NULL)
    {
        /** Authenticator does not support reauthentication */
        rv = 0;
        goto retblock;
    }

    auth_ret = dcb->authfunc.reauthenticate(dcb,
                                            username,
                                            auth_token,
                                            auth_token_len,
                                            client_protocol->scramble,
                                            sizeof(client_protocol->scramble),
                                            client_sha1,
                                            sizeof(client_sha1));

    strcpy(current_session->db, current_database);

    if (auth_ret != 0)
    {
        if (service_refresh_users(backend->session->client_dcb->service) == 0)
        {
            /*
             * Try authentication again with new repository data
             * Note: if no auth client authentication will fail
             */
            *current_session->db = 0;

            auth_ret = dcb->authfunc.reauthenticate(dcb,
                                                    username,
                                                    auth_token,
                                                    auth_token_len,
                                                    client_protocol->scramble,
                                                    sizeof(client_protocol->scramble),
                                                    client_sha1,
                                                    sizeof(client_sha1));

            strcpy(current_session->db, current_database);
        }
    }

    MXS_FREE(auth_token);

    if (auth_ret != 0)
    {
        bool password_set = false;
        char* message = NULL;

        if (auth_token_len > 0)
        {
            // If the length of the authentication token is non-0, then
            // it means that the client provided a password.
            password_set = true;
        }

        /**
         * Create an error message and make it look like legit reply
         * from backend server. Then make it look like an incoming event
         * so that thread gets new task of it, calls clientReply
         * which filters out duplicate errors from same cause and forward
         * reply to the client.
         */
        message = create_auth_fail_str(username,
                                       backend->session->client_dcb->remote,
                                       password_set,
                                       NULL,
                                       auth_ret);
        if (message == NULL)
        {
            MXS_ERROR("Creating error message failed.");
            rv = 0;
            goto retblock;
        }

        modutil_reply_auth_error(backend, message, 0);
        rv = 1;
    }
    else
    {
        /** This assumes that authentication will succeed. If authentication fails,
         * the internal session will represent the wrong user. This is wrong and
         * a check whether the COM_CHANGE_USER succeeded should be done in the
         * backend protocol reply handling.
         *
         * For the time being, it is simpler to assume a COM_CHANGE_USER will always
         * succeed if the authentication in MaxScale is successful. In practice this
         * might not be true but these cases are handled by the router modules
         * and the servers that fail to execute the COM_CHANGE_USER are discarded. */
        strcpy(current_session->user, username);
        strcpy(current_session->db, database);
        memcpy(current_session->client_sha1, client_sha1, sizeof(current_session->client_sha1));
        rv = gw_send_change_user_to_backend(database, username, client_sha1, backend_protocol);
    }

retblock:
    gwbuf_free(queue);

    return rv;
}

/**
 * Create COM_CHANGE_USER packet and store it to GWBUF
 *
 * @param mses          MySQL session
 * @param protocol      protocol structure of the backend
 *
 * @return GWBUF buffer consisting of COM_CHANGE_USER packet
 *
 * @note the function doesn't fail
 */
static GWBUF* gw_create_change_user_packet(MYSQL_session* mses,
                                           MySQLProtocol* protocol)
{
    char* db;
    char* user;
    uint8_t* pwd;
    GWBUF* buffer;
    uint8_t* payload = NULL;
    uint8_t* payload_start = NULL;
    long bytes;
    char dbpass[MYSQL_USER_MAXLEN + 1] = "";
    char* curr_db = NULL;
    uint8_t* curr_passwd = NULL;
    unsigned int charset;

    db = mses->db;
    user = mses->user;
    pwd = mses->client_sha1;

    if (strlen(db) > 0)
    {
        curr_db = db;
    }

    if (memcmp(pwd, null_client_sha1, MYSQL_SCRAMBLE_LEN))
    {
        curr_passwd = pwd;
    }

    /* get charset the client sent and use it for connection auth */
    charset = protocol->charset;

    /**
     * Protocol MySQL COM_CHANGE_USER for CLIENT_PROTOCOL_41
     * 1 byte COMMAND
     */
    bytes = 1;

    /** add the user and a terminating char */
    bytes += strlen(user);
    bytes++;
    /**
     * next will be + 1 (scramble_len) + 20 (fixed_scramble) +
     * (db + NULL term) + 2 bytes charset
     */
    if (curr_passwd != NULL)
    {
        bytes += GW_MYSQL_SCRAMBLE_SIZE;
    }
    /** 1 byte for scramble_len */
    bytes++;
    /** db name and terminating char */
    if (curr_db != NULL)
    {
        bytes += strlen(curr_db);
    }
    bytes++;

    /** the charset */
    bytes += 2;
    bytes += strlen("mysql_native_password");
    bytes++;

    /** the packet header */
    bytes += 4;

    buffer = gwbuf_alloc(bytes);

    // The COM_CHANGE_USER is a session command so the result must be collected
    gwbuf_set_type(buffer, GWBUF_TYPE_COLLECT_RESULT);

    payload = GWBUF_DATA(buffer);
    memset(payload, '\0', bytes);
    payload_start = payload;

    /** set packet number to 0 */
    payload[3] = 0x00;
    payload += 4;

    /** set the command COM_CHANGE_USER 0x11 */
    payload[0] = 0x11;
    payload++;
    memcpy(payload, user, strlen(user));
    payload += strlen(user);
    payload++;

    if (curr_passwd != NULL)
    {
        uint8_t hash1[GW_MYSQL_SCRAMBLE_SIZE] = "";
        uint8_t hash2[GW_MYSQL_SCRAMBLE_SIZE] = "";
        uint8_t new_sha[GW_MYSQL_SCRAMBLE_SIZE] = "";
        uint8_t client_scramble[GW_MYSQL_SCRAMBLE_SIZE];

        /** hash1 is the function input, SHA1(real_password) */
        memcpy(hash1, pwd, GW_MYSQL_SCRAMBLE_SIZE);

        /**
         * hash2 is the SHA1(input data), where
         * input_data = SHA1(real_password)
         */
        gw_sha1_str(hash1, GW_MYSQL_SCRAMBLE_SIZE, hash2);

        /** dbpass is the HEX form of SHA1(SHA1(real_password)) */
        gw_bin2hex(dbpass, hash2, GW_MYSQL_SCRAMBLE_SIZE);

        /** new_sha is the SHA1(CONCAT(scramble, hash2) */
        gw_sha1_2_str(protocol->scramble,
                      GW_MYSQL_SCRAMBLE_SIZE,
                      hash2,
                      GW_MYSQL_SCRAMBLE_SIZE,
                      new_sha);

        /** compute the xor in client_scramble */
        gw_str_xor(client_scramble,
                   new_sha,
                   hash1,
                   GW_MYSQL_SCRAMBLE_SIZE);

        /** set the auth-length */
        *payload = GW_MYSQL_SCRAMBLE_SIZE;
        payload++;
        /**
         * copy the 20 bytes scramble data after
         * packet_buffer + 36 + user + NULL + 1 (byte of auth-length)
         */
        memcpy(payload, client_scramble, GW_MYSQL_SCRAMBLE_SIZE);
        payload += GW_MYSQL_SCRAMBLE_SIZE;
    }
    else
    {
        /** skip the auth-length and leave the byte as NULL */
        payload++;
    }
    /** if the db is not NULL append it */
    if (curr_db != NULL)
    {
        memcpy(payload, curr_db, strlen(curr_db));
        payload += strlen(curr_db);
    }
    payload++;
    /** set the charset, 2 bytes */
    *payload = charset;
    payload++;
    *payload = '\x00';
    payload++;
    memcpy(payload, "mysql_native_password", strlen("mysql_native_password"));
    /*
     * Following needed if more to be added
     * payload += strlen("mysql_native_password");
     ** put here the paylod size: bytes to write - 4 bytes packet header
     */
    gw_mysql_set_byte3(payload_start, (bytes - 4));

    return buffer;
}

/**
 * Write a MySQL CHANGE_USER packet to backend server
 *
 * @param conn  MySQL protocol structure
 * @param dbname The selected database
 * @param user The selected user
 * @param passwd The SHA1(real_password)
 * @return 1 on success, 0 on failure
 */
static int gw_send_change_user_to_backend(char* dbname,
                                          char* user,
                                          uint8_t* passwd,
                                          MySQLProtocol* conn)
{
    GWBUF* buffer;
    int rc;
    MYSQL_session* mses;

    mses = (MYSQL_session*)conn->owner_dcb->session->client_dcb->data;
    buffer = gw_create_change_user_packet(mses, conn);
    rc = conn->owner_dcb->func.write(conn->owner_dcb, buffer);

    if (rc != 0)
    {
        conn->changing_user = true;
        rc = 1;
    }

    return rc;
}

/* Send proxy protocol header. See
 * http://www.haproxy.org/download/1.8/doc/proxy-protocol.txt
 * for more information. Currently only supports the text version (v1) of
 * the protocol. Binary version may be added later.
 *
 * @param backend_dcb The target dcb.
 */
static bool gw_send_proxy_protocol_header(DCB* backend_dcb)
{
    // TODO: Add support for chained proxies. Requires reading the client header.

    // The header contains the original client address and the backend server address.
    // Client dbc always exists, as it's only freed at session close.
    const DCB* client_dcb = backend_dcb->session->client_dcb;
    const auto* client_addr = &client_dcb->ip;      // Client address was filled in by accept().

    // Fill in the target server's address.
    const char* server_name = backend_dcb->server->name();
    sockaddr_storage server_addr {};
    socklen_t server_addrlen = sizeof(server_addr);
    int res = getpeername(backend_dcb->fd, (sockaddr*)&server_addr, &server_addrlen);
    if (res != 0)
    {
        int eno = errno;
        MXS_ERROR("getpeername()' failed on connection to '%s' when forming proxy protocol header. "
                  "Error %d: '%s'", server_name, eno, mxb_strerror(eno));
        return false;
    }

    auto client_res = get_ip_string_and_port(client_addr);
    auto server_res = get_ip_string_and_port(&server_addr);

    bool success = false;
    if (client_res.success && server_res.success)
    {
        const auto cli_addr_fam = client_addr->ss_family;
        const auto srv_addr_fam = server_addr.ss_family;
        // The proxy header must contain the client address & port + server address & port. Both should have
        // the same address family. Since the two are separate connections, it's possible one is IPv4 and
        // the other IPv6. In this case, convert any IPv4-addresses to IPv6-format.
        int ret = -1;
        char proxy_header[108] {};      // 108 is the worst-case length
        if ((cli_addr_fam == AF_INET || cli_addr_fam == AF_INET6)
            && (srv_addr_fam == AF_INET || srv_addr_fam == AF_INET6))
        {
            if (cli_addr_fam == srv_addr_fam)
            {
                auto family_str = (cli_addr_fam == AF_INET) ? "TCP4" : "TCP6";
                ret = snprintf(proxy_header, sizeof(proxy_header), "PROXY %s %s %s %d %d\r\n",
                               family_str, client_res.addr, server_res.addr, client_res.port,
                               server_res.port);
            }
            else if (cli_addr_fam == AF_INET)
            {
                // server conn is already ipv6
                ret = snprintf(proxy_header, sizeof(proxy_header), "PROXY TCP6 ::ffff:%s %s %d %d\r\n",
                               client_res.addr, server_res.addr, client_res.port, server_res.port);
            }
            else
            {
                // client conn is already ipv6
                ret = snprintf(proxy_header, sizeof(proxy_header), "PROXY TCP6 %s ::ffff:%s %d %d\r\n",
                               client_res.addr, server_res.addr, client_res.port, server_res.port);
            }
        }
        else
        {
            ret = snprintf(proxy_header, sizeof(proxy_header), "PROXY UNKNOWN\r\n");
        }

        if (ret < 0 || ret >= (int)sizeof(proxy_header))
        {
            MXS_ERROR("Proxy header printing error, produced '%s'.", proxy_header);
        }
        else
        {
            GWBUF* headerbuf = gwbuf_alloc_and_load(strlen(proxy_header), proxy_header);
            if (headerbuf)
            {
                MXS_INFO("Sending proxy-protocol header '%s' to server '%s'.", proxy_header, server_name);
                if (dcb_write(backend_dcb, headerbuf))
                {
                    success = true;
                }
                else
                {
                    gwbuf_free(headerbuf);
                }
            }
        }
    }
    else if (!client_res.success)
    {
        MXS_ERROR("Could not convert network address of '%s@%s' to string form. %s",
                  client_dcb->user, client_dcb->remote, client_res.error_msg.c_str());
    }
    else
    {
        MXS_ERROR("Could not convert network address of server '%s' to string form. %s",
                  server_name, server_res.error_msg.c_str());
    }
    return success;
}

/* Read IP and port from socket address structure, return IP as string and port
 * as host byte order integer.
 *
 * @param sa A sockaddr_storage containing either an IPv4 or v6 address
 * @return Result structure
 */
static AddressInfo get_ip_string_and_port(const sockaddr_storage* sa)
{
    AddressInfo rval;

    const char errmsg_fmt[] = "'inet_ntop' failed. Error: '";
    switch (sa->ss_family)
    {
    case AF_INET:
        {
            const auto* sock_info = (const sockaddr_in*)sa;
            const in_addr* addr = &(sock_info->sin_addr);
            if (inet_ntop(AF_INET, addr, rval.addr, sizeof(rval.addr)))
            {
                rval.port = ntohs(sock_info->sin_port);
                rval.success = true;
            }
            else
            {
                rval.error_msg = std::string(errmsg_fmt) + mxb_strerror(errno) + "'";
            }
        }
        break;

    case AF_INET6:
        {
            const auto* sock_info = (const sockaddr_in6*)sa;
            const in6_addr* addr = &(sock_info->sin6_addr);
            if (inet_ntop(AF_INET6, addr, rval.addr, sizeof(rval.addr)))
            {
                rval.port = ntohs(sock_info->sin6_port);
                rval.success = true;
            }
            else
            {
                rval.error_msg = std::string(errmsg_fmt) + mxb_strerror(errno) + "'";
            }
        }
        break;

    default:
        {
            rval.error_msg = "Unrecognized socket address family " + std::to_string(sa->ss_family) + ".";
        }
    }

    return rval;
}

static bool gw_connection_established(DCB* dcb)
{
    MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol;
    return proto->protocol_auth_state == MXS_AUTH_STATE_COMPLETE
           && (proto->ignore_replies == 0)
           && !proto->stored_query;
}

static bool gw_auth_is_complete(DCB* dcb)
{
    MySQLProtocol* proto = (MySQLProtocol*)dcb->protocol;

    switch (proto->protocol_auth_state)
    {
    case MXS_AUTH_STATE_FAILED:
    case MXS_AUTH_STATE_HANDSHAKE_FAILED:
    case MXS_AUTH_STATE_COMPLETE:
        MXS_DEBUG("(%lu) Auth is complete for DCB %lu", dcb->session->ses_id, dcb->m_uid);
        return true;

    default:
        MXS_DEBUG("(%lu) Auth not yet complete for DCB %lu", dcb->session->ses_id, dcb->m_uid);
        return false;
    }
}

json_t* gw_json_diagnostics(DCB* dcb)
{
    MySQLProtocol* proto = static_cast<MySQLProtocol*>(dcb->protocol);
    json_t* obj = json_object();
    json_object_set_new(obj, "connection_id", json_integer(proto->thread_id));
    return obj;
}
