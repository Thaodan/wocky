/*
 * wocky-porter.c - Source for WockyPorter
 * Copyright (C) 2009 Collabora Ltd.
 * @author Guillaume Desmottes <guillaume.desmottes@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * SECTION: wocky-porter
 * @title: WockyPorter
 * @short_description: Wrapper around a #WockyXmppConnection providing a
 * higher level API.
 *
 * Sends and receives #WockyXmppStanza from an underlying
 * #WockyXmppConnection.
 */


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#include "wocky-porter.h"
#include "wocky-signals-marshal.h"
#include "wocky-utils.h"

#define DEBUG_FLAG DEBUG_PORTER
#include "wocky-debug.h"

G_DEFINE_TYPE(WockyPorter, wocky_porter, G_TYPE_OBJECT)

/* properties */
enum
{
  PROP_CONNECTION = 1,
};

/* signal enum */
enum
{
    REMOTE_CLOSED,
    REMOTE_ERROR,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* private structure */
typedef struct _WockyPorterPrivate WockyPorterPrivate;

struct _WockyPorterPrivate
{
  gboolean dispose_has_run;

  /* Queue of (sending_queue_elem *) */
  GQueue *sending_queue;
  GCancellable *receive_cancellable;

  GSimpleAsyncResult *close_result;
  gboolean remote_closed;
  gboolean local_closed;
  GCancellable *close_cancellable;

  /* guint => owned (StanzaHandler *) */
  GHashTable *handlers_by_id;
  /* Sort listed (by decreasing priority) of borrowed (StanzaHandler *) */
  GList *handlers;
  guint next_handler_id;
  /* (const gchar *) => owned (StanzaIqHandler *)
   * This key is the ID of the IQ */
  GHashTable *iq_reply_handlers;

  WockyXmppConnection *connection;
};

/**
 * wocky_porter_error_quark
 *
 * Get the error quark used by the porter.
 *
 * Returns: the quark for porter errors.
 */
GQuark
wocky_porter_error_quark (void)
{
  static GQuark quark = 0;

  if (quark == 0)
    quark = g_quark_from_static_string ("wocky-porter-error");

  return quark;
}

#define WOCKY_PORTER_GET_PRIVATE(o)  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((o), WOCKY_TYPE_PORTER, \
    WockyPorterPrivate))

typedef struct
{
  WockyPorter *self;
  WockyXmppStanza *stanza;
  GCancellable *cancellable;
  GSimpleAsyncResult *result;
  gulong cancelled_sig_id;
} sending_queue_elem;

static sending_queue_elem *
sending_queue_elem_new (WockyPorter *self,
  WockyXmppStanza *stanza,
  GCancellable *cancellable,
  GAsyncReadyCallback callback,
  gpointer user_data)
{
  sending_queue_elem *elem = g_slice_new0 (sending_queue_elem);

  elem->self = self;
  elem->stanza = g_object_ref (stanza);
  if (cancellable != NULL)
    elem->cancellable = g_object_ref (cancellable);

  elem->result = g_simple_async_result_new (G_OBJECT (self),
    callback, user_data, wocky_porter_send_finish);

  return elem;
}

static void
sending_queue_elem_free (sending_queue_elem *elem)
{
  g_object_unref (elem->stanza);
  if (elem->cancellable != NULL)
    {
      g_object_unref (elem->cancellable);
      if (elem->cancelled_sig_id != 0)
        g_signal_handler_disconnect (elem->cancellable, elem->cancelled_sig_id);
      /* FIXME: we should use g_cancellable_disconnect but it raises a dead
       * lock (#587300) */
    }
  g_object_unref (elem->result);

  g_slice_free (sending_queue_elem, elem);
}

typedef struct
{
  WockyStanzaType type;
  WockyStanzaSubType sub_type;
  gchar *node;
  gchar *domain;
  gchar *resource;
  guint priority;
  WockyXmppStanza *match;
  WockyPorterHandlerFunc callback;
  gpointer user_data;
} StanzaHandler;

