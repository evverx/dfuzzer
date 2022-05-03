/** @file dfuzzer.c */
/*
 * dfuzzer - tool for fuzz testing processes communicating through D-Bus.
 *
 * Copyright(C) 2013,2014,2015, Red Hat, Inc.
 *     Matus Marhefka <mmarhefk@redhat.com>
 *     Miroslav Vadkerti <mvadkert@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>

#include "bus.h"
#include "dfuzzer.h"
#include "introspection.h"
#include "fuzz.h"
#include "util.h"


/** Structure containing D-Bus name, object path and interface of process */
static struct fuzzing_target target_proc = { "", "", "" };
/** Debug flag */
static int df_verbose_flag;
/** Verbose flag */
static int df_debug_flag;
/** Option for listing names on the bus */
static int df_list_names;
/** Maximum buffer size for generated strings by rand module (in Bytes) */
static long df_buf_size;
/** Contains method name or NULL. When not NULL, only method with this name
  * will be tested (do not free - points to argv) */
static char *df_test_method;
/** Tested process PID */
static int df_pid = -1;
/** NULL terminated struct of methods names which will be skipped from testing */
static struct suppression_item *suppressions[MAXLEN];
/** If -s option is passed 1, otherwise 0 */
static int df_supflg;
/** Suppression file #1 */
#define SF1 "./dfuzzer.conf"
/** Suppression file #2 (home dir) */
#define SF2 ".dfuzzer.conf"
/** Suppression file #3 (mandatory) */
#define SF3 "/etc/dfuzzer.conf"
/** Command/Script to execute by dfuzzer after each method call.
  * If command/script returns >0, dfuzzer prints fail message,
  * if 0 it continues */
static char *df_execute_cmd;
/** If -L is passed, full log of method calls and their return values will be
  * written to a [BUS_NAME.log] file */
static int df_full_log_flag;
/** Path to directory containing output logs */
static char *log_dir_name;
/** Pointer to a file for full logging  */
FILE* logfile;


/**
 * @function Main function controls fuzzing.
 * @param argc Number of program arguments
 * @param argv Pointer on string with program arguments
 * @return 0 on success, 1 on error, 2 when testing detected any failures
 * and/or warnings, 3 when testing detected only warnings
 */
int main(int argc, char **argv)
{
        const char *log_file_name;
        int rses = 0;               // return value from session bus testing
        int rsys = 0;               // return value from system bus testing
        int ret = 0;
        df_parse_parameters(argc, argv);

        if (df_full_log_flag) {
                log_file_name = strjoina(log_dir_name, "/", target_proc.name);
                logfile = fopen(log_file_name, "a+");
                if(!logfile) {
                        df_fail("Error opening file %s; detailed logs will not be written\n", log_file_name);
                        df_full_log_flag = 0;
                }
        }
        if (!df_supflg) {       // if -s option was not passed
                if (df_load_suppressions() < 0) {
                        printf("%sExit status: 1%s\n", ansi_bold(), ansi_normal());
                        ret = 1;
                        goto cleanup;
                }
        }

        rses = df_process_bus(G_BUS_TYPE_SESSION);
        rsys = df_process_bus(G_BUS_TYPE_SYSTEM);

        // both tests ended with error
        if (rses == DF_BUS_ERROR || rsys == DF_BUS_ERROR)
                ret = 1;
        else if (rses == DF_BUS_FAIL || rsys == DF_BUS_FAIL)
                // at least one test found failures
                ret = 2;
        else if (rses == DF_BUS_WARNING || rsys == DF_BUS_WARNING)
                // at least one test found warnings
                ret = 3;
        else if (rses == DF_BUS_OK || rsys == DF_BUS_OK)
                // at least one of the tests passed (and the other one is not in
                // a fail state)
                ret = 0;
        else
                // all remaining combinations, like both results missing
                ret = 4;

        fprintf(stderr, "%sExit status: %d%s\n", ansi_bold(), ret, ansi_normal());

cleanup:
        // free all suppressions and their descriptions
        for (int i = 0; suppressions[i]; i++) {
                free(suppressions[i]->method);
                free(suppressions[i]->description);
                free(suppressions[i]);
        }

        return ret;
}

