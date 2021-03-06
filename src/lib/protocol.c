#include <czmq.h>
#include "dd.h"
#include "protocol.h"
const uint32_t dd_cmd_send = DD_CMD_SEND;
const uint32_t dd_cmd_forward = DD_CMD_FORWARD;
const uint32_t dd_cmd_ping = DD_CMD_PING;
const uint32_t dd_cmd_addlcl = DD_CMD_ADDLCL;
const uint32_t dd_cmd_adddcl = DD_CMD_ADDDCL;
const uint32_t dd_cmd_addbr = DD_CMD_ADDBR;
const uint32_t dd_cmd_unreg = DD_CMD_UNREG;
const uint32_t dd_cmd_unregdcli = DD_CMD_UNREGDCLI;
const uint32_t dd_cmd_unregbr = DD_CMD_UNREGBR;
const uint32_t dd_cmd_data = DD_CMD_DATA;
const uint32_t dd_cmd_error = DD_CMD_ERROR;
const uint32_t dd_cmd_regok = DD_CMD_REGOK;
const uint32_t dd_cmd_pong = DD_CMD_PONG;
const uint32_t dd_cmd_chall = DD_CMD_CHALL;
const uint32_t dd_cmd_challok = DD_CMD_CHALLOK;
const uint32_t dd_cmd_pub = DD_CMD_PUB;
const uint32_t dd_cmd_sub = DD_CMD_SUB;
const uint32_t dd_cmd_unsub = DD_CMD_UNSUB;
const uint32_t dd_cmd_sendpublic = DD_CMD_SENDPUBLIC;
const uint32_t dd_cmd_pubpublic = DD_CMD_PUBPUBLIC;
const uint32_t dd_cmd_sendpt = DD_CMD_SENDPT;
const uint32_t dd_cmd_forwardpt = DD_CMD_FORWARDPT;
const uint32_t dd_cmd_datapt = DD_CMD_DATAPT;
const uint32_t dd_cmd_subok = DD_CMD_SUBOK;
const uint32_t dd_version = DD_VERSION;
const uint32_t dd_error_regfail = DD_ERROR_REGFAIL;
const uint32_t dd_error_nodst = DD_ERROR_NODST;
const uint32_t dd_error_version = DD_ERROR_VERSION;
