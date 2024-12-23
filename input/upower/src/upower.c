/*
 * j4status - Status line generator
 *
 * Copyright © 2012-2018 Quentin "Sardem FF7" Glidic
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

#include "config.h"

#include <string.h>

#include <glib.h>
#include <upower.h>

#include "j4status-plugin-input.h"

#define J4STATUS_UPOWER_DEFAULT_FORMAT "${status:[;0;3;Empty;Full;Chr;Bat]}${charge:+ ${charge(f.2)}%}${time:+ (${time(d%{days:+%{days}d }%{hours:!00}%{hours(f02)}:%{minutes:!00}%{minutes(f02)}:%{seconds:!00}%{seconds(f02)})})}"

struct _J4statusPluginContext {
    J4statusCoreInterface *core;
    GList *sections;
    J4statusFormatString *format;
    UpClient *up_client;
    gboolean started;
    guint8 threshold_bad, threshold_urgent;
};

typedef struct {
    J4statusPluginContext *context;
    GObject *device;
    J4statusSection *section;
} J4statusUpowerSection;

typedef enum {
    TOKEN_STATUS,
    TOKEN_CHARGE,
    TOKEN_TIME,
} J4statusUpowerFormatToken;

static const gchar * const _j4status_upower_format_tokens[] = {
    [TOKEN_STATUS] = "status",
    [TOKEN_CHARGE] = "charge",
    [TOKEN_TIME]   = "time",
};

typedef enum {
    STATE_EMPTY,
    STATE_FULL,
    STATE_CHARGING,
    STATE_DISCHARGING,
} J4statusUpowerStatus;

typedef struct {
    J4statusUpowerStatus status;
    gdouble percentage;
    gint64 time;
} J4statusUpowerFormatData;

static GVariant *
_j4status_upower_format_callback(G_GNUC_UNUSED const gchar *token, guint64 value, gconstpointer user_data)
{
    const J4statusUpowerFormatData *data = user_data;

    switch ( (J4statusUpowerFormatToken) value )
    {
    case TOKEN_STATUS:
        return g_variant_new_byte(data->status);
    case TOKEN_CHARGE:
        if ( data->percentage < 0 )
            return NULL;
        return g_variant_new_double(data->percentage);
    case TOKEN_TIME:
        if ( data->time < 0 )
            return NULL;
        return g_variant_new_int64(data->time);
    }
    return NULL;
}

static void
_j4status_upower_device_changed(GObject *device, GParamSpec *pspec, gpointer user_data)
{
    J4statusUpowerSection *section = user_data;

    UpDeviceState device_state;
    J4statusState state = J4STATUS_STATE_NO_STATE;
    J4statusUpowerFormatData data = {
        .status = STATE_EMPTY,
        .percentage = -1,
        .time = -1,
    };

    g_object_get(device, "percentage", &data.percentage, "state", &device_state, NULL);

    switch ( device_state )
    {
    case UP_DEVICE_STATE_LAST: /* Size placeholder */
    case UP_DEVICE_STATE_UNKNOWN:
        j4status_section_set_state(section->section, J4STATUS_STATE_UNAVAILABLE);
        j4status_section_set_value(section->section, g_strdup("No battery"));
        return;
    case UP_DEVICE_STATE_EMPTY:
        state = J4STATUS_STATE_BAD | J4STATUS_STATE_URGENT;
    break;
    case UP_DEVICE_STATE_FULLY_CHARGED:
        data.status = STATE_FULL;
        state = J4STATUS_STATE_GOOD;
    break;
    case UP_DEVICE_STATE_CHARGING:
    case UP_DEVICE_STATE_PENDING_CHARGE:
        data.status = STATE_CHARGING;
        state = J4STATUS_STATE_AVERAGE;

        g_object_get(device, "time-to-full", &data.time, NULL);
    break;
    case UP_DEVICE_STATE_DISCHARGING:
    case UP_DEVICE_STATE_PENDING_DISCHARGE:
        data.status = STATE_DISCHARGING;
        if ( data.percentage < section->context->threshold_bad )
            state = J4STATUS_STATE_BAD;
        else
            state = J4STATUS_STATE_AVERAGE;

        if ( data.percentage < section->context->threshold_urgent )
            state |= J4STATUS_STATE_URGENT;

        g_object_get(device, "time-to-empty", &data.time, NULL);
    break;
    }
    j4status_section_set_state(section->section, state);


    gchar *value;
    value = j4status_format_string_replace(section->context->format, _j4status_upower_format_callback, &data);
    j4status_section_set_value(section->section, value);
}

