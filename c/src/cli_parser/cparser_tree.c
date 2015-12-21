/*----------------------------------------------------------------------
 * This file is generated by mk_parser.py.
 *----------------------------------------------------------------------*/
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "cparser.h"
#include "cparser_priv.h"
#include "cparser_token.h"
#include "cparser_tree.h"

cparser_result_t cparser_glue_show_subscriptions(cparser_t *parser) {
  cparser_cmd_show_subscriptions(&parser->context);
  return CPARSER_OK;
}

cparser_result_t cparser_glue_show_status(cparser_t *parser) {
  cparser_cmd_show_status(&parser->context);
  return CPARSER_OK;
}

cparser_result_t cparser_glue_show_keys(cparser_t *parser) {
  cparser_cmd_show_keys(&parser->context);
  return CPARSER_OK;
}

cparser_result_t cparser_glue_subscribe_topic_scope(cparser_t *parser) {
  char *topic_val;
  char **topic_ptr = NULL;
  char *scope_val;
  char **scope_ptr = NULL;
  cparser_result_t rc;

  rc = cparser_get_string(&parser->tokens[1], &topic_val);
  assert(CPARSER_OK == rc);
  topic_ptr = &topic_val;
  rc = cparser_get_string(&parser->tokens[2], &scope_val);
  assert(CPARSER_OK == rc);
  scope_ptr = &scope_val;
  cparser_cmd_subscribe_topic_scope(&parser->context, topic_ptr,
                                    scope_ptr);
  return CPARSER_OK;
}

cparser_result_t cparser_glue_no_subscribe_topic_scope(cparser_t *parser) {
  char *topic_val;
  char **topic_ptr = NULL;
  char *scope_val;
  char **scope_ptr = NULL;
  cparser_result_t rc;

  rc = cparser_get_string(&parser->tokens[2], &topic_val);
  assert(CPARSER_OK == rc);
  topic_ptr = &topic_val;
  rc = cparser_get_string(&parser->tokens[3], &scope_val);
  assert(CPARSER_OK == rc);
  scope_ptr = &scope_val;
  cparser_cmd_no_subscribe_topic_scope(&parser->context, topic_ptr,
                                       scope_ptr);
  return CPARSER_OK;
}

cparser_result_t cparser_glue_publish_topic_message(cparser_t *parser) {
  char *topic_val;
  char **topic_ptr = NULL;
  char *message_val;
  char **message_ptr = NULL;
  cparser_result_t rc;

  rc = cparser_get_string(&parser->tokens[1], &topic_val);
  assert(CPARSER_OK == rc);
  topic_ptr = &topic_val;
  rc = cparser_get_string(&parser->tokens[2], &message_val);
  assert(CPARSER_OK == rc);
  message_ptr = &message_val;
  cparser_cmd_publish_topic_message(&parser->context, topic_ptr,
                                    message_ptr);
  return CPARSER_OK;
}

cparser_result_t
cparser_glue_notify_destination_message(cparser_t *parser) {
  char *destination_val;
  char **destination_ptr = NULL;
  char *message_val;
  char **message_ptr = NULL;
  cparser_result_t rc;

  rc = cparser_get_string(&parser->tokens[1], &destination_val);
  assert(CPARSER_OK == rc);
  destination_ptr = &destination_val;
  rc = cparser_get_string(&parser->tokens[2], &message_val);
  assert(CPARSER_OK == rc);
  message_ptr = &message_val;
  cparser_cmd_notify_destination_message(&parser->context, destination_ptr,
                                         message_ptr);
  return CPARSER_OK;
}

cparser_result_t cparser_glue_quit(cparser_t *parser) {
  cparser_cmd_quit(&parser->context);
  return CPARSER_OK;
}

cparser_result_t cparser_glue_help(cparser_t *parser) {
  cparser_cmd_help(&parser->context);
  return CPARSER_OK;
}

cparser_node_t cparser_node_help_eol = {
    CPARSER_NODE_END, 0, cparser_glue_help, "Get some help", NULL, NULL};

cparser_node_t cparser_node_help = {CPARSER_NODE_KEYWORD, 0, "help", NULL,
                                    NULL, &cparser_node_help_eol};

cparser_node_t cparser_node_quit_eol = {
    CPARSER_NODE_END, 0, cparser_glue_quit, "Stop the client", NULL, NULL};

cparser_node_t cparser_node_quit = {CPARSER_NODE_KEYWORD, 0, "quit", NULL,
                                    &cparser_node_help,
                                    &cparser_node_quit_eol};

cparser_node_t cparser_node_notify_destination_message_eol = {
    CPARSER_NODE_END, 0, cparser_glue_notify_destination_message,
    "Send message to destination ", NULL, NULL};