static StanzaHandler *
stanza_handler_new (
    WockyStanzaType type,
    WockyStanzaSubType sub_type,
    const gchar *from,
    guint priority,
    WockyXmppStanza *stanza,
    WockyPorterHandlerFunc callback,
    gpointer user_data)
{
  StanzaHandler *result = g_slice_new0 (StanzaHandler);

  result->type = type;
  result->sub_type = sub_type;
  result->priority = priority;
  result->callback = callback;
  result->user_data = user_data;
  result->match = g_object_ref (stanza);

  if (from != NULL)
    {
      wocky_decode_jid (from, &(result->node),
          &(result->domain), &(result->resource));
    }

  return result;
}

static void
stanza_handler_free (StanzaHandler *handler)
{
  g_free (handler->node);
  g_free (handler->domain);
  g_free (handler->resource);
  g_object_unref (handler->match);
  g_slice_free (StanzaHandler, handler);
}

typedef struct
{
  WockyPorter *self;
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  gulong cancelled_sig_id;
  gchar *recipient;
} StanzaIqHandler;

static StanzaIqHandler *
stanza_iq_handler_new (WockyPorter *self,
    GSimpleAsyncResult *result,
    GCancellable *cancellable,
    const gchar *recipient)
{
  StanzaIqHandler *handler = g_slice_new0 (StanzaIqHandler);

  handler->self = self;
  handler->result = result;
  if (cancellable != NULL)
    handler->cancellable = g_object_ref (cancellable);
  handler->recipient = g_strdup (recipient);

  return handler;
}

static void
stanza_iq_handler_free (StanzaIqHandler *handler)
{
  g_object_unref (handler->result);
  if (handler->cancellable != NULL)
    {
      g_signal_handler_disconnect (handler->cancellable,
          handler->cancelled_sig_id);
      g_object_unref (handler->cancellable);
    }
  g_free (handler->recipient);
  g_slice_free (StanzaIqHandler, handler);
}

static void send_stanza_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data);

static void send_close (WockyPorter *self);

static gboolean handle_iq_reply (WockyPorter *self,
    WockyXmppStanza *reply,
    gpointer user_data);

static void
wocky_porter_init (WockyPorter *obj)
{
  WockyPorter *self = WOCKY_PORTER (obj);
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);

  priv->sending_queue = g_queue_new ();

  priv->handlers_by_id = g_hash_table_new_full (g_direct_hash, g_direct_equal,
      NULL, (GDestroyNotify) stanza_handler_free);
  priv->next_handler_id = 0;
  priv->handlers = NULL;

  priv->iq_reply_handlers = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) stanza_iq_handler_free);
}

static void wocky_porter_dispose (GObject *object);
static void wocky_porter_finalize (GObject *object);

