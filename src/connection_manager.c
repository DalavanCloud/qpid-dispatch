/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <qpid/dispatch/connection_manager.h>
#include <qpid/dispatch/ctools.h>
#include <qpid/dispatch/threading.h>
#include <qpid/dispatch/atomic.h>
#include <qpid/dispatch/failoverlist.h>
#include <proton/listener.h>
#include "dispatch_private.h"
#include "connection_manager_private.h"
#include "server_private.h"
#include "entity.h"
#include "entity_cache.h"
#include "schema_enum.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

struct qd_config_ssl_profile_t {
    DEQ_LINKS(qd_config_ssl_profile_t);
    char        *name;
    char        *ssl_password;
    char        *ssl_trusted_certificate_db;
    char        *ssl_trusted_certificates;
    char        *ssl_uid_format;
    char        *uid_name_mapping_file;
    char        *ssl_certificate_file;
    char        *ssl_private_key_file;
    char        *ssl_ciphers;
    char        *ssl_protocols;
};

DEQ_DECLARE(qd_config_ssl_profile_t, qd_config_ssl_profile_list_t);

struct qd_config_sasl_plugin_t {
    DEQ_LINKS(qd_config_sasl_plugin_t);
    char        *name;
    char        *auth_service;
    char        *sasl_init_hostname;
    char        *auth_ssl_profile;
};

DEQ_DECLARE(qd_config_sasl_plugin_t, qd_config_sasl_plugin_list_t);

struct qd_connection_manager_t {
    qd_log_source_t              *log_source;
    qd_server_t                  *server;
    qd_listener_list_t            listeners;
    qd_connector_list_t           connectors;
    qd_config_ssl_profile_list_t  config_ssl_profiles;
    qd_config_sasl_plugin_list_t  config_sasl_plugins;
};

const char *qd_log_message_components[] =
    {"message-id",
     "user-id",
     "to",
     "subject",
     "reply-to",
     "correlation-id",
     "content-type",
     "content-encoding",
     "absolute-expiry-time",
     "creation-time",
     "group-id",
     "group-sequence",
     "reply-to-group-id",
     "app-properties",
     0};

const char *ALL = "all";
const char *NONE = "none";

/**
 * Search the list of config_ssl_profiles for an ssl-profile that matches the passed in name
 */
static qd_config_ssl_profile_t *qd_find_ssl_profile(qd_connection_manager_t *cm, char *name)
{
    qd_config_ssl_profile_t *ssl_profile = DEQ_HEAD(cm->config_ssl_profiles);
    while (ssl_profile) {
        if (strcmp(ssl_profile->name, name) == 0)
            return ssl_profile;
        ssl_profile = DEQ_NEXT(ssl_profile);
    }

    return 0;
}

/**
 * Search the list of config_sasl_plugins for an sasl-profile that matches the passed in name
 */
static qd_config_sasl_plugin_t *qd_find_sasl_plugin(qd_connection_manager_t *cm, char *name)
{
    qd_config_sasl_plugin_t *sasl_plugin = DEQ_HEAD(cm->config_sasl_plugins);
    while (sasl_plugin) {
        if (strcmp(sasl_plugin->name, name) == 0)
            return sasl_plugin;
        sasl_plugin = DEQ_NEXT(sasl_plugin);
    }

    return 0;
}

