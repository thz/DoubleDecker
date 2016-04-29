#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <czmq.h>
#include "../config.h"
#include "../../include/dd.h"
#include "../../include/dd_classes.h"
#include "../../include/protocol.h"
#include "../../include/ddkeys.h"

// Structure of the ddclient class
struct _dd_t {
  void *socket; //  Socket for clients & workers
  void *pipe;
  int verbose;                //  Print activity to stdout
  unsigned char *endpoint;    //  Broker binds to this endpoint
  unsigned char *customer;    //  Our customer id
  unsigned char *keyfile;     // JSON file with pub/priv keys
  unsigned char *client_name; // This client name
  int timeout;                // Incremental timeout (trigger > 3)
  int state;                  // Internal state
  int registration_loop;      // Timer ID for registration loop
  int heartbeat_loop;         // Timer ID for heartbeat loop
  uint64_t cookie;            // Cookie from authentication
  struct ddkeystate *keys;    // Encryption keys loaded from JSON file
  zlistx_t *sublist;          // List of subscriptions, and if they're active
  zloop_t *loop;
  int style;
  unsigned char nonce[crypto_box_NONCEBYTES];
  dd_con(*on_reg);
  dd_discon(*on_discon);
  dd_data(*on_data);
  dd_pub(*on_pub);
  dd_error(*on_error);
};

static void sublist_resubscribe(dd_t *self) {
  ddtopic_t *item;
  while ((item = zlistx_next((zlistx_t *)dd_get_subscriptions(self)))) {
    zsock_send(self->socket, "bbbss", &dd_version, 4, &dd_cmd_sub, 4,
               &self->cookie, sizeof(self->cookie), dd_sub_get_topic(item),
               dd_sub_get_scope(item));
  }
}

// ////////////////////////////////////////////////////
// // Commands for subscribe / publish / sendmessage //
// ////////////////////////////////////////////////////
const char *dd_get_version() { return PACKAGE_VERSION; }

int dd_get_state(dd_t *self) { return self->state; }

const char *dd_get_endpoint(dd_t *self) { return (const char *)self->endpoint; }

const char *dd_get_keyfile(dd_t *self) { return (const char *)self->keyfile; }

char *dd_get_privkey(dd_t *self) {
  char *hex = malloc(100);
  sodium_bin2hex(hex, 100, self->keys->privkey, crypto_box_SECRETKEYBYTES);
  return hex;
}
char *dd_get_pubkey(dd_t *self) {
  assert(self);
  char *hex = malloc(100);
  assert(hex);
  sodium_bin2hex(hex, 100, self->keys->pubkey, crypto_box_PUBLICKEYBYTES);
  return hex;
}
char *dd_get_publickey(dd_t *self) {
  assert(self);
  char *hex = malloc(100);
  assert(hex);
  sodium_bin2hex(hex, 100, self->keys->publicpubkey, crypto_box_PUBLICKEYBYTES);
  return hex;
}

const zlistx_t *dd_get_subscriptions(dd_t *self) { return self->sublist; }

int dd_subscribe(dd_t *self, char *topic, char *scope) {
  char *scopestr;
  if (strcmp(scope, "all") == 0) {
    scopestr = "/";
  } else if (strcmp(scope, "region") == 0) {
    scopestr = "/*/";
  } else if (strcmp(scope, "cluster") == 0) {
    scopestr = "/*/*/";
  } else if (strcmp(scope, "node") == 0) {
    scopestr = "/*/*/*/";
  } else if (strcmp(scope, "noscope") == 0) {
    scopestr = "noscope";
  } else {
    // TODO
    // check that scope follows re.fullmatch("/((\d)+/)+", scope):
    scopestr = scope;
  }
  sublist_add(topic, scopestr, 0, self);
  if (self->state == DD_STATE_REGISTERED) {
    zsock_send(self->socket, "bbbss", &dd_version, 4, &dd_cmd_sub, 4,
               &self->cookie, sizeof(self->cookie), topic, scopestr);
    return 0;
  }
  return -1;
}

