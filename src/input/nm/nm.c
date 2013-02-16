/*
 * j4status - Status line generator
 *
 * Copyright © 2012 Quentin "Sardem FF7" Glidic
 *
 * This file is part of j4status.
 *
 * j4status is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * j4status is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with j4status. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <netdb.h>
#include <glib.h>
#include <libnm-glib/nm-client.h>
#include <libnm-glib/nm-device.h>
#include <libnm-glib/nm-device-ethernet.h>
#include <libnm-glib/nm-device-wifi.h>

#include <j4status-plugin.h>
#include <libj4status-config.h>

struct _J4statusPluginContext {
    J4statusCoreContext *core;
    J4statusCoreInterface *core_interface;
    GList *sections;
    gchar **interfaces;

    /* Generic configuration */
    gboolean show_unknown;
    gboolean show_unmanaged;
    gboolean hide_unavailable;

    /* WiFi devices configuration */
    struct {
        gboolean show_available_aps_number;
    } wifi;

    NMClient *nm_client;
    gboolean started;
};

typedef struct {
    J4statusPluginContext *context;
    NMDevice *device;

    gulong state_changed_handler;

    /* For WiFi devices */
    NMAccessPoint *ap;

    gulong bitrate_handler;
    gulong active_access_point_handler;
    gulong strength_handler;
} J4statusNmSectionContext;