void qd_server_config_free(qd_server_config_t *cf)
{
    if (!cf) return;
    free(cf->host);
    free(cf->port);
    free(cf->host_port);
    free(cf->role);
    if (cf->http_root_dir)   free(cf->http_root_dir);
    if (cf->name)            free(cf->name);
    if (cf->protocol_family) free(cf->protocol_family);
    if (cf->sasl_username)   free(cf->sasl_username);
    if (cf->sasl_password)   free(cf->sasl_password);
    if (cf->sasl_mechanisms) free(cf->sasl_mechanisms);
    if (cf->ssl_profile)     free(cf->ssl_profile);
    if (cf->failover_list)   qd_failover_list_free(cf->failover_list);
    if (cf->log_message)     free(cf->log_message);

    if (cf->ssl_certificate_file)       free(cf->ssl_certificate_file);
    if (cf->ssl_private_key_file)       free(cf->ssl_private_key_file);
    if (cf->ssl_ciphers)                free(cf->ssl_ciphers);
    if (cf->ssl_protocols)              free(cf->ssl_protocols);
    if (cf->ssl_password)               free(cf->ssl_password);
    if (cf->ssl_trusted_certificate_db) free(cf->ssl_trusted_certificate_db);
    if (cf->ssl_trusted_certificates)   free(cf->ssl_trusted_certificates);
    if (cf->ssl_uid_format)             free(cf->ssl_uid_format);
    if (cf->ssl_uid_name_mapping_file)  free(cf->ssl_uid_name_mapping_file);

    if (cf->sasl_plugin_config.auth_service)               free(cf->sasl_plugin_config.auth_service);
    if (cf->sasl_plugin_config.sasl_init_hostname)         free(cf->sasl_plugin_config.sasl_init_hostname);
    if (cf->sasl_plugin_config.ssl_certificate_file)       free(cf->sasl_plugin_config.ssl_certificate_file);
    if (cf->sasl_plugin_config.ssl_private_key_file)       free(cf->sasl_plugin_config.ssl_private_key_file);
    if (cf->sasl_plugin_config.ssl_ciphers)                free(cf->sasl_plugin_config.ssl_ciphers);
    if (cf->sasl_plugin_config.ssl_protocols)              free(cf->sasl_plugin_config.ssl_protocols);
    if (cf->sasl_plugin_config.ssl_password)               free(cf->sasl_plugin_config.ssl_password);
    if (cf->sasl_plugin_config.ssl_trusted_certificate_db) free(cf->sasl_plugin_config.ssl_trusted_certificate_db);
    if (cf->sasl_plugin_config.ssl_trusted_certificates)   free(cf->sasl_plugin_config.ssl_trusted_certificates);
    if (cf->sasl_plugin_config.ssl_uid_format)             free(cf->sasl_plugin_config.ssl_uid_format);
    if (cf->sasl_plugin_config.ssl_uid_name_mapping_file)  free(cf->sasl_plugin_config.ssl_uid_name_mapping_file);

    memset(cf, 0, sizeof(*cf));
}

#define CHECK() if (qd_error_code()) goto error
#define SSTRDUP(S) ((S) ? strdup(S) : NULL)

/**
 * Private function to set the values of booleans strip_inbound_annotations and strip_outbound_annotations
 * based on the corresponding values for the settings in qdrouter.json
 * strip_inbound_annotations and strip_outbound_annotations are defaulted to true
 */
static void load_strip_annotations(qd_server_config_t *config, const char* stripAnnotations)
{
    if (stripAnnotations) {
    	if      (strcmp(stripAnnotations, "both") == 0) {
    		config->strip_inbound_annotations  = true;
    		config->strip_outbound_annotations = true;
    	}
    	else if (strcmp(stripAnnotations, "in") == 0) {
    		config->strip_inbound_annotations  = true;
    		config->strip_outbound_annotations = false;
    	}
    	else if (strcmp(stripAnnotations, "out") == 0) {
    		config->strip_inbound_annotations  = false;
    		config->strip_outbound_annotations = true;
    	}
    	else if (strcmp(stripAnnotations, "no") == 0) {
    		config->strip_inbound_annotations  = false;
    		config->strip_outbound_annotations = false;
    	}
    }
    else {
    	assert(stripAnnotations);
    	//This is just for safety. Default to stripInboundAnnotations and stripOutboundAnnotations to true (to "both").
		config->strip_inbound_annotations  = true;
		config->strip_outbound_annotations = true;
    }
}

/**
 * Since both the host and the addr have defaults of 127.0.0.1, we will have to use the non-default wherever it is available.
 */
static void set_config_host(qd_server_config_t *config, qd_entity_t* entity)
{
    config->host = qd_entity_opt_string(entity, "host", 0);

    assert(config->host);

    int hplen = strlen(config->host) + strlen(config->port) + 2;
    config->host_port = malloc(hplen);
    snprintf(config->host_port, hplen, "%s:%s", config->host, config->port);
}

static void qd_config_ssl_profile_process_password(qd_config_ssl_profile_t* ssl_profile)
{
    char *pw = ssl_profile->ssl_password;
    if (!pw)
        return;

    //
    // If the "password" starts with "env:" then the remaining
    // text is the environment variable that contains the password
    //
    if (strncmp(pw, "env:", 4) == 0) {
        char *env = pw + 4;
        // skip the leading whitespace if it is there
        while (*env == ' ') ++env;

        const char* passwd = getenv(env);
        if (passwd) {
            //
            // Replace the allocated directive with the looked-up password
            //
            free(ssl_profile->ssl_password);
            ssl_profile->ssl_password = strdup(passwd);
        } else {
            qd_error(QD_ERROR_NOT_FOUND, "Failed to find a password in the environment variable");
        }
    }

    //
    // If the "password" starts with "literal:" then
    // the remaining text is the password and the heading should be
    // stripped off
    //
    else if (strncmp(pw, "literal:", 8) == 0) {
        // skip the "literal:" header
        pw += 8;

        // skip the whitespace if it is there
        while (*pw == ' ') ++pw;

        // Replace the password with a copy of the string after "literal:"
        char *copy = strdup(pw);
        free(ssl_profile->ssl_password);
        ssl_profile->ssl_password = copy;
    }
}