int dd_unsubscribe(dd_t *self, char *topic, char *scope) {
  char *scopestr;
  if (strcmp(scope, "all") == 0) {
    scopestr = "/";
  } else if (strcmp(scope, "region") == 0) {
    scopestr = "/*/";
  } else if (strcmp(scope, "cluster") == 0) {
    scopestr = "/*/*/";
  } else if (strcmp(scope, "node") == 0) {
    scopestr = "/*/*/*/";
  } else if (strcmp(scope, "noscope") == 0) {
    scopestr = "noscope";
  } else {
    // TODO
    // check that scope follows re.fullmatch("/((\d)+/)+", scope):
    scopestr = scope;
  }
  sublist_delete(topic, scopestr, self);
  if (self->state == DD_STATE_REGISTERED)
    zsock_send(self->socket, "bbbss", &dd_version, 4, &dd_cmd_unsub, 4,
               &self->cookie, sizeof(self->cookie), topic, scopestr);
  return 0;
}

int dd_publish(dd_t *self, char *topic, char *message, int mlen) {
  unsigned char *precalck = NULL;
  int srcpublic = 0;
  int dstpublic = 0;
  int retval;

  // printf ("dd->publish called t: %s m: %s l: %d\n", topic, message,
  // mlen);

  if (streq((const char *)self->customer, "public")) {
    srcpublic = 1;
  }
  if (strncmp("public.", topic, strlen("public.")) == 0) {
    dstpublic = 1;
  }

  char *dot = strchr(topic, '.');
  if (dot && srcpublic) {
    *dot = '\0';
    precalck = zhash_lookup(self->keys->clientkeys, topic);
    if (precalck) {
      //	printf("encrypting with tenant key: %s\n",topic);
      // TODO: This is not allowed by the broker
      // We should return an error if this is happening
      fprintf(stderr, "Public client cannot publish to tenants!\n");
      return -1;
    }
    *dot = '.';
  }
  if (!precalck && !dstpublic) {
    precalck = self->keys->custboxk;
    //      printf ("encrypting with my own key\n");
  } else if (dstpublic) {
    precalck = self->keys->pubboxk;
    //    printf("encrypting with public key\n");
  }

  int enclen = mlen + crypto_box_NONCEBYTES + crypto_box_MACBYTES;
  //  printf ("encrypted message will be %d bytes\n", enclen);
  // unsigned char ciphertext[enclen];
  unsigned char *dest = calloc(1, enclen);
  unsigned char *ciphertext = dest; // dest+crypto_box_NONCEBYTES;

  // increment nonce
  sodium_increment(self->nonce, crypto_box_NONCEBYTES);
  memcpy(dest, self->nonce, crypto_box_NONCEBYTES);

  dest += crypto_box_NONCEBYTES;
  retval = crypto_box_easy_afternm(dest, (const unsigned char *)message, mlen,
                                   self->nonce, precalck);
  //  char *hex = calloc (1, 1000);
  //  sodium_bin2hex (hex, 1000, ciphertext, enclen);
  //  printf ("ciphertext size %d: %s\n", enclen, hex);
  //  free (hex);

  if (retval != 0) {
    fprintf(stderr, "DD: Unable to encrypt %d bytes!\n", mlen);
    free(ciphertext);
    return -1;
  }
  if (self->state == DD_STATE_REGISTERED) {
    zsock_send(self->socket, "bbbszb", &dd_version, 4, &dd_cmd_pub, 4,
               &self->cookie, sizeof(self->cookie), topic, ciphertext, enclen);
  }
  free(ciphertext);
  return 0;
}

