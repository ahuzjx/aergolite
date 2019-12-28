#include <stdbool.h>
#include "../common/uv_msg_framing.c"
#include "../common/uv_send_message.c"
#if TARGET_OS_IPHONE
#include "../common/uv_callback.c"
#endif
#include "../../core/sqlite3.h"
#define SQLITE_PRIVATE static
#include "../../common/sha256.h"



#define NEW_BLOCK_WAIT_INTERVAL  3000  /* 3 seconds - it should be a variable! */



/* peer communication */

#define PLUGIN_CMD                 0x43292173

#define PLUGIN_VERSION_NUMBER      1

/* peer message commands */

#define PLUGIN_VERSION             0xcd00

#define PLUGIN_CMD_ID              0xcd01     /* peer identification */
//#define PLUGIN_REQUEST_NODE_ID     0xcd02     /* request a node id */
//#define PLUGIN_NEW_NODE_ID         0xcd03     /* send the new node id */
#define PLUGIN_RANDOM              0xcd02
#define PLUGIN_ID_CONFLICT         0xcd03     /* there is another node with the same id */

#define PLUGIN_GET_PEERS           0xcd04     /* request the list of peers */
#define PLUGIN_PEERS               0xcd05     /* list of peers */

#define PLUGIN_CMD_PING            0xcd06     /* check if alive */
#define PLUGIN_CMD_PONG            0xcd07     /* I am alive */

#define PLUGIN_TEXT                0xcd08     /* text message via TCP */


#define PLUGIN_REQUEST_STATE_DIFF  0xdb01
#define PLUGIN_UPTODATE            0xdb02
#define PLUGIN_DB_PAGE             0xdb03
#define PLUGIN_APPLY_UPDATE        0xdb04

#define PLUGIN_INSERT_TRANSACTION  0xdb11     /* follower -> leader */
#define PLUGIN_NEW_TRANSACTION     0xdb12     /* follower <- leader -> follower (broadcast) */
#define PLUGIN_TRANSACTION_FAILED  0xdb13     /* follower <- leader (response) */

#define PLUGIN_NEW_BLOCK           0xdb21     /* follower <- leader */
#define PLUGIN_BLOCK_APPROVED      0xdb22     /* follower -> all (broadcast) */

#define PLUGIN_GET_MEMPOOL         0xdb51     /* any -> any (request) */

#define PLUGIN_GET_TRANSACTION     0xdb31     /* follower -> leader (request) */
#define PLUGIN_REQUESTED_TRANSACTION  0xdb32     /* follower <- leader (response) */
#define PLUGIN_TXN_NOTFOUND        0xdb33     /* follower <- leader (response) */

#define PLUGIN_GET_BLOCK           0xdb41     /* follower -> leader (request) */
#define PLUGIN_REQUESTED_BLOCK     0xdb42     /* follower <- leader (response) */
#define PLUGIN_BLOCK_NOTFOUND      0xdb43     /* follower <- leader (response) */

//#define PLUGIN_LOG_EXISTS          0xdb20     /* follower <- leader (response) */

#define PLUGIN_PUBKEY              0xdb51
#define PLUGIN_SIGNATURE           0xdb52

#define PLUGIN_AUTHORIZATION       0xdb53

#define PLUGIN_CPU                 0xc0de021
#define PLUGIN_OS                  0xc0de022
#define PLUGIN_HOSTNAME            0xc0de023
#define PLUGIN_APP                 0xc0de024

#define PLUGIN_CONTENT             0xc0de011
#define PLUGIN_PGNO                0xc0de012
#define PLUGIN_DBPAGE              0xc0de013
#define PLUGIN_STATE               0xc0de014
#define PLUGIN_HEIGHT              0xc0de015
#define PLUGIN_HEADER              0xc0de016
#define PLUGIN_BODY                0xc0de017
#define PLUGIN_SIGNATURES          0xc0de018
#define PLUGIN_MOD_PAGES           0xc0de019
#define PLUGIN_HASH                0xc0de020


/* peer message parameters */
#define PLUGIN_OK                  0xc0de001  /*  */
#define PLUGIN_ERROR               0xc0de002  /*  */

#define PLUGIN_NODE_ID             0xc0de003  /*  */
#define PLUGIN_NODE_INFO           0xc0de004  /*  */
#define PLUGIN_PORT                0xc0de005  /*  */

#define PLUGIN_SEQ                 0xc0de006  /*  */
#define PLUGIN_TID                 0xc0de007  /*  */
#define PLUGIN_NONCE               0xc0de008  /*  */
#define PLUGIN_SQL_CMDS            0xc0de009  /*  */


//#define BLOCK_HEIGHT    0x34
//#define STATE_HASH      0x35


// the state of the slave peer or connection
#define STATE_CONN_NONE        0       /*  */
#define STATE_IDENTIFIED       1       /*  */
#define STATE_UPDATING         2       /*  */
#define STATE_IN_SYNC          3       /*  */
#define STATE_CONN_LOST        4       /*  */
#define STATE_INVALID_PEER     5       /*  */
#define STATE_BUSY             6       /*  */
#define STATE_ERROR            7       /*  */
#define STATE_RECEIVING_UPDATE 8       /* the slave is receiving a page change from the master while it is in the sync process */