static qd_log_bits populate_log_message(const qd_server_config_t *config)
{
    //May have to copy this string since strtok modifies original string.
    char *log_message = config->log_message;

    int32_t ret_val = 0;

    if (!log_message || strcmp(log_message, NONE) == 0)
        return ret_val;

    //If log_message is set to 'all', turn on all bits.
    if (strcmp(log_message, ALL) == 0)
        return INT32_MAX;

    char *delim = ",";

    /* get the first token */
    char *token = strtok(log_message, delim);

    const char *component = 0;

    /* walk through other tokens */
    while( token != NULL ) {
       for (int i=0;; i++) {
           component = qd_log_message_components[i];
           if (component == 0)
               break;

           if (strcmp(component, token) == 0) {
                   ret_val |= 1 << i;
           }
       }
       token = strtok(NULL, delim);
    }

    return ret_val;
}


static qd_error_t load_server_config(qd_dispatch_t *qd, qd_server_config_t *config, qd_entity_t* entity, bool is_listener)
{
    qd_error_clear();

    bool authenticatePeer   = qd_entity_opt_bool(entity, "authenticatePeer",  false);    CHECK();
    bool verifyHostName     = qd_entity_opt_bool(entity, "verifyHostname",    true);     CHECK();
    bool requireEncryption  = qd_entity_opt_bool(entity, "requireEncryption", false);    CHECK();
    bool requireSsl         = qd_entity_opt_bool(entity, "requireSsl",        false);    CHECK();

    memset(config, 0, sizeof(*config));
    config->log_message          = qd_entity_opt_string(entity, "messageLoggingComponents", 0);     CHECK();
    config->log_bits             = populate_log_message(config);
    config->port                 = qd_entity_get_string(entity, "port");              CHECK();
    config->name                 = qd_entity_opt_string(entity, "name", 0);           CHECK();
    config->role                 = qd_entity_get_string(entity, "role");              CHECK();
    config->inter_router_cost    = qd_entity_opt_long(entity, "cost", 1);             CHECK();
    config->protocol_family      = qd_entity_opt_string(entity, "protocolFamily", 0); CHECK();
    config->metrics              = qd_entity_opt_bool(entity, "metrics", true);       CHECK();
    config->http                 = qd_entity_opt_bool(entity, "http", false);         CHECK();
    config->http_root_dir        = qd_entity_opt_string(entity, "httpRootDir", false);   CHECK();

    // Because of a potential conflict when console is installed or not installed,
    // we now want to require that the config file explicitly specify HTTP root
    // if it wants full HTTP service. If it does not specify HTTP root, it will
    // be useful for AMQP-over-websockets, but will not serve any static content.
    config->http = config->http || config->http_root_dir;
    if (config->http && ! config->http_root_dir) {
        qd_log(qd->connection_manager->log_source, QD_LOG_INFO, "HTTP service is requested but no httpRootDir specified. The router will serve AMQP-over-websockets but no static content.");
    }
    if (config->metrics && !config->http) {
        qd_log(qd->connection_manager->log_source, QD_LOG_INFO, "Metrics can only be exported on listener with http enabled.");
    }

    config->max_frame_size       = qd_entity_get_long(entity, "maxFrameSize");        CHECK();
    config->max_sessions         = qd_entity_get_long(entity, "maxSessions");         CHECK();
    uint64_t ssn_frames          = qd_entity_opt_long(entity, "maxSessionFrames", 0); CHECK();
    config->idle_timeout_seconds = qd_entity_get_long(entity, "idleTimeoutSeconds");  CHECK();
    if (is_listener) {
        config->initial_handshake_timeout_seconds = qd_entity_get_long(entity, "initialHandshakeTimeoutSeconds");  CHECK();
    }
    config->sasl_username        = qd_entity_opt_string(entity, "saslUsername", 0);   CHECK();
    config->sasl_password        = qd_entity_opt_string(entity, "saslPassword", 0);   CHECK();
    config->sasl_mechanisms      = qd_entity_opt_string(entity, "saslMechanisms", 0); CHECK();
    config->ssl_profile          = qd_entity_opt_string(entity, "sslProfile", 0);     CHECK();
    config->sasl_plugin          = qd_entity_opt_string(entity, "saslPlugin", 0);   CHECK();
    config->link_capacity        = qd_entity_opt_long(entity, "linkCapacity", 0);     CHECK();
    config->multi_tenant         = qd_entity_opt_bool(entity, "multiTenant", false);  CHECK();
    set_config_host(config, entity);

    //
    // Handle the defaults for various settings
    //
    if (config->link_capacity == 0)
        config->link_capacity = 250;

    if (config->max_sessions == 0 || config->max_sessions > 32768)
        // Proton disallows > 32768
        config->max_sessions = 32768;

    if (config->max_frame_size < QD_AMQP_MIN_MAX_FRAME_SIZE)
        // Silently promote the minimum max-frame-size
        // Proton will do this but the number is needed for the
        // incoming capacity calculation.
        config->max_frame_size = QD_AMQP_MIN_MAX_FRAME_SIZE;

    //
    // Given session frame count and max frame size compute session incoming_capacity
    //
    if (ssn_frames == 0)
        config->incoming_capacity = (sizeof(size_t) < 8) ? 0x7FFFFFFFLL : 0x7FFFFFFFLL * config->max_frame_size;
    else {
        uint64_t mfs      = (uint64_t) config->max_frame_size;
        uint64_t trial_ic = ssn_frames * mfs;
        uint64_t limit    = (sizeof(size_t) < 8) ? (1ll << 31) - 1 : 0;
        if (limit == 0 || trial_ic < limit) {
            // Silently promote incoming capacity of zero to one
            config->incoming_capacity = 
                (trial_ic < QD_AMQP_MIN_MAX_FRAME_SIZE ? QD_AMQP_MIN_MAX_FRAME_SIZE : trial_ic);
        } else {
            config->incoming_capacity = limit;
            uint64_t computed_ssn_frames = limit / mfs;
            qd_log(qd->connection_manager->log_source, QD_LOG_WARNING,
                   "Server configuation for I/O adapter entity name:'%s', host:'%s', port:'%s', "
                   "requested maxSessionFrames truncated from %"PRId64" to %"PRId64,
                   config->name, config->host, config->port, ssn_frames, computed_ssn_frames);
        }
    }

    //
    // For now we are hardwiring this attribute to true.  If there's an outcry from the
    // user community, we can revisit this later.
    //
    config->allowInsecureAuthentication = true;
    config->verify_host_name = verifyHostName;

    char *stripAnnotations  = qd_entity_opt_string(entity, "stripAnnotations", 0);
    load_strip_annotations(config, stripAnnotations);
    free(stripAnnotations);
    stripAnnotations = 0;
    CHECK();

    config->requireAuthentication = authenticatePeer;
    config->requireEncryption     = requireEncryption || requireSsl;

    if (config->ssl_profile) {
        config->ssl_required = requireSsl;
        config->ssl_require_peer_authentication = config->sasl_mechanisms &&
            strstr(config->sasl_mechanisms, "EXTERNAL") != 0;

        qd_config_ssl_profile_t *ssl_profile =
            qd_find_ssl_profile(qd->connection_manager, config->ssl_profile);
        if (ssl_profile) {
            config->ssl_certificate_file = SSTRDUP(ssl_profile->ssl_certificate_file);
            config->ssl_private_key_file = SSTRDUP(ssl_profile->ssl_private_key_file);
            config->ssl_ciphers = SSTRDUP(ssl_profile->ssl_ciphers);
            config->ssl_protocols = SSTRDUP(ssl_profile->ssl_protocols);
            config->ssl_password = SSTRDUP(ssl_profile->ssl_password);
            config->ssl_trusted_certificate_db = SSTRDUP(ssl_profile->ssl_trusted_certificate_db);
            config->ssl_trusted_certificates = SSTRDUP(ssl_profile->ssl_trusted_certificates);
            config->ssl_uid_format = SSTRDUP(ssl_profile->ssl_uid_format);
            config->ssl_uid_name_mapping_file = SSTRDUP(ssl_profile->uid_name_mapping_file);
        }
    }

    if (config->sasl_plugin) {
        qd_config_sasl_plugin_t *sasl_plugin =
            qd_find_sasl_plugin(qd->connection_manager, config->sasl_plugin);
        if (sasl_plugin) {
            config->sasl_plugin_config.auth_service = SSTRDUP(sasl_plugin->auth_service);
            config->sasl_plugin_config.sasl_init_hostname = SSTRDUP(sasl_plugin->sasl_init_hostname);
            qd_log(qd->connection_manager->log_source, QD_LOG_INFO, "Using auth service %s from  SASL Plugin %s", config->sasl_plugin_config.auth_service, config->sasl_plugin);

            if (sasl_plugin->auth_ssl_profile) {
                config->sasl_plugin_config.use_ssl = true;
                qd_config_ssl_profile_t *auth_ssl_profile =
                    qd_find_ssl_profile(qd->connection_manager, sasl_plugin->auth_ssl_profile);

                config->sasl_plugin_config.ssl_certificate_file = SSTRDUP(auth_ssl_profile->ssl_certificate_file);
                config->sasl_plugin_config.ssl_private_key_file = SSTRDUP(auth_ssl_profile->ssl_private_key_file);
                config->sasl_plugin_config.ssl_ciphers = SSTRDUP(auth_ssl_profile->ssl_ciphers);
                config->sasl_plugin_config.ssl_protocols = SSTRDUP(auth_ssl_profile->ssl_protocols);
                config->sasl_plugin_config.ssl_password = SSTRDUP(auth_ssl_profile->ssl_password);
                config->sasl_plugin_config.ssl_trusted_certificate_db = SSTRDUP(auth_ssl_profile->ssl_trusted_certificate_db);
                config->sasl_plugin_config.ssl_trusted_certificates = SSTRDUP(auth_ssl_profile->ssl_trusted_certificates);
                config->sasl_plugin_config.ssl_uid_format = SSTRDUP(auth_ssl_profile->ssl_uid_format);
                config->sasl_plugin_config.ssl_uid_name_mapping_file = SSTRDUP(auth_ssl_profile->uid_name_mapping_file);
            } else {
                config->sasl_plugin_config.use_ssl = false;
            }
        } else {
            qd_error(QD_ERROR_RUNTIME, "Cannot find sasl plugin %s", config->sasl_plugin); CHECK();
        }
    }

    return QD_ERROR_NONE;

  error:
    qd_server_config_free(config);
    return qd_error_code();
}


