/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One-shot client of public libssc APIs; no private QMI API or daemon. */
#include <libssc.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    GMainLoop *loop;
    GCancellable *cancellable;
    SSCSensor *sensor;
    guint watchdog;
    guint timer;
    gulong report_handler;
    gint seconds;
    gint result;
    gboolean stopping;
    guint decoded;
} Client;

static gboolean
print_report (guint message_id, GBytes *payload)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GVariant) fields = ssc_sensor_decode_vector_report (message_id, payload, &error);
    g_autofree gchar *printed = fields ? g_variant_print (fields, TRUE) : NULL;
    gsize length;
    const guint8 *bytes = g_bytes_get_data (payload, &length);

    printf ("message=%u bytes=%" G_GSIZE_FORMAT, message_id, length);
    if (fields)
        printf (" fields=%s", printed);
    else
        printf (" decode_error=%s", error->message);
    printf (" raw_hex=");
    for (gsize i = 0; i < length; i++)
        printf ("%02x", bytes[i]);
    putchar ('\n');
    fflush (stdout);
    return fields != NULL;
}

static void
report_received (SSCSensor *sensor, guint message_id, GBytes *payload, Client *client)
{
    if (print_report (message_id, payload))
        client->decoded++;
}

static gboolean stop (gpointer data);

static gboolean
watchdog (gpointer data)
{
    Client *client = data;

    client->watchdog = 0;
    client->result = 2;
    if (!client->stopping) {
        fprintf (stderr, "initialization/open timed out; cancelling and closing\n");
        stop (client);
    } else {
        fprintf (stderr, "cancellation/close timed out; sensor close is unconfirmed\n");
        g_cancellable_cancel (client->cancellable);
        g_main_loop_quit (client->loop);
    }
    return G_SOURCE_REMOVE;
}

static void
closed (GObject *object, GAsyncResult *result, gpointer data)
{
    Client *client = data;
    g_autoptr (GError) error = NULL;

    if (!ssc_sensor_close_finish (SSC_SENSOR (object), result, &error)) {
        client->result = 2;
        fprintf (stderr, "close failed: %s\n", error ? error->message : "no error details");
    } else {
        printf ("closed=true decoded_reports=%u\n", client->decoded);
    }
    g_main_loop_quit (client->loop);
}

static gboolean
stop (gpointer data)
{
    Client *client = data;

    if (client->stopping)
        return G_SOURCE_REMOVE;
    client->stopping = TRUE;
    g_cancellable_cancel (client->cancellable);
    if (client->watchdog) {
        g_source_remove (client->watchdog);
        client->watchdog = 0;
    }
    client->watchdog = g_timeout_add_seconds (5, watchdog, client);
    if (client->sensor)
        ssc_sensor_close (client->sensor, NULL, closed, client);
    /* Discovery may finish after cancellation. Keep its callback context alive
     * until it returns or the bounded cancellation watchdog expires. */
    return G_SOURCE_REMOVE;
}

static gboolean
timed_stop (gpointer data)
{
    Client *client = data;
    client->timer = 0;
    return stop (data);
}

static gboolean
signal_stop (gpointer data)
{
    stop (data);
    return G_SOURCE_CONTINUE;
}

static void
opened (GObject *object, GAsyncResult *result, gpointer data)
{
    Client *client = data;
    g_autoptr (GError) error = NULL;

    if (!ssc_sensor_open_finish (SSC_SENSOR (object), result, &error)) {
        client->result = 2;
        fprintf (stderr, "open failed: %s\n", error ? error->message : "no error details");
        stop (client);
        return;
    }
    if (client->stopping)
        return;
    if (client->watchdog) {
        g_source_remove (client->watchdog);
        client->watchdog = 0;
    }
    client->timer = g_timeout_add_seconds (client->seconds, timed_stop, client);
}

static void
created (GObject *object, GAsyncResult *result, gpointer data)
{
    Client *client = data;
    g_autoptr (GError) error = NULL;
    g_autoptr (SSCSensor) discovered = ssc_sensor_new_finish (result, &error);

    if (!discovered) {
        client->result = 2;
        fprintf (stderr, "sensor discovery failed: %s\n", error ? error->message : "no error details");
        g_main_loop_quit (client->loop);
        return;
    }
    client->sensor = g_steal_pointer (&discovered);
    if (client->stopping) {
        fprintf (stderr, "discovery completed after stop; closing without opening\n");
        ssc_sensor_close (client->sensor, NULL, closed, client);
        return;
    }
    client->report_handler = g_signal_connect (client->sensor, "raw-report",
                                               G_CALLBACK (report_received), client);
    ssc_sensor_open (client->sensor, client->cancellable, opened, client);
}

int
main (int argc, char **argv)
{
    g_autofree gchar *sensor = NULL;
    g_autofree gchar *hex = NULL;
    gint decode = 0, seconds = 3;
    g_autoptr (GError) error = NULL;
    g_autoptr (GOptionContext) options = g_option_context_new ("- original SSC vector fields, unknown units");
    GOptionEntry entries[] = {
        {"sensor", 0, 0, G_OPTION_ARG_STRING, &sensor, "Subscribe to the exact public SSC data type", "TYPE"},
        {"seconds", 0, 0, G_OPTION_ARG_INT, &seconds, "Read for 1..30 seconds after open completes", "N"},
        {"decode", 0, 0, G_OPTION_ARG_INT, &decode, "Offline decode of original SSC event message ID", "ID"},
        {"payload-hex", 0, 0, G_OPTION_ARG_STRING, &hex, "Offline original protobuf payload", "HEX"},
        {NULL}
    };

    g_option_context_add_main_entries (options, entries, NULL);
    if (!g_option_context_parse (options, &argc, &argv, &error)) {
        fprintf (stderr, "%s\n", error->message);
        return 2;
    }
    if (hex) {
        g_autofree guint8 *bytes = NULL;
        g_autoptr (GBytes) payload = NULL;
        gsize chars = strlen (hex);

        if (sensor || decode <= 0 || chars == 0 || chars % 2 || chars > 131072)
            return 2;
        bytes = g_malloc (chars / 2);
        for (gsize i = 0; i < chars; i += 2) {
            gint high = g_ascii_xdigit_value (hex[i]), low = g_ascii_xdigit_value (hex[i + 1]);
            if (high < 0 || low < 0)
                return 2;
            bytes[i / 2] = (high << 4) | low;
        }
        payload = g_bytes_new (bytes, chars / 2);
        return print_report (decode, payload) ? 0 : 2;
    }
    if (!sensor || decode != 0 || seconds < 1 || seconds > 30)
        return 2;
    Client client = {.loop = g_main_loop_new (NULL, FALSE),
                     .cancellable = g_cancellable_new (), .seconds = seconds};
    guint interrupt_id = g_unix_signal_add (SIGINT, signal_stop, &client);
    guint terminate_id = g_unix_signal_add (SIGTERM, signal_stop, &client);

    client.watchdog = g_timeout_add_seconds (10, watchdog, &client);
    ssc_sensor_new (sensor, client.cancellable, created, &client);
    g_main_loop_run (client.loop);
    if (client.watchdog)
        g_source_remove (client.watchdog);
    if (client.timer)
        g_source_remove (client.timer);
    g_source_remove (interrupt_id);
    g_source_remove (terminate_id);
    if (client.sensor && client.report_handler)
        g_signal_handler_disconnect (client.sensor, client.report_handler);
    g_clear_object (&client.sensor);
    g_clear_object (&client.cancellable);
    g_main_loop_unref (client.loop);
    return client.result;
}
