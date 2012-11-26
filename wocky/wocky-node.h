/*
 * wocky-node.h - Header for Wocky xmpp nodes
 * Copyright (C) 2006-2010 Collabora Ltd.
 * @author Sjoerd Simons <sjoerd@luon.net>
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
#if !defined (WOCKY_H_INSIDE) && !defined (WOCKY_COMPILATION)
# error "Only <wocky/wocky.h> can be included directly."
#endif

#ifndef __WOCKY__NODE_H__
#define __WOCKY__NODE_H__

#include <glib.h>

#include "wocky-types.h"

G_BEGIN_DECLS

/**
 * WockyNodeBuildTag:
 * @WOCKY_NODE_START: Start of a node
 * @WOCKY_NODE_TEXT: Text content of a node
 * @WOCKY_NODE_END: End of a node
 * @WOCKY_NODE_ATTRIBUTE: A node attribute
 * @WOCKY_NODE_XMLNS: A node XML namespace
 * @WOCKY_NODE_ASSIGN_TO: a #WockyNode to assign
 *
 * Tags for building a stanza using wocky_stanza_build() or
 * wocky_node_add_build().
 */
typedef enum
{
  WOCKY_NODE_START = '(',
  WOCKY_NODE_TEXT = '$',
  WOCKY_NODE_END = 41, /* this is actually ')', but gtk-doc is broken: bgo#644291 */
  WOCKY_NODE_ATTRIBUTE = '@',
  WOCKY_NODE_XMLNS = ':',
  WOCKY_NODE_ASSIGN_TO = '*',
  WOCKY_NODE_LANGUAGE = '#'
} WockyNodeBuildTag;

typedef struct _WockyNode WockyNode;

/**
 * WockyNode:
 * @name: name of the node
 * @content: content of the node
 *
 * A single #WockyNode structure that relates to an element in an XMPP
 * stanza.
 */
struct _WockyNode {
  gchar *name;
  gchar *content;

  /*< private >*/
  gchar *language;
  GQuark ns;
  GSList *attributes;
  GSList *children;
};

/**
 * wocky_node_each_attr_func:
 * @key: the attribute's key
 * @value: the attribute's value
 * @pref: the attribute's prefix
 * @ns: the attribute's namespace
 * @user_data: user data passed to wocky_node_each_attribute()
 *
 * Specifies the type of functions passed to wocky_node_each_attribute().
 *
 * Returns: %FALSE to stop further attributes from being examined.
 */
typedef gboolean (*wocky_node_each_attr_func) (const gchar *key,
    const gchar *value, const gchar *pref, const gchar *ns, gpointer user_data);

/**
 * wocky_node_each_child_func:
 * @node: a #WockyNode
 * @user_data: user data passed to wocky_node_each_child()
 *
 * Specifies the type of functions passed to wocky_node_each_child().
 *
 * Returns: %FALSE to stop further children from being examined.
 */
typedef gboolean (*wocky_node_each_child_func) (WockyNode *node,
    gpointer user_data);

void wocky_node_each_attribute (WockyNode *node,
    wocky_node_each_attr_func func, gpointer user_data);

void wocky_node_each_child (WockyNode *node,
    wocky_node_each_child_func func, gpointer user_data);

const gchar *wocky_node_get_attribute (WockyNode *node,
    const gchar *key);

const gchar *wocky_node_get_attribute_ns (WockyNode *node,
    const gchar *key, const gchar *ns);

void  wocky_node_set_attribute (WockyNode *node, const gchar *key,
    const gchar *value);

void  wocky_node_set_attributes (WockyNode *node, const gchar *key,
    ...);

void  wocky_node_set_attribute_ns (WockyNode *node,
    const gchar *key, const gchar *value, const gchar *ns);

/* Set attribute with the given size for the value */
void wocky_node_set_attribute_n (WockyNode *node, const gchar *key,
    const gchar *value, gsize value_size);

void wocky_node_set_attribute_n_ns (WockyNode *node,
    const gchar *key, const gchar *value, gsize value_size, const gchar *ns);

/* namespaced attributes: when we want to override autogenerated prefixes */
const gchar *wocky_node_attribute_ns_get_prefix_from_urn (const gchar *urn);
const gchar *wocky_node_attribute_ns_get_prefix_from_quark (GQuark ns);
void wocky_node_attribute_ns_set_prefix (GQuark ns, const gchar *prefix);