bool is_log_component_enabled(qd_log_bits log_message, const char *component_name) {

    for(int i=0;;i++) {
        const char *component = qd_log_message_components[i];
        if (component == 0)
            break;
        if (strcmp(component_name, component) == 0)
            return (log_message >> i) & 1;
    }

    return 0;
}


static bool config_ssl_profile_free(qd_connection_manager_t *cm, qd_config_ssl_profile_t *ssl_profile)
{
    DEQ_REMOVE(cm->config_ssl_profiles, ssl_profile);

    free(ssl_profile->name);
    free(ssl_profile->ssl_password);
    free(ssl_profile->ssl_trusted_certificate_db);
    free(ssl_profile->ssl_trusted_certificates);
    free(ssl_profile->ssl_uid_format);
    free(ssl_profile->uid_name_mapping_file);
    free(ssl_profile->ssl_certificate_file);
    free(ssl_profile->ssl_private_key_file);
    free(ssl_profile->ssl_ciphers);
    free(ssl_profile->ssl_protocols);
    free(ssl_profile);
    return true;

}

static bool config_sasl_plugin_free(qd_connection_manager_t *cm, qd_config_sasl_plugin_t *sasl_plugin)
{
    DEQ_REMOVE(cm->config_sasl_plugins, sasl_plugin);

    free(sasl_plugin->name);
    free(sasl_plugin->auth_service);
    free(sasl_plugin->sasl_init_hostname);
    free(sasl_plugin->auth_ssl_profile);
    free(sasl_plugin);
    return true;

}