static void
wocky_porter_set_property (GObject *object,
    guint property_id,
    const GValue *value,
    GParamSpec *pspec)
{
  WockyPorter *connection = WOCKY_PORTER (object);
  WockyPorterPrivate *priv =
      WOCKY_PORTER_GET_PRIVATE (connection);

  switch (property_id)
    {
      case PROP_CONNECTION:
        g_assert (priv->connection == NULL);
        priv->connection = g_value_dup_object (value);
        g_assert (priv->connection != NULL);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
wocky_porter_get_property (GObject *object,
    guint property_id,
    GValue *value,
    GParamSpec *pspec)
{
  WockyPorter *connection = WOCKY_PORTER (object);
  WockyPorterPrivate *priv =
      WOCKY_PORTER_GET_PRIVATE (connection);

  switch (property_id)
    {
      case PROP_CONNECTION:
        g_value_set_object (value, priv->connection);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
wocky_porter_constructed (GObject *object)
{
  WockyPorter *self = WOCKY_PORTER (object);
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);

  g_assert (priv->connection != NULL);

  /* Register the IQ reply handler */
  wocky_porter_register_handler (self,
      WOCKY_STANZA_TYPE_IQ, WOCKY_STANZA_SUB_TYPE_RESULT, NULL,
      WOCKY_PORTER_HANDLER_PRIORITY_MAX,
      handle_iq_reply, self, WOCKY_STANZA_END);

  wocky_porter_register_handler (self,
      WOCKY_STANZA_TYPE_IQ, WOCKY_STANZA_SUB_TYPE_ERROR, NULL,
      WOCKY_PORTER_HANDLER_PRIORITY_MAX,
      handle_iq_reply, self, WOCKY_STANZA_END);
}

static void
wocky_porter_class_init (
    WockyPorterClass *wocky_porter_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (wocky_porter_class);
  GParamSpec *spec;

  g_type_class_add_private (wocky_porter_class,
      sizeof (WockyPorterPrivate));

  object_class->constructed = wocky_porter_constructed;
  object_class->set_property = wocky_porter_set_property;
  object_class->get_property = wocky_porter_get_property;
  object_class->dispose = wocky_porter_dispose;
  object_class->finalize = wocky_porter_finalize;

  signals[REMOTE_CLOSED] = g_signal_new ("remote-closed",
      G_OBJECT_CLASS_TYPE (wocky_porter_class),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE, 0);

  signals[REMOTE_ERROR] = g_signal_new ("remote-error",
      G_OBJECT_CLASS_TYPE (wocky_porter_class),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      _wocky_signals_marshal_VOID__UINT_INT_STRING,
      G_TYPE_NONE, 3, G_TYPE_UINT, G_TYPE_INT, G_TYPE_STRING);

  spec = g_param_spec_object ("connection", "XMPP connection",
    "the XMPP connection used by this porter",
    WOCKY_TYPE_XMPP_CONNECTION,
    G_PARAM_READWRITE |
    G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_property (object_class, PROP_CONNECTION, spec);
}

void
wocky_porter_dispose (GObject *object)
{
  WockyPorter *self = WOCKY_PORTER (object);
  WockyPorterPrivate *priv =
      WOCKY_PORTER_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (priv->connection != NULL)
    {
      g_object_unref (priv->connection);
      priv->connection = NULL;
    }

  if (priv->receive_cancellable != NULL)
    {
      g_warning ("Disposing an open XMPP porter");
      g_cancellable_cancel (priv->receive_cancellable);
      g_object_unref (priv->receive_cancellable);
      priv->receive_cancellable = NULL;
    }

  if (priv->close_result != NULL)
    {
      g_object_unref (priv->close_result);
      priv->close_result = NULL;
    }

  if (G_OBJECT_CLASS (wocky_porter_parent_class)->dispose)
    G_OBJECT_CLASS (wocky_porter_parent_class)->dispose (object);
}

void
wocky_porter_finalize (GObject *object)
{
  WockyPorter *self = WOCKY_PORTER (object);
  WockyPorterPrivate *priv =
      WOCKY_PORTER_GET_PRIVATE (self);

  /* sending_queue_elem keeps a ref on the Porter (through the
   * GSimpleAsyncResult) so it shouldn't be destroyed while there are
   * elements in the queue. */
  g_assert_cmpuint (g_queue_get_length (priv->sending_queue), ==, 0);
  g_queue_free (priv->sending_queue);

  g_hash_table_destroy (priv->handlers_by_id);
  g_list_free (priv->handlers);
  g_hash_table_destroy (priv->iq_reply_handlers);

  G_OBJECT_CLASS (wocky_porter_parent_class)->finalize (object);
}

/**
 * wocky_porter_new:
 * @connection: #WockyXmppConnection which will be used to receive and send
 * #WockyXmppStanza
 *
 * Convenience function to create a new #WockyPorter.
 *
 * Returns: a new #WockyPorter.
 */
WockyPorter *
wocky_porter_new (WockyXmppConnection *connection)
{
  WockyPorter *result;

  result = g_object_new (WOCKY_TYPE_PORTER,
    "connection", connection,
    NULL);

  return result;
}

static void
send_head_stanza (WockyPorter *self)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  sending_queue_elem *elem;

  elem = g_queue_peek_head (priv->sending_queue);
  if (elem == NULL)
    /* Nothing to send */
    return;

  if (elem->cancelled_sig_id != 0)
    {
      /* We are going to start sending the stanza. Lower layers are now
       * responsible of handling the cancellable. */
      g_signal_handler_disconnect (elem->cancellable, elem->cancelled_sig_id);
      elem->cancelled_sig_id = 0;
    }

  wocky_xmpp_connection_send_stanza_async (priv->connection,
      elem->stanza, elem->cancellable, send_stanza_cb, self);
}

static void
send_stanza_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  WockyPorter *self = WOCKY_PORTER (user_data);
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  sending_queue_elem *elem;
  GError *error = NULL;

  elem = g_queue_pop_head (priv->sending_queue);

  if (!wocky_xmpp_connection_send_stanza_finish (
        WOCKY_XMPP_CONNECTION (source), res, &error))
    {
      /* Sending failed. Cancel this sending operation and all the others
       * pending ones as we won't be able to send any more stanza. */

      while (elem != NULL)
        {
          g_simple_async_result_set_from_error (elem->result, error);
          g_simple_async_result_complete (elem->result);
          sending_queue_elem_free (elem);
          elem = g_queue_pop_head (priv->sending_queue);
        }

      g_error_free (error);
    }
  else
    {
      g_assert (elem != NULL);
      g_simple_async_result_complete (elem->result);

      sending_queue_elem_free (elem);

      if (g_queue_get_length (priv->sending_queue) > 0)
        {
          /* Send next stanza */
          send_head_stanza (self);
        }
    }

  if (priv->close_result != NULL &&
      g_queue_get_length (priv->sending_queue) == 0)
    {
      /* Queue is empty and we are waiting to close the connection. */
      DEBUG ("Queue has been flushed. Closing the connection.");
      send_close (self);
    }
}

static void
send_cancelled_cb (GCancellable *cancellable,
    gpointer user_data)
{
  sending_queue_elem *elem = (sending_queue_elem *) user_data;
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (
      elem->self);
  GError error = { G_IO_ERROR, G_IO_ERROR_CANCELLED, "Sending was cancelled" };

  g_simple_async_result_set_from_error (elem->result, &error);
  g_simple_async_result_complete_in_idle (elem->result);

  g_queue_remove (priv->sending_queue, elem);
  sending_queue_elem_free (elem);
}

void
wocky_porter_send_async (WockyPorter *self,
    WockyXmppStanza *stanza,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  sending_queue_elem *elem;

  if (priv->close_result != NULL)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (self), callback,
          user_data, WOCKY_PORTER_ERROR,
          WOCKY_PORTER_ERROR_CLOSING,
          "Porter is closing");
      return;
    }

  elem = sending_queue_elem_new (self, stanza, cancellable, callback,
      user_data);
  g_queue_push_tail (priv->sending_queue, elem);

  if (g_queue_get_length (priv->sending_queue) == 1)
    {
      send_head_stanza (self);
    }
  else if (cancellable != NULL)
    {
      elem->cancelled_sig_id = g_cancellable_connect (cancellable,
          G_CALLBACK (send_cancelled_cb), elem, NULL);
    }
}