// - Publish between public and other customers not working
int dd_notify(dd_t *self, char *target, char *message, int mlen) {
  unsigned char *precalck = NULL;
  int srcpublic = 0;
  int dstpublic = 0;

  if (streq((const char *)self->customer, "public")) {
    srcpublic = 1;
  }
  if (strncmp("public.", target, strlen("public.")) == 0) {
    dstpublic = 1;
  }

  /* printf ("self->sendmsg called t: %s m: %s l: %d\n", target, message,
   * mlen);
   */
  char *dot = strchr(target, '.');

  int retval;
  if (dot && srcpublic) {
    *dot = '\0';
    precalck = zhash_lookup(self->keys->clientkeys, target);
    if (precalck) {
      /* printf("encrypting with tenant key: %s\n",target); */
    }
    *dot = '.';
  }
  if (!precalck && !dstpublic) {
    precalck = self->keys->custboxk;
    /* printf ("encrypting with my own key\n"); */
  } else if (dstpublic) {
    precalck = self->keys->pubboxk;
    /* printf("encrypting with public key\n"); */
  }

  int enclen = mlen + crypto_box_NONCEBYTES + crypto_box_MACBYTES;
  /* printf ("encrypted message will be %d bytes\n", enclen); */
  // unsigned char ciphertext[enclen];
  unsigned char *dest = calloc(1, enclen);
  unsigned char *ciphertext = dest; // dest+crypto_box_NONCEBYTES;

  // increment nonce
  sodium_increment(self->nonce, crypto_box_NONCEBYTES);
  memcpy(dest, self->nonce, crypto_box_NONCEBYTES);

  dest += crypto_box_NONCEBYTES;
  retval = crypto_box_easy_afternm(dest, (const unsigned char *)message, mlen,
                                   self->nonce, precalck);
  /* char *hex = calloc (1, 1000); */
  /* sodium_bin2hex (hex, 1000, ciphertext, enclen); */
  /* printf ("ciphertext size %d: %s\n", enclen, hex); */
  /* free (hex); */

  if (retval == 0) {
  } else {
    fprintf(stderr, "DD: Unable to encrypt %d bytes!\n", mlen);
    free(ciphertext);
    return -1;
  }
  if (self->state == DD_STATE_REGISTERED) {
    zsock_send(self->socket, "bbbsb", &dd_version, 4, &dd_cmd_send, 4,
               &self->cookie, sizeof(self->cookie), target, ciphertext, enclen);
  }
  free(ciphertext);
  return 0;
}

int dd_destroy(dd_t **self) {
  printf("DD: Shutting down ddthread..\n");
  if ((*self)->state == DD_STATE_REGISTERED) {
    zsock_send((*self)->socket, "bbb", &dd_version, 4, &dd_cmd_unreg, 4,
               &(*self)->cookie, sizeof((*self)->cookie));
  }
  if ((*self)->sublist)
    zlistx_destroy(&(*self)->sublist);
  if ((*self)->loop)
    zloop_destroy(&(*self)->loop);
  if ((*self)->socket)
    zsock_destroy((zsock_t **)&(*self)->socket);
  (*self)->state = DD_STATE_EXIT;
  return 0;
}

// ////////////////////////
// callbacks from zloop //
// ////////////////////////

static int s_on_dealer_msg(zloop_t *loop, zsock_t *handle, void *args);

static int s_ask_registration(zloop_t *loop, int timerid, void *args);

static int s_ping(zloop_t *loop, int timerid, void *args) {
  dd_t *self = (dd_t *)args;
  if (self->state == DD_STATE_REGISTERED)
    zsock_send(self->socket, "bbb", &dd_version, 4, &dd_cmd_ping, 4,
               &self->cookie, sizeof(self->cookie));
  return 0;
}

static int s_heartbeat(zloop_t *loop, int timerid, void *args) {
  dd_t *self = (dd_t *)args;
  self->timeout++;
  if (self->timeout > 3) {
    self->state = DD_STATE_UNREG;
    self->registration_loop =
        zloop_timer(loop, 1000, 0, s_ask_registration, self);
    zloop_timer_end(loop, self->heartbeat_loop);
    sublist_deactivate_all(self);
    self->on_discon(self);
  }
  return 0;
}

static int s_ask_registration(zloop_t *loop, int timerid, void *args) {
  dd_t *self = (dd_t *)args;
  if (self->state == DD_STATE_UNREG) {
    zsock_set_linger(self->socket, 0);
    zloop_reader_end(loop, self->socket);
    zsock_destroy((zsock_t **)&self->socket);
    self->socket = zsock_new_dealer(NULL);
    if (!self->socket) {
      fprintf(stderr, "DD: Error in zsock_new_dealer: %s\n",
              zmq_strerror(errno));
      free(self);
      return -1;
    }
    int rc = zsock_connect(self->socket, (const char *)self->endpoint);
    if (rc != 0) {
      fprintf(stderr, "DD: Error in zmq_connect: %s\n", zmq_strerror(errno));
      free(self);
      return -1;
    }
    zloop_reader(loop, self->socket, s_on_dealer_msg, self);
    struct ddkeystate *k = self->keys;
    zsock_send(self->socket, "bbs", &dd_version, 4, &dd_cmd_addlcl, 4, k->hash);
  }
  return 0;
}