static void
_j4status_upower_section_free(gpointer data)
{
    J4statusUpowerSection *section = data;

    j4status_section_free(section->section);

    g_object_unref(section->device);

    g_free(section);
}

static void
_j4status_upower_section_new(J4statusPluginContext *context, GObject *device, gboolean all_devices)
{
    UpDeviceKind kind;

    g_object_get(device, "kind", &kind, NULL);

    const gchar *path;
    const gchar *name = NULL;
    const gchar *instance = NULL;
    const gchar *label = NULL;

    path = up_device_get_object_path(UP_DEVICE(device));
    instance = g_utf8_strrchr(path, -1, '/') + strlen("/") + strlen(up_device_kind_to_string(kind)) + strlen("_");

    switch ( kind )
    {
    case UP_DEVICE_KIND_BATTERY:
        name = "upower-battery";
    break;
#define EXTRA_KIND(e, n, l) \
    case UP_DEVICE_KIND_##e: \
        if ( ! all_devices ) \
            return; \
        name = "upower-" #n; \
        label = #l; \
    break;
    EXTRA_KIND(UPS, ups, UPS)
    EXTRA_KIND(MONITOR, monitor, Monitor)
    EXTRA_KIND(MOUSE, mouse, Mouse)
    EXTRA_KIND(KEYBOARD, keyboard, Keyboard)
    EXTRA_KIND(PDA, pda, PDA)
    EXTRA_KIND(PHONE, phone, Phone)
    EXTRA_KIND(MEDIA_PLAYER, media-player, Media player)
    EXTRA_KIND(TABLET, tablet, Tablet)
    EXTRA_KIND(COMPUTER, computer, Computer)
    EXTRA_KIND(GAMING_INPUT, gaming-input, Gaming input)
#if UP_CHECK_VERSION(0,99,12)
    EXTRA_KIND(PEN, pen, Pen)
    EXTRA_KIND(TOUCHPAD, touchpad, Touchpad)
    EXTRA_KIND(MODEM, modem, Modem)
    EXTRA_KIND(NETWORK, network, Network)
    EXTRA_KIND(HEADSET, headset, Headset)
    EXTRA_KIND(SPEAKERS, speakers, Speakers)
    EXTRA_KIND(HEADPHONES, headphones, Headphones)
    EXTRA_KIND(VIDEO, video, Video)
    EXTRA_KIND(OTHER_AUDIO, other-audio, Other audio)
    EXTRA_KIND(REMOTE_CONTROL, remote-control, Remote control)
    EXTRA_KIND(PRINTER, printer, Printer)
    EXTRA_KIND(SCANNER, scanner, Scanner)
    EXTRA_KIND(CAMERA, camera, Camera)
    EXTRA_KIND(WEARABLE, wearable, Wearable)
    EXTRA_KIND(TOY, toy, Toy)
    EXTRA_KIND(BLUETOOTH_GENERIC, bluetooth-generic, Bluetooth generic)
#endif /* UP_CHECK_VERSION(0,99,12) */
#undef EXTRA_KIND
    case UP_DEVICE_KIND_UNKNOWN:
    case UP_DEVICE_KIND_LINE_POWER:
    case UP_DEVICE_KIND_LAST: /* Size placeholder */
        return;
    }

    J4statusUpowerSection *section;
    section = g_new0(J4statusUpowerSection, 1);
    section->context = context;
    section->device = g_object_ref(device);
    section->section = j4status_section_new(context->core);

    j4status_section_set_name(section->section, name);
    j4status_section_set_instance(section->section, instance);
    if ( label != NULL )
        j4status_section_set_label(section->section, label);

    if ( j4status_section_insert(section->section) )
    {
        context->sections = g_list_prepend(context->sections, section);

        g_signal_connect(device, "notify", G_CALLBACK(_j4status_upower_device_changed), section);
        _j4status_upower_device_changed(device, NULL, section);
    }
    else
        _j4status_upower_section_free(section);
}