gboolean
wocky_porter_send_finish (WockyPorter *self,
    GAsyncResult *result,
    GError **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
      error))
    return FALSE;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
    G_OBJECT (self), wocky_porter_send_finish), FALSE);

  return TRUE;
}

void
wocky_porter_send (WockyPorter *self,
    WockyXmppStanza *stanza)
{
  wocky_porter_send_async (self, stanza, NULL, NULL, NULL);
}

static void receive_stanza (WockyPorter *self);

static void
complete_close (WockyPorter *self)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);

  if (g_cancellable_is_cancelled (priv->close_cancellable))
    {
      g_simple_async_result_set_error (priv->close_result, G_IO_ERROR,
          G_IO_ERROR_CANCELLED, "closing operation was cancelled");
    }

  g_simple_async_result_complete (priv->close_result);

  g_object_unref (priv->close_result);
  priv->close_result = NULL;
  priv->close_cancellable = NULL;
}

static gboolean
handle_iq_reply (WockyPorter *self,
    WockyXmppStanza *reply,
    gpointer user_data)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  const gchar *id, *from;
  StanzaIqHandler *handler;

  id = wocky_xmpp_node_get_attribute (reply->node, "id");
  if (id == NULL)
    {
      DEBUG ("Ignoring reply without IQ id");
      return FALSE;
    }

  handler = g_hash_table_lookup (priv->iq_reply_handlers, id);

  if (handler == NULL)
    {
      DEBUG ("Ignored IQ reply");
      return FALSE;
    }

  from = wocky_xmpp_node_get_attribute (reply->node, "from");
  /* FIXME: If handler->recipient is NULL, we should check if the 'from' is
   * either NULL, our bare jid or our full jid. */
  if (handler->recipient != NULL &&
      wocky_strdiff (from, handler->recipient))
    {
      DEBUG ("%s attempts to spoof an IQ reply", from);
      return FALSE;
    }

  if (!g_cancellable_is_cancelled (handler->cancellable))
    {
      g_simple_async_result_set_op_res_gpointer (handler->result,
          reply, NULL);
      g_simple_async_result_complete (handler->result);
    }

  g_hash_table_remove (priv->iq_reply_handlers, id);
  return TRUE;
}