qd_config_ssl_profile_t *qd_dispatch_configure_ssl_profile(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_error_clear();
    qd_connection_manager_t *cm = qd->connection_manager;

    qd_config_ssl_profile_t *ssl_profile = NEW(qd_config_ssl_profile_t);
    DEQ_ITEM_INIT(ssl_profile);
    DEQ_INSERT_TAIL(cm->config_ssl_profiles, ssl_profile);
    ssl_profile->name                       = qd_entity_opt_string(entity, "name", 0); CHECK();
    ssl_profile->ssl_certificate_file       = qd_entity_opt_string(entity, "certFile", 0); CHECK();
    ssl_profile->ssl_private_key_file       = qd_entity_opt_string(entity, "privateKeyFile", 0); CHECK();
    ssl_profile->ssl_password               = qd_entity_opt_string(entity, "password", 0); CHECK();

    if (ssl_profile->ssl_password) {
        qd_log(cm->log_source, QD_LOG_WARNING, "Attribute password of entity sslProfile has been deprecated. Use passwordFile instead.");
    }


    if (!ssl_profile->ssl_password) {
        // SSL password not provided. Check if passwordFile property is specified.
        char *password_file = qd_entity_opt_string(entity, "passwordFile", 0); CHECK();

        if (password_file) {
            FILE *file = fopen(password_file, "r");

            if (file) {
                char buffer[200];

                int c;
                int i=0;

                while (i < 200 - 1) {
                    c = fgetc(file);
                    if (c == EOF || c == '\n')
                        break;
                    buffer[i++] = c;
                }

                if (i != 0) {
                    buffer[i] = '\0';
                    free(ssl_profile->ssl_password);
                    ssl_profile->ssl_password = strdup(buffer);
                }
                fclose(file);
            }
        }
        free(password_file);
    }
    ssl_profile->ssl_ciphers   = qd_entity_opt_string(entity, "ciphers", 0);                   CHECK();
    ssl_profile->ssl_protocols = qd_entity_opt_string(entity, "protocols", 0);                 CHECK();
    ssl_profile->ssl_trusted_certificate_db = qd_entity_opt_string(entity, "caCertFile", 0);   CHECK();
    ssl_profile->ssl_trusted_certificates   = qd_entity_opt_string(entity, "trustedCertsFile", 0);   CHECK();
    ssl_profile->ssl_uid_format             = qd_entity_opt_string(entity, "uidFormat", 0);          CHECK();
    ssl_profile->uid_name_mapping_file      = qd_entity_opt_string(entity, "uidNameMappingFile", 0); CHECK();

    //
    // Process the password to handle any modifications or lookups needed
    //
    qd_config_ssl_profile_process_password(ssl_profile); CHECK();

    qd_log(cm->log_source, QD_LOG_INFO, "Created SSL Profile with name %s ", ssl_profile->name);
    return ssl_profile;

    error:
        qd_log(cm->log_source, QD_LOG_ERROR, "Unable to create ssl profile: %s", qd_error_message());
        config_ssl_profile_free(cm, ssl_profile);
        return 0;
}