static void _j4status_upower_uninit(J4statusPluginContext *context);

static void
get_threshold(GKeyFile *key_file, const gchar *key, guint8 *target)
{
    GError * e = NULL;
    gint threshold = g_key_file_get_integer(key_file, "UPower", key, &e);

    if (e)
    {
        if ( e->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND )
            g_warning("UPower.%s: %s", key, e->message);
        g_free(e);
    }
    else if ( threshold < 0 || threshold > 100 )
        g_warning("UPower.%s=%d: %s", key, threshold, g_strerror(ERANGE));
    else
        *target = threshold;
}

static J4statusPluginContext *
_j4status_upower_init(J4statusCoreInterface *core)
{
    J4statusPluginContext *context;
    context = g_new0(J4statusPluginContext, 1);
    context->core = core;

    context->up_client = up_client_new();
    context->threshold_bad = 15;
    context->threshold_urgent = 5;

    gboolean all_devices = FALSE;
    gchar *format = NULL;

    GKeyFile *key_file;
    key_file = j4status_config_get_key_file("UPower");
    if ( key_file != NULL )
    {
        all_devices = g_key_file_get_boolean(key_file, "UPower", "AllDevices", NULL);
        format = g_key_file_get_string(key_file, "UPower", "Format", NULL);
        get_threshold(key_file, "BadThreshold", &context->threshold_bad);
        get_threshold(key_file, "UrgentThreshold", &context->threshold_urgent);
        g_key_file_free(key_file);
        if ( context->threshold_bad < context->threshold_urgent )
        {
            g_warning("BadThreshold < UrgentThreshold (%hhu < %hhu); resetting to defaults",
                      context->threshold_bad, context->threshold_urgent);
            context->threshold_bad = 15;
            context->threshold_urgent = 5;
        }
    }
    context->format = j4status_format_string_parse(format, _j4status_upower_format_tokens, G_N_ELEMENTS(_j4status_upower_format_tokens), J4STATUS_UPOWER_DEFAULT_FORMAT, NULL);

    GPtrArray *devices;
    guint i;

    devices = up_client_get_devices2(context->up_client);
    if ( devices == NULL )
    {
        g_warning("No devices to monitor, aborting");
        g_object_unref(context->up_client);
        g_free(context);
        return NULL;
    }
    for ( i = 0 ; i < devices->len ; ++i )
        _j4status_upower_section_new(context, g_ptr_array_index(devices, i), all_devices);
    g_ptr_array_unref(devices);

    if ( context->sections == NULL )
    {
        g_message("Missing configuration: No device to monitor, aborting");
        _j4status_upower_uninit(context);
        return NULL;
    }


    return context;
}

static void
_j4status_upower_uninit(J4statusPluginContext *context)
{
    j4status_format_string_unref(context->format);

    g_list_free_full(context->sections, _j4status_upower_section_free);

    g_free(context);
}

static void
_j4status_upower_start(J4statusPluginContext *context)
{
    context->started = TRUE;
}

static void
_j4status_upower_stop(J4statusPluginContext *context)
{
    context->started = FALSE;
}

J4STATUS_EXPORT void
j4status_input_plugin(J4statusInputPluginInterface *interface)
{
    libj4status_input_plugin_interface_add_init_callback(interface, _j4status_upower_init);
    libj4status_input_plugin_interface_add_uninit_callback(interface, _j4status_upower_uninit);

    libj4status_input_plugin_interface_add_start_callback(interface, _j4status_upower_start);
    libj4status_input_plugin_interface_add_stop_callback(interface, _j4status_upower_stop);
}
