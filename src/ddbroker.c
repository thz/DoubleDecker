/* Local Variables:  */
/* flycheck-gcc-include-path:
 * "/home/eponsko/double-decker/c-version/include/" */
/* End:              */
/*
   Copyright (c) 2015 Pontus Sköldström, Bertrand Pechenot

   This file is part of libdd, the DoubleDecker hierarchical
   messaging system DoubleDecker is free software; you can
   redistribute it and/or modify it under the terms of the GNU Lesser
   General Public License (LGPL) version 2.1 as published by the Free
   Software Foundation.

   As a special exception, the Authors give you permission to link this
   library with independent modules to produce an executable,
   regardless of the license terms of these independent modules, and to
   copy and distribute the resulting executable under terms of your
   choice, provided that you also meet, for each linked independent
   module, the terms and conditions of the license of that module. An
   independent module is a module which is not derived from or based on
   this library.  If you modify this library, you must extend this
   exception to your version of the library.  DoubleDecker is
   distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
   License for more details.  You should have received a copy of the
   GNU Lesser General Public License along with this program.  If not,
   see <http://www.gnu.org/licenses/>.

 * broker.c --- Filename: broker.c Description: Initial idea for a C
 * implementation of double-decker based around czmq and cds_lfht cds_lfht
 * is a high-performance multi-thread supporting hashtable Idea is to
 * have one thread (main) recieving all messages which are pushed using
 * inproc threads to processing threads.  Processing threads then perform
 * lookups in the shared hashtables and forward to the zmqsockets (they
 * are threadsafe I hope..?) Hashtable and usersparce RCU library
 * implementation at: git://git.lttng.org/userspace-rcu.git
 * http://lwn.net/Articles/573431/ Author: Pontus Sköldström
 * <ponsko@acreo.se> Created: tis mar 10 22:31:03 2015 (+0100)
 * Last-Updated: By:
 */
#define _GNU_SOURCE
#include "../include/dd.h"
#include "../include/dd_classes.h"
#include "../include/protocol.h"
#include "../config.h"
#include "../include/broker.h"
#include "../include/ddhtable.h"
#include "../include/ddlog.h"
#include "../include/trie.h"
#include "sodium.h"
#include <czmq.h>
#include <err.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <urcu.h>
#include <zmq.h>
#ifdef HAVE_JSON_C_JSON_H
#include <json-c/json.h>
#elif HAVE_JSON_H
#include <json.h>
#elif HAVE_JSON_JSON_H
#include <json/json.h>
#endif

#define IPC_REGEX "(ipc://)(.+)"
#define TCP_REGEX "(tcp://[^:]+:)(\\d+)"

// Package these up into
// ddbroker_config_t:  for initial configuration
// and  _ddbroker_t_ as the object, returned by ddbroker_new(ddbroker_config_t)

char *broker_scope;
char *dealer_connect = NULL;
char *router_bind = NULL;
char *reststr = NULL;
char *pub_bind = NULL, *pub_connect = NULL;
char *sub_bind = NULL, *sub_connect = NULL;
int loglevel = DD_LOG_INFO;
char *logfile = NULL;
char *syslog_enabled = NULL;

char nonce[crypto_box_NONCEBYTES];
ddbrokerkeys_t *keys;
mode_t rw_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
// timer IDs
int br_timeout_loop, cli_timeout_loop, heartbeat_loop, reg_loop;

int daemonize = -1;
int state = DD_STATE_UNREG, timeout = 0, verbose = 0;
struct nn_trie topics_trie;
// Broker Identity, assigned by higher broker
zframe_t *broker_id = NULL, *broker_id_null;
zlist_t *scope;
zlist_t *rstrings;
zloop_t *loop;
zsock_t *pubN = NULL, *subN = NULL;
zsock_t *pubS = NULL, *subS = NULL;
zsock_t *rsock = NULL, *dsock = NULL, *http = NULL;

void print_ddbrokerkeys(ddbrokerkeys_t *keys);
void dest_invalid_rsock(zframe_t *sockid, char *src_string, char *dst_string);
void dest_invalid_dsock(char *src_string, char *dst_string);