int df_process_bus(GBusType bus_type)
{
        _cleanup_(g_dbus_connection_unrefp) GDBusConnection *dcon = NULL;
        _cleanup_(g_error_freep) GError *error = NULL;

        switch (bus_type) {
        case G_BUS_TYPE_SESSION:
                fprintf(stderr, "%s%s[SESSION BUS]%s\n", ansi_cr(), ansi_cyan(), ansi_normal());
                break;
        case G_BUS_TYPE_SYSTEM:
                fprintf(stderr, "%s%s[SYSTEM BUS]%s\n", ansi_cr(), ansi_cyan(), ansi_normal());
                break;
        default:
                df_fail("Invalid bus type\n");
                return DF_BUS_ERROR;
        }

        dcon = g_bus_get_sync(bus_type, NULL, &error);
        if (!dcon) {
                df_fail("Bus not found.\n");
                df_error("Error in g_bus_get_sync()", error);
                return DF_BUS_SKIP;
        }

        if (df_list_names) {
                // list names on the bus
                if (df_list_bus_names(dcon) == -1) {
                        df_debug("Error in df_list_bus_names() for session bus\n");
                        return DF_BUS_ERROR;
                }
        } else {
                // gets pid of tested process
                df_pid = df_get_pid(dcon, TRUE);
                if (df_pid > 0) {
                        df_print_process_info(df_pid);
                        fprintf(stderr, "%s%s[CONNECTED TO PID: %d]%s\n", ansi_cr(), ansi_cyan(), df_pid, ansi_normal());
                        if (!isempty(target_proc.interface)) {
                                fprintf(stderr, "Object: %s%s%s\n", ansi_bold(), target_proc.obj_path, ansi_normal());
                                fprintf(stderr, " Interface: %s%s%s\n", ansi_bold(), target_proc.interface, ansi_normal());
                                if (!df_is_object_on_bus(dcon, DF_BUS_ROOT_NODE)) {
                                        df_fail("Error: Unknown object path '%s'.\n", target_proc.obj_path);
                                        return DF_BUS_ERROR;
                                } else
                                        return df_fuzz(dcon, target_proc.name, target_proc.obj_path, target_proc.interface);
                        } else if (!isempty(target_proc.obj_path)) {
                                fprintf(stderr, "Object: %s%s%s\n", ansi_bold(), target_proc.obj_path, ansi_normal());
                                if (!df_is_object_on_bus(dcon, DF_BUS_ROOT_NODE)) {
                                        df_fail("Error: Unknown object path '%s'.\n", target_proc.obj_path);
                                        return DF_BUS_ERROR;
                                } else
                                        return df_traverse_node(dcon, target_proc.obj_path);
                        } else {
                                fprintf(stderr, "Object: %s/%s\n", ansi_bold(), ansi_normal());
                                return df_traverse_node(dcon, DF_BUS_ROOT_NODE);
                        }
                } else {
                        df_fail("Couldn't get the PID of the tested process\n");
                        return DF_BUS_NO_PID;
                }
        }

        return DF_BUS_OK;
}

/**
 * @function Calls method ListNames to get all available connection names
 * on the bus and prints them on the program output.
 * @param dcon D-Bus connection structure
 * @return 0 on success, -1 on error
 */
int df_list_bus_names(GDBusConnection *dcon)
{
        _cleanup_(g_dbus_proxy_unrefp) GDBusProxy *proxy = NULL;
        _cleanup_(g_variant_iter_freep) GVariantIter *iter = NULL;
        _cleanup_(g_variant_unrefp) GVariant *response = NULL;
        char *str;

        proxy = df_bus_new(dcon,
                           "org.freedesktop.DBus",
                           "/org/freedesktop/DBus",
                           "org.freedesktop.DBus",
                           G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES|G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS);
        if (!proxy)
                return -1;

        response = df_bus_call(proxy, "ListNames", NULL, G_DBUS_CALL_FLAGS_NONE);
        if (!response)
                return -1;

        g_variant_get(response, "(as)", &iter);
        while (g_variant_iter_loop(iter, "s", &str)) {
                if (str[0] != ':')
                        printf("%s\n", str);
        }

        response = safe_g_variant_unref(response);
        iter = safe_g_variant_iter_free(iter);

        response = df_bus_call(proxy, "ListActivatableNames", NULL, G_DBUS_CALL_FLAGS_NONE);
        if (!response)
                return -1;

        g_variant_get(response, "(as)", &iter);
        while (g_variant_iter_loop(iter, "s", &str)) {
                if (str[0] != ':')
                        printf("%s (activatable)\n", str);
        }

        return 0;
}

/**
 * @function Traverses through all objects of bus name target_proc.name
 * and is looking for object path target_proc.obj_path
 * @param dcon D-Bus connection structure
 * @param root_node Starting object path (all nodes from this object path
 * will be traversed)
 * @return 1 when obj. path target_proc.obj_path is found on bus, 0 otherwise
 */
