#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <glib.h>

#include <wocky/wocky-roster.h>
#include <wocky/wocky-porter.h>
#include <wocky/wocky-utils.h>
#include <wocky/wocky-xmpp-connection.h>
#include <wocky/wocky-contact.h>
#include <wocky/wocky-namespaces.h>

#include "wocky-test-stream.h"
#include "wocky-test-helper.h"

/* Test to instantiate a WockyRoster object */
static void
test_instantiation (void)
{
  WockyRoster *roster;
  WockyXmppConnection *connection;
  WockyPorter *porter;
  WockyTestStream *stream;

  stream = g_object_new (WOCKY_TYPE_TEST_STREAM, NULL);
  connection = wocky_xmpp_connection_new (stream->stream0);
  porter = wocky_porter_new (connection);

  roster = wocky_roster_new (porter);

  g_assert (roster != NULL);

  g_object_unref (roster);
  g_object_unref (porter);
  g_object_unref (connection);
  g_object_unref (stream);
}

/* Test if the Roster sends the right IQ query when fetching the roster */
static gboolean
fetch_roster_send_iq_cb (WockyPorter *porter,
    WockyXmppStanza *stanza,
    gpointer user_data)
{
  test_data_t *test = (test_data_t *) user_data;
  WockyStanzaType type;
  WockyStanzaSubType sub_type;
  WockyXmppNode *node;
  WockyXmppStanza *reply;
  const char *id;

  /* Make sure stanza is as expected. */
  wocky_xmpp_stanza_get_type_info (stanza, &type, &sub_type);

  g_assert (type == WOCKY_STANZA_TYPE_IQ);
  g_assert (sub_type == WOCKY_STANZA_SUB_TYPE_GET);

  node = wocky_xmpp_node_get_child (stanza->node, "query");

  g_assert (stanza->node != NULL);
  g_assert (!wocky_strdiff (wocky_xmpp_node_get_ns (node),
          "jabber:iq:roster"));

  id = wocky_xmpp_node_get_attribute (stanza->node, "id");
  g_assert (id != NULL);

  reply = wocky_xmpp_stanza_build (WOCKY_STANZA_TYPE_IQ,
      WOCKY_STANZA_SUB_TYPE_RESULT,
      NULL, NULL,
      WOCKY_NODE_ATTRIBUTE, "id", id,
      WOCKY_NODE, "query",
        WOCKY_NODE_XMLNS, "jabber:iq:roster",
        WOCKY_NODE, "item",
          WOCKY_NODE_ATTRIBUTE, "jid", "romeo@example.net",
          WOCKY_NODE_ATTRIBUTE, "name", "Romeo",
          WOCKY_NODE_ATTRIBUTE, "subscription", "both",
          WOCKY_NODE, "group",
            WOCKY_NODE_TEXT, "Friends",
          WOCKY_NODE_END,
        WOCKY_NODE_END,
      WOCKY_NODE_END,
      WOCKY_STANZA_END);

  wocky_porter_send (porter, reply);
  g_object_unref (reply);

  test->outstanding--;
  g_main_loop_quit (test->loop);
  return TRUE;
}

static void
fetch_roster_fetched_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  test_data_t *test = (test_data_t *) user_data;

  g_return_if_fail (wocky_roster_fetch_roster_finish (
          WOCKY_ROSTER (source_object), res, NULL));

  test->outstanding--;
  g_main_loop_quit (test->loop);
}

static void
test_fetch_roster_send_iq (void)
{
  WockyRoster *roster;
  test_data_t *test = setup_test ();

  test_open_both_connections (test);

  wocky_porter_register_handler (test->sched_out,
      WOCKY_STANZA_TYPE_IQ, WOCKY_STANZA_SUB_TYPE_GET, NULL,
      WOCKY_PORTER_HANDLER_PRIORITY_MAX,
      fetch_roster_send_iq_cb, test, WOCKY_STANZA_END);

  wocky_porter_start (test->sched_out);
  wocky_porter_start (test->sched_in);

  roster = wocky_roster_new (test->sched_in);

  wocky_roster_fetch_roster_async (roster, NULL, fetch_roster_fetched_cb, test);

  test->outstanding += 2;
  test_wait_pending (test);

  test_close_both_porters (test);
  g_object_unref (roster);
  teardown_test (test);
}

/* Test if the Roster object is properly populated when receiving its fetch
 * reply */