/* Getting children */
WockyNode *wocky_node_get_child (WockyNode *node,
    const gchar *name);

WockyNode *wocky_node_get_child_ns (WockyNode *node,
    const gchar *name, const gchar *ns);

WockyNode *wocky_node_get_first_child (WockyNode *node);
WockyNode *wocky_node_get_first_child_ns (WockyNode *node,
    const gchar *ns);

/* Getting content from children */
const gchar *wocky_node_get_content_from_child (WockyNode *node,
    const gchar *name);

const gchar *wocky_node_get_content_from_child_ns (WockyNode *node,
    const gchar *name,
    const gchar *ns);

/* Creating child nodes */
WockyNode *wocky_node_add_child (WockyNode *node,
    const gchar *name);

WockyNode *wocky_node_add_child_ns (WockyNode *node,
    const gchar *name, const gchar *ns);

WockyNode *wocky_node_add_child_ns_q (WockyNode *node,
    const gchar *name, GQuark ns);

WockyNode *wocky_node_add_child_with_content (WockyNode *node,
    const gchar *name, const char *content);

WockyNode *wocky_node_add_child_with_content_ns (
    WockyNode *node, const gchar *name, const gchar *content,
    const gchar *ns);

WockyNode *wocky_node_add_child_with_content_ns_q (
    WockyNode *node, const gchar *name, const gchar *content,
    GQuark ns);

/* Getting namespaces */
const gchar *wocky_node_get_ns (WockyNode *node);
gboolean wocky_node_has_ns (WockyNode *node, const gchar *ns);
gboolean wocky_node_has_ns_q (WockyNode *node, GQuark ns);

/* Matching element name and namespace */
gboolean wocky_node_matches_q (
    WockyNode *node,
    const gchar *name,
    GQuark ns);
gboolean wocky_node_matches (
    WockyNode *node,
    const gchar *name,
    const gchar *ns);

/* Setting/Getting language */
const gchar *wocky_node_get_language (WockyNode *node);
void wocky_node_set_language (WockyNode *node, const gchar *lang);
void wocky_node_set_language_n (WockyNode *node, const gchar *lang,
    gsize lang_size);


/* Setting or adding content */
void wocky_node_set_content (WockyNode *node, const gchar *content);
void wocky_node_append_content (WockyNode *node,
    const gchar *content);

void wocky_node_append_content_n (WockyNode *node,
    const gchar *content, gsize size);

/* Return a reading friendly representation of a node and its children.
 * Should be use for debugging purpose only. */
gchar *wocky_node_to_string (WockyNode *node);

/* Create a new standalone node, usually only used by the stanza object */
WockyNode *wocky_node_new (const char *name, const gchar *ns);

/* Frees the node and all it's children! */
void wocky_node_free (WockyNode *node);

/* Compare two nodes and all their children */
gboolean wocky_node_equal (WockyNode *node0,
    WockyNode *node1);

gboolean wocky_node_is_superset (WockyNode *node,
    WockyNode *subset);

/**
 * WockyNodeIter:
 *
 * Iterate over a node's children. See wocky_node_iter_init() for more
 * details.
 */
typedef struct {
  /*<private>*/
  WockyNode *node;
  GSList *pending;
  GSList *current;
  const gchar *name;
  GQuark ns;
} WockyNodeIter;

void wocky_node_iter_init (WockyNodeIter *iter,
    WockyNode *node,
    const gchar *name,
    const gchar *ns);

gboolean wocky_node_iter_next (WockyNodeIter *iter,
    WockyNode **next);

void wocky_node_iter_remove (WockyNodeIter *iter);

void wocky_node_add_build (WockyNode *node,
    ...) G_GNUC_NULL_TERMINATED;

void wocky_node_add_build_va (WockyNode *node,
    va_list va);

void wocky_node_add_node_tree (WockyNode *node, WockyNodeTree *tree);
void wocky_node_prepend_node_tree (
    WockyNode *node,
    WockyNodeTree *tree);

void wocky_node_init (void);
void wocky_node_deinit (void);

G_END_DECLS

#endif /* #ifndef __WOCKY__NODE_H__*/