int df_is_object_on_bus(GDBusConnection *dcon, const char *root_node)
{
        char *intro_iface = "org.freedesktop.DBus.Introspectable";
        char *intro_method = "Introspect";
        _cleanup_(g_variant_unrefp) GVariant *response = NULL;
        _cleanup_(g_dbus_proxy_unrefp) GDBusProxy *dproxy = NULL;
        _cleanup_(g_freep) gchar *introspection_xml = NULL;
        _cleanup_(g_error_freep) GError *error = NULL;
        /** Information about nodes in a remote object hierarchy. */
        _cleanup_(g_dbus_node_info_unrefp) GDBusNodeInfo *node_data = NULL;
        GDBusNodeInfo *node = NULL;
        int i = 0;
        int ret = 0;        // return value of this function

        if (strstr(root_node, target_proc.obj_path) != NULL)
                return 1;

        if (!df_is_valid_dbus(target_proc.name, root_node, intro_iface))
                return 0;
        dproxy = df_bus_new(dcon, target_proc.name, root_node, intro_iface,
                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES|G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS);
        if (!dproxy)
                return 0;

        response = df_bus_call(dproxy, intro_method, NULL, G_DBUS_CALL_FLAGS_NONE);
        if (!response)
                return 0;

        g_variant_get(response, "(s)", &introspection_xml);
        if (!introspection_xml) {
                df_fail("Error: Unable to get introspection data from GVariant.\n");
                return 0;
        }

        // Parses introspection_xml and returns a GDBusNodeInfo representing
        // the data.
        node_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
        if (!node_data) {
                df_fail("Error: Unable to get introspection data.\n");
                df_error("Error in g_dbus_node_info_new_for_xml()", error);
                return 0;
        }

        // go through all nodes
        i = 0;
        node = node_data->nodes[i++];
        while (node != NULL) {
                _cleanup_free_ char *object = NULL;
                // create next object path
                object = strjoin(root_node, strlen(root_node) == 1 ? "" : "/", node->path);
                if (object == NULL) {
                        df_fail("Error: Could not allocate memory for object string.\n");
                        return DF_BUS_ERROR;
                }
                ret = df_is_object_on_bus(dcon, object);
                if (ret == 1)
                        return 1;
                // move to next node
                node = node_data->nodes[i++];
        }

        return ret;
}

/**
 * @function Traverses through all interfaces and objects of bus
 * name target_proc.name and for each interface it calls df_fuzz()
 * to fuzz test all its methods.
 * @param dcon D-Bus connection structure
 * @param root_node Starting object path (all nodes from this object path
 * will be traversed)
 * @return 0 on success, 1 on error, 2 when testing detected any failures
 * or warnings, 3 on warnings
 */
int df_traverse_node(GDBusConnection *dcon, const char *root_node)
{
        char *intro_iface = "org.freedesktop.DBus.Introspectable";
        char *intro_method = "Introspect";
        _cleanup_(g_variant_unrefp) GVariant *response = NULL;
        _cleanup_(g_dbus_proxy_unrefp) GDBusProxy *dproxy = NULL;
        _cleanup_(g_freep) gchar *introspection_xml = NULL;
        _cleanup_(g_error_freep) GError *error = NULL;
        /** Information about nodes in a remote object hierarchy. */
        _cleanup_(g_dbus_node_info_unrefp) GDBusNodeInfo *node_data = NULL;
        GDBusNodeInfo *node = NULL;
        int i = 0;
        /** Information about a D-Bus interface. */
        GDBusInterfaceInfo *interface = NULL;
        /** Return values */
        int rd = 0;          // return value from df_fuzz()
        int rt = 0;          // return value from recursive transition
        int ret = DF_BUS_OK; // return value of this function


        if (!df_is_valid_dbus(target_proc.name, root_node, intro_iface))
                return DF_BUS_ERROR;

        dproxy = df_bus_new(dcon, target_proc.name, root_node, intro_iface,
                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES|G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS);
        if (!dproxy)
                return DF_BUS_ERROR;

        response = df_bus_call_full(dproxy, intro_method, NULL, G_DBUS_CALL_FLAGS_NONE, &error);
        if (!response) {
                _cleanup_(g_freep) gchar *dbus_error = NULL;
                // D-Bus exceptions
                if ((dbus_error = g_dbus_error_get_remote_error(error)) != NULL) {
                        // if process does not respond
                        if (strcmp(dbus_error, "org.freedesktop.DBus.Error.NoReply") == 0)
                                return DF_BUS_FAIL;
                        if (strcmp(dbus_error, "org.freedesktop.DBus.Error.Timeout") == 0)
                                return DF_BUS_FAIL;
                        return DF_BUS_OK;
                } else {
                        g_dbus_error_strip_remote_error(error);
                        df_fail("Error: %s.\n", error->message);
                        df_error("Error in g_dbus_proxy_call_sync()", error);
                        return DF_BUS_ERROR;
                }
        }
        g_variant_get(response, "(s)", &introspection_xml);
        if (!introspection_xml) {
                df_fail("Error: Unable to get introspection data from GVariant.\n");
                return DF_BUS_ERROR;
        }

        // Parses introspection_xml and returns a GDBusNodeInfo representing
        // the data.
        node_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
        if (!node_data) {
                df_fail("Error: Unable to get introspection data.\n");
                df_error("Error in g_dbus_node_info_new_for_xml()", error);
                return DF_BUS_ERROR;
        }

        // go through all interfaces
        i = 0;
        interface = node_data->interfaces[i++];
        while (interface != NULL) {
                fprintf(stderr, " Interface: %s%s%s\n",
                        ansi_bold(), interface->name, ansi_normal());
                // start fuzzing on the target_proc.name
                rd = df_fuzz(dcon, target_proc.name, root_node, interface->name);
                if (rd == DF_BUS_ERROR)
                        return DF_BUS_ERROR;
                else if (ret != DF_BUS_FAIL) {
                        if (rd != DF_BUS_OK)
                                ret = rd;
                }
                interface = node_data->interfaces[i++];
        }

        // if object path was set as dfuzzer option, do not traverse
        // through all objects
        if (strlen(target_proc.obj_path) != 0)
                return ret;

        // go through all nodes
        i = 0;
        node = node_data->nodes[i++];
        while (node != NULL) {
                _cleanup_free_ char *object = NULL;
                // create next object path
                object = strjoin(root_node, strlen(root_node) == 1 ? "" : "/", node->path);
                if (object == NULL) {
                        df_fail("Error: Could not allocate memory for root_node string.\n");
                        return DF_BUS_ERROR;
                }
                fprintf(stderr, "Object: %s%s%s\n", ansi_bold(), object, ansi_normal());
                rt = df_traverse_node(dcon, object);
                if (rt == DF_BUS_ERROR)
                        return DF_BUS_ERROR;
                else if (ret != DF_BUS_FAIL) {
                        if (rt != DF_BUS_OK)
                                ret = rt;
                }
                // move to next node
                node = node_data->nodes[i++];
        }

        return ret;
}