static WockyContact *
create_romeo (void)
{
  const gchar *groups[] = { "Friends", NULL };

  return g_object_new (WOCKY_TYPE_CONTACT,
      "jid", "romeo@example.net",
      "name", "Romeo",
      "subscription", WOCKY_ROSTER_SUBSCRIPTION_TYPE_BOTH,
      "groups", groups,
      NULL);
}

static WockyContact *
create_juliet (void)
{
  const gchar *groups[] = { "Friends", "Girlz", NULL };

  return g_object_new (WOCKY_TYPE_CONTACT,
      "jid", "juliet@example.net",
      "name", "Juliet",
      "subscription", WOCKY_ROSTER_SUBSCRIPTION_TYPE_TO,
      "groups", groups,
      NULL);
}

static int
find_contact (gconstpointer a,
    gconstpointer b)
{
  if (wocky_contact_equal (WOCKY_CONTACT (a), WOCKY_CONTACT (b)))
    return 0;

  return 1;
}

static void
fetch_roster_reply_roster_cb (GObject *source_object,
    GAsyncResult *res,
    gpointer user_data)
{
  test_data_t *test = (test_data_t *) user_data;
  WockyContact *contact;
  WockyRoster *roster = WOCKY_ROSTER (source_object);
  WockyContact *romeo, *juliet;
  GSList *contacts;

  g_return_if_fail (wocky_roster_fetch_roster_finish (roster, res, NULL));

  contacts = wocky_roster_get_all_contacts (roster);
  g_assert_cmpuint (g_slist_length (contacts), ==, 2);

  contact = wocky_roster_get_contact (roster, "romeo@example.net");
  romeo = create_romeo ();
  g_assert (wocky_contact_equal (contact, romeo));
  g_assert (g_slist_find_custom (contacts, romeo, find_contact) != NULL);
  g_object_unref (romeo);

  contact = wocky_roster_get_contact (roster, "juliet@example.net");
  juliet = create_juliet ();
  g_assert (wocky_contact_equal (contact, juliet));
  g_assert (g_slist_find_custom (contacts, juliet, find_contact) != NULL);
  g_object_unref (juliet);

  g_slist_free (contacts);
  test->outstanding--;
  g_main_loop_quit (test->loop);
}

static gboolean
fetch_roster_reply_cb (WockyPorter *porter,
    WockyXmppStanza *stanza,
    gpointer user_data)
{
  WockyXmppStanza *reply;

  /* We're acting like the server here. The client doesn't need to send a
   * "from" attribute, and in fact it doesn't when fetch_roster is called. It
   * is left up to the server to know which client is the user and then throw
   * in a correct to attribute. Here we're just adding a from attribute so the
   * IQ result builder doesn't complain. */
  if (wocky_xmpp_node_get_attribute (stanza->node, "from") == NULL)
    wocky_xmpp_node_set_attribute (stanza->node, "from",
        "juliet@example.com/balcony");

  reply = wocky_xmpp_stanza_build_iq_result (stanza,
      WOCKY_NODE, "query",
        WOCKY_NODE_XMLNS, "jabber:iq:roster",
        /* Romeo */
        WOCKY_NODE, "item",
          WOCKY_NODE_ATTRIBUTE, "jid", "romeo@example.net",
          WOCKY_NODE_ATTRIBUTE, "name", "Romeo",
          WOCKY_NODE_ATTRIBUTE, "subscription", "both",
          WOCKY_NODE, "group",
            WOCKY_NODE_TEXT, "Friends",
          WOCKY_NODE_END,
        /* Juliet */
        WOCKY_NODE_END,
        WOCKY_NODE, "item",
          WOCKY_NODE_ATTRIBUTE, "jid", "juliet@example.net",
          WOCKY_NODE_ATTRIBUTE, "name", "Juliet",
          WOCKY_NODE_ATTRIBUTE, "subscription", "to",
          WOCKY_NODE, "group",
            WOCKY_NODE_TEXT, "Friends",
          WOCKY_NODE_END,
          WOCKY_NODE, "group",
            WOCKY_NODE_TEXT, "Girlz",
          WOCKY_NODE_END,
        WOCKY_NODE_END,
      WOCKY_NODE_END,
      WOCKY_STANZA_END);

  wocky_porter_send (porter, reply);

  g_object_unref (reply);

  return TRUE;
}