// /////////////////////////////////////
// / callbacks for different messages //
// ////////////////////////////////////
static void cb_regok(dd_t *self, zmsg_t *msg, zloop_t *loop) {
  zframe_t *cookie_frame;
  cookie_frame = zmsg_pop(msg);
  if (cookie_frame == NULL) {
    fprintf(stderr, "DD: Misformed REGOK message, missing COOKIE!\n");
    return;
  }
  uint64_t *cookie2 = (uint64_t *)zframe_data(cookie_frame);
  self->cookie = *cookie2;
  zframe_destroy(&cookie_frame);
  self->state = DD_STATE_REGISTERED;
  zsock_send(self->socket, "bbb", &dd_version, 4, &dd_cmd_ping, 4,
             &self->cookie, sizeof(self->cookie));

  self->heartbeat_loop = zloop_timer(loop, 1500, 0, s_heartbeat, self);
  zloop_timer_end(loop, self->registration_loop);
  // if this is re-registration, we should try to subscribe again
  sublist_resubscribe(self);
  self->on_reg(self);
}

static void cb_pong(dd_t *self, zmsg_t *msg, zloop_t *loop) {
  zloop_timer(loop, 1500, 1, s_ping, self);
}

static void cb_chall(dd_t *self, zmsg_t *msg) {
  int retval = 0;
  //  fprintf(stderr,"cb_chall called\n");

  // zmsg_print(msg);
  zframe_t *encrypted = zmsg_first(msg);
  unsigned char *data = zframe_data(encrypted);
  int enclen = zframe_size(encrypted);
  unsigned char *decrypted = calloc(1, enclen);

  retval = crypto_box_open_easy_afternm(decrypted, data + crypto_box_NONCEBYTES,
                                        enclen - crypto_box_NONCEBYTES, data,
                                        self->keys->ddboxk);
  if (retval != 0) {
    fprintf(stderr, "Unable to decrypt CHALLENGE from broker\n");
    return;
  }

  zsock_send(self->socket, "bbfss", &dd_version, 4, &dd_cmd_challok, 4,
             zframe_new(decrypted,
                        enclen - crypto_box_NONCEBYTES - crypto_box_MACBYTES),
             self->keys->hash, self->client_name);
}

static void cb_data(dd_t *self, zmsg_t *msg) {
  int retval;
  char *source = zmsg_popstr(msg);
  /* printf("cb_data: S: %s\n", source); */
  zframe_t *encrypted = zmsg_first(msg);
  unsigned char *data = zframe_data(encrypted);
  int enclen = zframe_size(encrypted);
  unsigned char *decrypted =
      calloc(1, enclen - crypto_box_NONCEBYTES - crypto_box_MACBYTES);
  unsigned char *precalck = NULL;
  char *dot = strchr(source, '.');
  if (dot) {
    *dot = '\0';
    precalck = zhash_lookup(self->keys->clientkeys, source);
    if (precalck) {
      // printf("decrypting with tenant key:%s\n", source);
    }
    *dot = '.';
  }

  if (!precalck) {
    if (strncmp("public.", source, strlen("public.")) == 0) {
      precalck = self->keys->pubboxk;
      //	printf("decrypting with public tenant key\n");
    } else {
      precalck = self->keys->custboxk;
      //      printf("decrypting with my own key\n");
    }
  }
  retval = crypto_box_open_easy_afternm(decrypted, data + crypto_box_NONCEBYTES,
                                        enclen - crypto_box_NONCEBYTES, data,
                                        precalck);
  if (retval == 0) {
    self->on_data(source, decrypted,
                  enclen - crypto_box_NONCEBYTES - crypto_box_MACBYTES, self);
  } else {
    fprintf(stderr, "DD: Unable to decrypt %d bytes from %s\n",
            enclen - crypto_box_NONCEBYTES - crypto_box_MACBYTES, source);
  }
  free(decrypted);
}