static void
_j4status_nm_device_update(J4statusPluginContext *context, J4statusSection *section, NMDevice *device)
{
    J4statusNmSectionContext *section_context = section->user_data;

    g_free(section->value);
    switch ( nm_device_get_state(device) )
    {
    case NM_DEVICE_STATE_UNKNOWN:
        section->value = context->show_unknown ? g_strdup("Unknown") : NULL;
        section->state = J4STATUS_STATE_NO_STATE;
    break;
    case NM_DEVICE_STATE_UNMANAGED:
        section->value = context->show_unmanaged ? g_strdup("Unmanaged") : NULL;
        section->state = J4STATUS_STATE_NO_STATE;
    break;
    case NM_DEVICE_STATE_UNAVAILABLE:
        section->value = context->hide_unavailable ? NULL : g_strdup("Unavailable");
        section->state = J4STATUS_STATE_UNAVAILABLE;
    break;
    case NM_DEVICE_STATE_DISCONNECTED:
        section->state = J4STATUS_STATE_BAD;
        switch ( nm_device_get_device_type(device) )
        {
        case NM_DEVICE_TYPE_WIFI:
        {
            gchar *aps_number = NULL;
            if ( context->wifi.show_available_aps_number )
            {
                const GPtrArray *aps;
                aps = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(device));
                if ( aps != NULL )
                    aps_number = g_strdup_printf(" (%u APs)", aps->len);
            }
            section->value = g_strdup_printf("Down%s", ( aps_number != NULL ) ? aps_number : "");
            g_free(aps_number);
        }
        break;
        default:
            section->value = g_strdup("Down");
        break;
        }
    break;
    case NM_DEVICE_STATE_PREPARE:
        section->value = g_strdup("Prepare");
        section->state = J4STATUS_STATE_AVERAGE;
    break;
    case NM_DEVICE_STATE_CONFIG:
        section->value = g_strdup("Config");
        section->state = J4STATUS_STATE_AVERAGE;
    break;
    case NM_DEVICE_STATE_NEED_AUTH:
        section->value = g_strdup("Need auth");
        section->state = J4STATUS_STATE_AVERAGE;
    break;
    case NM_DEVICE_STATE_IP_CONFIG:
        section->value = g_strdup("IP configuration");
        section->state = J4STATUS_STATE_GOOD;
    break;
    case NM_DEVICE_STATE_IP_CHECK:
        section->value = g_strdup("IP check");
        section->state = J4STATUS_STATE_GOOD;
    break;
    case NM_DEVICE_STATE_SECONDARIES:
        section->value = g_strdup("Secondaries");
        section->state = J4STATUS_STATE_AVERAGE;
    break;
    case NM_DEVICE_STATE_ACTIVATED:
    {
        NMIP4Config *ip4_config;
        NMIP6Config *ip6_config;
        GString *addresses;

        addresses = g_string_new("");
        ip4_config = nm_device_get_ip4_config(device);
        if ( ip4_config != NULL )
        {
            const GSList *address_;
            for ( address_ = nm_ip4_config_get_addresses(ip4_config) ; address_ != NULL ; address_ = g_slist_next(address_) )
            {
                guint32 address;
                address = nm_ip4_address_get_address(address_->data);
                g_string_append_printf(addresses, "%u.%u.%u.%u", (address >> 0) & 255, (address >> 8) & 255, (address >> 16) & 255, (address >> 24) & 255);
                if (g_slist_next(address_) != NULL )
                    g_string_append(addresses, ", ");
            }
            g_string_append(addresses, " ");
        }
        ip6_config = nm_device_get_ip6_config(device);
        if ( ip6_config != NULL )
        {
            const GSList *address_;
            for ( address_ = nm_ip6_config_get_addresses(ip6_config) ; address_ != NULL ; address_ = g_slist_next(address_) )
            {
                const struct in6_addr *address;
                address = nm_ip6_address_get_address(address_->data);

                guint b = 0;
                gboolean shortened = FALSE;
                gboolean was_shortened = FALSE;
                guint fragment;
                for (;;)
                {
                    fragment = ( address->s6_addr[b] << 8 ) + address->s6_addr[b+1];
                    if ( ( fragment == 0 ) && ( ! was_shortened ) )
                        shortened = TRUE;
                    else
                    {
                        if ( shortened )
                        {
                            g_string_append_c(addresses, ':');
                            shortened = FALSE;
                            was_shortened = TRUE;
                        }
                        g_string_append_printf(addresses, "%x", fragment);
                    }
                    b += 2;
                    if ( b >= 16 )
                        break;
                    if ( ( ! shortened ) || ( b == 0 ) )
                        g_string_append_c(addresses, ':');
                }

                if (g_slist_next(address_) != NULL )
                    g_string_append(addresses, ", ");
            }
            g_string_append(addresses, " ");
        }
        switch ( nm_device_get_device_type(device) )
        {
        case NM_DEVICE_TYPE_WIFI:
        {
            section->state = J4STATUS_STATE_AVERAGE;

            gchar *ap_text = NULL;
            if ( section_context->ap != NULL )
            {
                guint8 strength;
                strength =  nm_access_point_get_strength(section_context->ap);
                if ( strength > 75 )
                    section->state = J4STATUS_STATE_GOOD;
                else if ( strength < 25 )
                    section->state = J4STATUS_STATE_BAD;

                const GByteArray *raw_ssid;
                raw_ssid = nm_access_point_get_ssid(section_context->ap);
                ap_text = g_strdup_printf("%03u%% at %.*s, ", strength, raw_ssid->len, (const gchar *)raw_ssid->data);
            }

            guint32 bitrate;
            bitrate = nm_device_wifi_get_bitrate(NM_DEVICE_WIFI(device));

            section->value = g_strdup_printf("%s(%s%uMb/s)", addresses->str, ( ap_text != NULL ) ? ap_text : "", bitrate/1000);

            g_free(ap_text);
            g_string_free(addresses, TRUE);
        }
        break;
        case NM_DEVICE_TYPE_ETHERNET:
        {
            section->state = J4STATUS_STATE_GOOD;

            guint32 speed;
            speed = nm_device_ethernet_get_speed(NM_DEVICE_ETHERNET(device));

            if ( speed == 0 )
            {
                g_string_truncate(addresses, addresses->len - 1);
                section->value = g_string_free(addresses, FALSE);
            }
            else
            {
                    if ( ( speed % 1000 ) == 0 )
                    section->value = g_strdup_printf("%s(%uGb/s)", addresses->str, speed/1000);
                else
                    section->value = g_strdup_printf("%s(%uMb/s)", addresses->str, speed);

                g_string_free(addresses, TRUE);
            }
        }
        break;
        default:
            section->state = J4STATUS_STATE_GOOD;
            section->value = g_string_free(addresses, FALSE);
        break;
        }
    }
    break;
    case NM_DEVICE_STATE_DEACTIVATING:
        section->value = g_strdup("Disconnecting");
        section->state = J4STATUS_STATE_BAD;
    break;
    case NM_DEVICE_STATE_FAILED:
        section->value = g_strdup("Failed");
        section->state = J4STATUS_STATE_BAD;
    break;
    }
    section->dirty = TRUE;
    context->core_interface->trigger_display(context->core);
}