static WockyRoster *
create_initial_roster (test_data_t *test)
{
  WockyRoster *roster;

  wocky_porter_register_handler (test->sched_out,
      WOCKY_STANZA_TYPE_IQ, WOCKY_STANZA_SUB_TYPE_GET, NULL,
      WOCKY_PORTER_HANDLER_PRIORITY_MAX,
      fetch_roster_reply_cb, test, WOCKY_STANZA_END);

  wocky_porter_start (test->sched_out);
  wocky_porter_start (test->sched_in);

  roster = wocky_roster_new (test->sched_in);

  wocky_roster_fetch_roster_async (roster, NULL,
      fetch_roster_reply_roster_cb, test);

  test->outstanding++;
  test_wait_pending (test);

  return roster;
}

static void
test_fetch_roster_reply (void)
{
  WockyRoster *roster;
  test_data_t *test = setup_test ();

  test_open_both_connections (test);

  roster = create_initial_roster (test);

  test_close_both_porters (test);
  g_object_unref (roster);
  teardown_test (test);
}

/* Test if roster is properly upgraded when a contact is added to it */
static WockyContact *
create_nurse (void)
{
  const gchar *groups[] = { NULL };

  return g_object_new (WOCKY_TYPE_CONTACT,
      "jid", "nurse@example.net",
      "name", "Nurse",
      "subscription", WOCKY_ROSTER_SUBSCRIPTION_TYPE_NONE,
      "groups", groups,
      NULL);
}

static void
roster_added_cb (WockyRoster *roster,
    WockyContact *contact,
    test_data_t *test)
{
  WockyContact *nurse;
  GSList *contacts;

  /* Is that the right contact? */
  nurse = create_nurse ();
  g_assert (wocky_contact_equal (contact, nurse));

  /* Check if the contact has been added to the roster */
  g_assert (wocky_roster_get_contact (roster, "nurse@example.net") == contact);
  contacts = wocky_roster_get_all_contacts (roster);
  g_assert (g_slist_find_custom (contacts, nurse, (GCompareFunc) find_contact));

  g_object_unref (nurse);
  g_slist_free (contacts);
  test->outstanding--;
  g_main_loop_quit (test->loop);
}

static void
roster_update_reply_cb (GObject *source,
    GAsyncResult *res,
    gpointer user_data)
{
  test_data_t *test = (test_data_t *) user_data;
  WockyXmppStanza *reply;
  WockyStanzaType type;
  WockyStanzaSubType sub_type;

  reply = wocky_porter_send_iq_finish (WOCKY_PORTER (source), res, NULL);
  g_assert (reply != NULL);

  wocky_xmpp_stanza_get_type_info (reply, &type, &sub_type);
  g_assert (type == WOCKY_STANZA_TYPE_IQ);
  g_assert (sub_type == WOCKY_STANZA_SUB_TYPE_RESULT);

  g_object_unref (reply);
  test->outstanding--;
  g_main_loop_quit (test->loop);
}

static void
test_roster_upgrade_add (void)
{
  WockyRoster *roster;
  test_data_t *test = setup_test ();
  WockyXmppStanza *iq;

  test_open_both_connections (test);

  roster = create_initial_roster (test);

  g_signal_connect (roster, "added", G_CALLBACK (roster_added_cb), test);

  /* server sends a roster update */
  iq = wocky_xmpp_stanza_build (WOCKY_STANZA_TYPE_IQ,
    WOCKY_STANZA_SUB_TYPE_SET, NULL, NULL,
    WOCKY_NODE, "query",
      WOCKY_NODE_XMLNS, WOCKY_XMPP_NS_ROSTER,
      WOCKY_NODE, "item",
        WOCKY_NODE_ATTRIBUTE, "jid", "nurse@example.net",
        WOCKY_NODE_ATTRIBUTE, "name", "Nurse",
        WOCKY_NODE_ATTRIBUTE, "subscription", "none",
      WOCKY_NODE_END,
    WOCKY_NODE_END,
    WOCKY_STANZA_END);

  wocky_porter_send_iq_async (test->sched_out, iq, NULL,
      roster_update_reply_cb, test);
  g_object_unref (iq);

  test->outstanding += 2;
  test_wait_pending (test);

  test_close_both_porters (test);
  g_object_unref (roster);
  teardown_test (test);
}

int
main (int argc, char **argv)
{
  int result;

  test_init (argc, argv);

  g_test_add_func ("/xmpp-roster/instantiation", test_instantiation);
  g_test_add_func ("/xmpp-roster/fetch-roster-send-iq",
      test_fetch_roster_send_iq);
  g_test_add_func ("/xmpp-roster/fetch-roster-reply", test_fetch_roster_reply);
  g_test_add_func ("/xmpp-roster/roster-upgrade-add", test_roster_upgrade_add);

  result = g_test_run ();
  test_deinit ();
  return result;
}