void handler(int sig) {
  void *array[10];
  size_t size;

  // get void*'s for all entries on the stack
  size = backtrace(array, 10);

  // print out all the frames to stderr
  dd_error("Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(EXIT_FAILURE);
}

int is_int(char *s) {
  while (*s) {
    if (isdigit(*s++) == 0)
      return 0;
  }

  return 1;
}
static void remote_reg_failed(zframe_t *sockid, char *cli_name) {
  zsock_send(rsock, "fbbbss", sockid, &dd_version, 4, &dd_cmd_error, 4,
             &dd_error_regfail, 4, cli_name);
}

void delete_dist_clients(local_broker *br) {
  struct cds_lfht_iter iter;
  dist_client *mp;

  cds_lfht_first(dist_cli_ht, &iter);
  struct cds_lfht_node *ht_node = cds_lfht_iter_get_node(&iter);
  dd_debug("delete_dist_clients:");
  //  zframe_print(br->sockid, "broker");
  while (ht_node != NULL) {
    mp = caa_container_of(ht_node, dist_client, node);
    dd_debug("Distclient %s", mp->name);
    //    zframe_print(mp->broker, "client");
    if (zframe_eq(mp->broker, br->sockid)) {
      char buf[256] = "";
      dd_debug("Was under missing broker %s", zframe_tostr(br->sockid, buf));
      del_cli_up(mp->name);
      rcu_read_lock();
      int ret = cds_lfht_del(dist_cli_ht, ht_node);
      rcu_read_unlock();
    }
    cds_lfht_next(dist_cli_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
  }
}

/** Functions for handling incoming messages */

void cmd_cb_high_error(zmsg_t *msg) {
  zframe_t *code_frame = zmsg_pop(msg);
  if (code_frame == NULL) {
    dd_error("DD: Misformed ERROR message, missing ERROR_CODE!\n");
    return;
  }
  local_client *ln;
  struct dist_node *dn;

  int32_t *error_code = (int32_t *)zframe_data(code_frame);
  switch (*error_code) {
  case DD_ERROR_NODST:
    dd_debug("Recieved ERROR_NODST from higher broker!");
    // original destination
    char *dst_string = zmsg_popstr(msg);
    // source of failing command
    char *src_string = zmsg_popstr(msg);

    // Check if src_string is a local client
    if ((ln = hashtable_has_rev_local_node(src_string, 0))) {
      dd_debug("Source of NODST is local!");
      char *dot = strchr(dst_string, '.');
      dest_invalid_rsock(ln->sockid, src_string, dot + 1);

    } else if ((dn = hashtable_has_dist_node(src_string))) {
      dd_debug("Source of NODST is distant!");
      dest_invalid_rsock(dn->broker, src_string, dst_string);
    } else {
      dd_warning("Could not find NODST source, cannot 'raise' error");
    }

    free(dst_string);
    free(src_string);
    break;
  case DD_ERROR_REGFAIL:
    dd_debug("Recived ERROR_REGFAIL from higher broker!");
    char *cli_name = zmsg_popstr(msg);
    dn = hashtable_has_dist_node(cli_name);
    ln = hashtable_has_rev_local_node(cli_name, 0);
    // is cli_name a local client?
    if ((ln = hashtable_has_rev_local_node(cli_name, 0))) {
      dd_info(" - Removed local client: %s", ln->prefix_name);
      int a = remove_subscriptions(ln->sockid);
      dd_info("   - Removed %d subscriptions", a);
      hashtable_unlink_local_node(ln->sockid, ln->cookie);
      hashtable_unlink_rev_local_node(ln->prefix_name);
      remote_reg_failed(ln->sockid, "remote");
      zframe_destroy(&ln->sockid);
      free(ln->prefix_name);
      free(ln->name);
      free(ln);
    } else if ((dn = hashtable_has_dist_node(cli_name))) {
      dd_info(" - Removed distant client: %s", cli_name);
      remote_reg_failed(dn->broker, cli_name);
      hashtable_remove_dist_node(cli_name);
    } else {
      dd_warning("Could not locate offending client!");
    }
    free(cli_name);
    break;
  case DD_ERROR_VERSION:
    dd_error("ERROR_VERSION from higher broker!");
    break;
  default:
    dd_error("Unknown error code from higher broker!");
    break;
  }
  zframe_destroy(&code_frame);
}

void cmd_cb_addbr(zframe_t *sockid, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_addbr called");
  zframe_print(sockid, "sockid");
  zmsg_print(msg);
#endif
  char *hash = zmsg_popstr(msg);
  if (hash == NULL) {
    dd_error("Error, got ADDBR without hash!");
    return;
  }
  //  printf("comparing hash %s with keys->hash %s\n", hash, keys->hash);
  if (strcmp(hash, keys->hash) != 0) {
    // TODO send error
    dd_error("Error, got ADDBR with wrong hash!");
    return;
  }

  int enclen = sizeof(uint64_t) + crypto_box_NONCEBYTES + crypto_box_MACBYTES;
  unsigned char *dest = calloc(1, enclen);
  unsigned char *ciphertext = dest; // dest+crypto_box_NONCEBYTES;

  // increment nonce
  sodium_increment((unsigned char *)nonce, crypto_box_NONCEBYTES);
  memcpy(dest, nonce, crypto_box_NONCEBYTES);

  dest += crypto_box_NONCEBYTES;
  int retval = crypto_box_easy_afternm(dest, (unsigned char *)&keys->cookie,
                                       sizeof(keys->cookie),
                                       (unsigned char *)nonce, keys->ddboxk);

  retval = zsock_send(rsock, "fbbbf", sockid, &dd_version, 4, &dd_cmd_chall, 4,
                      ciphertext, enclen, sockid);
  if (retval != 0) {
    dd_error("Error sending challenge!");
  }
}

void cmd_cb_addlcl(zframe_t *sockid, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_addlcl called");
  zframe_print(sockid, "sockid");
  zmsg_print(msg);
#endif

  char *hash = zmsg_popstr(msg);
  if (hash == NULL) {
    dd_error("Error, got ADDLCL without hash!");
    return;
  }
  ddtenant_t *ten;

  ten = zhash_lookup(keys->tenantkeys, hash);
  free(hash);
  if (ten == NULL) {
    dd_error("Could not find key for client");
    zsock_send(rsock, "fbbbs", sockid, &dd_version, 4, &dd_cmd_error, 4,
               &dd_error_regfail, 4, "Authentication failed!");
    return;
  }

  int enclen = sizeof(uint64_t) + crypto_box_NONCEBYTES + crypto_box_MACBYTES;

  unsigned char *dest = calloc(1, enclen);
  unsigned char *ciphertext = dest; // dest+crypto_box_NONCEBYTES;

  // increment nonce
  sodium_increment((unsigned char *)nonce, crypto_box_NONCEBYTES);
  memcpy(dest, nonce, crypto_box_NONCEBYTES);

  dest += crypto_box_NONCEBYTES;
  int retval = crypto_box_easy_afternm(
      dest, (unsigned char *)&ten->cookie, sizeof(ten->cookie),
      (unsigned char *)nonce, (const unsigned char *)ten->boxk);

  retval = zsock_send(rsock, "fbbb", sockid, &dd_version, 4, &dd_cmd_chall, 4,
                      ciphertext, enclen);
  free(ciphertext);
  if (retval != 0) {
    dd_error("Error sending challenge!");
  }
}

void cmd_cb_adddcl(zframe_t *sockid, zframe_t *cookie_frame, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_adddcl called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie_frame, "cookie");
  zmsg_print(msg);
#endif
  uint64_t *cookie = (uint64_t *)zframe_data(cookie_frame);
  if (hashtable_has_local_broker(sockid, *cookie, 0) == NULL) {
    dd_warning("Got ADDDCL from unregistered broker...");
    return;
  }

  struct dist_node *dn;
  char *name = zmsg_popstr(msg);
  zframe_t *dist_frame = zmsg_pop(msg);
  int *dist = (int *)zframe_data(dist_frame);
  // does name exist in local hashtable?
  local_client *ln;

  if ((ln = hashtable_has_rev_local_node(name, 0))) {
    dd_info(" - Local client '%s' already exists!", name);
    remote_reg_failed(sockid, name);
    free(name);

  } else if ((dn = hashtable_has_dist_node(name))) {
    dd_info(" - Remote client '%s' already exists!", name);
    remote_reg_failed(sockid, name);
    free(name);

  } else {
    hashtable_insert_dist_node(name, sockid, *dist);
    dd_info(" + Added remote client: %s (%d)", name, *dist);
    add_cli_up(name, *dist);
  }
  zframe_destroy(&dist_frame);
}

void cmd_cb_chall(zmsg_t *msg) {
  int retval = 0;
#ifdef DEBUG
  dd_debug("cmd_cb_chall called");
  zmsg_print(msg);
#endif
  zframe_t *encrypted = zmsg_pop(msg);
  if (broker_id)
    zframe_destroy(&broker_id);
  broker_id = zmsg_pop(msg);
  unsigned char *data = zframe_data(encrypted);

  int enclen = zframe_size(encrypted);
  unsigned char *decrypted = calloc(1, enclen);

  retval = crypto_box_open_easy_afternm(decrypted, data + crypto_box_NONCEBYTES,
                                        enclen - crypto_box_NONCEBYTES, data,
                                        keys->ddboxk);
  if (retval != 0) {
    dd_error("Unable to decrypt CHALLENGE from broker");
    goto cleanup;
  }

  zsock_send(dsock, "bbfss", &dd_version, 4, &dd_cmd_challok, 4,
             zframe_new(decrypted,
                        enclen - crypto_box_NONCEBYTES - crypto_box_MACBYTES),
             keys->hash, "broker");
cleanup:

  zframe_destroy(&encrypted);
  free(decrypted);
}

void cmd_cb_challok(zframe_t *sockid, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_challok called");
  zframe_print(sockid, "sockid");
  zmsg_print(msg);
#endif

  int retval;
  zframe_t *cook = zmsg_pop(msg);
  uint64_t *cookie = (uint64_t *)zframe_data(cook);
  char *hash = zmsg_popstr(msg);
  char *client_name = zmsg_popstr(msg);

  if (cook == NULL || cookie == NULL || hash == NULL || client_name == NULL) {

    dd_error("DD_CMD_CHALLOK: misformed message!");
    goto cleanup;
  }
  // broker <-> broker authentication
  if (strcmp(hash, keys->hash) == 0) {
    if (keys->cookie != *cookie) {
      dd_warning("DD_CHALL_OK: authentication error!");
      // TODO: send error message
      goto cleanup;
    }
    dd_debug("Authentication of broker %s successful!", client_name);
    if (NULL == hashtable_has_local_broker(sockid, *cookie, 0)) {
      hashtable_insert_local_broker(sockid, *cookie);

      const char *pubs_endpoint = zsock_endpoint(pubS);
      const char *subs_endpoint = zsock_endpoint(subS);

      zsock_send(rsock, "fbbbss", sockid, &dd_version, 4, &dd_cmd_regok, 4,
                 &keys->cookie, sizeof(keys->cookie), pubs_endpoint,
                 subs_endpoint);
      char buf[256];
      dd_info(" + Added broker: %s", zframe_tostr(sockid, buf));
      goto cleanup;
    }
    goto cleanup;
  }
  // tenant <-> broker authentication
  ddtenant_t *ten;
  ten = zhash_lookup(keys->tenantkeys, hash);
  if (ten == NULL) {
    dd_warning("DD_CHALL_OK: could not find tenant for %s", hash);
    goto cleanup;
  }

  if (ten->cookie != *cookie) {
    dd_warning("DD_CHALL_OK: authentication error!");
    // TODO: send error message
    goto cleanup;
  }

  dd_info("Authentication of %s.%s successful!", ten->name, client_name);
  if (strcmp(client_name, "public") == 0) {
    // TODO: send error message
    dd_error("Client trying to use reserved name 'public'!");
    goto cleanup;
    return;
  }

  retval = insert_local_client(sockid, ten, client_name);
  if (retval == -1) {
    // TODO: send error message
    remote_reg_failed(sockid, "local");
    dd_error("DD_CMD_CHALLOK: Couldn't insert local client!");
    goto cleanup;
    return;
  }
  zsock_send(rsock, "fbbb", sockid, &dd_version, 4, &dd_cmd_regok, 4,
             &ten->cookie, sizeof(ten->cookie));
  dd_info(" + Added local client: %s.%s", ten->name, client_name);
  char prefix_name[MAXTENANTNAME];
  int prelen = snprintf(prefix_name, 200, "%s.%s", ten->name, client_name);
  if (state != DD_STATE_ROOT)
    add_cli_up(prefix_name, 0);

cleanup:
  if (hash)
    free(hash);
  if (client_name)
    free(client_name);
  if (cook)
    zframe_destroy(&cook);
}

void cmd_cb_forward_dsock(zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_forward_dsock called");
  zmsg_print(msg);
#endif

  if (zmsg_size(msg) < 2)
    return;
  char *src = zmsg_popstr(msg);
  char *dst = zmsg_popstr(msg);

  int srcpublic = 0, dstpublic = 0;

  if (strncmp(src, "public.", 7) == 0)
    srcpublic = 1;
  if (strncmp(dst, "public.", 7) == 0)
    dstpublic = 1;

  dd_debug("Forward_dsock: srcpublic = %d, dstpublic = %d\n", srcpublic,
           dstpublic);

  struct dist_node *dn;
  local_client *ln;

  if ((ln = hashtable_has_rev_local_node(dst, 0))) {
    if ((srcpublic && !dstpublic) || (!srcpublic && dstpublic)) {
      dd_debug("Forward_dsock, not stripping tenant %s", src);
      forward_locally(ln->sockid, src, msg);
    } else {
      dd_debug("Forward_dsock, stripping tenant %s", src);
      char *dot = strchr(src, '.');
      forward_locally(ln->sockid, dot + 1, msg);
    }
  } else if ((dn = hashtable_has_dist_node(dst))) {
    forward_down(src, dst, dn->broker, msg);
  } else if (state == DD_STATE_ROOT) {
    dest_invalid_dsock(src, dst);
  } else {
    forward_up(src, dst, msg);
  }
  free(src);
  free(dst);
}

void cmd_cb_forward_rsock(zframe_t *sockid, zframe_t *cookie_frame,
                          zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_forward_rsock called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie_frame, "cookie");
  zmsg_print(msg);
#endif

  uint64_t *cookie;
  cookie = (uint64_t *)zframe_data(cookie_frame);
  if (!hashtable_has_local_broker(sockid, *cookie, 1)) {
    dd_warning("Unregistered broker trying to forward!");
    return;
  }

  if (zmsg_size(msg) < 2)
    return;

  char *src_string = zmsg_popstr(msg);
  char *dst_string = zmsg_popstr(msg);

  int srcpublic = 0, dstpublic = 0;

  if (strncmp(src_string, "public.", 7) == 0)
    srcpublic = 1;
  if (strncmp(dst_string, "public.", 7) == 0)
    dstpublic = 1;

  dd_debug("Forward_rsock: srcpublic = %d, dstpublic = %d\n", srcpublic,
           dstpublic);
  struct dist_node *dn;
  local_client *ln;
  if ((ln = hashtable_has_rev_local_node(dst_string, 0))) {
    if ((srcpublic && !dstpublic) || (!srcpublic && dstpublic)) {
      dd_debug("Forward_rsock, not stripping tenant %s", src_string);
      forward_locally(ln->sockid, src_string, msg);
    } else {
      dd_debug("Forward_dsock, stripping tenant %s", src_string);
      char *dot = strchr(src_string, '.');
      forward_locally(ln->sockid, dot + 1, msg);
    }
  } else if ((dn = hashtable_has_dist_node(dst_string))) {
    forward_down(src_string, dst_string, dn->broker, msg);
  } else if (state == DD_STATE_ROOT) {
    dest_invalid_rsock(sockid, src_string, dst_string);
  } else {
    forward_up(src_string, dst_string, msg);
  }
  free(src_string);
  free(dst_string);
}

/*
 * TODO: Add a lookup for dist_cli here as well!
 */
void cmd_cb_nodst_dsock(zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_nodst_dsock called");
  zmsg_print(msg);
#endif

  local_client *ln;
  char *dst_string = zmsg_popstr(msg);
  char *src_string = zmsg_popstr(msg);
  dd_debug("cmd_cb_nodst_dsock called!)");

  if ((ln = hashtable_has_rev_local_node(src_string, 0))) {
    zsock_send(rsock, "fbbbss", ln->sockid, &dd_version, 4, &dd_cmd_error, 4,
               &dd_error_nodst, 4, dst_string, src_string);
  } else {
    dd_error("Could not forward NODST message downwards");
  }
}

void cmd_cb_nodst_rsock(zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_nodst_rsock called");
  zmsg_print(msg);
#endif
  dd_error("cmd_cb_nodst_rsock called, not implemented!");
}

void cmd_cb_pub(zframe_t *sockid, zframe_t *cookie, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_pub called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie, "cookie");
  zmsg_print(msg);