qd_config_sasl_plugin_t *qd_dispatch_configure_sasl_plugin(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_error_clear();
    qd_connection_manager_t *cm = qd->connection_manager;

    qd_config_sasl_plugin_t *sasl_plugin = NEW(qd_config_sasl_plugin_t);
    ZERO(sasl_plugin);
    DEQ_ITEM_INIT(sasl_plugin);
    DEQ_INSERT_TAIL(cm->config_sasl_plugins, sasl_plugin);
    sasl_plugin->name                       = qd_entity_opt_string(entity, "name", 0); CHECK();

    char *auth_host = qd_entity_opt_string(entity, "host", 0);
    char *auth_port = qd_entity_opt_string(entity, "port", 0);

    if (auth_host && auth_port) {
        int strlen_auth_host = strlen(auth_host);
        int strlen_auth_port = strlen(auth_port);

        if (strlen_auth_host > 0 && strlen_auth_port > 0) {

            int hplen = strlen(auth_host) + strlen(auth_port) + 2;
            if (hplen > 2) {
                sasl_plugin->auth_service = malloc(hplen);
                snprintf(sasl_plugin->auth_service, hplen, "%s:%s", auth_host, auth_port);
            }
        }
    }

    free(auth_host);
    free(auth_port);

    if (!sasl_plugin->auth_service) {
        sasl_plugin->auth_service               = qd_entity_opt_string(entity, "authService", 0); CHECK();
        qd_log(cm->log_source, QD_LOG_WARNING, "Attribute authService of entity authServicePlugin has been deprecated. Use host and port instead.");
    }

    sasl_plugin->sasl_init_hostname         = qd_entity_opt_string(entity, "realm", 0); CHECK();
    sasl_plugin->auth_ssl_profile           = qd_entity_opt_string(entity, "sslProfile", 0); CHECK();

    qd_log(cm->log_source, QD_LOG_INFO, "Created SASL plugin config with name %s", sasl_plugin->name);
    return sasl_plugin;

    error:
        qd_log(cm->log_source, QD_LOG_ERROR, "Unable to create SASL plugin config: %s", qd_error_message());
        config_sasl_plugin_free(cm, sasl_plugin);
        return 0;
}

static void log_config(qd_log_source_t *log, qd_server_config_t *c, const char *what) {
    qd_log(log, QD_LOG_INFO, "Configured %s: %s proto=%s, role=%s%s%s%s",
           what, c->host_port, c->protocol_family ? c->protocol_family : "any",
           c->role,
           c->http ? ", http" : "",
           c->ssl_profile ? ", sslProfile=":"",
           c->ssl_profile ? c->ssl_profile:"");
}


qd_listener_t *qd_dispatch_configure_listener(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_connection_manager_t *cm = qd->connection_manager;
    qd_listener_t *li = qd_server_listener(qd->server);
    if (!li || load_server_config(qd, &li->config, entity, true) != QD_ERROR_NONE) {
        qd_log(cm->log_source, QD_LOG_ERROR, "Unable to create listener: %s", qd_error_message());
        qd_listener_decref(li);
        return 0;
    }
    char *fol = qd_entity_opt_string(entity, "failoverUrls", 0);
    if (fol) {
        li->config.failover_list = qd_failover_list(fol);
        free(fol);
        if (li->config.failover_list == 0) {
            qd_log(cm->log_source, QD_LOG_ERROR, "Unable to create listener, bad failover list: %s",
                   qd_error_message());
            qd_listener_decref(li);
            return 0;
        }
    } else {
        li->config.failover_list = 0;
    }
    DEQ_ITEM_INIT(li);
    DEQ_INSERT_TAIL(cm->listeners, li);
    log_config(cm->log_source, &li->config, "Listener");
    return li;
}


