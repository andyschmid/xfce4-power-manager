/*
 * * Copyright (C) 2013 Sonal Santan <sonal.santan@gmail.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <libxfce4util/libxfce4util.h>

#include "xfpm-kbd-backlight.h"
#include "egg-idletime.h"
#include "xfpm-button.h"
#include "xfpm-notify.h"
#include "xfpm-xfconf.h"
#include "xfpm-config.h"
#include "xfpm-power.h"
#include "xfpm-debug.h"

#define ALARM_DISABLED 9

#define XFPM_KBD_BACKLIGHT_GET_PRIVATE(o) \
(G_TYPE_INSTANCE_GET_PRIVATE ((o), XFPM_TYPE_KBD_BACKLIGHT, XfpmKbdBacklightPrivate))

static void xfpm_kbd_backlight_finalize     (GObject *object);

struct XfpmKbdBacklightPrivate
{
    XfpmPower       *power;
    XfpmButton      *button;
    EggIdletime     *idle;
    XfpmXfconf      *conf;

    GDBusConnection *bus;
    GDBusProxy      *proxy;

    gboolean         dimmed;
    gboolean	     block;
    gboolean         on_battery;
    gint32           max_level;
    gint32           last_level;
    gint             min_level;
    gint             step;

    XfpmNotify      *notify;
    NotifyNotification *n;
};

G_DEFINE_TYPE (XfpmKbdBacklight, xfpm_kbd_backlight, G_TYPE_OBJECT)


static gint
calculate_step( gint max_level )
{
    if ( max_level < 20 )
        return 1;
    else
        return max_level / 20;
}

static void
xfpm_kbd_backlight_init_max_level (XfpmKbdBacklight *backlight)
{
    GError *error = NULL;
    GVariant *var;

    var = g_dbus_proxy_call_sync (backlight->priv->proxy, "GetMaxBrightness",
                                  NULL,
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1, NULL,
                                  &error);

    if (var)
    {
	g_variant_get (var,
		       "(i)",
		       &backlight->priv->max_level);
	g_variant_unref (var);
    }

    if ( error )
    {
        g_warning ("Failed to get keyboard max brightness level : %s", error->message);
        g_error_free (error);
    }
}


static void
xfpm_kbd_backlight_show_notification (XfpmKbdBacklight *self, gfloat value)
{
    gchar *summary;

    if ( self->priv->n == NULL )
    {
        self->priv->n = xfpm_notify_new_notification (self->priv->notify,
                "",
                "",
                "keyboard-brightness",
                0,
                XFPM_NOTIFY_NORMAL);
    }

    /* generate a human-readable summary for the notification */
    summary = g_strdup_printf (_("Keyboard Brightness: %.0f percent"), value);
    notify_notification_update (self->priv->n, summary, NULL, NULL);
    g_free (summary);

    /* add the brightness value to the notification */
    notify_notification_set_hint_int32 (self->priv->n, "value", value);

    /* show the notification */
    notify_notification_show (self->priv->n, NULL);
}


static gint
xfpm_kbd_backlight_get_level (XfpmKbdBacklight *backlight)
{
    GError *error = NULL;
    gint32 level = -1;
    GVariant *var;

    var = g_dbus_proxy_call_sync (backlight->priv->proxy, "GetBrightness",
                                  NULL,
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1, NULL,
                                  &error);
    if (var)
    {
	g_variant_get (var,
		       "(i)",
		       &level);
	g_variant_unref (var);
    }

    if ( error )
    {
        g_warning ("Failed to get keyboard brightness level : %s", error->message);
        g_error_free (error);
    }
    return level;
}


static gboolean
xfpm_kbd_backlight_set_level (XfpmKbdBacklight *backlight, gint32 level)
{
    GError *error = NULL;
    gfloat percent;
    GVariant *var;
    gboolean ret = TRUE;

    var = g_dbus_proxy_call_sync (backlight->priv->proxy, "SetBrightness",
                                  g_variant_new("(i)", level),
                                  G_DBUS_CALL_FLAGS_NONE,
                                  -1, NULL,
                                  &error);

    if (var)
        g_variant_unref (var);

    if ( error )
    {
        g_warning ("Failed to set keyboard brightness level : %s", error->message);
        g_error_free (error);
        ret = FALSE; // error condition
    }

    return ret;
}