/**
 * @function Controls fuzz testing of all methods of specified interface (intf)
 * and reports results.
 * @param dcon D-Bus connection structure
 * @param name D-Bus name
 * @param obj D-Bus object path
 * @param intf D-Bus interface
 * @return 0 on success, 1 on error, 2 when testing detected any failures,
 * 3 on warnings
 */
int df_fuzz(GDBusConnection *dcon, const char *name, const char *object, const char *interface)
{
        _cleanup_(g_dbus_proxy_unrefp) GDBusProxy *dproxy = NULL;
        _cleanup_(g_dbus_node_info_unrefp) GDBusNodeInfo *node_info = NULL;
        GDBusInterfaceInfo *interface_info = NULL;
        int ret = 0;
        int method_found = 0;   // If df_test_method is found in an interface,
        // method_found is set to 1, otherwise is 0.
        int rv = DF_BUS_OK;     // return value of function
        int i;

        // Sanity check fuzzing target
        if (isempty(name) || isempty(object) || isempty(interface)) {
                df_fail("Error in target specification.\n");
                return DF_BUS_ERROR;
        }

        // Creates a proxy for accessing intf on the remote object at path obj
        // owned by name at dcon.
        if (!df_is_valid_dbus(name, object, interface))
                return DF_BUS_ERROR;

        dproxy = df_bus_new(dcon, name, object, interface,
                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES|G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS);
        if (!dproxy)
                return DF_BUS_ERROR;

        node_info = df_get_interface_info(dproxy, interface, &interface_info);
        if (!node_info)
                return DF_BUS_ERROR;

        // tells fuzz module to call methods on dproxy, use FD statfd
        // for monitoring tested process and memory limit for process
        if (df_fuzz_init(dproxy) == -1) {
                df_debug("Error in df_fuzz_add_proxy()\n");
                return DF_BUS_ERROR;
        }

        for (GDBusMethodInfo **pm = interface_info->methods, *m = *pm; m; m = *(++pm)) {
                _cleanup_(df_dbus_method_cleanup) struct df_dbus_method dbus_method = {0,};

                // testing only one method with name df_test_method
                if (df_test_method != NULL) {
                        if (strcmp(df_test_method, m->name) != 0)
                                continue;
                        method_found = 1;
                }

                // if method name is in the suppressions array, skip it
                if (suppressions[0] != NULL) {
                        int skipflg = 0;
                        for (i = 0; suppressions[i] != NULL; i++) {
                                if (strcmp(suppressions[i]->method, m->name) == 0) {
                                        skipflg++;
                                        break;
                                }
                        }
                        if (skipflg) {
                                if (suppressions[i]->description) {
                                        df_verbose("%s  %sSKIP%s %s - %s\n",
                                                   ansi_cr(), ansi_blue(), ansi_normal(),
                                                   suppressions[i]->method, suppressions[i]->description);
                                } else
                                        df_verbose("%s  %sSKIP%s %s - suppressed method\n",
                                                   ansi_cr(), ansi_blue(), ansi_normal(), suppressions[i]->method);
                                continue;
                        }
                }

                dbus_method.name = strdup(m->name);
                dbus_method.signature = df_method_get_full_signature(m);
                dbus_method.returns_value = !!*(m->out_args);
                dbus_method.fuzz_on_str_len = (strstr(dbus_method.signature, "s") || strstr(dbus_method.signature, "v"));

                // tests for method
                ret = df_fuzz_test_method(
                                &dbus_method,
                                df_buf_size,
                                name,
                                object,
                                interface,
                                df_pid,
                                df_execute_cmd);
                if (ret == -1) {
                        // error during testing method
                        df_debug("Error in df_fuzz_test_method()\n");
                        return DF_BUS_ERROR;
                } else if (ret == 1 && df_test_method == NULL) {
                        // launch process again after crash
                        rv = DF_BUS_FAIL;
                        g_object_unref(dproxy);
                        dproxy = NULL;

                        if (!df_is_valid_dbus(name, object, interface))
                                return DF_BUS_ERROR;

                        dproxy = df_bus_new(dcon, name, object, interface,
                                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES|G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS);
                        if (!dproxy)
                                return DF_BUS_ERROR;

                        sleep(5);       // wait for application to launch

                        // gets pid of tested process
                        df_pid = df_get_pid(dcon, FALSE);
                        if (df_pid < 0) {
                                df_debug("Error in df_get_pid() on getting pid of process\n");
                                return DF_BUS_ERROR;
                        }
                        fprintf(stderr, "%s%s[RE-CONNECTED TO PID: %d]%s\n",
                                        ansi_cr(), ansi_cyan(), df_pid, ansi_blue());

                        // tells fuzz module to call methods on different dproxy and to use
                        // new status file of process with PID df_pid
                        if (df_fuzz_init(dproxy) == -1) {
                                df_debug("Error in df_fuzz_add_proxy()\n");
                                return DF_BUS_ERROR;
                        }
                } else if (ret == 1 && df_test_method != NULL) {
                        // for one method, testing ends with failure
                        rv = DF_BUS_FAIL;
                } else if (ret == 2) {
                        // method returning void is returning illegal value
                        rv = DF_BUS_FAIL;
                } else if (ret == 3) {
                        // warnings
                        if (rv != 2)
                                rv = DF_BUS_WARNING;
                } else if (ret == 4) {
                        // executed command finished unsuccessfuly
                        rv = DF_BUS_FAIL;
                }

        }


        if (method_found == 0 && df_test_method != NULL) {
                df_fail("Error: Method '%s' is not in the interface '%s'.\n", df_test_method, interface);
                return rv;
        }

        return rv;
}