qd_error_t qd_entity_refresh_listener(qd_entity_t* entity, void *impl)
{
    return QD_ERROR_NONE;
}


/**
 * Calculates the total length of the failover  list string.
 * For example, the failover list string can look like this - "amqp://0.0.0.0:62616, amqp://0.0.0.0:61616"
 * This function calculates the length of the above string by adding up the scheme (amqp or qmqps) and host_port for each failover item.
 * It also assumes that there will be a comma and a space between each failover item.
 *
 */
static int get_failover_info_length(qd_failover_item_list_t   conn_info_list)
{
    int arr_length = 0;
    qd_failover_item_t *item = DEQ_HEAD(conn_info_list);

    while(item) {
        if (item->scheme) {
            // The +3 is for the '://'
            arr_length += strlen(item->scheme) + 3;
        }
        if (item->host_port) {
            arr_length += strlen(item->host_port);
        }
        item = DEQ_NEXT(item);
        if (item) {
            // This is for the comma and space between the items
            arr_length += 2;
        }
    }

    if (arr_length > 0)
        // This is for the final '\0'
        arr_length += 1;

    return arr_length;
}

/**
 *
 * Creates a failover url list. This comma separated failover list shows a list of urls that the router will attempt
 * to connect to in case the primary connection fails. The router will attempt these failover connections to urls in
 * the order that they appear in the list.
 *
 */
qd_error_t qd_entity_refresh_connector(qd_entity_t* entity, void *impl)
{
    qd_connector_t *ct = (qd_connector_t*) impl;

    int conn_index = ct->conn_index;

    int i = 1;
    int num_items = 0;

    qd_failover_item_list_t   conn_info_list = ct->conn_info_list;

    int conn_info_len = DEQ_SIZE(conn_info_list);

    qd_failover_item_t *item = DEQ_HEAD(conn_info_list);

    int arr_length = get_failover_info_length(conn_info_list);

    // This is the string that will contain the comma separated failover list
    char failover_info[arr_length];

    memset(failover_info, 0, sizeof(failover_info));

    while(item) {

        // Break out of the loop when we have hit all items in the list.
        if (num_items >= conn_info_len)
            break;

        if (num_items >= 1) {
            strcat(failover_info, ", ");
        }

        // We need to go to the elements in the list to get to the
        // element that matches the connection index. This is the first
        // url that the router will try to connect on ffailover.
        if (conn_index == i) {
            num_items += 1;
            if (item->scheme) {
                strcat(failover_info, item->scheme);
                strcat(failover_info, "://");
            }
            if (item->host_port) {
                strcat(failover_info, item->host_port);
            }
        }
        else {
            if (num_items > 0) {
                num_items += 1;
                if (item->scheme) {
                    strcat(failover_info, item->scheme);
                    strcat(failover_info, "://");
                }
                if (item->host_port) {
                    strcat(failover_info, item->host_port);
                }
            }
        }

        i += 1;

        item = DEQ_NEXT(item);
        if (item == 0)
            item = DEQ_HEAD(conn_info_list);
    }

    if (qd_entity_set_string(entity, "failoverUrls", failover_info) == 0)
        return QD_ERROR_NONE;

    return qd_error_code();
}


qd_connector_t *qd_dispatch_configure_connector(qd_dispatch_t *qd, qd_entity_t *entity)
{
    qd_connection_manager_t *cm = qd->connection_manager;
    qd_connector_t *ct = qd_server_connector(qd->server);
    if (ct && load_server_config(qd, &ct->config, entity, false) == QD_ERROR_NONE) {
        DEQ_ITEM_INIT(ct);
        DEQ_INSERT_TAIL(cm->connectors, ct);
        log_config(cm->log_source, &ct->config, "Connector");

        //
        // Add the first item to the ct->conn_info_list
        // The initial connection information and any backup connection information is stored in the conn_info_list
        //
        qd_failover_item_t *item = NEW(qd_failover_item_t);
        ZERO(item);
        if (ct->config.ssl_required)
            item->scheme   = strdup("amqps");
        else
            item->scheme   = strdup("amqp");

        item->host     = strdup(ct->config.host);
        item->port     = strdup(ct->config.port);

        int hplen = strlen(item->host) + strlen(item->port) + 2;
        item->host_port = malloc(hplen);
        snprintf(item->host_port, hplen, "%s:%s", item->host , item->port);

        DEQ_INSERT_TAIL(ct->conn_info_list, item);

        return ct;
    }
    qd_log(cm->log_source, QD_LOG_ERROR, "Unable to create connector: %s", qd_error_message());
    qd_connector_decref(ct);
    return 0;
}