static void
xfpm_kbd_backlight_set_level_from_input (XfpmKbdBacklight *backlight, gint32 level)
{
    GError *error = NULL;
    gfloat percent;
    GVariant *var;
    gboolean success;

    success = xfpm_kbd_backlight_set_level (backlight, level);

    if ( success )
    {
        percent = 100.0 * ((gfloat)level / (gfloat)backlight->priv->max_level);
        xfpm_kbd_backlight_show_notification (backlight, percent);

        if (backlight->priv->on_battery)
        {
            if (!xfconf_channel_set_uint (xfpm_xfconf_get_channel(backlight->priv->conf),
                                          PROPERTIES_PREFIX KBD_BRIGHTNESS_LEVEL_ON_BATTERY,
                        level))
            g_critical ("Cannot set value for property %s\n", KBD_BRIGHTNESS_LEVEL_ON_BATTERY);
        }
        else
        {
            if (!xfconf_channel_set_uint (xfpm_xfconf_get_channel(backlight->priv->conf),
                                          PROPERTIES_PREFIX KBD_BRIGHTNESS_LEVEL_ON_AC,
                        level))
            g_critical ("Cannot set value for property %s\n", KBD_BRIGHTNESS_LEVEL_ON_AC);
        }

    }
}

static void
xfpm_kbd_backlight_up (XfpmKbdBacklight *backlight)
{
    gint level;

    level = xfpm_kbd_backlight_get_level(backlight);

    if ( level == -1)
        return;

    if ( level == backlight->priv->max_level )
        return;

    level += backlight->priv->step;

    if ( level > backlight->priv->max_level )
        level = backlight->priv->max_level;

    xfpm_kbd_backlight_set_level_from_input(backlight, level);
}


static void
xfpm_kbd_backlight_down (XfpmKbdBacklight *backlight)
{
    gint level;

    level = xfpm_kbd_backlight_get_level(backlight);

    if ( level == -1)
        return;

    if ( level == backlight->priv->min_level )
        return;

    level -= backlight->priv->step;

    if ( level < backlight->priv->min_level )
        level = backlight->priv->min_level;

    xfpm_kbd_backlight_set_level_from_input(backlight, level);
}

static void
xfpm_kbd_backlight_dim_brightness (XfpmKbdBacklight *backlight)
{
    gboolean ret;

    gint32 dim_level;

    g_object_get (G_OBJECT (backlight->priv->conf),
                  backlight->priv->on_battery ? KBD_BRIGHTNESS_LEVEL_ON_BATTERY_DIM : KBD_BRIGHTNESS_LEVEL_ON_AC_DIM, &dim_level,
                  NULL);

    backlight->priv->last_level = xfpm_kbd_backlight_get_level (backlight);

    if ( backlight->priv->last_level == -1 )
    {
        g_warning ("Unable to get current keyboard brightness level");
        return;
    }

    /**
     * Only reduce if the current level is brighter than
     * the configured dim_level
     **/
    if (backlight->priv->last_level > dim_level)
    {
        XFPM_DEBUG ("Current keyboard brightness level before dimming : %d, new %d", backlight->priv->last_level, dim_level);
        backlight->priv->dimmed = xfpm_kbd_backlight_set_level (backlight, dim_level);
    }
}

static void
xfpm_kbd_backlight_alarm_timeout_cb (EggIdletime *idle, guint id, XfpmKbdBacklight *backlight)
{
    backlight->priv->block = FALSE;

    if ( id == TIMEOUT_KBD_BRIGHTNESS_ON_AC && !backlight->priv->on_battery)
        xfpm_kbd_backlight_dim_brightness (backlight);
    else if ( id == TIMEOUT_KBD_BRIGHTNESS_ON_BATTERY && backlight->priv->on_battery)
        xfpm_kbd_backlight_dim_brightness (backlight);
}

static void
xfpm_kbd_backlight_reset_cb (EggIdletime *idle, XfpmKbdBacklight *backlight)
{
    if ( backlight->priv->dimmed)
    {
        if ( !backlight->priv->block)
        {
            XFPM_DEBUG ("Alarm reset, setting level to %d", backlight->priv->last_level);
            xfpm_kbd_backlight_set_level (backlight, backlight->priv->last_level);
        }
        backlight->priv->dimmed = FALSE;
    }
}

static void
xfpm_kbd_backlight_brightness_on_battery_settings_changed (XfpmKbdBacklight *backlight)
{
    guint level;

    g_object_get (G_OBJECT (backlight->priv->conf),
                  KBD_BRIGHTNESS_LEVEL_ON_BATTERY, &level,
                  NULL);

    if (backlight->priv->on_battery && !backlight->priv->dimmed)
        xfpm_kbd_backlight_set_level (backlight, level);
}