/**
 * @function Checks if name is valid D-Bus name, obj is valid
 * D-Bus object path and intf is valid D-Bus interface.
 * @param name D-Bus name
 * @param obj D-Bus object path
 * @param intf D-Bus interface
 * @return 1 if name, obj and intf are valid, 0 otherwise
 */
int df_is_valid_dbus(const char *name, const char *obj, const char *intf)
{
        if (!g_dbus_is_name(name)) {
                df_fail("Error: Unknown bus name '%s'.\n", name);
                return 0;
        }
        if (!g_variant_is_object_path(obj)) {
                df_fail("Error: Unknown object path '%s'.\n", obj);
                return 0;
        }
        if (!g_dbus_is_interface_name(intf)) {
                df_fail("Error: Unknown interface '%s'.\n", intf);
                return 0;
        }
        return 1;
}

/**
 * @function Calls method GetConnectionUnixProcessID on the interface
 * org.freedesktop.DBus to get process pid.
 * @param dcon D-Bus connection structure
 * @return Process PID on success, -1 on error
 */
int df_get_pid(GDBusConnection *dcon, gboolean activate)
{
        _cleanup_(g_dbus_proxy_unrefp) GDBusProxy *pproxy = NULL;
        _cleanup_(g_variant_unrefp) GVariant *variant_pid = NULL;
        int pid = -1;

        pproxy = df_bus_new(dcon,
                            "org.freedesktop.DBus",
                            "/org/freedesktop/DBus",
                            "org.freedesktop.DBus",
                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES|G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS);
        if (!pproxy)
                return -1;

        /* Attempt to activate the remote side. Since we can't use any well-known
         * remote method for auto-activation, fall back to calling
         * the org.freedesktop.DBus.StartServiceByName method.
         *
         * See:
         *  - https://dbus.freedesktop.org/doc/dbus-specification.html#bus-messages-start-service-by-name
         *  - https://dbus.freedesktop.org/doc/system-activation.txt
         */
        if (activate) {
                _cleanup_(g_error_freep) GError *act_error = NULL;
                _cleanup_(g_variant_unrefp) GVariant *act_res = NULL;

                act_res = df_bus_call_full(pproxy,
                                           "StartServiceByName",
                                           g_variant_new("(su)", target_proc.name, 0),
                                           G_DBUS_CALL_FLAGS_NONE,
                                           &act_error);
                if (!act_res) {
                        g_dbus_error_strip_remote_error(act_error);
                        df_verbose("Error while activating '%s': %s.\n", target_proc.name, act_error->message);
                        df_error("Failed to activate the target", act_error);
                        /* Don't make this a hard fail */
                }
        }

        variant_pid = df_bus_call(pproxy,
                                  "GetConnectionUnixProcessID",
                                  g_variant_new("(s)", target_proc.name),
                                  G_DBUS_CALL_FLAGS_NONE);
        if (!variant_pid)
                return -1;

        g_variant_get(variant_pid, "(u)", &pid);

        return pid;
}