struct tcp_address {
  struct tcp_address *next;
  char host[64];
  int port;
  int is_broadcast;
  int reconnect_interval;
};

struct connect_req {
  uv_connect_t connect;
  struct sockaddr_in addr;
};


#define DEFAULT_BACKLOG 128  /* used in uv_listen */

/* connection type */
#define CONN_UNKNOWN    0
#define CONN_INCOMING   1
#define CONN_OUTGOING   2

/* connection type */

#define ADDRESS_BIND     1    /*  */
#define ADDRESS_CONNECT  2    /*  */

/* connection state */
#define CONN_STATE_UNKNOWN       0
#define CONN_STATE_CONNECTING    1
#define CONN_STATE_CONNECTED     2
#define CONN_STATE_CONN_LOST     3
#define CONN_STATE_DISCONNECTED  4
#define CONN_STATE_FAILED        5

/* db state - sync_down_state and sync_up_state */
#define DB_STATE_UNKNOWN         0  /* if there is no connection it doesn't know if there are remote changes */
#define DB_STATE_SYNCHRONIZING   1
#define DB_STATE_IN_SYNC         2
#define DB_STATE_LOCAL_CHANGES   3  /* there are local changes */
#define DB_STATE_OUTDATED        4  /* if the connection dropped while updating, or some other error state */
#define DB_STATE_ERROR           5


/* worker thread commands */
#define WORKER_THREAD_NEW_TRANSACTION  0xcd01  /*  */
#define WORKER_THREAD_EXIT             0xcd02  /*  */
#define WORKER_THREAD_OK               0xcd03  /*  */


typedef uint32_t Pgno;

typedef struct plugin plugin;
typedef struct node node;

struct node_id_conflict {
  node *existing_node;
  node *new_node;
  uv_timer_t timer;
};

struct leader_votes {
  int id;
  int count;
  struct leader_votes *next;
};

struct node {
  node *next;            /* Next item in the list */
  int   id;              /* Node id */

  binn *conn_id;
  struct node_id_conflict *id_conflict;

  int   conn_type;       /* outgoing or incoming connection */
  char  host[64];        /* Remode IP address */
  int   port;            /* Remote port */
  int   bind_port;       /* Remote bind port. Used on incoming TCP connections */

  char  cpu[256];        /* CPU information */
  char  os[256];         /* OS information */
  char  hostname[256];   /* node's hostname */
  char  app[256];        /* application path and name */

  char  *info;           /* Dynamic information set by this peer */

  char  pubkey[36];      /* node's public key */
  int   pklen;           /* node's public key length */

  uv_msg_t socket;       /* Socket used to connect with the other peer */
  int   conn_state;      /* The state of this connection/peer */

  aergolite *this_node;  /* x */
  plugin *plugin;        /* x */

  int64 last_block;      /* The height of the last block */

  BOOL    is_authorized;
  BOOL    authorization_sent;

  /* used for the query status */
  int     db_state;
};

struct node_nonce {
  int node_id;
  int64 last_nonce;
};

struct transaction {
  struct transaction *next;
  int node_id;
  int64 nonce;
  int64 id;
  void *log;
  int64 block_height;
};

#ifndef AERGOLITE_AMALGAMATION
struct block {
  struct block *next;
  int64 height;
  unsigned char id[32];
  void *header;
  void *body;
  void *signatures;

  void *votes;
  int  ack_count;
  int  downloading_txns;
};
#endif

struct block_vote {
  struct block_vote *next;
  int64 height;
  uchar block_id[32];
  int node_id;
};

struct txn_list {
  struct txn_list *next;
  void *log;
};

struct plugin {
  int node_id;                /* Node id */
  char *pubkey;               /* the public key for this node */
  int pklen;                  /* public key length */

  aergolite *this_node;       /* Reference to the aergolite instance */

  struct tcp_address *bind;   /* Address(es) to bind */
  struct tcp_address *discovery;  /* Node discovery address(es) */
  struct tcp_address *broadcast;  /* Broadcast address(es) */
  uv_udp_t *udp_sock;         /* Socket used for UDP communication */

  node *peers;                /* Remote nodes connected to this one */
  int total_authorized_nodes; /* Including those that are currently off-line */

  BOOL is_authorized;         /* Whether this node is authorized on the network */

  BOOL is_leader;             /* True if this node is the current leader */
  node *leader_node;          /* Points to the leader node if it is connected */
  node *last_leader;          /* Points to the previous leader node */
  void *leader_query;
  struct leader_votes *leader_votes;
  BOOL in_leader_query;       /* True if in a leader query */
  BOOL in_election;           /* True if in a leader election */
  BOOL some_nodes_in_election;/* True if any node is in a leader election during leader query */