// up to the user to free the memory!
static void cb_pub(dd_t *self, zmsg_t *msg) {
  int retval;
  char *source = zmsg_popstr(msg);
  char *topic = zmsg_popstr(msg);
  zframe_t *encrypted = zmsg_first(msg);
  unsigned char *data = zframe_data(encrypted);
  int enclen = zframe_size(encrypted);

  int mlen = enclen - crypto_box_NONCEBYTES - crypto_box_MACBYTES;
  unsigned char *decrypted = calloc(1, mlen);

  unsigned char *precalck = NULL;
  char *dot = strchr(source, '.');
  if (dot) {
    *dot = '\0';
    precalck = zhash_lookup(self->keys->clientkeys, source);
    if (precalck) {
      //	printf("decrypting with tenant key:%s\n", source);
    }
    *dot = '.';
  }
  if (!precalck) {
    if (strncmp("public.", source, strlen("public.")) == 0) {
      precalck = self->keys->pubboxk;
      //	printf("decrypting with public tenant key\n");
    } else {
      precalck = self->keys->custboxk;
      //      printf("decrypting with my own key\n");
    }
  }

  retval = crypto_box_open_easy_afternm(decrypted, data + crypto_box_NONCEBYTES,
                                        enclen - crypto_box_NONCEBYTES, data,
                                        precalck);

  if (retval == 0) {
    self->on_pub(source, topic, decrypted, mlen, self);
  } else {
    fprintf(stderr, "DD: Unable to decrypt %d bytes from %s, topic %s\n", mlen,
            source, topic);
  }
  free(decrypted);
}

static void cb_subok(dd_t *self, zmsg_t *msg) {
  char *topic = zmsg_popstr(msg);
  char *scope = zmsg_popstr(msg);
  sublist_activate(topic, scope, self);
}

static void cb_error(dd_t *self, zmsg_t *msg) {
  zframe_t *code_frame;
  code_frame = zmsg_pop(msg);
  if (code_frame == NULL) {
    fprintf(stderr, "DD: Misformed ERROR message, missing ERROR_CODE!\n");
    return;
  }

  int32_t *error_code = (int32_t *)zframe_data(code_frame);
  char *error_msg = zmsg_popstr(msg);
  self->on_error(*error_code, error_msg, self);
  zframe_destroy(&code_frame);
  free(error_msg);
}

// static void cb_nodst(zmsg_t *msg, dd_t *self) {
//  char *destination = zmsg_popstr(msg);
//  self->on_nodst(destination, self);
//}

static int s_on_pipe_msg(zloop_t *loop, zsock_t *handle, void *args) {
  dd_t *self = (dd_t *)args;
  zmsg_t *msg = zmsg_recv(handle);
  zmsg_print(msg);
  char *command = zmsg_popstr(msg);
  //  All actors must handle $TERM in this way
  // returning -1 should stop zloop_start and terminate the actor
  if (streq(command, "$TERM")) {
    fprintf(stderr, "s_on_pipe_msg, got $TERM, quitting\n");

    return -1;
  } else if (streq(command, "subscribe")) {
    char *topic = zmsg_popstr(msg);
    char *scope = zmsg_popstr(msg);
    dd_subscribe(self, topic, scope);
    //    self->subscribe(topic, scope, dd);
    free(topic);
    free(scope);
  } else if (streq(command, "unsubscribe")) {
    char *topic = zmsg_popstr(msg);
    char *scope = zmsg_popstr(msg);
    dd_unsubscribe(self, topic, scope);
    //    self->unsubscribe(topic, scope, dd);
    free(topic);
    free(scope);
  } else if (streq(command, "publish")) {
    char *topic = zmsg_popstr(msg);
    char *message = zmsg_popstr(msg);
    zframe_t *mlen = zmsg_pop(msg);
    uint32_t len = *((uint32_t *)zframe_data(mlen));
    dd_publish(self, topic, message, len);
    //    self->publish(topic, message, len, dd);
    zframe_destroy(&mlen);
    free(topic);
    free(message);

  } else if (streq(command, "notify")) {
    char *target = zmsg_popstr(msg);
    char *message = zmsg_popstr(msg);
    zframe_t *mlen = zmsg_pop(msg);
    uint32_t len = *((uint32_t *)zframe_data(mlen));
    dd_notify(self, target, message, len);
    zframe_destroy(&mlen);
    free(target);
    free(message);
  } else {
    fprintf(stderr, "s_on_pipe_msg, got unknown command: %s\n", command);
  }
  zmsg_destroy(&msg);
  free(command);
  return 0;
}