qd_connection_manager_t *qd_connection_manager(qd_dispatch_t *qd)
{
    qd_connection_manager_t *cm = NEW(qd_connection_manager_t);
    if (!cm)
        return 0;

    cm->log_source = qd_log_source("CONN_MGR");
    cm->server     = qd->server;
    DEQ_INIT(cm->listeners);
    DEQ_INIT(cm->connectors);
    DEQ_INIT(cm->config_ssl_profiles);
    DEQ_INIT(cm->config_sasl_plugins);

    return cm;
}


void qd_connection_manager_free(qd_connection_manager_t *cm)
{
    if (!cm) return;
    qd_listener_t *li = DEQ_HEAD(cm->listeners);
    while (li) {
        DEQ_REMOVE_HEAD(cm->listeners);
        qd_listener_decref(li);
        li = DEQ_HEAD(cm->listeners);
    }

    qd_connector_t *c = DEQ_HEAD(cm->connectors);
    while (c) {
        DEQ_REMOVE_HEAD(cm->connectors);
        qd_connector_decref(c);
        c = DEQ_HEAD(cm->connectors);
    }

    qd_config_ssl_profile_t *sslp = DEQ_HEAD(cm->config_ssl_profiles);
    while (sslp) {
        config_ssl_profile_free(cm, sslp);
        sslp = DEQ_HEAD(cm->config_ssl_profiles);
    }

    qd_config_sasl_plugin_t *saslp = DEQ_HEAD(cm->config_sasl_plugins);
    while (saslp) {
        config_sasl_plugin_free(cm, saslp);
        saslp = DEQ_HEAD(cm->config_sasl_plugins);
    }
}


/** NOTE: non-static qd_connection_manager_* functions are called from the python agent */


void qd_connection_manager_start(qd_dispatch_t *qd)
{
    static bool first_start = true;
    qd_listener_t  *li = DEQ_HEAD(qd->connection_manager->listeners);
    qd_connector_t *ct = DEQ_HEAD(qd->connection_manager->connectors);

    while (li) {
        if (!li->pn_listener) {
            if (!qd_listener_listen(li) && first_start) {
                qd_log(qd->connection_manager->log_source, QD_LOG_CRITICAL,
                       "Listen on %s failed during initial config", li->config.host_port);
                exit(1);
            } else {
                li->exit_on_error = first_start;
            }
        }
        li = DEQ_NEXT(li);
    }

    while (ct) {

        if (ct->state == CXTR_STATE_OPEN || ct->state == CXTR_STATE_CONNECTING) {
            ct = DEQ_NEXT(ct);
            continue;
        }

        qd_connector_connect(ct);
        ct = DEQ_NEXT(ct);
    }

    first_start = false;
}


void qd_connection_manager_delete_listener(qd_dispatch_t *qd, void *impl)
{
    qd_listener_t *li = (qd_listener_t*) impl;
    if (li) {
        if (li->pn_listener) {
            pn_listener_close(li->pn_listener);
        }
        DEQ_REMOVE(qd->connection_manager->listeners, li);
        qd_listener_decref(li);
    }
}


void qd_connection_manager_delete_ssl_profile(qd_dispatch_t *qd, void *impl)
{
    qd_config_ssl_profile_t *ssl_profile = (qd_config_ssl_profile_t*) impl;
    config_ssl_profile_free(qd->connection_manager, ssl_profile);
}

void qd_connection_manager_delete_sasl_plugin(qd_dispatch_t *qd, void *impl)
{
    qd_config_sasl_plugin_t *sasl_plugin = (qd_config_sasl_plugin_t*) impl;
    config_sasl_plugin_free(qd->connection_manager, sasl_plugin);
}


static void deferred_close(void *context, bool discard) {
    if (!discard) {
        pn_connection_close((pn_connection_t*)context);
    }
}


void qd_connection_manager_delete_connector(qd_dispatch_t *qd, void *impl)
{
    qd_connector_t *ct = (qd_connector_t*) impl;
    if (ct) {
        sys_mutex_lock(ct->lock);
        if (ct->ctx) {
            ct->ctx->connector = 0;
            if(ct->ctx->pn_conn)
                qd_connection_invoke_deferred(ct->ctx, deferred_close, ct->ctx->pn_conn);

        }
        sys_mutex_unlock(ct->lock);
        DEQ_REMOVE(qd->connection_manager->connectors, ct);
        qd_connector_decref(ct);
    }
}


const char *qd_connector_name(qd_connector_t *ct)
{
    return ct ? ct->config.name : 0;
}