cparser_node_t cparser_node_notify_destination_message = {
    CPARSER_NODE_STRING, 0, "<STRING:message>", NULL, NULL,
    &cparser_node_notify_destination_message_eol};

cparser_node_t cparser_node_notify_destination = {
    CPARSER_NODE_STRING, 0, "<STRING:destination>", NULL, NULL,
    &cparser_node_notify_destination_message};

cparser_node_t cparser_node_notify = {CPARSER_NODE_KEYWORD, 0, "notify",
                                      NULL, &cparser_node_quit,
                                      &cparser_node_notify_destination};

cparser_node_t cparser_node_publish_topic_message_eol = {
    CPARSER_NODE_END, 0, cparser_glue_publish_topic_message,
    "Publish a message on topic", NULL, NULL};

cparser_node_t cparser_node_publish_topic_message = {
    CPARSER_NODE_STRING, 0, "<STRING:message>", NULL, NULL,
    &cparser_node_publish_topic_message_eol};

cparser_node_t cparser_node_publish_topic = {
    CPARSER_NODE_STRING, 0, "<STRING:topic>", NULL, NULL,
    &cparser_node_publish_topic_message};

cparser_node_t cparser_node_publish = {CPARSER_NODE_KEYWORD, 0, "publish",
                                       NULL, &cparser_node_notify,
                                       &cparser_node_publish_topic};

cparser_node_t cparser_node_no_subscribe_topic_scope_eol = {
    CPARSER_NODE_END, 0, cparser_glue_no_subscribe_topic_scope,
    "Unsubscribe to topic/scope", NULL, NULL};

cparser_node_t cparser_node_no_subscribe_topic_scope = {
    CPARSER_NODE_STRING, 0, "<STRING:scope>", NULL, NULL,
    &cparser_node_no_subscribe_topic_scope_eol};

cparser_node_t cparser_node_no_subscribe_topic = {
    CPARSER_NODE_STRING, 0, "<STRING:topic>", NULL, NULL,
    &cparser_node_no_subscribe_topic_scope};

cparser_node_t cparser_node_no_subscribe = {
    CPARSER_NODE_KEYWORD, 0, "subscribe", NULL, NULL,
    &cparser_node_no_subscribe_topic};

cparser_node_t cparser_node_no = {CPARSER_NODE_KEYWORD, 0, "no", NULL,
                                  &cparser_node_publish,
                                  &cparser_node_no_subscribe};

cparser_node_t cparser_node_subscribe_topic_scope_eol = {
    CPARSER_NODE_END, 0, cparser_glue_subscribe_topic_scope,
    "Subscribe to topic/scope", NULL, NULL};

cparser_node_t cparser_node_subscribe_topic_scope = {
    CPARSER_NODE_STRING, 0, "<STRING:scope>", NULL, NULL,
    &cparser_node_subscribe_topic_scope_eol};

cparser_node_t cparser_node_subscribe_topic = {
    CPARSER_NODE_STRING, 0, "<STRING:topic>", NULL, NULL,
    &cparser_node_subscribe_topic_scope};

cparser_node_t cparser_node_subscribe = {
    CPARSER_NODE_KEYWORD, 0, "subscribe", NULL, &cparser_node_no,
    &cparser_node_subscribe_topic};

cparser_node_t cparser_node_show_keys_eol = {
    CPARSER_NODE_END, 0, cparser_glue_show_keys, "Show loaded keys", NULL,
    NULL};

cparser_node_t cparser_node_show_keys = {CPARSER_NODE_KEYWORD, 0, "keys",
                                         NULL, NULL,
                                         &cparser_node_show_keys_eol};

cparser_node_t cparser_node_show_status_eol = {
    CPARSER_NODE_END, 0, cparser_glue_show_status, "Show overall status",
    NULL, NULL};

cparser_node_t cparser_node_show_status = {
    CPARSER_NODE_KEYWORD, 0, "status", NULL, &cparser_node_show_keys,
    &cparser_node_show_status_eol};

cparser_node_t cparser_node_show_subscriptions_eol = {
    CPARSER_NODE_END, 0, cparser_glue_show_subscriptions,
    "Show all subscriptions", NULL, NULL};

cparser_node_t cparser_node_show_subscriptions = {
    CPARSER_NODE_KEYWORD, 0, "subscriptions", NULL,
    &cparser_node_show_status, &cparser_node_show_subscriptions_eol};

cparser_node_t cparser_node_show = {CPARSER_NODE_KEYWORD, 0, "show", NULL,
                                    &cparser_node_subscribe,
                                    &cparser_node_show_subscriptions};

cparser_node_t cparser_root = {CPARSER_NODE_ROOT, 0, NULL,
                               "Root node of the parser tree", NULL,
                               &cparser_node_show};