static void
handle_stanza (WockyPorter *self,
    WockyXmppStanza *stanza)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  GList *l;
  const gchar *from;
  WockyStanzaType type;
  WockyStanzaSubType sub_type;
  gchar *node = NULL, *domain = NULL, *resource = NULL;

  wocky_xmpp_stanza_get_type_info (stanza, &type, &sub_type);

  /* The from attribute of the stanza need not always be present, for example
   * when receiving roster items, so don't enforce it. */
  from = wocky_xmpp_node_get_attribute (stanza->node, "from");

  if (from != NULL)
    wocky_decode_jid (from, &node, &domain, &resource);

  for (l = priv->handlers; l != NULL; l = g_list_next (l))
    {
      StanzaHandler *handler = (StanzaHandler *) l->data;

      if (type != handler->type)
        continue;

      if (sub_type != handler->sub_type &&
          handler->sub_type != WOCKY_STANZA_SUB_TYPE_NONE)
        continue;

      if (handler->node != NULL)
        {
          g_assert (handler->domain != NULL);

          if (wocky_strdiff (node, handler->node))
            continue;

          if (wocky_strdiff (domain, handler->domain))
            continue;

          if (handler->resource != NULL)
            {
              /* A ressource is defined so we want to match exactly the same
               * JID */

              if (wocky_strdiff (resource, handler->resource))
                continue;
            }
        }

      /* Check if the stanza matches the pattern */
      if (!wocky_xmpp_node_is_superset (stanza->node, handler->match->node))
        continue;

      if (handler->callback (self, stanza, handler->user_data))
        goto out;
    }

  DEBUG ("Stanza not handled");
out:
  g_free (node);
  g_free (domain);
  g_free (resource);
}

static void
stanza_received_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  WockyPorter *self = WOCKY_PORTER (user_data);
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  WockyXmppStanza *stanza;
  GError *error = NULL;

  stanza = wocky_xmpp_connection_recv_stanza_finish (
      WOCKY_XMPP_CONNECTION (source), res, &error);
  if (stanza == NULL)
    {
      if (g_error_matches (error, WOCKY_XMPP_CONNECTION_ERROR,
            WOCKY_XMPP_CONNECTION_ERROR_CLOSED))
        {
          if (priv->close_result != NULL)
            {
              /* Close completed */
              complete_close (self);
            }

          DEBUG ("Remote connection has been closed");
          g_signal_emit (self, signals[REMOTE_CLOSED], 0);
        }
      else
        {
          DEBUG ("Error receiving stanza: %s\n", error->message);
          g_signal_emit (self, signals[REMOTE_ERROR], 0, error->domain,
              error->code, error->message);
        }

      if (priv->receive_cancellable != NULL)
        {
          g_object_unref (priv->receive_cancellable);
          priv->receive_cancellable = NULL;
        }

      priv->remote_closed = TRUE;
      g_error_free (error);
      return;
    }

  handle_stanza (self, stanza);

  g_object_unref (stanza);

  /* wait for next stanza */
  receive_stanza (self);
}

static void
receive_stanza (WockyPorter *self)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);

  wocky_xmpp_connection_recv_stanza_async (priv->connection,
      priv->receive_cancellable, stanza_received_cb, self);
}

void
wocky_porter_start (WockyPorter *self)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);

  if (priv->receive_cancellable != NULL)
    /* Porter has already been started */
    return;

  priv->receive_cancellable = g_cancellable_new ();

  receive_stanza (self);
}