  struct transaction *mempool;
  struct txn_list *special_txn; /* New special transaction */

  void *nonces;

  struct block *current_block;
  struct block *new_block;
  struct block_vote *block_votes;

#if TARGET_OS_IPHONE
  uv_callback_t worker_cb;    /* callback handle to send msg to the worker thread */
#else
  char worker_address[64];    /* unix socket or named pipe address for the worker thread */
#endif
  uv_thread_t worker_thread;  /* Thread used to receive dblog commands and events */
  int thread_running;         /* Whether the worker thread for this pager is running */
  int thread_active;          /* Whether the worker thread for this pager is active */
  uv_loop_t *loop;            /* libuv event loop for this thread */

  uv_timer_t aergolite_core_timer;  /* Timer for the aergolite periodic function */

  uv_timer_t after_connections_timer;

  uv_timer_t leader_check_timer;
  uv_timer_t election_info_timer;

  uv_timer_t process_transactions_timer;
  uv_timer_t new_block_timer;
  int block_interval;

  uv_timer_t reconnect_timer;
  int reconnect_timer_enabled;

  sqlite3_mutex *mutex;

  bool is_updating_state;
  int sync_down_state;        /* downstream synchronization state */
  int sync_up_state;          /* upstream synchronization state */
};


/* peers and network */

SQLITE_PRIVATE int is_local_ip_address(char *address);

SQLITE_PRIVATE int send_tcp_broadcast(plugin *plugin, char *message);
SQLITE_PRIVATE int send_udp_broadcast(plugin *plugin, char *message);
SQLITE_PRIVATE int send_udp_message(plugin *plugin, const struct sockaddr *address, char *message);

SQLITE_PRIVATE void on_text_command_received(node *node, char *message);

SQLITE_PRIVATE BOOL has_nodes_for_consensus(plugin *plugin);

/* leader checking and election */

SQLITE_PRIVATE void on_new_election_request(plugin *plugin, node *node, char *arg);

SQLITE_PRIVATE void on_leader_check_timeout(uv_timer_t* handle);

SQLITE_PRIVATE void check_current_leader(plugin *plugin);
SQLITE_PRIVATE void start_leader_election(plugin *plugin);

SQLITE_PRIVATE void leader_node_process_local_transactions(plugin *plugin);

/* mempool */

SQLITE_PRIVATE int  store_transaction_on_mempool(
  plugin *plugin, int node_id, int64 nonce, void *log, struct transaction **ptxn
);
SQLITE_PRIVATE void discard_mempool_transaction(plugin *plugin, struct transaction *txn);
SQLITE_PRIVATE int  check_mempool_transactions(plugin *plugin);

/* blockchain */

SQLITE_PRIVATE void start_downstream_db_sync(plugin *plugin);
SQLITE_PRIVATE void start_upstream_db_sync(plugin *plugin);

SQLITE_PRIVATE int  load_current_state(plugin *plugin);
SQLITE_PRIVATE void request_state_update(plugin *plugin);

SQLITE_PRIVATE void start_new_block_timer(plugin *plugin);
SQLITE_PRIVATE int  broadcast_new_block(plugin *plugin);
SQLITE_PRIVATE void send_new_block(plugin *plugin, node *node);
SQLITE_PRIVATE void rollback_block(plugin *plugin);

/* event loop and timers */

SQLITE_PRIVATE void process_transactions_timer_cb(uv_timer_t* handle);
SQLITE_PRIVATE void reconnect_timer_cb(uv_timer_t* handle);
SQLITE_PRIVATE void log_rotation_timer_cb(uv_timer_t* handle);

SQLITE_PRIVATE void enable_reconnect_timer(plugin *plugin);

SQLITE_PRIVATE void worker_thread_on_close(uv_handle_t *handle);

SQLITE_PRIVATE int  send_notification_to_worker(plugin *plugin, void *data, int size);

/* UDP messages */

typedef void (*udp_message_callback)(plugin *plugin, uv_udp_t *socket, const struct sockaddr *addr, char *sender, char *arg);

struct udp_message {
  char name[32];
  udp_message_callback callback;
};

SQLITE_PRIVATE void register_udp_message(char *name, udp_message_callback callback);

/* TCP text messages */

typedef void (*tcp_message_callback)(plugin *plugin, node *node, char *arg);

struct tcp_message {
  char name[32];
  tcp_message_callback callback;
};

SQLITE_PRIVATE void register_tcp_message(char *name, tcp_message_callback callback);

/* node discovery */

SQLITE_PRIVATE void start_node_discovery(plugin *plugin);

SQLITE_PRIVATE void request_peer_list(plugin *plugin, node *node);
SQLITE_PRIVATE void send_peer_list(plugin *plugin, node *to_node);
SQLITE_PRIVATE void on_peer_list_request(node *node, void *msg, int size);
SQLITE_PRIVATE void on_peer_list_received(node *node, void *msg, int size);

/* node authorization */

SQLITE_PRIVATE int on_new_authorization(plugin *plugin, void *log, BOOL from_network);
