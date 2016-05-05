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
#include "dd.h"
FILE *logfp;
int daemonize = 0;

void usage() {
  printf(
      "Usage: broker [OPTIONS] ...\n"
      "REQUIRED OPTIONS\n"
      "-r [ADDR]\n"
      "       For example tcp://127.0.0.1:5555\n"
      "       Multiple addresses with comma tcp://127.0.0.1:5555,ipc:///file\n"
      "       Router is where clients connect\n"
      "-k [FILE]\n"
      "       JSON file containing the broker keys\n"
      "-s [SCOPE]\n"
      "       Scope of the broker, e.g. for region 1, cluster 2, node 3\n"
      "       \"1/2/3\"\n"
      "OPTIONAL\n"
      "-d [ADDR]\n"
      "       For example tcp://1.2.3.4:5555\n"
      "       Dealer should be connected to Router of another broker\n"
      "-l [CHAR]\n"
      "       e:ERROR,w:WARNING,n:NOTICE,i:INFO,d:DEBUG,q:QUIET\n"
      "-w [ADDR]\n"
      "       Open a REST socket, eg tcp://*:8080\n"
      "-f [FILE]\n"
      "       Read configuration file\n"
      "-D\n"
      "       Daemonize\n");
}

int s_ddactor_msg(zloop_t *loop, zsock_t *handle, void *arg) {
  zmsg_t *msg = zmsg_recv(handle);
  printf("s_ddactor_msg message %p from dd_broker_actor!\n", msg);
  if (msg != NULL) {
    zmsg_print(msg);
    zmsg_destroy(&msg);
  }
  return 0;
}

int get_config(dd_broker_t *self, char *conffile) {
  zconfig_t *root = zconfig_load(conffile);
  if (root == NULL) {
    fprintf(stderr, "Could not read configuration file \"%s\"\n", conffile);
    exit(EXIT_FAILURE);
  }
  zconfig_t *child = zconfig_child(root);
  while (child != NULL) {
    if (streq(zconfig_name(child), "dealer")) {
      dd_broker_set_dealer(self, zconfig_value(child));
    } else if (streq(zconfig_name(child), "scope")) {
      dd_broker_set_scope(self, zconfig_value(child));
    } else if (streq(zconfig_name(child), "router")) {
      dd_broker_add_router(self, zconfig_value(child));
    } else if (streq(zconfig_name(child), "rest")) {
      dd_broker_set_rest(self, zconfig_value(child));
    } else if (streq(zconfig_name(child), "loglevel")) {
      dd_broker_set_loglevel(self, zconfig_value(child));
    } else if (streq(zconfig_name(child), "keyfile")) {
      dd_broker_set_keyfile(self, zconfig_value(child));
    } else if (streq(zconfig_name(child), "syslog")) {
      zsys_set_logsystem(true);
    } else if (streq(zconfig_name(child), "daemonize")) {
      daemonize = 1;
    } else if (streq(zconfig_name(child), "logfile")) {
      logfp = fopen(optarg, "w");
      if (logfp == NULL) {
        fprintf(stderr, "Cannot open logfile %s\n", optarg);
        perror(errno);
        exit(EXIT_FAILURE);
      }
      zsys_set_logstream(logfp);
    } else {
      fprintf(stderr, "Unknown key in configuration file, \"%s\"",
              zconfig_name(child));
    }
    child = zconfig_next(child);
  }
  zconfig_destroy(&root);
  return 0;
}

int main(int argc, char **argv) {

  int c;
  char *configfile = NULL;
  void *ctx = zsys_init();
  zsys_set_logident("DD");
  dd_broker_t *broker = dd_broker_new();
  opterr = 0;
  while ((c = getopt(argc, argv, "d:r:l:k:s:h:f:w:DSL")) != -1)
    switch (c) {
    case 'r':
      dd_broker_add_router(broker, optarg);
      break;
    case 's':
      dd_broker_set_scope(broker, optarg);
      break;
    case 'k':
      dd_broker_set_keyfile(broker, optarg);
      break;
    case 'h':
      usage();
      exit(EXIT_FAILURE);
      break;
    case 'd':
      dd_broker_set_dealer(broker, optarg);
      break;
    case 'l':
      dd_broker_set_loglevel(broker, optarg);
      break;
    case 'w':
      dd_broker_set_rest(broker, optarg);
      break;
    case 'f':
      get_config(broker, optarg);
      break;
    case 'S':
      zsys_set_logsystem(true);
      break;
    case 'L':
      logfp = fopen(optarg, "w");
      if (logfp == NULL) {
        fprintf(stderr, "Cannot open logfile %s\n", optarg);
        perror(errno);
        exit(EXIT_FAILURE);
      }
      zsys_set_logstream(logfp);
      break;
    case 'D':
      daemonize = 1;
      break;
    case '?':
      if (optopt == 'c' || optopt == 's') {
        printf("Option -%c requires an argument.\n", optopt);
        usage();
        exit(EXIT_FAILURE);
      } else if (isprint(optopt)) {
        printf("Unknown option `-%c'.\n", optopt);
        usage();
        exit(EXIT_FAILURE);

      } else {
        printf("Unknown option character `\\x%x'.\n", optopt);
      }
      return 1;
    default:
      printf("unknown argument %c\n", optopt);
    }

  if (daemonize == 1) {
    zsys_daemonize("/");
  }

  zactor_t *actor = dd_broker_actor(broker);
  if (actor == NULL) {
    usage();
    exit(EXIT_FAILURE);
  }
  zloop_t *loop = zloop_new();
  int rc = zloop_reader(loop, actor, s_ddactor_msg, NULL);
  rc = zloop_start(loop);

  while (!zsys_interrupted) {
    zmsg_t *msg = zmsg_recv(actor);
    if (msg != NULL) {
      zmsg_print(msg);
      zmsg_destroy(&msg);
    }
  }
  zactor_destroy(&actor);
  printf("Done, quitting!\n");
  return 1;
}