static void
close_sent_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  WockyPorter *self = WOCKY_PORTER (user_data);
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  GError *error = NULL;

  priv->local_closed = TRUE;

  if (!wocky_xmpp_connection_send_close_finish (WOCKY_XMPP_CONNECTION (source),
        res, &error))
    {
      g_simple_async_result_set_from_error (priv->close_result, error);
      g_error_free (error);

      goto out;
    }

  if (!g_cancellable_is_cancelled (priv->close_cancellable)
      && !priv->remote_closed)
    {
      /* we'll complete the close operation once the remote side closes it's
       * connection */
       return;
    }

out:
  if (priv->close_result != NULL)
    {
      /* close operation could already be completed if the other side closes
       * before we send our close */
      complete_close (self);
    }
}

static void
send_close (WockyPorter *self)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);

  wocky_xmpp_connection_send_close_async (priv->connection,
      NULL, close_sent_cb, self);
}

void
wocky_porter_close_async (WockyPorter *self,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);

  if (priv->local_closed)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (self), callback,
          user_data, WOCKY_PORTER_ERROR,
          WOCKY_PORTER_ERROR_CLOSED,
          "Porter has already been closed");
      return;
    }

  if (priv->receive_cancellable == NULL && !priv->remote_closed)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (self), callback,
          user_data, WOCKY_PORTER_ERROR,
          WOCKY_PORTER_ERROR_NOT_STARTED,
          "Porter has not been started");
      return;
    }

  if (priv->close_result != NULL)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (self), callback,
          user_data, G_IO_ERROR,
          G_IO_ERROR_PENDING,
          "Another close operation is pending");
      return;
    }

  priv->close_result = g_simple_async_result_new (G_OBJECT (self),
    callback, user_data, wocky_porter_close_finish);

  priv->close_cancellable = cancellable;

  if (g_queue_get_length (priv->sending_queue) > 0)
    {
      DEBUG ("Sending queue is not empty. Flushing it before "
          "closing the connection.");
      return;
    }

  send_close (self);
}

gboolean
wocky_porter_close_finish (
    WockyPorter *self,
    GAsyncResult *result,
    GError **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
      error))
    return FALSE;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
    G_OBJECT (self), wocky_porter_close_finish), FALSE);

  return TRUE;
}

static gint
compare_handler (StanzaHandler *a,
    StanzaHandler *b)
{
  /* List is sorted by decreasing priority */
  if (a->priority < b->priority)
    return 1;
  else if (a->priority > b->priority)
    return -1;
  else
    return 0;
}

guint
wocky_porter_register_handler (WockyPorter *self,
    WockyStanzaType type,
    WockyStanzaSubType sub_type,
    const gchar *from,
    guint priority,
    WockyPorterHandlerFunc callback,
    gpointer user_data,
    WockyBuildTag spec,
    ...)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  StanzaHandler *handler;
  WockyXmppStanza *stanza;
  va_list ap;

  va_start (ap, spec);
  stanza = wocky_xmpp_stanza_build_va (type, WOCKY_STANZA_SUB_TYPE_NONE,
      NULL, NULL, spec, ap);
  g_assert (stanza != NULL);
  va_end (ap);

  handler = stanza_handler_new (type, sub_type, from, priority, stanza,
      callback, user_data);
  g_object_unref (stanza);

  g_hash_table_insert (priv->handlers_by_id,
      GUINT_TO_POINTER (priv->next_handler_id), handler);
  priv->handlers = g_list_insert_sorted (priv->handlers, handler,
      (GCompareFunc) compare_handler);

  return priv->next_handler_id++;
}

void
wocky_porter_unregister_handler (WockyPorter *self,
    guint id)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  StanzaHandler *handler;

  handler = g_hash_table_lookup (priv->handlers_by_id, GUINT_TO_POINTER (id));
  if (handler == NULL)
    {
      g_warning ("Trying to remove an unregistered handler: %u", id);
      return;
    }

  priv->handlers = g_list_remove (priv->handlers, handler);
  g_hash_table_remove (priv->handlers_by_id, GUINT_TO_POINTER (id));
}