void actor_con(void *args) {
  dd_t *self = (dd_t *)args;
  fprintf(stderr, "actor Registered with broker %s!\n", self->endpoint);
  zsock_send(self->pipe, "ss", "reg", self->endpoint);
}

void actor_discon(void *args) {
  dd_t *self = (dd_t *)args;
  fprintf(stderr, "actor Got disconnected from broker %s!\n", self->endpoint);
  zsock_send(self->pipe, "ss", "discon", self->endpoint);
}

void actor_pub(char *source, char *topic, unsigned char *data, int length,
               void *args) {
  dd_t *self = (dd_t *)args;
  fprintf(stderr, "actor PUB S: %s T: %s L: %d D: '%s'", source, topic, length,
          data);
  zsock_send(self->pipe, "sssbb", "pub", source, topic, &length, sizeof(length),
             data, length);
}

void actor_data(char *source, unsigned char *data, int length, void *args) {
  dd_t *self = (dd_t *)args;
  fprintf(stderr, "actor DATA S: %s L: %d D: '%s'", source, length, data);
  zsock_send(self->pipe, "ssbb", "data", source, &length, sizeof(length), data,
             length);
}
void actor_error(int error_code, char *error_message, void *args) {
  dd_t *self = (dd_t *)args;
  fprintf(stderr, "actor Error %d : %s", error_code, error_message);
  zsock_send(self->pipe, "ssb", "error", error_message, &error_code,
             sizeof(error_code));
}

static int s_on_dealer_msg(zloop_t *loop, zsock_t *handle, void *args) {
  dd_t *self = (dd_t *)args;
  self->timeout = 0;
  zmsg_t *msg = zmsg_recv(handle);
  // zmsg_print(msg);

  if (msg == NULL) {
    fprintf(stderr, "DD: zmsg_recv returned NULL\n");
    return 0;
  }
  if (zmsg_size(msg) < 2) {
    fprintf(stderr, "DD: Message length less than 2, error!\n");
    zmsg_destroy(&msg);
    return 0;
  }

  zframe_t *proto_frame = zmsg_pop(msg);

  if (*((uint32_t *)zframe_data(proto_frame)) != DD_VERSION) {
    fprintf(stderr, "DD: Wrong version, expected 0x%x, got 0x%x\n", DD_VERSION,
            *zframe_data(proto_frame));
    zframe_destroy(&proto_frame);
    return 0;
  }
  zframe_t *cmd_frame = zmsg_pop(msg);
  uint32_t cmd = *((uint32_t *)zframe_data(cmd_frame));
  zframe_destroy(&cmd_frame);
  switch (cmd) {
  case DD_CMD_SEND:
    fprintf(stderr, "DD: Got command DD_CMD_SEND\n");
    break;
  case DD_CMD_FORWARD:
    fprintf(stderr, "DD: Got command DD_CMD_FORWARD\n");
    break;
  case DD_CMD_PING:
    fprintf(stderr, "DD: Got command DD_CMD_PING\n");
    break;
  case DD_CMD_ADDLCL:
    fprintf(stderr, "DD: Got command DD_CMD_ADDLCL\n");
    break;
  case DD_CMD_ADDDCL:
    fprintf(stderr, "DD: Got command DD_CMD_ADDDCL\n");
    break;
  case DD_CMD_ADDBR:
    fprintf(stderr, "DD: Got command DD_CMD_ADDBR\n");
    break;
  case DD_CMD_UNREG:
    fprintf(stderr, "DD: Got command DD_CMD_UNREG\n");
    break;
  case DD_CMD_UNREGDCLI:
    fprintf(stderr, "DD: Got command DD_CMD_UNREGDCLI\n");
    break;
  case DD_CMD_UNREGBR:
    fprintf(stderr, "DD: Got command DD_CMD_UNREGBR\n");
    break;
  case DD_CMD_DATA:
    cb_data(self, msg);
    break;
  case DD_CMD_ERROR:
    cb_error(self, msg);
    break;
  case DD_CMD_REGOK:
    cb_regok(self, msg, loop);
    break;
  case DD_CMD_PONG:
    cb_pong(self, msg, loop);
    break;
  case DD_CMD_CHALL:
    cb_chall(self, msg);
    break;
  case DD_CMD_CHALLOK:
    fprintf(stderr, "DD: Got command DD_CMD_CHALLOK\n");
    break;
  case DD_CMD_PUB:
    cb_pub(self, msg);
    break;
  case DD_CMD_SUB:
    fprintf(stderr, "DD: Got command DD_CMD_SUB\n");
    break;
  case DD_CMD_UNSUB:
    fprintf(stderr, "DD: Got command DD_CMD_UNSUB\n");
    break;
  case DD_CMD_SENDPUBLIC:
    fprintf(stderr, "DD: Got command DD_CMD_SENDPUBLIC\n");
    break;
  case DD_CMD_PUBPUBLIC:
    fprintf(stderr, "DD: Got command DD_CMD_PUBPUBLIC\n");
    break;
  case DD_CMD_SENDPT:
    fprintf(stderr, "DD: Got command DD_CMD_SENDPT\n");
    break;
  case DD_CMD_FORWARDPT:
    fprintf(stderr, "DD: Got command DD_CMD_FORWARDPT\n");
    break;
  case DD_CMD_DATAPT:
    fprintf(stderr, "DD: Got command DD_CMD_DATAPT\n");
    break;
  case DD_CMD_SUBOK:
    cb_subok(self, msg);
    break;
  default:
    fprintf(stderr, "DD: Unknown command, value: 0x%x\n", cmd);
    break;
  }
  zmsg_destroy(&msg);
  return 0;
}