static void
_j4status_nm_access_point_property_changed(NMAccessPoint *device, GParamSpec *pspec, gpointer user_data)
{
    J4statusSection *section = user_data;
    J4statusNmSectionContext *section_context = section->user_data;
    _j4status_nm_device_update(section_context->context, section, section_context->device);
}

static void
_j4status_nm_device_property_changed(NMDevice *device, GParamSpec *pspec, gpointer user_data)
{
    J4statusSection *section = user_data;
    J4statusNmSectionContext *section_context = section->user_data;
    if ( g_str_equal("active-access-point", pspec->name) )
    {
        if ( section_context->ap != NULL )
        {
            g_signal_handler_disconnect(section_context->ap, section_context->strength_handler);
            g_object_unref(section_context->ap);
        }
        section_context->ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(device));
        if ( section_context->ap != NULL )
            section_context->strength_handler = g_signal_connect(g_object_ref(section_context->ap), "notify::strength", G_CALLBACK(_j4status_nm_access_point_property_changed), section);
    }
    _j4status_nm_device_update(section_context->context, section, device);
}

static void
_j4status_nm_device_state_changed(NMDevice *device, guint state, guint arg2, guint arg3, gpointer user_data)
{
    J4statusSection *section = user_data;
    J4statusNmSectionContext *section_context = section->user_data;
    _j4status_nm_device_update(section_context->context, section, device);
}

static void
_j4status_nm_device_monitor(gpointer data, gpointer user_data)
{
    J4statusSection *section = data;
    J4statusNmSectionContext *section_context = section->user_data;

    switch ( nm_device_get_device_type(section_context->device) )
    {
    case NM_DEVICE_TYPE_WIFI:
        section_context->bitrate_handler = g_signal_connect(section_context->device, "notify::bitrate", G_CALLBACK(_j4status_nm_device_property_changed), section);
        section_context->active_access_point_handler = g_signal_connect(section_context->device, "notify::active-access-point", G_CALLBACK(_j4status_nm_device_property_changed), section);
        if ( section_context->ap != NULL )
            section_context->strength_handler = g_signal_connect(section_context->ap, "notify::strength", G_CALLBACK(_j4status_nm_access_point_property_changed), section);
    break;
    default:
    break;
    }
    section_context->state_changed_handler = g_signal_connect(section_context->device, "state-changed", G_CALLBACK(_j4status_nm_device_state_changed), section);
    _j4status_nm_device_update(section_context->context, section, section_context->device);
}

static void
_j4status_nm_device_unmonitor(gpointer data, gpointer user_data)
{
    J4statusSection *section = data;
    J4statusNmSectionContext *section_context = section->user_data;

    switch ( nm_device_get_device_type(section_context->device) )
    {
    case NM_DEVICE_TYPE_WIFI:
        g_signal_handler_disconnect(section_context->device, section_context->bitrate_handler);
        g_signal_handler_disconnect(section_context->device, section_context->active_access_point_handler);
        if ( section_context->ap != NULL )
            g_signal_handler_disconnect(section_context->ap, section_context->strength_handler);
    break;
    default:
    break;
    }
    g_signal_handler_disconnect(section_context->device, section_context->state_changed_handler);
}