static gboolean
remove_iq_reply_using_cancellable (gpointer key,
    gpointer value,
    gpointer cancellable)
{
  StanzaIqHandler *handler = (StanzaIqHandler *) value;

  return handler->cancellable == cancellable;
}

static void
send_iq_cancelled_cb (GCancellable *cancellable,
    gpointer user_data)
{
  StanzaIqHandler *handler = (StanzaIqHandler *) user_data;
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (
      handler->self);
  GError error = { G_IO_ERROR, G_IO_ERROR_CANCELLED,
      "IQ sending was cancelled" };

  g_simple_async_result_set_from_error (handler->result, &error);
  g_simple_async_result_complete (handler->result);

  /* Remove the handlers associated with this GCancellable */
  g_hash_table_foreach_remove (priv->iq_reply_handlers,
      remove_iq_reply_using_cancellable, cancellable);
}

static gboolean
remove_iq_reply (gpointer key,
    gpointer value,
    gpointer handler)
{
  return value == handler;
}

static void
iq_sent_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  WockyPorter *self = WOCKY_PORTER (source);
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  StanzaIqHandler *handler = (StanzaIqHandler *) user_data;
  GError *error = NULL;

  if (wocky_porter_send_finish (self, res, &error))
    /* IQ has been properly sent. Operation will be finished once the reply
     * received */
    return;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      /* Operation has been cancelled */
      g_error_free (error);
      return;
    }

  /* Raise an error */
  g_simple_async_result_set_from_error (handler->result, error);
  g_simple_async_result_complete (handler->result);

  g_hash_table_foreach_remove (priv->iq_reply_handlers,
      remove_iq_reply, handler);
  g_error_free (error);
}

void
wocky_porter_send_iq_async (WockyPorter *self,
    WockyXmppStanza *stanza,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data)
{
  WockyPorterPrivate *priv = WOCKY_PORTER_GET_PRIVATE (self);
  StanzaIqHandler *handler;
  const gchar *recipient;
  gchar *id = NULL;
  GSimpleAsyncResult *result;
  WockyStanzaType type;
  WockyStanzaSubType sub_type;

  wocky_xmpp_stanza_get_type_info (stanza, &type, &sub_type);

  if (type != WOCKY_STANZA_TYPE_IQ)
    goto wrong_stanza;

  if (sub_type != WOCKY_STANZA_SUB_TYPE_GET &&
      sub_type != WOCKY_STANZA_SUB_TYPE_SET)
    goto wrong_stanza;

  recipient = wocky_xmpp_node_get_attribute (stanza->node, "to");

  /* Set an unique ID */
  do
    {
      g_free (id);
      id = wocky_xmpp_connection_new_id (priv->connection);
    }
  while (g_hash_table_lookup (priv->iq_reply_handlers, id) != NULL);

  wocky_xmpp_node_set_attribute (stanza->node, "id", id);

  result = g_simple_async_result_new (G_OBJECT (self),
    callback, user_data, wocky_porter_send_iq_finish);

  handler = stanza_iq_handler_new (self, result, cancellable,
      recipient);

  if (cancellable != NULL)
    {
      handler->cancelled_sig_id = g_cancellable_connect (cancellable,
          G_CALLBACK (send_iq_cancelled_cb), handler, NULL);
    }

  g_hash_table_insert (priv->iq_reply_handlers, id, handler);

  wocky_porter_send_async (self, stanza, cancellable, iq_sent_cb,
      handler);
  return;

wrong_stanza:
  g_simple_async_report_error_in_idle (G_OBJECT (self), callback,
      user_data, WOCKY_PORTER_ERROR,
      WOCKY_PORTER_ERROR_NOT_IQ,
      "Stanza is not an IQ query");
}

WockyXmppStanza * wocky_porter_send_iq_finish (
    WockyPorter *self,
    GAsyncResult *result,
    GError **error)
{
  WockyXmppStanza *reply;

  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result),
      error))
    return NULL;

  g_return_val_if_fail (g_simple_async_result_is_valid (result,
    G_OBJECT (self), wocky_porter_send_iq_finish), NULL);

  reply = g_simple_async_result_get_op_res_gpointer (
      G_SIMPLE_ASYNC_RESULT (result));

  return g_object_ref (reply);
}