/**
 * @function Prints process name and package to which process belongs.
 * @param pid PID of process
 * Note: Any error in this function is suppressed. On error, process name
 *       and package is just not printed.
 */
void df_print_process_info(int pid)
{
        char proc_path[15 + DECIMAL_STR_MAX(int)]; // "/proc/(int)/[exe|cmdline]"
        char name[PATH_MAX + 1];
        _cleanup_close_ int fd = -1;
        int ret;

        sprintf(proc_path, "/proc/%d/exe", pid);
        ret = readlink(proc_path, name, PATH_MAX);
        if (ret > 0) {
                name[ret] = '\0';

                if (ret == PATH_MAX)
                        df_verbose("The process name was truncated\n");

                if (!strstr(name, "python") && !strstr(name, "perl")) {
                        fprintf(stderr, "%s%s[PROCESS: %s]%s\n",
                                ansi_cr(), ansi_cyan(), name, ansi_normal());
                        return;
                }
        }

        // if readlink failed or executable was interpret (and our target is
        // interpreted script), try to read cmdline
        sprintf(proc_path, "/proc/%d/cmdline", pid);
        fd = open(proc_path, O_RDONLY);
        if (fd <= 0) {
                perror("open");
                return;
        }

        for (int i = 0;; i++) {
                if (i >= PATH_MAX) {
                        df_verbose("The process name was truncated\n");
                        name[PATH_MAX] = '\0';
                        break;
                }

                ret = read(fd, (name + i), 1);
                if (ret < 0) {
                        perror("read");
                        return;
                }

                if (name[i] == '\0')
                        break;
        }

        fprintf(stderr, "%s%s[PROCESS: %s]%s\n",
                ansi_cr(), ansi_cyan(), name, ansi_normal());
}

/**
 * @function Parses program options and stores them into global
 * variables:
 *  - df_buf_size -
 *     Maximum buffer size for generated strings by rand
 *     module (in Bytes)
 *  - df_test_method -
 *     Contains method name or NULL. When not NULL, only
 *     method with this name will be tested
 *  - target_proc -
 *     Is of type struct fuzzing_target and is used
 *     to store bus name, object path and interface
 *  - df_verbose_flag -
 *     Be verbose
 *  - df_debug_flag -
 *     Include debug output
 *  - df_supflg -
 *     If -s option is passed 1, otherwise 0
 *  - df_execute_cmd -
 *     Command/script to execute after each method call
 * If error occures function ends program.
 * @param argc Count of options
 * @param argv Pointer on strings containing options of program
 */