static J4statusSection *
_j4status_nm_add_device(J4statusPluginContext *context, gchar *instance, NMDevice *device, GList *sibling)
{
    J4statusSection *section;
    section = g_new0(J4statusSection, 1);

    J4statusNmSectionContext *section_context;
    section_context = g_new0(J4statusNmSectionContext, 1);
    section_context->context = context;
    section_context->device = g_object_ref(device);

    section->user_data = section_context;

    switch ( nm_device_get_device_type(device) )
    {
    case NM_DEVICE_TYPE_UNKNOWN:
    case NM_DEVICE_TYPE_UNUSED1:
    case NM_DEVICE_TYPE_UNUSED2:
        section->name = "nm-unknown";
        section->label = g_strdup("Unknownw");
    break;
    case NM_DEVICE_TYPE_ETHERNET:
        section->name = "nm-ethernet";
        section->label = g_strdup("E");
    break;
    case NM_DEVICE_TYPE_WIFI:
        section->name = "nm-wifi";
        section->label = g_strdup("W");
        section_context->ap = nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(device));
        if ( section_context->ap != NULL )
            g_object_ref(section_context->ap);
    break;
    case NM_DEVICE_TYPE_BT:
        section->name = "nm-bluetooth";
        section->label = g_strdup("B");
    break;
    case NM_DEVICE_TYPE_OLPC_MESH:
        section->name = "nm-olpc-mesh";
        section->label = g_strdup("OM");
    break;
    case NM_DEVICE_TYPE_WIMAX:
        section->name = "nm-wimax";
        section->label = g_strdup("Wx");
    break;
    case NM_DEVICE_TYPE_MODEM:
        section->name = "nm-modem";
        section->label = g_strdup("M");
    break;
    case NM_DEVICE_TYPE_INFINIBAND:
        section->name = "nm-infiniband";
        section->label = g_strdup("I");
    break;
    case NM_DEVICE_TYPE_BOND:
        section->name = "nm-bond";
        section->label = g_strdup("Bo");
    break;
    case NM_DEVICE_TYPE_VLAN:
        section->name = "nm-vlan";
        section->label = g_strdup("V");
    break;
#if NM_CHECK_VERSION(0,9,5)
    case NM_DEVICE_TYPE_ADSL:
        section->name = "nm-adsl";
        section->label = g_strdup("A");
    break;
#endif /* NM_CHECK_VERSION(0.9.5) */
    }
    section->instance = instance;
    _j4status_nm_device_update(context, section, device);
    context->sections = g_list_insert_before(context->sections, sibling, section);

    return section;
}

static gint
_j4status_nm_find_interface(gconstpointer data, gconstpointer iface)
{
    const J4statusSection *section = data;
    return g_strcmp0(section->instance, iface);
}

static void
_j4status_nm_client_device_added(NMClient *client, NMDevice *device, gpointer user_data)
{
    J4statusPluginContext *context = user_data;
    const gchar *iface = nm_device_get_iface(device);
    gchar **interface;
    for ( interface = context->interfaces ; *interface != NULL ; ++interface )
    {
        if ( ! g_str_equal(*interface, iface) )
            continue;

        GList *sibling = NULL;
        if ( *(interface+1) != NULL )
            sibling = g_list_find_custom(context->sections, *(interface+1), _j4status_nm_find_interface);

        J4statusSection *section;
        section = _j4status_nm_add_device(context, *interface, device, sibling);
        if ( context->started )
            _j4status_nm_device_monitor(section, NULL);
    }
}

static void
_j4status_nm_client_device_removed(NMClient *client, NMDevice *device, gpointer user_data)
{
    J4statusPluginContext *context = user_data;
    GList *section_;
    section_ = g_list_find_custom(context->sections, nm_device_get_iface(device), _j4status_nm_find_interface);
    if ( section_ == NULL )
        return;

    J4statusSection *section;
    section = section_->data;
    context->sections = g_list_delete_link(context->sections, section_);

    g_free(section->value);
    g_free(section->label);
    g_free(section);
}