#endif

  char *topic = zmsg_popstr(msg);
  zframe_t *pathv = zmsg_pop(msg);

  local_client *ln;
  ln = hashtable_has_local_node(sockid, cookie, 1);
  if (!ln) {
    dd_warning("Unregistered client trying to send!");
    zframe_destroy(&pathv);
    free(topic);
    return;
  }
  int srcpublic = 0;
  int dstpublic = 0;
  if (strcmp("public", ln->tenant) == 0)
    srcpublic = 1;
  if (strncmp(topic, "public.", 7) == 0)
    dstpublic = 1;

  char newtopic[256];
  char *prefix_topic = NULL;

  if (topic[strlen(topic) - 1] == '$') {
    topic[strlen(topic) - 1] = '\0';
    prefix_topic = topic;
  } else {
    snprintf(&newtopic[0], 256, "%s%s", topic, broker_scope);
    prefix_topic = &newtopic[0];
  }
  char *name = NULL;
  char *pubtopic = NULL;
  char tentopic[256];
  if (dstpublic) {
    pubtopic = prefix_topic;
    name = ln->prefix_name;
  } else {
    snprintf(&tentopic[0], 256, "%s.%s", ln->tenant, prefix_topic);
    pubtopic = &tentopic[0];
    name = ln->name;
  }

  if (pubN) {
    dd_debug("publishing north %s %s ", pubtopic, name);
    zsock_send(pubN, "ssfm", pubtopic, name, broker_id, msg);
  }

  if (pubS) {
    dd_debug("publishing south %s %s", pubtopic, name);

    zsock_send(pubS, "ssfm", pubtopic, name, broker_id_null, msg);
  }

  zlist_t *socks =
      nn_trie_tree(&topics_trie, (const uint8_t *)pubtopic, strlen(pubtopic));

  if (socks != NULL) {
    zframe_t *s = zlist_first(socks);
    dd_debug("Local sockids to send to: ");
    while (s) {
      print_zframe(s);
      zsock_send(rsock, "fbbssm", s, &dd_version, 4, &dd_cmd_pub, 4, name,
                 topic, msg);
      s = zlist_next(socks);
    }
    zlist_destroy(&socks);
  } else {
    dd_debug("No matching nodes found by nn_trie_tree");
  }
  free(topic);
  zframe_destroy(&pathv);
}