// Threads
void *ddthread(void *args) {
  dd_t *self = (dd_t *)args;
  int rc;

  self->socket = zsock_new_dealer(NULL);
  if (!self->socket) {
    fprintf(stderr, "DD: Error in zsock_new_dealer: %s\n", zmq_strerror(errno));
    free(self);
    return NULL;
  }
  //  zsock_set_identity (self->socket, self->client_name);
  rc = zsock_connect(self->socket, (const char *)self->endpoint);
  if (rc != 0) {
    fprintf(stderr, "DD: Error in zmq_connect: %s\n", zmq_strerror(errno));
    free(self);
    return NULL;
  }

  self->keys = read_ddkeys((char *)self->keyfile, (char *)self->customer);
  if (self->keys == NULL) {
    fprintf(stderr, "DD: Error reading keyfile!\n");
    return NULL;
  }

  self->sublist = zlistx_new();
  zlistx_set_destructor(self->sublist, (czmq_destructor *)sublist_free);
  zlistx_set_duplicator(self->sublist, (czmq_duplicator *)sublist_dup);
  zlistx_set_comparator(self->sublist, (czmq_comparator *)sublist_cmp);

  self->loop = zloop_new();
  assert(self->loop);
  self->registration_loop =
      zloop_timer(self->loop, 1000, 0, s_ask_registration, self);
  rc = zloop_reader(self->loop, self->socket, s_on_dealer_msg, self);
  zloop_start(self->loop);
  zloop_destroy(&self->loop);
  return self;
}