void df_parse_parameters(int argc, char **argv)
{
        int c = 0;

        static const struct option options[] = {
                { "buffer-limit",       required_argument,  NULL,   'b' },
                { "debug",              no_argument,        NULL,   'd' },
                { "command",            required_argument,  NULL,   'e' },
                { "help",               no_argument,        NULL,   'h' },
                { "interface",          required_argument,  NULL,   'i' },
                { "list",               no_argument,        NULL,   'l' },
                { "mem-limit",          required_argument,  NULL,   'm' },
                { "bus",                required_argument,  NULL,   'n' },
                { "object",             required_argument,  NULL,   'o' },
                { "no-suppressions",    no_argument,        NULL,   's' },
                { "method",             required_argument,  NULL,   't' },
                { "verbose",            no_argument,        NULL,   'v' },
                { "log-dir",            required_argument,  NULL,   'L' },
                { "version",            no_argument,        NULL,   'V' },
                {}
        };

        while ((c = getopt_long(argc, argv, "n:o:i:m:b:t:e:L:sdvlhV", options, NULL)) >= 0) {
                switch (c) {
                        case 'n':
                                if (strlen(optarg) >= MAXLEN) {
                                        df_fail("%s: maximum %d characters for option --"
                                                " 'n'\n", argv[0], MAXLEN - 1);
                                        exit(1);
                                }
                                target_proc.name = optarg;
                                break;
                        case 'o':
                                if (strlen(optarg) >= MAXLEN) {
                                        df_fail("%s: maximum %d characters for option --"
                                                " 'o'\n", argv[0], MAXLEN - 1);
                                        exit(1);
                                }
                                target_proc.obj_path = optarg;
                                break;
                        case 'i':
                                if (strlen(optarg) >= MAXLEN) {
                                        df_fail("%s: maximum %d characters for option --"
                                                " 'i'\n", argv[0], MAXLEN - 1);
                                        exit(1);
                                }
                                target_proc.interface = optarg;
                                break;
                        case 'm':
                                df_verbose("Option -m has no effect anymore");
                                break;
                        case 'b':
                                df_buf_size = strtol(optarg, NULL, 10);
                                if (df_buf_size < MINLEN || errno == ERANGE || errno == EINVAL) {
                                        df_fail("%s: invalid value for option -- 'b'\n"
                                                " -- at least %d B are required\n", argv[0], MINLEN);
                                        exit(1);
                                }
                                break;
                        case 't':
                                df_test_method = optarg;
                                break;
                        case 'e':
                                df_execute_cmd = optarg;
                                break;
                        case 's':
                                df_supflg = 1;
                                break;
                        case 'd':
                                df_debug_flag = 1;
                                break;
                        case 'v':
                                df_verbose_flag = 1;
                                break;
                        case 'l':
                                df_list_names = 1;
                                break;
                        case 'V':
                                printf("%s", DF_VERSION);
                                exit(0);
                                break;
                        case 'h':
                                df_print_help(argv[0]);
                                exit(0);
                                break;
                        case 'L':
                                //we need at least 1 more char than usual for directory separator
                                if (strlen(optarg) >= MAXLEN -1) {
                                        df_fail("%s: maximum %d characters for option --"
                                                " 'L'\n", argv[0], MAXLEN - 1);
                                        exit(1);
                                }
                                log_dir_name = optarg;
                                df_full_log_flag = 1;
                                break;
                        default:    // '?'
                                exit(1);
                                break;
                }
        }

        if (isempty(target_proc.name) && !df_list_names) {
                df_fail("Error: Connection name is required!\nSee -h for help.\n");
                exit(1);
        }

        if (!isempty(target_proc.interface) && isempty(target_proc.obj_path)) {
                df_fail("Error: Object path is required if interface specified!\nSee -h for help.\n");
                exit(1);
        }
}

/**
 * @function Searches target_proc.name in suppression file SF1, SF2 and SF3
 * (the file which is opened first is parsed). If it is found, the suppressions
 * array is seeded with names of methods and the reason  why methods are skipped.
 * Suppression file is in format:
 * [bus_name]
 * method1 description
 * method2 description
 * [bus_name2]
 * method1 description
 * method2 description
 * ...
 * @return 0 on success, -1 on error
 */
int df_load_suppressions(void)
{
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *line = NULL, *home_supp = NULL;
        char *env = NULL;
        int name_found = 0, i = 0;
        size_t len = 0;
        ssize_t n;

        if (isempty(target_proc.name))
                return 0;

        env = getenv("HOME");
        if (env) {
                home_supp = strjoin(env, "/", SF2);
                if (!home_supp)
                        return df_oom();
        }

        char *paths[3] = { SF1, home_supp, SF3 };

        for (i = 0; i < 3; i++) {
                if (!paths[i])
                        continue;

                f = fopen(paths[i], "r");
                if (f) {
                        df_verbose("Loading suppressions from file '%s'\n", paths[i]);
                        break;
                }

                df_verbose("Cannot open suppression file '%s'\n", paths[i]);
        }

        if (!f) {
                df_fail("Cannot open any pre-defined suppression file\n");
                return -1;
        }

        // determines if currently tested bus name is in suppression file
        while (getline(&line, &len, f) > 0) {
                if (strstr(line, target_proc.name)) {
                        name_found++;
                        break;
                }
        }

        if (ferror(f)) {
                df_fail("Error while reading from the suppression file: %m\n");
                return -1;
        }

        // no suppressions for tested bus name
        if (!name_found)
                return 0;

        df_verbose("Found suppressions for bus: '%s'\n", target_proc.name);

        i = 0;
        while (i < (MAXLEN - 1) && (n = getline(&line, &len, f)) > 0) {
                _cleanup_free_ char *suppression = NULL, *description = NULL;
                if (line[0] == '[')
                        break;

                /* The line contains only whitespace, skip it */
                if (strspn(line, " \t\r\n") == (size_t) n)
                        continue;

                /* The suppression description is optional, so let's accept such
                 * lines as well */
                if (sscanf(line, "%ms %m[^\n]", &suppression, &description) < 1) {
                        df_fail("Failed to parse line '%s'\n", line);
                        return -1;
                }

                suppressions[i] = malloc(sizeof(struct suppression_item));
                if (!suppressions[i])
                        return df_oom();

                suppressions[i]->method = TAKE_PTR(suppression);
                suppressions[i]->description = TAKE_PTR(description);
                df_verbose("Loaded suppression for method: %s (%s)\n",
                           suppressions[i]->method,
                           suppressions[i]->description ?: "n/a");
                i++;
        }

        suppressions[i] = NULL;

        if (ferror(f)) {
                df_fail("Error while reading from the suppression file: %m\n");
                return -1;
        }

        return 0;
}