static void
xfpm_kbd_backlight_brightness_on_ac_settings_changed (XfpmKbdBacklight *backlight)
{
    guint level;

    g_object_get (G_OBJECT (backlight->priv->conf),
                  KBD_BRIGHTNESS_LEVEL_ON_AC, &level,
                  NULL);

    if (!backlight->priv->on_battery && !backlight->priv->dimmed)
        xfpm_kbd_backlight_set_level (backlight, level);
}


static void
xfpm_kbd_backlight_inactivity_on_ac_settings_changed (XfpmKbdBacklight *backlight)
{
    guint timeout_on_ac;

    g_object_get (G_OBJECT (backlight->priv->conf),
                  KBD_BRIGHTNESS_ON_AC_TIMEOUT, &timeout_on_ac,
                  NULL);

    XFPM_DEBUG ("Alarm on ac timeout changed %u", timeout_on_ac);

    if ( timeout_on_ac == ALARM_DISABLED )
    {
        egg_idletime_alarm_remove (backlight->priv->idle, TIMEOUT_KBD_BRIGHTNESS_ON_AC );
    }
    else
    {
        egg_idletime_alarm_set (backlight->priv->idle, TIMEOUT_KBD_BRIGHTNESS_ON_AC, timeout_on_ac * 1000);
    }
}

static void
xfpm_kbd_backlight_inactivity_on_battery_settings_changed (XfpmKbdBacklight *backlight)
{
    guint timeout_on_battery ;

    g_object_get (G_OBJECT (backlight->priv->conf),
                  KBD_BRIGHTNESS_ON_BATTERY_TIMEOUT, &timeout_on_battery,
                  NULL);

    XFPM_DEBUG ("Alarm on battery timeout changed %u", timeout_on_battery);

    if ( timeout_on_battery == ALARM_DISABLED )
    {
        egg_idletime_alarm_remove (backlight->priv->idle, TIMEOUT_KBD_BRIGHTNESS_ON_BATTERY );
    }
    else
    {
        egg_idletime_alarm_set (backlight->priv->idle, TIMEOUT_KBD_BRIGHTNESS_ON_BATTERY, timeout_on_battery * 1000);
    }
}

static void
xfpm_kbd_backlight_set_brightness (XfpmKbdBacklight *backlight)
{
    xfpm_kbd_backlight_brightness_on_battery_settings_changed(backlight);
    xfpm_kbd_backlight_brightness_on_ac_settings_changed(backlight);
}

static void
xfpm_kbd_backlight_on_battery_changed_cb (XfpmPower *power, gboolean on_battery, XfpmKbdBacklight *backlight)
{
    backlight->priv->on_battery = on_battery;

    xfpm_kbd_backlight_set_brightness(backlight);
}

static void
xfpm_kbd_backlight_set_timeouts (XfpmKbdBacklight *backlight)
{
    xfpm_kbd_backlight_inactivity_on_ac_settings_changed (backlight);
    xfpm_kbd_backlight_inactivity_on_battery_settings_changed (backlight);
}

static void
xfpm_kbd_backlight_button_pressed_cb (XfpmButton *button, XfpmButtonKey type, XfpmKbdBacklight *backlight)
{
    if ( type != BUTTON_KBD_BRIGHTNESS_UP && type != BUTTON_KBD_BRIGHTNESS_DOWN )
        return; /* sanity check, can this ever happen? */

    backlight->priv->block = TRUE;

    if ( type == BUTTON_KBD_BRIGHTNESS_UP )
    {
        xfpm_kbd_backlight_up (backlight);
    }
    else if ( type == BUTTON_KBD_BRIGHTNESS_DOWN )
    {
        xfpm_kbd_backlight_down (backlight);
    }
}


static void
xfpm_kbd_backlight_class_init (XfpmKbdBacklightClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = xfpm_kbd_backlight_finalize;

    g_type_class_add_private (klass, sizeof (XfpmKbdBacklightPrivate));
}