void dd_actor(zsock_t *pipe, void *args) {
  dd_t *self = (dd_t *)args;
  int rc;
  zsock_signal(pipe, 0);
  self->pipe = pipe;
  self->socket = zsock_new_dealer(NULL);
  if (!self->socket) {
    fprintf(stderr, "DD: Error in zsock_new_dealer: %s\n", zmq_strerror(errno));
    zsock_send(self->pipe, "ss", "$TERM", "Error creating socket");
    dd_destroy(&self);
    //    self->shutdown(dd);
    //    free(self);
    return;
  }
  //  zsock_set_identity (self->socket, self->client_name);
  rc = zsock_connect(self->socket, (const char *)self->endpoint);
  if (rc != 0) {
    fprintf(stderr, "DD: Error in zmq_connect: %s\n", zmq_strerror(errno));
    zsock_send(self->pipe, "ss", "$TERM", "Connection failed");
    dd_destroy(&self);
    //    self->shutdown(dd);
    // free(self);
    return;
  }

  self->keys = read_ddkeys((char *)self->keyfile, (char *)self->customer);
  if (self->keys == NULL) {
    fprintf(stderr, "DD: Error reading keyfile!\n");
    zsock_send(self->pipe, "ss", "$TERM", "Missing keyfile");
    dd_destroy(&self);
    //    self->shutdown(dd);
    // free(self);
    return;
  }

  self->sublist = zlistx_new();
  zlistx_set_destructor(self->sublist, (czmq_destructor *)sublist_free);
  zlistx_set_duplicator(self->sublist, (czmq_duplicator *)sublist_dup);
  zlistx_set_comparator(self->sublist, (czmq_comparator *)sublist_cmp);

  self->loop = zloop_new();
  assert(self->loop);
  self->registration_loop =
      zloop_timer(self->loop, 1000, 0, s_ask_registration, self);
  rc = zloop_reader(self->loop, self->socket, s_on_dealer_msg, self);
  rc = zloop_reader(self->loop, pipe, s_on_pipe_msg, self);
  zloop_start(self->loop);
  zloop_destroy(&self->loop);
  zsock_destroy(&pipe);
}

zactor_t *ddactor_new(char *client_name, char *customer, char *endpoint,
                      char *keyfile) {
  dd_t *self = malloc(sizeof(dd_t));
  self->style = DD_ACTOR;
  self->client_name = (unsigned char *)strdup(client_name);
  self->customer = (unsigned char *)strdup(customer);
  self->endpoint = (unsigned char *)strdup(endpoint);
  self->keyfile = (unsigned char *)strdup(keyfile);
  self->timeout = 0;
  self->state = DD_STATE_UNREG;

  self->pipe = NULL;
  self->sublist = NULL;
  self->loop = NULL;

  randombytes_buf(self->nonce, crypto_box_NONCEBYTES);
  self->on_reg = actor_con;
  self->on_discon = actor_discon;
  self->on_data = actor_data;
  self->on_pub = actor_pub;
  self->on_error = actor_error;
  /*  self->subscribe = subscribe;
  self->unsubscribe = unsubscribe;
  self->publish = publish;
  self->notify = notify;
  self->shutdown = ddthread_shutdown;*/
  zactor_t *actor = zactor_new(dd_actor, self);
  return actor;
}

dd_t *dd_new(char *client_name, char *customer, char *endpoint, char *keyfile,
             dd_con con, dd_discon discon, dd_data data, dd_pub pub,
             dd_error error) {
  dd_t *self = malloc(sizeof(dd_t));
  self->style = DD_CALLBACK;
  self->client_name = (unsigned char *)strdup(client_name);
  self->customer = (unsigned char *)strdup(customer);
  self->endpoint = (unsigned char *)strdup(endpoint);
  self->keyfile = (unsigned char *)strdup(keyfile);
  self->timeout = 0;
  self->state = DD_STATE_UNREG;
  randombytes_buf(self->nonce, crypto_box_NONCEBYTES);
  self->on_reg = con;
  self->on_discon = discon;
  self->on_data = data;
  self->on_pub = pub;
  self->on_error = error;
  zthread_new(ddthread, self);
  return self;
}

static void print_ddkeystate(ddkeystate_t *keys) {
  char *hex = malloc(100);
  printf("Hash value: \t%s", keys->hash);
  printf("Private key: \t%s", sodium_bin2hex(hex, 100, keys->privkey, 32));
  printf("Public key: \t%s", sodium_bin2hex(hex, 100, keys->pubkey, 32));
  printf("DDPublic key: \t%s", sodium_bin2hex(hex, 100, keys->ddpubkey, 32));
  printf("PublicPub key: \t%s",
         sodium_bin2hex(hex, 100, keys->publicpubkey, 32));
  free(hex);
}