void cmd_cb_ping(zframe_t *sockid, zframe_t *cookie) {
#ifdef DEBUG
  dd_debug("cmd_cb_ping called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie, "cookie");
#endif

  if (hashtable_has_local_node(sockid, cookie, 1)) {
    zsock_send(rsock, "fbb", sockid, &dd_version, 4, &dd_cmd_pong, 4);
    return;
  }

  uint64_t *cook;
  cook = (uint64_t *)zframe_data(cookie);
  if (hashtable_has_local_broker(sockid, *cook, 1)) {
    zsock_send(rsock, "fbb", sockid, &dd_version, 4, &dd_cmd_pong, 4);
    return;
  }
  dd_warning("Ping from unregistered client/broker: ");
  zframe_print(sockid, NULL);
}

void cmd_cb_regok(zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_regok called");
  zmsg_print(msg);
#endif

  state = DD_STATE_REGISTERED;

  // stop trying to register
  zloop_timer_end(loop, reg_loop);
  heartbeat_loop = zloop_timer(loop, 1000, 0, s_heartbeat, dsock);

  // iterate through local clients and add_cli_up to transmit to next
  // broker
  struct cds_lfht_iter iter;
  local_client *np;
  rcu_read_lock();
  cds_lfht_first(lcl_cli_ht, &iter);
  struct cds_lfht_node *ht_node = cds_lfht_iter_get_node(&iter);
  while (ht_node != NULL) {
    rcu_read_unlock();
    np = caa_container_of(ht_node, local_client, lcl_node);
    dd_debug("Registering, found local client: %s", np->name);
    add_cli_up(np->prefix_name, 0);
    rcu_read_lock();
    cds_lfht_next(lcl_cli_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
  };
  rcu_read_unlock();
  // iterate through dist clients and add_cli_up to transmit to next
  // broker

  struct dist_node *nd;
  rcu_read_lock();
  cds_lfht_first(dist_cli_ht, &iter);
  ht_node = cds_lfht_iter_get_node(&iter);
  while (ht_node != NULL) {
    rcu_read_unlock();
    nd = caa_container_of(ht_node, struct dist_node, node);
    dd_debug("Registering, found distant client: %s", nd->name);
    add_cli_up(nd->name, nd->distance);
    rcu_read_lock();
    cds_lfht_next(dist_cli_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
  };
  rcu_read_unlock();

  if (3 == zmsg_size(msg)) {
    zframe_t *cook = zmsg_pop(msg);
    char *puburl = zmsg_popstr(msg);
    char *suburl = zmsg_popstr(msg);
    connect_pubsubN(puburl, suburl);
    free(puburl);
    free(suburl);
    zframe_destroy(&cook);
  } else {
    dd_warning("No PUB/SUB interface configured");
  }
}

void cmd_cb_send(zframe_t *sockid, zframe_t *cookie, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_send called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie, "cookie");
  zmsg_print(msg);
#endif
  char *dest = zmsg_popstr(msg);

  int srcpublic = 0;
  int dstpublic = 0;

  // Add DEFINE for length here
  char dest_buf[MAXTENANTNAME];

  char *dst_string;
  char *src_string;

  local_client *ln;
  ln = hashtable_has_local_node(sockid, cookie, 1);
  if (!ln) {
    dd_error("Unregistered client trying to send!");
    // TODO
    // free some stuff here..
    return;
  }
  if (strcmp(ln->tenant, "public") == 0)
    srcpublic = 1;
  if (strncmp(dest, "public.", 7) == 0)
    dstpublic = 1;

  dd_debug("cmd_cb_send, srcpublic %d, dstpublic %d", srcpublic, dstpublic);

  // if destination is public, add tenant to source_name
  // but not on prefix_dst_name
  if (dstpublic == 1 && srcpublic == 0) {

    src_string = ln->prefix_name;
    dst_string = dest;
    dd_debug("s:0 d:1 , s: %s d: %s", src_string, dst_string);
    // if source is public and destination is public,
    // don't add additional 'public.' to prefix
  } else if (dstpublic == 1 && srcpublic == 1) {
    src_string = ln->prefix_name;
    dst_string = dest;
    dd_debug("s:1 d:1 , s: %s d: %s", src_string, dst_string);
    // if source is public but not destination, check if
    // 'public.' should be added.
    // if dest starts with "tenant." don't add public.
  } else if (dstpublic == 0 && srcpublic == 1) {
    dd_debug("dst not public, but src is");
    int add_prefix = 1;
    char *dot = strchr(dest, '.');
    if (dot) {
      dd_debug("destination has . in name");
      *dot = '\0';
      char *k = NULL;
      k = zlist_first(keys->tenants);
      while (k) {
        if (strncmp(dest, k, strlen(k)) == 0) {
          dd_debug("found matching tenant: %s, not adding prefix!", k);
          add_prefix = 0;
          break;
        }
        k = zlist_next(keys->tenants);
      }
      *dot = '.';
    }
    dd_debug("add_prefix: %d", add_prefix);
    if (add_prefix == 1) {
      src_string = ln->prefix_name;
      snprintf(dest_buf, MAXTENANTNAME, "%s.%s", ln->tenant, dest);
      dst_string = dest_buf;
    } else {
      dst_string = dest;
      src_string = ln->prefix_name;
    }
    dd_debug("s:1 d:0, s: %s d: %s", src_string, dst_string);
  } else {
    src_string = ln->prefix_name;
    snprintf(dest_buf, MAXTENANTNAME, "%s.%s", ln->tenant, dest);
    dst_string = dest_buf;
    dd_debug("s:0 d:0, s: %s d: %s", src_string, dst_string);
  }

  dd_debug("cmd_cb_send: src \"%s\", dst \"%s\"", src_string, dst_string);

  struct dist_node *dn;
  if ((ln = hashtable_has_rev_local_node(dst_string, 0))) {
    if ((!srcpublic && !dstpublic) || (srcpublic && dstpublic)) {
      char *dot = strchr(src_string, '.');
      forward_locally(ln->sockid, dot + 1, msg);
    } else {
      forward_locally(ln->sockid, src_string, msg);
    }
  } else if ((dn = hashtable_has_dist_node(dst_string))) {
    dd_debug("calling forward down");
    forward_down(src_string, dst_string, dn->broker, msg);
  } else if (state == DD_STATE_ROOT) {
    if ((!srcpublic && !dstpublic) || (srcpublic && dstpublic)) {
      char *src_dot = strchr(src_string, '.');
      char *dst_dot = strchr(dst_string, '.');
      dest_invalid_rsock(sockid, src_dot + 1, dst_dot + 1);
    } else {
      dest_invalid_rsock(sockid, src_string, dst_string);
    }
  } else {
    forward_up(src_string, dst_string, msg);
  }

  free(dest);
}

void cmd_cb_sub(zframe_t *sockid, zframe_t *cookie, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_sub called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie, "cookie");
  zmsg_print(msg);
#endif

  char *topic = zmsg_popstr(msg);
  char *scopestr = zmsg_popstr(msg);

  local_client *ln;
  ln = hashtable_has_local_node(sockid, cookie, 1);
  if (!ln) {
    dd_warning("DD: Unregistered client trying to send!");
    free(topic);
    free(scopestr);
    return;
  }

  if (strcmp(topic, "public") == 0) {
    zsock_send(rsock, "fbbss", sockid, &dd_version, 4, &dd_cmd_data, 4,
               "ERROR: protected topic");
    free(topic);
    free(scopestr);
    return;
  }

  // scopestr = "/*/*/*/"
  // scopestr = "/43/*/"
  // scopestr ..
  // replace * with appropriate scope number assigned to broker
  int j;
  char *str1, *token, *saveptr1;
  char *brscope;
  char *t = zlist_first(scope);
  char *scopedup = strdup(scopestr);
  char newtopic[257];
  char *ntptr = &newtopic[1];
  char newscope[128];
  char *nsptr = &newscope[0];
  int len = 128;
  int retval;
  if (strcmp(scopestr, "noscope") == 0) {
    retval = snprintf(nsptr, len, "");
    len -= retval;
    nsptr += retval;
  } else {
    for (j = 1, str1 = scopestr;; j++, str1 = NULL) {
      token = strtok_r(str1, "/", &saveptr1);
      if (token == NULL)
        break;

      if (t != NULL) {
        if (strcmp(token, "*") == 0) {
          retval = snprintf(nsptr, len, "/%s", t);
          len -= retval;
          nsptr += retval;
        } else {
          if (is_int(token) == 0) {
            dd_error("%s in scope string is not an integer", token);
          }
          retval = snprintf(nsptr, len, "/%s", token);
          len -= retval;
          nsptr += retval;
        }
        t = zlist_next(scope);
      } else {
        dd_error("Requested scope is longer than assigned scope!");

        free(scopedup);
        free(scopestr);
        free(topic);
      }
    }
    retval = snprintf(nsptr, len, "/");
    len -= retval;
    nsptr += retval;
  }
  zsock_send(rsock, "fbbss", sockid, &dd_version, 4, &dd_cmd_subok, 4, topic,
             scopedup);
  free(scopedup);

  retval =
      snprintf(ntptr, 256, "%s.%s%s", ln->tenant, topic, (char *)&newscope[0]);
  //  dd_debug("newtopic = %s, len = %d\n", ntptr, retval);

  int new = 0;
  // Hashtable
  // subscriptions[sockid(5byte array)] = [topic,topic,topic]
  retval = insert_subscription(sockid, ntptr);

  if (retval != 0)
    new += 1;

#ifdef DEBUG
  print_sub_ht();
#endif

  // Trie
  // topics_trie[newtopic(char*)] = [sockid, sockid, sockid]
  retval = nn_trie_subscribe(&topics_trie, (const uint8_t *)ntptr,
                             strlen(ntptr), sockid, 1);
  // doesn't really matter
  if (retval == 0) {
    dd_info("topic %s already in trie!", ntptr);
  } else if (retval == 1) {
    dd_debug("new topic %s", ntptr);
  } else if (retval == 2) {
    dd_debug("inserted new sockid on topic %s", ntptr);
  }

  free(scopestr);
  free(topic);
#ifdef DEBUG
  nn_trie_dump(&topics_trie);
#endif
  // refcount -> integrate in the topic_trie as refcount_s and refcount_n
  // topic_north[newtopic(char*)] = int
  // topic_south[newtopic(char*)] = int

  if (retval != 2)
    return;

  // add subscription to the north and south sub sockets
  newtopic[0] = 1;
  ntptr = &newtopic[0];
  if (subN) {
    dd_debug("adding subscription for %s to north SUB", &newtopic[1]);
    retval = zsock_send(subN, "b", &newtopic[0], 1 + strlen(&newtopic[1]));
  }
  if (subS) {
    dd_debug("adding subscription for %s to south SUB", &newtopic[1]);
    retval = zsock_send(subS, "b", &newtopic[0], 1 + strlen(&newtopic[1]));
  }
}

/*
 * TODO fix this
 */
void cmd_cb_unreg_br(char *name, zmsg_t *msg) {
  dd_debug("cmd_cb_unreg_br called, not implemented");
  /*
   * tmp_to_del = [] print('unregistering', name) for cli in
   * self.dist_cli: print(cli, self.dist_cli[cli][0]) if
   * self.dist_cli[cli][0] == name: tmp_to_del.append(cli)
   *
   * for i in tmp_to_del: self.unreg_dist_cli(name, [i])
   * self.local_br.pop(name)
   */
}

void cmd_cb_unreg_cli(zframe_t *sockid, zframe_t *cookie, zmsg_t *msg) {

#ifdef DEBUG
  dd_debug("cmd_cb_unreg_cli called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie, "cookie");
//        zmsg_print(msg);
#endif

  local_client *ln;
  if ((ln = hashtable_has_local_node(sockid, cookie, 0))) {
    dd_info(" - Removed local client: %s", ln->prefix_name);
    del_cli_up(ln->prefix_name);
    int a = remove_subscriptions(sockid);
    dd_info("   - Removed %d subscriptions", a);
    hashtable_unlink_local_node(ln->sockid, ln->cookie);
    hashtable_unlink_rev_local_node(ln->prefix_name);
    zframe_destroy(&ln->sockid);
    free(ln->prefix_name);
    free(ln->name);
    free(ln);
    print_local_ht();
  } else {
    dd_warning("Request to remove unknown client");
  }
}

void cmd_cb_unreg_dist_cli(zframe_t *sockid, zframe_t *cookie_frame,
                           zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_unreg_dist_cli called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie_frame, "cookie");
  zmsg_print(msg);
#endif

  uint64_t *cook = (uint64_t *)zframe_data(cookie_frame);
  if (!hashtable_has_local_broker(sockid, *cook, 0)) {
    dd_error("Unregistered broker trying to remove clients!");
    return;
  }

  struct dist_node *dn;
  char *name = zmsg_popstr(msg);
  dd_debug("trying to remove distant client: %s", name);

  if ((dn = hashtable_has_dist_node(name))) {
    dd_info(" - Removed distant client: %s", name);
    hashtable_remove_dist_node(name);
    del_cli_up(name);
  }
  free(name);
}

void cmd_cb_unsub(zframe_t *sockid, zframe_t *cookie, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("cmd_cb_unsub called");
  zframe_print(sockid, "sockid");
  zframe_print(cookie, "cookie");
  zmsg_print(msg);
#endif

  char *topic = zmsg_popstr(msg);
  char *scopestr = zmsg_popstr(msg);

  local_client *ln;
  ln = hashtable_has_local_node(sockid, cookie, 1);
  if (!ln) {
    dd_warning("Unregistered client trying to send!\n");
    free(topic);
    free(scopestr);
    return;
  }

  if (strcmp(topic, "public") == 0) {
    zsock_send(rsock, "fbbss", sockid, &dd_version, 4, &dd_cmd_data, 4,
               "ERROR: protected topic");
    free(topic);
    free(scopestr);
    return;
  }

  // scopestr = "/*/*/*/"
  // scopestr = "/43/*/"
  // scopestr ..
  // replace * with appropriate scope number assigned to broker
  int j;
  char *str1, *token, *saveptr1;
  char *brscope;
  char *t = zlist_first(scope);
  char *scopedup = strdup(scopestr);
  char newtopic[257];
  char *ntptr = &newtopic[1];
  char newscope[128];
  char *nsptr = &newscope[0];
  int len = 128;
  int retval;
  if (strcmp(scopestr, "noscope") == 0) {
    retval = snprintf(nsptr, len, "");
    len -= retval;
    nsptr += retval;
  } else {
    for (j = 1, str1 = scopestr;; j++, str1 = NULL) {
      token = strtok_r(str1, "/", &saveptr1);
      if (token == NULL)
        break;

      if (t != NULL) {
        if (strcmp(token, "*") == 0) {
          retval = snprintf(nsptr, len, "/%s", t);
          len -= retval;
          nsptr += retval;
        } else {
          if (is_int(token) == 0) {
            dd_error("%s in scope string is not an integer", token);
          }
          retval = snprintf(nsptr, len, "/%s", token);
          len -= retval;
          nsptr += retval;
        }
        t = zlist_next(scope);
      } else {
        dd_error("Requested scope is longer than assigned scope!");

        free(scopedup);
        free(scopestr);
        free(topic);
      }
    }
    retval = snprintf(nsptr, len, "/");
    len -= retval;
    nsptr += retval;
  }
  free(scopedup);

  retval =
      snprintf(ntptr, 256, "%s.%s%s", ln->tenant, topic, (char *)&newscope[0]);
  dd_info("deltopic = %s, len = %d\n", ntptr, retval);

  int new = 0;
  retval = remove_subscription(sockid, ntptr);

  // only delete a subscription if something was actually removed
  // from the trie and/or hashtable Otherwise multiple unsub from
  // a single client will f up for the  others

  if (retval == 0)
    return;

  newtopic[0] = 0;
  ntptr = &newtopic[0];
  if (subN) {
    dd_debug("deleting 1 subscription for %s to north SUB", &newtopic[1]);
    retval = zsock_send(subN, "b", &newtopic[0], 1 + strlen(&newtopic[1]));
  }
  if (subS) {
    dd_debug("deleting 1 subscription for %s to south SUB", &newtopic[1]);
    retval = zsock_send(subS, "b", &newtopic[0], 1 + strlen(&newtopic[1]));
  }
}

/* Functions called from zloop on timers or when message recieved */

int s_on_subN_msg(zloop_t *loop, zsock_t *handle, void *arg) {
  zmsg_t *msg = zmsg_recv(handle);

#ifdef DEBUG
  dd_debug("s_on_subN_msg called");
  zmsg_print(msg);
#endif

  char *pubtopic = zmsg_popstr(msg);
  char *name = zmsg_popstr(msg);
  zframe_t *pathv = zmsg_pop(msg);

  if (zframe_eq(pathv, broker_id)) {
    goto cleanup;
  }

  dd_debug("pubtopic: %s source: %s", pubtopic, name);
  // zframe_print(pathv, "pathv: ");
  zlist_t *socks =
      nn_trie_tree(&topics_trie, (const uint8_t *)pubtopic, strlen(pubtopic));

  if (socks != NULL) {
    zframe_t *s = zlist_first(socks);
    dd_debug("Local sockids to send to: ");
    char *dot = strchr(pubtopic, '.');
    dot++;
    char *slash = strchr(dot, '/');
    if (slash)
      *slash = '\0';

    while (s) {
      print_zframe(s);
      zsock_send(rsock, "fbbssm", s, &dd_version, 4, &dd_cmd_pub, 4, name, dot,
                 msg);
      s = zlist_next(socks);
    }
    *slash = '/';
    zlist_destroy(&socks);
  } else {
    dd_debug("No matching nodes found by nn_trie_tree");
  }

  // If from north, only send south (only one reciever in the north)
  if (pubS)
    zsock_send(pubS, "ssfm", pubtopic, name, broker_id_null, msg);

cleanup:
  free(pubtopic);
  free(name);
  zframe_destroy(&pathv);
  zmsg_destroy(&msg);
  return 0;
}

int s_on_subS_msg(zloop_t *loop, zsock_t *handle, void *arg) {
  zmsg_t *msg = zmsg_recv(handle);
#ifdef DEBUG
  dd_debug("s_on_subS_msg called");
  zmsg_print(msg);
#endif

  char *pubtopic = zmsg_popstr(msg);
  char *name = zmsg_popstr(msg);
  zframe_t *pathv = zmsg_pop(msg);

  dd_debug("pubtopic: %s source: %s", pubtopic, name);
  // zframe_print(pathv, "pathv: ");
  zlist_t *socks =
      nn_trie_tree(&topics_trie, (const uint8_t *)pubtopic, strlen(pubtopic));

  if (socks != NULL) {
    zframe_t *s = zlist_first(socks);
    dd_debug("Local sockids to send to: ");

    // TODO, this is a simplification, should take into account
    // srcpublic/dstpublic
    char *dot = strchr(pubtopic, '.');
    dot++;
    char *slash = strchr(dot, '/');
    if (slash)
      *slash = '\0';

    while (s) {
      print_zframe(s);
      zsock_send(rsock, "fbbssm", s, &dd_version, 4, &dd_cmd_pub, 4, name, dot,
                 msg);
      s = zlist_next(socks);
    }
    *slash = '/';
    zlist_destroy(&socks);
  } else {
    dd_debug("No matching nodes found by nn_trie_tree");
  }

  // if from the south, send north & south, multiple recievers south
  if (pubN)
    zsock_send(pubN, "ssfm", pubtopic, name, broker_id, msg);
  if (pubS)
    zsock_send(pubS, "ssfm", pubtopic, name, pathv, msg);

cleanup:
  free(pubtopic);
  free(name);
  zframe_destroy(&pathv);
  zmsg_destroy(&msg);
  return 0;
}

int s_on_pubN_msg(zloop_t *loop, zsock_t *handle, void *arg) {
  zmsg_t *msg = zmsg_recv(handle);
#ifdef DEBUG
  dd_debug("s_on_pubN_msg called");
  zmsg_print(msg);
#endif

  zframe_t *topic_frame = zmsg_pop(msg);
  char *topic = (char *)zframe_data(topic_frame);

  if (topic[0] == 1) {
    dd_info(" + Got subscription for: %s", &topic[1]);
    nn_trie_add_sub_north(&topics_trie, (const uint8_t *)&topic[1],
                          zframe_size(topic_frame) - 1);
  }
  if (topic[0] == 0) {
    dd_info(" - Got unsubscription for: %s", &topic[1]);
    nn_trie_del_sub_north(&topics_trie, (const uint8_t *)&topic[1],
                          zframe_size(topic_frame) - 1);
  }

  // subs from north should continue down
  if (subS)
    zsock_send(subS, "f", topic_frame);

  zframe_destroy(&topic_frame);
  zmsg_destroy(&msg);
  return 0;
}

int s_on_pubS_msg(zloop_t *loop, zsock_t *handle, void *arg) {
  zmsg_t *msg = zmsg_recv(handle);
#ifdef DEBUG
  dd_debug("s_on_pubS_msg called");
  zmsg_print(msg);
#endif

  zframe_t *topic_frame = zmsg_pop(msg);
  char *topic = (char *)zframe_data(topic_frame);

  if (topic[0] == 1) {
    dd_info(" + Got subscription for: %s", &topic[1]);
    nn_trie_add_sub_south(&topics_trie, (const uint8_t *)&topic[1],
                          zframe_size(topic_frame) - 1);
  }
  if (topic[0] == 0) {
    dd_info(" - Got unsubscription for: %s", &topic[1]);
    nn_trie_del_sub_south(&topics_trie, (const uint8_t *)&topic[1],
                          zframe_size(topic_frame) - 1);
  }

  // subs from north should continue down
  if (subS)
    zsock_send(subS, "f", topic_frame);
  if (subN)
    zsock_send(subN, "f", topic_frame);

  zframe_destroy(&topic_frame);
  zmsg_destroy(&msg);
  return 0;
}

int s_on_router_msg(zloop_t *loop, zsock_t *handle, void *arg) {
  zmsg_t *msg = zmsg_recv(handle);

#ifdef DEBUG
  dd_debug("s_on_router_msg called");
  zmsg_print(msg);
#endif

  if (msg == NULL) {
    dd_error("zmsg_recv returned NULL");
    return 0;
  }
  if (zmsg_size(msg) < 3) {
    dd_error("message less than 3, error!");
    zmsg_destroy(&msg);
    return 0;
  }
  zframe_t *source_frame = NULL;
  zframe_t *proto_frame = NULL;
  zframe_t *cmd_frame = NULL;
  zframe_t *cookie_frame = NULL;

  source_frame = zmsg_pop(msg);
  if (source_frame == NULL) {
    dd_error("Malformed message, missing SOURCE");
    goto cleanup;
  }
  proto_frame = zmsg_pop(msg);
  uint32_t *pver;
  pver = (uint32_t *)zframe_data(proto_frame);
  if (*pver != DD_VERSION) {
    dd_error("Wrong version, expected 0x%x, got 0x%x", DD_VERSION, *pver);
    zsock_send(rsock, "fbbbs", source_frame, pver, 4, &dd_cmd_error, 4,
               &dd_error_version, 4, "Different versions in use");
    goto cleanup;
  }
  cmd_frame = zmsg_pop(msg);
  if (cmd_frame == NULL) {
    dd_error("Malformed message, missing CMD");
    goto cleanup;
  }
  uint32_t cmd = *((uint32_t *)zframe_data(cmd_frame));

  switch (cmd) {
  case DD_CMD_SEND:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed SEND, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_send(source_frame, cookie_frame, msg);
    break;

  case DD_CMD_FORWARD:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed FORWARD, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_forward_rsock(source_frame, cookie_frame, msg);
    break;

  case DD_CMD_PING:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed PING, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_ping(source_frame, cookie_frame);
    break;

  case DD_CMD_SUB:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed SUB, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_sub(source_frame, cookie_frame, msg);
    break;

  case DD_CMD_UNSUB:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed UNSUB, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_unsub(source_frame, cookie_frame, msg);
    break;

  case DD_CMD_PUB:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed PUB, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_pub(source_frame, cookie_frame, msg);
    break;

  case DD_CMD_ADDLCL:
    cmd_cb_addlcl(source_frame, msg);
    break;

  case DD_CMD_ADDDCL:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed ADDDCL, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_adddcl(source_frame, cookie_frame, msg);
    break;

  case DD_CMD_ADDBR:
    cmd_cb_addbr(source_frame, msg);
    break;

  case DD_CMD_UNREG:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed ADDBR, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_unreg_cli(source_frame, cookie_frame, msg);
    break;

  case DD_CMD_UNREGDCLI:
    cookie_frame = zmsg_pop(msg);
    if (cookie_frame == NULL) {
      dd_error("Malformed UNREGDCLI, missing COOKIE");
      goto cleanup;
    }
    cmd_cb_unreg_dist_cli(source_frame, cookie_frame, msg);
    break;

  case DD_CMD_UNREGBR:
    cmd_cb_unreg_br(NULL, msg);
    break;

  case DD_CMD_CHALLOK:
    cmd_cb_challok(source_frame, msg);
    break;

  case DD_CMD_ERROR:
    // TODO implment
    dd_error("Recived CMD_ERROR from a client!");
    break;

  default:
    dd_error("Unknown command, value: 0x%x", cmd);
    break;
  }

cleanup:
  if (source_frame)
    zframe_destroy(&source_frame);
  if (proto_frame)
    zframe_destroy(&proto_frame);
  if (cmd_frame)
    zframe_destroy(&cmd_frame);
  if (cookie_frame)
    zframe_destroy(&cookie_frame);
  if (msg)
    zmsg_destroy(&msg);

  return 0;
}

int s_on_dealer_msg(zloop_t *loop, zsock_t *handle, void *arg) {
  timeout = 0;
  zmsg_t *msg = zmsg_recv(handle);
#ifdef DEBUG
  dd_debug("s_on_dealer_msg called");
  zmsg_print(msg);
#endif

  if (msg == NULL) {
    dd_error("zmsg_recv returned NULL");
    return 0;
  }
  if (zmsg_size(msg) < 2) {
    dd_error("message less than 2, error!");
    zmsg_destroy(&msg);
    return 0;
  }

  zframe_t *proto_frame = zmsg_pop(msg);

  if (*((uint32_t *)zframe_data(proto_frame)) != DD_VERSION) {
    dd_error("Wrong version, expected 0x%x, got 0x%x", DD_VERSION,
             *zframe_data(proto_frame));
    zframe_destroy(&proto_frame);
    return 0;
  }
  zframe_t *cmd_frame = zmsg_pop(msg);
  uint32_t cmd = *((uint32_t *)zframe_data(cmd_frame));
  zframe_destroy(&cmd_frame);
  switch (cmd) {
  case DD_CMD_REGOK:
    cmd_cb_regok(msg);
    break;
  case DD_CMD_FORWARD:
    cmd_cb_forward_dsock(msg);
    break;
  case DD_CMD_CHALL:
    cmd_cb_chall(msg);
    break;
  case DD_CMD_PONG:
    break;
  case DD_CMD_ERROR:
    cmd_cb_high_error(msg);
    break;
  default:
    dd_error("Unknown command, value: 0x%x", cmd);
    break;
  }
  zmsg_destroy(&msg);
  return 0;
}

int s_register(zloop_t *loop, int timer_id, void *arg) {
  dd_debug("trying to register..");

  if (state == DD_STATE_UNREG || state == DD_STATE_ROOT) {
    zsock_set_linger(dsock, 0);
    zloop_reader_end(loop, dsock);
    zsock_destroy((zsock_t **)&dsock);
    dsock = zsock_new_dealer(NULL);
    if (!dsock) {
      dd_error("Error in zsock_new_dealer: %s", zmq_strerror(errno));
      return -1;
    }

    int rc = zsock_connect(dsock, dealer_connect);
    if (rc != 0) {
      dd_error("Error in zmq_connect: %s", zmq_strerror(errno));
      return -1;
    }
    zloop_reader(loop, dsock, s_on_dealer_msg, NULL);

    zsock_send(dsock, "bbs", &dd_version, 4, &dd_cmd_addbr, 4, keys->hash);
  }
  return 0;
}

int s_heartbeat(zloop_t *loop, int timer_id, void *arg) {
  timeout += 1;
  zsock_t *socket = arg;
  if (timeout > 3) {
    state = DD_STATE_ROOT;
    zloop_timer_end(loop, heartbeat_loop);
    reg_loop = zloop_timer(loop, 1000, 0, s_register, dsock);
  }
  zsock_send(socket, "bbb", &dd_version, sizeof(dd_version), &dd_cmd_ping,
             sizeof(dd_cmd_ping), &keys->cookie, sizeof(keys->cookie));
  return 0;
}

int s_check_cli_timeout(zloop_t *loop, int timer_fd, void *arg) {
  // iterate through local clients and check if they should time out
  struct cds_lfht_iter iter;
  local_client *np;
  rcu_read_lock();
  cds_lfht_first(lcl_cli_ht, &iter);
  struct cds_lfht_node *ht_node = cds_lfht_iter_get_node(&iter);
  while (ht_node != NULL) {
    rcu_read_unlock();
    np = caa_container_of(ht_node, local_client, lcl_node);
    if (np->timeout < 3) {
      np->timeout += 1;
    } else {
      dd_debug("deleting local client %s", np->prefix_name);
      unreg_cli(np->sockid, np->cookie);
    }
    rcu_read_lock();
    cds_lfht_next(lcl_cli_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
  };
  rcu_read_unlock();
  return 0;
}

int s_check_br_timeout(zloop_t *loop, int timer_fd, void *arg) {
  // iterate through local brokers and check if they should time out
  struct cds_lfht_iter iter;
  local_broker *np;
  rcu_read_lock();
  cds_lfht_first(lcl_br_ht, &iter);
  struct cds_lfht_node *ht_node = cds_lfht_iter_get_node(&iter);
  rcu_read_unlock();
  while (ht_node != NULL) {
    np = caa_container_of(ht_node, local_broker, node);
    if (np->timeout < 3) {
      np->timeout += 1;
    } else {
      char buf[256];
      dd_debug("Deleting local broker %s", zframe_tostr(np->sockid, buf));

      delete_dist_clients(np);

      rcu_read_lock();
      int ret = cds_lfht_del(lcl_br_ht, ht_node);
      rcu_read_unlock();
      if (ret) {
        dd_info(" - Local broker %s removed (concurrently)",
                zframe_tostr(np->sockid, buf));
        free(np);
      } else {
        synchronize_rcu();
        dd_info(" - Local broker %s removed", zframe_tostr(np->sockid, buf));
        free(np);
      }
    }
    rcu_read_lock();
    cds_lfht_next(lcl_cli_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
    rcu_read_unlock();
  }
  return 0;
}

/* helper functions */

void add_cli_up(char *prefix_name, int distance) {
  if (state == DD_STATE_ROOT)
    return;

  dd_debug("add_cli_up(%s,%d), state = %d", prefix_name, distance, state);
  zsock_send(dsock, "bbbsb", &dd_version, 4, &dd_cmd_adddcl, 4, &keys->cookie,
             sizeof(keys->cookie), prefix_name, &distance, sizeof(distance));
}

void del_cli_up(char *prefix_name) {
  if (state != DD_STATE_ROOT) {
    dd_debug("del_cli_up %s", prefix_name);
    zsock_send(dsock, "bbbs", &dd_version, 4, &dd_cmd_unregdcli, 4,
               &keys->cookie, sizeof(keys->cookie), prefix_name);
  }
}

void forward_locally(zframe_t *dest_sockid, char *src_string, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("forward_locally: src: %s", src_string);
  zframe_print(dest_sockid, "dest_sockid");
  zmsg_print(msg);
#endif

  zsock_send(rsock, "fbbsm", dest_sockid, &dd_version, 4, &dd_cmd_data, 4,
             src_string, msg);
}

void forward_down(char *src_string, char *dst_string, zframe_t *br_sockid,
                  zmsg_t *msg) {
#ifdef DEBUG
  dd_info("Sending CMD_FORWARD to broker with sockid");
  print_zframe(br_sockid);
#endif
  zsock_send(rsock, "fbbssm", br_sockid, &dd_version, 4, &dd_cmd_forward, 4,
             src_string, dst_string, msg);
}

void forward_up(char *src_string, char *dst_string, zmsg_t *msg) {
#ifdef DEBUG
  dd_debug("forward_up called s: %s d: %s", src_string, dst_string);
  zmsg_print(msg);
#endif
  if (state == DD_STATE_REGISTERED)
    zsock_send(dsock, "bbbssm", &dd_version, 4, &dd_cmd_forward, 4,
               &keys->cookie, sizeof(keys->cookie), src_string, dst_string,
               msg);
}

void dest_invalid_rsock(zframe_t *sockid, char *src_string, char *dst_string) {
  zsock_send(rsock, "fbbbss", sockid, &dd_version, 4, &dd_cmd_error, 4,
             &dd_error_nodst, 4, dst_string, src_string);
}

void dest_invalid_dsock(char *src_string, char *dst_string) {
  zsock_send(dsock, "bbss", &dd_version, 4, &dd_cmd_error, 4, &dd_error_nodst,
             4, dst_string, src_string);
}

void unreg_cli(zframe_t *sockid, uint64_t cookie) {
  zframe_t *cookie_frame = zframe_new(&cookie, sizeof cookie);
  cmd_cb_unreg_cli(sockid, cookie_frame, NULL);
  zframe_destroy(&cookie_frame);
}

void unreg_broker(local_broker *np) {
  dd_warning("unreg_broker called, unimplemented!\n");
}

void connect_pubsubN(char *deprecated1, char *deprecated2) {
  dd_debug("Connect pubsubN (%s, %s)", deprecated1, deprecated2);

  zrex_t *rexipc = zrex_new(IPC_REGEX);
  assert(zrex_valid(rexipc));
  zrex_t *rextcp = zrex_new(TCP_REGEX);
  assert(zrex_valid(rextcp));
  sub_connect = malloc(strlen(dealer_connect) + 5);
  pub_connect = malloc(strlen(dealer_connect) + 5);

  char tmpfile[1024];
  if (zrex_matches(rexipc, dealer_connect)) {
    sprintf(sub_connect, "%s.pub", dealer_connect);
    sprintf(pub_connect, "%s.sub", dealer_connect);
  } else if (zrex_matches(rextcp, dealer_connect)) {
    int port = atoi(zrex_hit(rextcp, 2));
    sprintf(pub_connect, "%s%d", zrex_hit(rextcp, 1), port + 2);
    sprintf(sub_connect, "%s%d", zrex_hit(rextcp, 1), port + 1);
  } else {
    dd_error("%s doesnt match anything!");
    exit(EXIT_FAILURE);
  }

  zrex_destroy(&rexipc);
  zrex_destroy(&rextcp);

  dd_info("pub_connect: %s sub_connect: %s", pub_connect, sub_connect);
  pubN = zsock_new(ZMQ_XPUB);
  subN = zsock_new(ZMQ_XSUB);
  int rc = zsock_connect(pubN, pub_connect);
  if (rc < 0) {
    dd_error("Unable to connect pubN to %s", pub_connect);
    perror("Error: ");
    exit(EXIT_FAILURE);
  }

  rc = zsock_connect(subN, sub_connect);
  if (rc < 0) {
    dd_error("Unable to connect subN to %s", sub_connect);
    perror("Error: ");
    exit(EXIT_FAILURE);
  }
  rc = zloop_reader(loop, pubN, s_on_pubN_msg, NULL);
  assert(rc == 0);
  zloop_reader_set_tolerant(loop, pubN);

  rc = zloop_reader(loop, subN, s_on_subN_msg, NULL);
  assert(rc == 0);
  zloop_reader_set_tolerant(loop, subN);
}

char *str_replace(const char *string, const char *substr,
                  const char *replacement) {
  char *tok = NULL;
  char *newstr = NULL;
  char *oldstr = NULL;
  char *head = NULL;
  /*
   * if either substr or replacement is NULL, duplicate string a let
   * caller handle it
   */
  if (substr == NULL || replacement == NULL)
    return strdup(string);

  newstr = strdup(string);
  head = newstr;
  while ((tok = strstr(head, substr))) {
    oldstr = newstr;
    newstr = malloc(strlen(oldstr) - strlen(substr) + strlen(replacement) + 1);
    /*
     * failed to alloc mem, free old string and return NULL
     */
    if (newstr == NULL) {
      free(oldstr);
      return NULL;
    }
    memcpy(newstr, oldstr, tok - oldstr);
    memcpy(newstr + (tok - oldstr), replacement, strlen(replacement));
    memcpy(newstr + (tok - oldstr) + strlen(replacement), tok + strlen(substr),
           strlen(oldstr) - strlen(substr) - (tok - oldstr));
    memset(newstr + strlen(oldstr) - strlen(substr) + strlen(replacement), 0,
           1);
    /*
     * move back head right after the last replacement
     */
    head = newstr + (tok - oldstr) + strlen(replacement);
    free(oldstr);
  }
  return newstr;
}

void print_ddbrokerkeys(ddbrokerkeys_t *keys) {
  int siz = zlist_size(keys->tenants);
  dd_debug("Loaded %d tenant keys: ", siz);

  char *k = NULL;
  k = zlist_first(keys->tenants);

  dd_debug("Tenant keys: ");
  zlist_t *precalc = zhash_keys(keys->tenantkeys);
  ddtenant_t *ten;
  k = zlist_first(precalc);
  while (k) {
    ten = zhash_lookup(keys->tenantkeys, k);
    dd_debug("\t name: %s \tcookie: %llu", ten->name, ten->cookie);
    k = zlist_next(precalc);
  }
  //  free(hex);
}

void stop_program(int sig) {
  dd_debug("Stop program called");
  if (http != NULL)
    zsock_destroy(&http);
  zsock_destroy(&pubS);

  zsock_destroy(&rsock);
  zsock_destroy(&dsock);
  zsock_destroy(&pubN);
  zsock_destroy(&subS);
  zsock_destroy(&subN);

  exit(EXIT_SUCCESS);
}

void usage() {
  printf("broker -m <name> -d <dealer addr> -r <router addr> -v [verbose] -h "
         "[help]\n"
         "REQUIRED OPTIONS\n"
         "-r <router addr> e.g. tcp://127.0.0.1:5555\n"
         "   Multiple addresses with comma tcp://127.0.0.1:5555,ipc:///file\n"
         "   Router is where clients connect\n"
         "-k <keyfile>\n"
         "   JSON file containing the broker keys\n"
         "-s <scope>\n"
         "   scope ~ \"1/2/3\"\n"
         "OPTIONAL\n"
         "-d <dealer addr> e.g tcp://1.2.3.4:5555\n"
         "   Multiple addresses with comma tcp://127.0.0.1:5555,ipc:///file\n"
         "   Dealer should be connected to Router of another broker\n"
         "-l <loglevel> e:ERROR,w:WARNING,n:NOTICE,i:INFO,d:DEBUG,q:QUIET\n"
         "-w <rest addr> open a REST socket at <rest addr>, eg tcp://*:8080\n"
         "-f <config file> read config from config file\n"
         "-D  daemonize\n"
         "-v [verbose]\n"
         "-h [help]\n");
}

static void change_permission(char *t) {
  dd_debug("Setting permission on \"%s\" to rw-rw-rw-", t);
  int rc = chmod(t, rw_mode);
  if (rc == -1) {
    perror("Error: ");
    dd_error("Couldn't set permissions on IPC socket\n");
    exit(EXIT_FAILURE);
  }
}

zlist_t *pub_strings, *sub_strings;
void start_pubsub() {
  zrex_t *rexipc = zrex_new(IPC_REGEX);
  assert(zrex_valid(rexipc));
  zrex_t *rextcp = zrex_new(TCP_REGEX);
  assert(zrex_valid(rextcp));

  pub_strings = zlist_new();
  sub_strings = zlist_new();
  char *t = zlist_first(rstrings);
  char tmpfile[1024];
  while (t != NULL) {
    if (zrex_matches(rexipc, t)) {
      sprintf(tmpfile, "%s.pub", zrex_hit(rexipc, 2));
      if (zfile_exists(tmpfile)) {
        dd_error("File %s already exists, aborting.", tmpfile);
        exit(EXIT_FAILURE);
      }
      sprintf(tmpfile, "%s.sub", zrex_hit(rexipc, 2));
      if (zfile_exists(tmpfile)) {
        dd_error("File %s already exists, aborting.", tmpfile);
        exit(EXIT_FAILURE);
      }
      char *sub_ipc = malloc(strlen(t) + 5);
      char *pub_ipc = malloc(strlen(t) + 5);
      sprintf(sub_ipc, "%s.sub", t);
      sprintf(pub_ipc, "%s.pub", t);
      zlist_append(sub_strings, sub_ipc);
      zlist_append(pub_strings, pub_ipc);
      // Should not be necessary, but weird results otherwise..
      zrex_destroy(&rexipc);
      rexipc = zrex_new(IPC_REGEX);
    } else if (zrex_matches(rextcp, t)) {
      int port = atoi(zrex_hit(rextcp, 2));
      char *sub_tcp = malloc(strlen(t) + 1);
      char *pub_tcp = malloc(strlen(t) + 1);
      sprintf(pub_tcp, "%s%d", zrex_hit(rextcp, 1), port + 1);
      sprintf(sub_tcp, "%s%d", zrex_hit(rextcp, 1), port + 2);
      zlist_append(sub_strings, sub_tcp);
      zlist_append(pub_strings, pub_tcp);
      // Should not be necessary, but weird results otherwise..
      zrex_destroy(&rextcp);
      rextcp = zrex_new(TCP_REGEX);
    } else {
      dd_error("%s doesnt match anything!");
      exit(EXIT_FAILURE);
    }
    t = zlist_next(rstrings);
  }

  zrex_destroy(&rextcp);
  zrex_destroy(&rexipc);

  t = zlist_first(pub_strings);
  int pub_strings_len = 0;
  while (t != NULL) {
    pub_strings_len += strlen(t) + 1;
    t = zlist_next(pub_strings);
  }

  int sub_strings_len = 0;
  t = zlist_first(sub_strings);
  while (t != NULL) {
    sub_strings_len += strlen(t) + 1;
    t = zlist_next(sub_strings);
  }

  if (zlist_size(pub_strings) < 1) {
    dd_error("pub_strings zlist empty!");
    exit(EXIT_FAILURE);
  }
  if (zlist_size(sub_strings) < 1) {
    dd_error("sub_strings zlist empty!");
    exit(EXIT_FAILURE);
  }

  pub_bind = malloc(pub_strings_len);
  sub_bind = malloc(sub_strings_len);

  int i, written = 0;
  int num_len = zlist_size(pub_strings);

  for (i = 0; i < num_len; i++) {
    if (i == 0) {
      t = zlist_first(pub_strings);
    } else {
      t = zlist_next(pub_strings);
    }
    written += snprintf(pub_bind + written, pub_strings_len - written,
                        (i != 0 ? ",%s" : "%s"), t);
    if (written == pub_strings_len)
      break;
  }

  written = 0;
  num_len = zlist_size(sub_strings);

  for (i = 0; i < num_len; i++) {
    if (i == 0) {
      t = zlist_first(sub_strings);
    } else {
      t = zlist_next(sub_strings);
    }
    written += snprintf(sub_bind + written, sub_strings_len - written,
                        (i != 0 ? ",%s" : "%s"), t);
    if (written == sub_strings_len)
      break;
  }

  pubS = zsock_new(ZMQ_XPUB);
  subS = zsock_new(ZMQ_XSUB);
  int rc = zsock_attach(pubS, pub_bind, true);
  if (rc < 0) {
    dd_error("Unable to attach pubS to %s", pub_bind);
    perror("Error: ");
    exit(EXIT_FAILURE);
  }
  rc = zsock_attach(subS, sub_bind, true);
  if (rc < 0) {
    dd_error("Unable to attach subS to %s", sub_bind);
    perror("Error: ");
    exit(EXIT_FAILURE);
  }

  t = zlist_first(pub_strings);
  while (t != NULL) {
    if (strcasestr(t, "ipc://")) {
      change_permission(t + 6);
    }
    t = zlist_next(pub_strings);
  }

  t = zlist_first(sub_strings);
  while (t != NULL) {
    if (strcasestr(t, "ipc://")) {
      change_permission(t + 6);
    }
    t = zlist_next(sub_strings);
  }

  rc = zloop_reader(loop, pubS, s_on_pubS_msg, NULL);
  assert(rc == 0);
  zloop_reader_set_tolerant(loop, pubS);

  rc = zloop_reader(loop, subS, s_on_subS_msg, NULL);
  assert(rc == 0);
  zloop_reader_set_tolerant(loop, subS);
}

char *zframe_tojson(zframe_t *self, char *buffer);
json_object *json_stats(int flags) {
  json_object *jobj = json_object_new_object();
  json_object *jdist_array = json_object_new_array();

  // iterate through distant clients
  struct cds_lfht_iter iter;
  dist_client *mp;
  cds_lfht_first(dist_cli_ht, &iter);
  struct cds_lfht_node *ht_node = cds_lfht_iter_get_node(&iter);
  while (ht_node != NULL) {
    mp = caa_container_of(ht_node, dist_client, node);
    json_object_array_add(jdist_array, json_object_new_string(mp->name));
    cds_lfht_next(dist_cli_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
  }

  // Iterate through local clients
  json_object *jlocal_obj = json_object_new_object();
  local_client *lp;
  cds_lfht_first(rev_lcl_cli_ht, &iter);
  ht_node = cds_lfht_iter_get_node(&iter);
  while (ht_node != NULL) {
    local_client *lp = caa_container_of(ht_node, local_client, rev_node);
    char buf[256];
    json_object *strval = json_object_new_string(lp->prefix_name);
    json_object_object_add(jlocal_obj, zframe_tojson(lp->sockid, buf), strval);
    cds_lfht_next(rev_lcl_cli_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
  }
  // iterate through brokers
  json_object *jbr_array = json_object_new_array();
  local_broker *br;
  cds_lfht_first(lcl_br_ht, &iter);
  ht_node = cds_lfht_iter_get_node(&iter);
  while (ht_node != NULL) {
    br = caa_container_of(ht_node, local_broker, node);
    char buf[256];
    json_object_array_add(
        jbr_array, json_object_new_string(zframe_tojson(br->sockid, buf)));
    cds_lfht_next(lcl_br_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
  }

  // iterate through subscriptions
  subscribe_node *sn;
  json_object *jsub_dict = json_object_new_object();
  cds_lfht_first(subscribe_ht, &iter);
  ht_node = cds_lfht_iter_get_node(&iter);
  while (ht_node != NULL) {
    sn = caa_container_of(ht_node, subscribe_node, node);
    json_object *jsub_array = json_object_new_array();
    if (sn->topics) {
      char *str = zlist_first(sn->topics);
      while (str) {
        json_object_array_add(jsub_array, json_object_new_string(str));
        str = zlist_next(sn->topics);
      }
    } else {
      json_object_array_add(jsub_array, json_object_new_string("empty!"));
    }
    char buf[256];
    json_object_object_add(jsub_dict, zframe_tojson(sn->sockid, buf),
                           jsub_array);
    cds_lfht_next(subscribe_ht, &iter);
    ht_node = cds_lfht_iter_get_node(&iter);
  }

  json_object_object_add(jobj, "brokers", jbr_array);
  json_object_object_add(jobj, "local", jlocal_obj);
  json_object_object_add(jobj, "distant", jdist_array);
  json_object_object_add(jobj, "subs", jsub_dict);
  json_object_object_add(jobj, "version",
                         json_object_new_string(PACKAGE_VERSION));
  return jobj;
}

// seperate to a different thread?
int s_on_http(zloop_t *loop, zsock_t *handle, void *arg) {
  zmsg_t *msg = zmsg_recv(handle);
  zframe_t *id = zmsg_pop(msg);
  zframe_t *data = zmsg_pop(msg);
  char *http_request = (char *)zframe_data(data);
  char *http_all = "GET / HTTP/1.1\r\n";
  char *http_dist = "GET /distant HTTP/1.1\r\n";
  char *http_local = "GET /local HTTP/1.1\r\n";
  char *http_sub = "GET /subscriptions HTTP/1.1\r\n";

  int flags = 0;
#define HTTP_ALL 0b111
#define HTTP_SUBS 0b001
#define HTTP_LOCAL 0b010
#define HTTP_DIST 0b100
  if (strncmp(http_all, http_request, strlen(http_all)) == 0) {
    flags |= HTTP_ALL;
  } else if (strncmp(http_dist, http_request, strlen(http_dist)) == 0) {
    flags |= HTTP_DIST;
  } else if (strncmp(http_local, http_request, strlen(http_local)) == 0) {
    flags |= HTTP_LOCAL;
  } else if (strncmp(http_sub, http_request, strlen(http_sub)) == 0) {
    flags |= HTTP_SUBS;
  }

  if (flags == 0) {
    dd_error("Got unknown http request %s", strchr(http_request, '\r'));
    char http_response[] = "HTTP/1.1 404 Not Found\r\n"
                           "Date: Fri, 22 Apr 2016 19:04:59 GMT\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "Access-Control-Allow-Methods: GET\r\n"
                           "Access-Control-Allow-Headers: Content-Type\r\n"
                           "Content-Type: application/json\r\n"
                           "Server: DoubleDecker\r\n"
                           "\r\n";
    zsock_send(handle, "fs", id, http_response);
    zsock_send(handle, "fz", id);
  } else {
    char timebuf[32];
    struct tm tmstruct;
    time_t inctime = time(NULL);
    if (!gmtime_r(&inctime, &tmstruct))
      return -1;
    int tlen = strftime(timebuf, 32, "%a, %d %b %Y %T GMT", &tmstruct);
    if (tlen <= 0)
      return -1;

    char *http_res;
    char http_ok[] = "HTTP/1.1 200 OK\r\n";
    char http_stat[] = "Access-Control-Allow-Origin: *\r\n"
                       "Access-Control-Allow-Methods: GET\r\n"
                       "Access-Control-Allow-Headers: Content-Type\r\n"
                       "Content-Type: application/json\r\n"
                       "Server: DoubleDecker\r\n"
                       "Connection: close\r\n";
    json_object *jobj = json_stats(flags);
    const char *json = json_object_to_json_string(jobj);
    // get rid of json object
    int retval = asprintf(&http_res, "%s%s\r\n%sContent-Length: %lu\r\n\r\n%s",
                          http_ok, timebuf, http_stat, strlen(json), json);
    zsock_send(handle, "fs", id, http_res);
    zsock_send(handle, "fz", id);
    free(http_res);
    json_object_put(jobj);
  }
  zframe_destroy(&id);
  zframe_destroy(&data);
  zmsg_destroy(&msg);
  return 0;
}

void start_httpd() {
  http = zsock_new(ZMQ_STREAM);
  int rc = zsock_bind(http, reststr);
  if (rc == -1) {
    dd_error("Could not initilize HTTP port 9080!");
    zsock_destroy(&http);
    return;
  }

  rc = zloop_reader(loop, http, s_on_http, NULL);
  assert(rc == 0);
  zloop_reader_set_tolerant(loop, http);
}

int start_broker(char *router_bind, char *dealer_connect, char *keyfile,
                 int verbose) {
  dd_info("Starting broker, router at %s, dealer at %s", router_bind,
          dealer_connect);
  keys = read_ddbrokerkeys(keyfile);
  if (keys == NULL) {
    exit(EXIT_FAILURE);
  }

  broker_id = zframe_new("root", 4);
  broker_id_null = zframe_new("", 0);

  print_ddbrokerkeys(keys);
  randombytes_buf(nonce, crypto_box_NONCEBYTES);

  zframe_t *f;
  // needs to be called for each thread using RCU lib
  rcu_register_thread();

  init_hashtables();

  loop = zloop_new();
  assert(loop);
  rsock = zsock_new(ZMQ_ROUTER);
  // Look for IPC strings in the rstrings list, check if the files already exist
  char *t = zlist_first(rstrings);
  char *needle;
  while (t != NULL) {
    needle = strcasestr(t, "ipc://");
    if (needle) {
      if (zfile_exists(t + 6)) {
        dd_error("File %s already exists, aborting.", t + 6);
        exit(EXIT_FAILURE);
      }
    }
    t = zlist_next(rstrings);
  }

  // zsock_bind(rsock, router_bind);
  // use zsock_attach instead, as it supports multiple endpoints
  int rc;
  dd_info("Attaching ROUTER socket to: %s", router_bind);
  rc = zsock_attach(rsock, router_bind, 1 == 1);
  if (rc == 0) {
    dd_info("Successfully bound router to %s", router_bind);
  } else {
    dd_info("Failed to bind router to %s", router_bind);
    exit(EXIT_FAILURE);
  }

  if (rsock == NULL) {
    dd_error("Couldn't bind router socket to %s", router_bind);
    perror("Error: ");
    exit(EXIT_FAILURE);
  }

  // change the permission on the IPC sockets to allow anyone to connect
  t = zlist_first(rstrings);
  while (t != NULL) {
    needle = strcasestr(t, "ipc://");
    if (needle) {
      change_permission(t + 6);
    }
    t = zlist_next(rstrings);
  }

  rc = zloop_reader(loop, rsock, s_on_router_msg, NULL);
  assert(rc == 0);
  zloop_reader_set_tolerant(loop, rsock);

  if (dealer_connect != NULL) {
    dsock = zsock_new(ZMQ_DEALER);
    zsock_connect(dsock, dealer_connect);
    if (dsock == NULL) {
      dd_error("Couldn't connect dealer socket to %s", dealer_connect);
      perror("Error: ");
      exit(EXIT_FAILURE);
    }
    rc = zloop_reader(loop, dsock, s_on_dealer_msg, NULL);
    assert(rc == 0);
    zloop_reader_set_tolerant(loop, dsock);
    reg_loop = zloop_timer(loop, 1000, 0, s_register, dsock);
  } else {
    dd_info("No dealer defined, the broker will act as the root");
    state = DD_STATE_ROOT;
  }

  cli_timeout_loop = zloop_timer(loop, 3000, 0, s_check_cli_timeout, NULL);
  br_timeout_loop = zloop_timer(loop, 1000, 0, s_check_br_timeout, NULL);

  // create and attach the pubsub southbound sockets
  start_pubsub();

  if (reststr != NULL)
    start_httpd();

  zloop_start(loop);

  zloop_destroy(&loop);
  if (http)
    zsock_set_linger(http, 0);
  if (pubS)
    zsock_set_linger(pubS, 0);
  if (pubN)
    zsock_set_linger(pubN, 0);
  if (subS)
    zsock_set_linger(subS, 0);
  if (subN)
    zsock_set_linger(subN, 0);
  if (dsock)
    zsock_set_linger(dsock, 0);
  if (rsock)
    zsock_set_linger(rsock, 0);

  zsock_destroy(&http);
  zsock_destroy(&pubS);
  zsock_destroy(&pubN);
  zsock_destroy(&subS);
  zsock_destroy(&subN);
  zsock_destroy(&dsock);
  zsock_destroy(&rsock);
  dd_info("Destroyed all open sockets, waiting a second..");
  // TODO:
  // Weird bug here, if run in interactive mode and killed with ctrl-c
  // All IPC unix domain socket files seems to be removed just fine
  // However, running in daemonized mode and killed with killall (sigterm)
  // unix socket files are sometimes left. sleeping a second here seems
  // to fix it.. some background threads that dont have time to finish properly?
  sleep(1);
  zsys_shutdown();
  return 1;
}
char *keyfile = NULL;
char *scopestr = NULL;
char *logstr = "i";

void load_config(char *configfile) {
  zconfig_t *root = zconfig_load(configfile);
  if (root == NULL) {
    dd_error("Could not read configuration file \"%s\"\n", configfile);
    exit(EXIT_FAILURE);
  }
  zconfig_t *child = zconfig_child(root);
  while (child != NULL) {
    if (strncmp(zconfig_name(child), "dealer", strlen("dealer")) == 0) {
      dealer_connect = zconfig_value(child);
    } else if (strncmp(zconfig_name(child), "scope", strlen("scope")) == 0) {
      scopestr = zconfig_value(child);
    } else if (strncmp(zconfig_name(child), "router", strlen("router")) == 0) {
      if (router_bind == NULL) {
        router_bind = zconfig_value(child);
      } else {
        char *new_router_bind;
        asprintf(&new_router_bind, "%s,%s", router_bind, zconfig_value(child));
        // free(router_bind);
        // can't free here since router_bind may be set by getopt - optarg
        // we'll loose some memory for every asprintf..
        router_bind = new_router_bind;
      }
    } else if (strncmp(zconfig_name(child), "rest", strlen("rest")) == 0) {
      reststr = zconfig_value(child);
    } else if (strncmp(zconfig_name(child), "loglevel", strlen("loglevel")) ==
               0) {
      logstr = zconfig_value(child);
    } else if (strncmp(zconfig_name(child), "keyfile", strlen("keyfile")) ==
               0) {
      keyfile = zconfig_value(child);
    } else if (strncmp(zconfig_name(child), "logfile", strlen("logfile")) ==
               0) {
      logfile = zconfig_value(child);
    } else if (strncmp(zconfig_name(child), "syslog", strlen("syslog")) == 0) {
      syslog_enabled = zconfig_value(child);
    } else if (strncmp(zconfig_name(child), "daemonize", strlen("daemonize")) ==
               0) {
      daemonize = 1;
    } else {
      dd_error("Unknown key in configuration file, \"%s\"",
               zconfig_name(child));
    }
    child = zconfig_next(child);
  }
  return;
}

int main(int argc, char **argv) {
  int c;
  char *configfile = NULL;

  void *ctx = zsys_init();
  zsys_set_logident("DD");
  opterr = 0;
  while ((c = getopt(argc, argv, "d:r:l:k:s:vhm:f:w:D")) != -1)
    switch (c) {
    case 'd':
      dealer_connect = optarg;
      break;
    case 'r':
      router_bind = optarg;
      break;
    case 'v':
      verbose = 1;
      break;
    case 'D':
      daemonize = 1;
      break;
    case 'h':
      usage();
      exit(EXIT_FAILURE);
      break;
    case 'k':
      keyfile = optarg;
      break;
    case 'l':
      logstr = optarg;
      int i;
      for (i = 0; logstr[i]; i++) {
        logstr[i] = tolower(logstr[i]);
      }
      break;
    case 's':
      scopestr = optarg;
      break;
    case 'w':
      reststr = optarg;
      break;
    case 'f':
      configfile = optarg;
      break;
    case '?':
      if (optopt == 'c' || optopt == 's') {
        dd_error("Option -%c requires an argument.", optopt);
      } else if (isprint(optopt)) {
        dd_error("Unknown option `-%c'.", optopt);
      } else {
        dd_error("Unknown option character `\\x%x'.", optopt);
      }
      return 1;
    default:
      abort();
    }

  if (configfile != NULL) {
    load_config(configfile);
  }

  if (strncmp(logstr, "e", 1) == 0) {
    loglevel = DD_LOG_ERROR;
  } else if (strncmp(logstr, "w", 1) == 0) {
    loglevel = DD_LOG_WARNING;
  } else if (strncmp(logstr, "n", 1) == 0) {
    loglevel = DD_LOG_NOTICE;
  } else if (strncmp(logstr, "i", 1) == 0) {
    loglevel = DD_LOG_INFO;
  } else if (strncmp(logstr, "d", 1) == 0) {
    loglevel = DD_LOG_DEBUG;
  } else if (strncmp(logstr, "q", 1) == 0) {
    loglevel = DD_LOG_NONE;
  }
  if (logfile) {
    FILE *logfp = fopen(logfile, "w+");
    if (logfp) {
      zsys_set_logstream(logfp);
    } else {
      dd_error("Couldn't open logfile %s", logfile);
      exit(EXIT_FAILURE);
    }
  }
  if (syslog_enabled) {
    dd_info("Logging to syslog..");
    zsys_set_logsystem(true);
  }
  if (keyfile == NULL) {
    dd_error("Keyfile required (-k <file>>)\n");
    usage();
    exit(EXIT_FAILURE);
  }
  if (scopestr == NULL) {
    dd_error("Scope required (-s <scopestr>>)\n");
    usage();
    exit(EXIT_FAILURE);
  }

  if (daemonize == 1) {
    daemon(0, 0);
  }
  nn_trie_init(&topics_trie);
  dd_info("%s - <%s> - %s", PACKAGE_STRING, PACKAGE_BUGREPORT, PACKAGE_URL);
  // if no router in config or as cli, set default
  if (router_bind == NULL)
    router_bind = "tcp://*:5555";

  scope = zlist_new();
  rstrings = zlist_new();
  char *str1, *str2, *token, *subtoken;
  char *saveptr1, *saveptr2;
  int j;

  char *rbind_cpy = strdup(router_bind);
  token = strtok(rbind_cpy, ",");
  while (token) {
    zlist_append(rstrings, token);
    token = strtok(NULL, ",");
  }

  char *t = zlist_first(rstrings);
  while (t != NULL) {
    dd_debug("Found router string %s", t);
    t = zlist_next(rstrings);
  }

  for (j = 1, str1 = scopestr;; j++, str1 = NULL) {
    token = strtok_r(str1, "/", &saveptr1);
    if (token == NULL)
      break;
    if (!is_int(token)) {
      dd_error("Only '/' and digits in scope, %s is not!", token);
      exit(EXIT_FAILURE);
    }
    zlist_append(scope, token);
  }

  char brokerscope[256];
  broker_scope = &brokerscope[0];
  int len = 256;
  int retval = snprintf(broker_scope, len, "/");
  broker_scope += retval;
  len -= retval;

  t = zlist_first(scope);
  while (t != NULL) {
    retval = snprintf(broker_scope, len, "%s/", t);
    broker_scope += retval;
    len -= retval;
    t = zlist_next(scope);
  }
  broker_scope = &brokerscope[0];
  dd_debug("broker scope set to: %s", broker_scope);

  start_broker(router_bind, dealer_connect, keyfile, verbose);
}