J4statusPluginContext *
_j4status_nm_init(J4statusCoreContext *core, J4statusCoreInterface *core_interface)
{
    GKeyFile *key_file;
    key_file = libj4status_config_get_key_file("NetworkManager");
    if ( key_file == NULL )
        return NULL;

    gchar **interfaces;
    interfaces = g_key_file_get_string_list(key_file, "NetworkManager", "Interfaces", NULL, NULL);
    if ( interfaces == NULL )
    {
        g_key_file_free(key_file);
        return NULL;
    }

    J4statusPluginContext *context;
    context = g_new0(J4statusPluginContext, 1);
    context->core = core;
    context->core_interface = core_interface;

    context->interfaces = interfaces;

    context->show_unknown = g_key_file_get_boolean(key_file, "NetworkManager", "ShowUnknown", NULL);
    context->show_unmanaged = g_key_file_get_boolean(key_file, "NetworkManager", "ShowUnmanaged", NULL);
    context->hide_unavailable = g_key_file_get_boolean(key_file, "NetworkManager", "HideUnavailable", NULL);

    context->wifi.show_available_aps_number = g_key_file_get_boolean(key_file, "NetworkManager", "WiFi-ShowAvailableAPsNumber", NULL);

    g_key_file_free(key_file);

    context->nm_client = nm_client_new();

    const GPtrArray *devices;
    devices = nm_client_get_devices(context->nm_client);
    gchar **interface;
    for ( interface = context->interfaces ; *interface != NULL ; ++interface )
    {
        NMDevice *device;
        guint i;
        for ( i = 0 ; i < devices->len ; ++i )
        {
            device = g_ptr_array_index(devices, i);
            if ( ! g_str_equal(*interface, nm_device_get_iface(device)) )
                continue;
            _j4status_nm_add_device(context, *interface, device, context->sections);
        }
    }

    g_signal_connect(context->nm_client, "device-added", G_CALLBACK(_j4status_nm_client_device_added), context);
    g_signal_connect(context->nm_client, "device-removed", G_CALLBACK(_j4status_nm_client_device_removed), context);

    context->sections = g_list_reverse(context->sections);
    return context;
}

static void
_j4status_nm_section_free(gpointer data)
{
    J4statusSection *section = data;
    J4statusNmSectionContext *section_context = section->user_data;

    g_object_unref(section_context->device);

    g_free(section_context);

    g_free(section->value);
    g_free(section->label);

    g_free(section);
}

static void
_j4status_nm_uninit(J4statusPluginContext *context)
{
    if ( context == NULL )
        return;

    g_list_free_full(context->sections, _j4status_nm_section_free);

    g_object_unref(context->nm_client);
    g_strfreev(context->interfaces);

    g_free(context);
}

static GList **
_j4status_nm_get_sections(J4statusPluginContext *context)
{
    if ( context == NULL )
        return NULL;

    return &context->sections;
}

static void
_j4status_nm_start(J4statusPluginContext *context)
{
    if ( context == NULL )
        return;

    g_list_foreach(context->sections, _j4status_nm_device_monitor, NULL);
    context->started = TRUE;
}

static void
_j4status_nm_stop(J4statusPluginContext *context)
{
    if ( context == NULL )
        return;

    context->started = FALSE;
    g_list_foreach(context->sections, _j4status_nm_device_unmonitor, NULL);
}

void
j4status_input_plugin(J4statusInputPluginInterface *interface)
{
    interface->init   = _j4status_nm_init;
    interface->uninit = _j4status_nm_uninit;

    interface->get_sections = _j4status_nm_get_sections;

    interface->start = _j4status_nm_start;
    interface->stop  = _j4status_nm_stop;
}