/**
 * @function Prints help.
 * @param name Name of program
 */
void df_print_help(const char *name)
{
        printf(
         "Usage: %1$s -n BUS_NAME [OTHER_OPTIONS]\n\n"
         "Tool for fuzz testing processes communicating through D-Bus.\n"
         "The fuzzer traverses through all the methods on the given bus name.\n"
         "By default only failures and warnings are printed."
         " Use -v for verbose mode.\n\n"
         "REQUIRED OPTIONS:\n"
         "  -n --bus=BUS_NAME           D-Bus service name.\n\n"
         "OTHER OPTIONS:\n"
         "  -V --version                Show dfuzzer version and exit.\n"
         "  -h --help                   Show this help text.\n"
         "  -l --list                   List all available services on both buses.\n"
         "  -v --verbose                Be more verbose.\n"
         "  -d --debug                  Enable debug logging; implies -v.\n"
         "  -L --log-dir=DIRNAME        Write full, parseable log into DIRNAME/BUS_NAME.\n"
         "                              The directory must already exist.\n"
         "  -s --no-suppressions        Don't load suppression file(s).\n"
         "  -o --object=OBJECT_PATH     Optional object path to test. All children objects are traversed.\n"
         "  -i --interface=INTERFACE    Interface to test. Requires -o to be set as well.\n"
         "  -t --method=METHOD_NAME     Test only given method, all other methods are skipped.\n"
         "                              Requires -o and -i to be set as well.\n"
         "  -b --buffer-limit=SIZE      Maximum buffer size for generated strings in bytes.\n"
         "                              Default: 50K, minimum: 256B.\n"
         "  -e --command=COMMAND        Command/script to execute after each method call.\n"
         "\nExamples:\n\n"
         "Test all methods of GNOME Shell. Be verbose.\n"
         "# %1$s -v -n org.gnome.Shell\n\n"
         "Test only method of the given bus name, object path and interface.\n"
         "# %1$s -n org.freedesktop.Avahi -o / -i org.freedesktop.Avahi.Server -t GetAlternativeServiceName\n\n"
         "Test all methods of Avahi and be verbose. Redirect all log messages including failures\n"
         "and warnings into avahi.log:\n"
         "# %1$s -v -n org.freedesktop.Avahi 2>&1 | tee avahi.log\n\n"
         "Test name org.freedesktop.Avahi, be verbose and do not use any suppression file:\n"
         "# %1$s -v -s -n org.freedesktop.Avahi\n",
         name);
}

/**
 * @function Displays an error message.
 * @param message Error message which will be printed
 * @param error Pointer on GError structure containing error specification
 */
void df_error(const char *message, GError *error)
{
        if (!df_debug_flag) {
                return;
        }
        if (error == NULL)
                fprintf(stderr, "%s\n", message);
        else
                fprintf(stderr, "%s: %s\n", message, error->message);
}

/**
 * @function Prints debug message.
 * @param format Format string
 */
void df_debug(const char *format, ...)
{
        if (!df_debug_flag)
                return;
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
        fflush(stdout);
}

/**
 * @function Prints verbose message.
 * @param format Format string
 */
void df_verbose(const char *format, ...)
{
        if (!df_verbose_flag && !df_debug_flag)
                return;
        va_list args;
        va_start(args, format);
        vprintf(format, args);
        va_end(args);
        fflush(stdout);
}

/**
 * @function Prints error message.
 * @param format Format string
 */
void df_fail(const char *format, ...)
{
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        fflush(stderr);
}