static void
xfpm_kbd_backlight_init (XfpmKbdBacklight *backlight)
{
    GError *error = NULL;

    backlight->priv = XFPM_KBD_BACKLIGHT_GET_PRIVATE (backlight);

    backlight->priv->bus = NULL;
    backlight->priv->proxy = NULL;
    backlight->priv->power = NULL;
    backlight->priv->conf   = NULL;
    backlight->priv->button = NULL;
    backlight->priv->dimmed = FALSE;
    backlight->priv->idle   = NULL;
    backlight->priv->on_battery = FALSE;
    backlight->priv->max_level = 0;
    backlight->priv->min_level = 0;
    backlight->priv->notify = NULL;
    backlight->priv->block = FALSE;
    backlight->priv->n = NULL;

    backlight->priv->bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);

    if ( error )
    {
        g_critical ("Unable to get system bus connection : %s", error->message);
        g_error_free (error);
        goto out;
    }

    backlight->priv->proxy = g_dbus_proxy_new_sync (backlight->priv->bus,
						    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
						    G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
						    NULL,
						    "org.freedesktop.UPower",
						    "/org/freedesktop/UPower/KbdBacklight",
						    "org.freedesktop.UPower.KbdBacklight",
						    NULL,
						    NULL);
    if ( backlight->priv->proxy == NULL )
    {
        g_warning ("Unable to get the interface, org.freedesktop.UPower.KbdBacklight");
        goto out;
    }

    xfpm_kbd_backlight_init_max_level (backlight);

    if ( backlight->priv->max_level == 0 )
        goto out;

    backlight->priv->step = calculate_step (backlight->priv->max_level);
    backlight->priv->power = xfpm_power_get ();
    backlight->priv->button = xfpm_button_new ();
    backlight->priv->notify = xfpm_notify_new ();
    backlight->priv->idle   = egg_idletime_new ();
    backlight->priv->conf   = xfpm_xfconf_new ();

    g_signal_connect (backlight->priv->idle, "alarm-expired",
                      G_CALLBACK (xfpm_kbd_backlight_alarm_timeout_cb), backlight);

    g_signal_connect (backlight->priv->idle, "reset",
                      G_CALLBACK(xfpm_kbd_backlight_reset_cb), backlight);

    g_signal_connect (backlight->priv->button, "button-pressed",
                      G_CALLBACK (xfpm_kbd_backlight_button_pressed_cb), backlight);

    g_signal_connect (backlight->priv->power, "on-battery-changed",
                      G_CALLBACK (xfpm_kbd_backlight_on_battery_changed_cb), backlight);

    g_object_get (G_OBJECT (backlight->priv->power),
                  "on-battery", &backlight->priv->on_battery,
                  NULL);

    g_signal_connect_swapped (backlight->priv->conf, "notify::" KBD_BRIGHTNESS_ON_AC_TIMEOUT,
            G_CALLBACK (xfpm_kbd_backlight_inactivity_on_ac_settings_changed), backlight);

    g_signal_connect_swapped (backlight->priv->conf, "notify::" KBD_BRIGHTNESS_ON_BATTERY_TIMEOUT,
            G_CALLBACK (xfpm_kbd_backlight_inactivity_on_battery_settings_changed), backlight);

    g_signal_connect_swapped (backlight->priv->conf, "notify::" KBD_BRIGHTNESS_LEVEL_ON_AC,
            G_CALLBACK (xfpm_kbd_backlight_brightness_on_ac_settings_changed), backlight);

    g_signal_connect_swapped (backlight->priv->conf, "notify::" KBD_BRIGHTNESS_LEVEL_ON_BATTERY,
            G_CALLBACK (xfpm_kbd_backlight_brightness_on_battery_settings_changed), backlight);

    xfpm_kbd_backlight_set_timeouts (backlight);
    xfpm_kbd_backlight_set_brightness(backlight);

    backlight->priv->last_level = xfpm_kbd_backlight_get_level (backlight);

out:
    ;
}


static void
xfpm_kbd_backlight_finalize (GObject *object)
{
    XfpmKbdBacklight *backlight = NULL;

    backlight = XFPM_KBD_BACKLIGHT (object);

    if ( backlight->priv->idle )
        g_object_unref (backlight->priv->idle);

    if ( backlight->priv->conf )
        g_object_unref (backlight->priv->conf);

    if ( backlight->priv->power )
        g_object_unref (backlight->priv->power );

    if ( backlight->priv->button )
        g_object_unref (backlight->priv->button);

    if ( backlight->priv->notify )
        g_object_unref (backlight->priv->notify);

    if ( backlight->priv->n )
        g_object_unref (backlight->priv->n);

    if ( backlight->priv->proxy )
        g_object_unref (backlight->priv->proxy);

    if ( backlight->priv->bus )
        g_object_unref (backlight->priv->bus);

    G_OBJECT_CLASS (xfpm_kbd_backlight_parent_class)->finalize (object);
}


XfpmKbdBacklight *
xfpm_kbd_backlight_new (void)
{
    XfpmKbdBacklight *backlight = NULL;
    backlight = g_object_new (XFPM_TYPE_KBD_BACKLIGHT, NULL);
    return backlight;
}


gboolean xfpm_kbd_backlight_has_hw (XfpmKbdBacklight *backlight)
{
    return ( backlight->priv->proxy == NULL ) ? FALSE : TRUE;
}

gint xfpm_kbd_backlight_get_max_level (XfpmKbdBacklight *backlight)
{
    return backlight->priv->max_level;
}