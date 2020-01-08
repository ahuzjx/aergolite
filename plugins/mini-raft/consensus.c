
SQLITE_PRIVATE int verify_last_block(plugin *plugin);
SQLITE_PRIVATE int commit_block(plugin *plugin, struct block *block);

/****************************************************************************/

SQLITE_PRIVATE void on_transaction_request_sent(send_message_t *req, int status) {

  if (status < 0) {
    SYNCTRACE("on_transaction_request_sent FAILED - (%d) %s\n", status, uv_strerror(status));
    uv_close2( (uv_handle_t*) ((uv_write_t*)req)->handle, worker_thread_on_close);  /* disconnect */
  }

}

/****************************************************************************/

SQLITE_PRIVATE void request_transaction(plugin *plugin, int64 tid){
  binn *map;

  SYNCTRACE("request_transaction - tid=%" INT64_FORMAT "\n", tid);
  assert(tid>0);

  if( !plugin->leader_node ) return;

  /* create request packet */
  map = binn_map();
  if( binn_map_set_int32(map, PLUGIN_CMD, PLUGIN_GET_TRANSACTION)==FALSE ) goto loc_failed;
  if( binn_map_set_int64(map, PLUGIN_TID, tid)==FALSE ) goto loc_failed;

  /* send the packet */
  if( send_peer_message(plugin->leader_node, map, on_transaction_request_sent)==FALSE ) goto loc_failed;

  binn_free(map);

  return;
loc_failed:
  if( map ) binn_free(map);
//  plugin->sync_down_state = DB_STATE_ERROR;

}

/****************************************************************************/

SQLITE_PRIVATE void on_requested_remote_transaction(node *node, void *msg, int size){
  plugin *plugin = node->plugin;
  int rc;

  SYNCTRACE("on_requested_remote_transaction\n");

  if( plugin->sync_down_state!=DB_STATE_SYNCHRONIZING && plugin->sync_down_state!=DB_STATE_IN_SYNC ){
    SYNCTRACE("--- FAILED: 'requested' remote transaction arrived while this node is not synchronizing\n");
    return;
  }

  rc = on_new_remote_transaction(node, msg, size);

  if( rc==SQLITE_OK ){
    verify_last_block(plugin);
  }

}

/****************************************************************************/

SQLITE_PRIVATE void on_requested_transaction_not_found(node *node, void *msg, int size){
  plugin *plugin = node->plugin;
  int rc;

  SYNCTRACE("on_requested_transaction_not_found\n");

  if( plugin->sync_down_state!=DB_STATE_SYNCHRONIZING && plugin->sync_down_state!=DB_STATE_IN_SYNC ){
    SYNCTRACE("--- FAILED: 'requested' remote transaction arrived while this node is not synchronizing\n");
    return;
  }

  plugin->sync_down_state = DB_STATE_OUTDATED;

  request_state_update(plugin);

}

/****************************************************************************/
/****************************************************************************/

SQLITE_PRIVATE bool add_block_vote(struct block *block, int node_id){
  int *node_ids, i, count;

  SYNCTRACE("add_block_vote\n");

  if( !block->votes ){
    block->votes = new_array(16, sizeof(int));
    if( !block->votes ) return false;
  }

  /* check if the vote is already stored */
  count = array_count(block->votes);
  node_ids = (int*) array_ptr(block->votes);
  for( i=0; i<count; i++ ){
    if( node_ids[i]==node_id ) return false;
  }

  /* add it to the array */
  array_append(&block->votes, &node_id);

  return true;
}

/****************************************************************************/

/*
** Store the votes temporarily while the block does not arrive
*/
SQLITE_PRIVATE void store_block_vote(node *node, int64 height, uchar *block_id){
  plugin *plugin = node->plugin;
  struct block_vote *vote;

  SYNCTRACE("store_block_vote\n");

  /* check if the vote is already stored */
  for( vote=plugin->block_votes; vote; vote=vote->next ){
    if( vote->height==height && memcmp(vote->block_id,block_id,32)==0 &&
        vote->node_id==node->id ){
      return;
    }
  }

  /* store the new vote */

  vote = sqlite3_malloc_zero(sizeof(struct block_vote));
  if( !vote ) return;

  vote->height = height;
  memcpy(vote->block_id, block_id, 32);
  vote->node_id = node->id;

  llist_add(&plugin->block_votes, vote);

}

/****************************************************************************/

SQLITE_PRIVATE void transfer_block_votes(plugin *plugin, struct block *block){
  struct block_vote *vote;

  SYNCTRACE("transfer_block_votes\n");

  for( vote=plugin->block_votes; vote; vote=vote->next ){
    if( vote->height==block->height && memcmp(vote->block_id,block->id,32)==0 ){
      if( add_block_vote(block,vote->node_id) ){
        block->ack_count++;
      }
    }
  }

}

/****************************************************************************/

SQLITE_PRIVATE void discard_old_block_votes(plugin *plugin){
  int64 current_height = plugin->current_block ? plugin->current_block->height : 0;
  struct block_vote *vote;

  SYNCTRACE("discard_old_block_votes\n");

loc_again:

  for( vote=plugin->block_votes; vote; vote=vote->next ){
    if( vote->height < current_height ){
      llist_remove(&plugin->block_votes, vote);
      sqlite3_free(vote);
      goto loc_again;
    }
  }

}

/****************************************************************************/

SQLITE_PRIVATE void clear_block_votes(plugin *plugin){

  SYNCTRACE("clear_block_votes\n");

  while( plugin->block_votes ){
    struct block_vote *next = plugin->block_votes->next;
    sqlite3_free(plugin->block_votes);
    plugin->block_votes = next;
  }

}

/****************************************************************************/
/****************************************************************************/
#if 0
SQLITE_PRIVATE void request_block(plugin *plugin, int64 height){
  binn *map;

  SYNCTRACE("request_block - height=%" INT64_FORMAT "\n", height);
  assert(height>0);

  if( !plugin->leader_node ) return;

  /* create request packet */
  map = binn_map();
  if( binn_map_set_int32(map, PLUGIN_CMD, PLUGIN_GET_BLOCK)==FALSE ) goto loc_failed;
  if( binn_map_set_int64(map, PLUGIN_HEIGHT, height)==FALSE ) goto loc_failed;

  /* send the packet */
  if( send_peer_message(plugin->leader_node, map, on_transaction_request_sent)==FALSE ) goto loc_failed;

  binn_free(map);

  return;
loc_failed:
  if( map ) binn_free(map);
//  plugin->sync_down_state = DB_STATE_ERROR;

}

/****************************************************************************/

SQLITE_PRIVATE void on_requested_block(node *node, void *msg, int size){
  plugin *plugin = node->plugin;
  int rc;

  SYNCTRACE("on_requested_block\n");

  if( plugin->sync_down_state!=DB_STATE_SYNCHRONIZING && plugin->sync_down_state!=DB_STATE_IN_SYNC ){
    SYNCTRACE("--- FAILED: 'requested' block arrived while this node is not synchronizing\n");
    return;
  }

  rc = store_new_block(node, msg, size);

  if( rc==SQLITE_OK ){
    apply_last_block(plugin);
  }

}
#endif

/****************************************************************************/

SQLITE_PRIVATE void rollback_block(plugin *plugin){
  aergolite *this_node = plugin->this_node;

  aergolite_rollback_block(this_node);

  discard_block(plugin->new_block);
  plugin->new_block = NULL;

}

/****************************************************************************/

// iterate the payload to check the transactions
// download those that are not in the local mempool
// when they arrive, call fn to check if it can apply
// execute txns from the payload

SQLITE_PRIVATE int verify_block(plugin *plugin, struct block *block){
  aergolite *this_node = plugin->this_node;
  struct transaction *txn;
  BOOL all_present = TRUE;
  binn_iter iter;
  binn value, *map;
  void *list;
  node *node;
  int rc;

  SYNCTRACE("verify_block\n");

  /* if this node is in a state update, return */
  if( plugin->sync_down_state!=DB_STATE_IN_SYNC ) return SQLITE_BUSY;

  //block = plugin->new_block;
  if( !block ) return SQLITE_EMPTY;
  assert(block->height>0);
  plugin->new_block = block;

  /* get the list of transactions ids */
  list = binn_map_list(block->body, BODY_TXN_IDS);  //  BLOCK_TRANSACTIONS);

  /* check whether all the transactions are present on the local mempool */
  binn_list_foreach(list, value){
    int64 txn_id = value.vint64;
    assert( value.type==BINN_INT64 );
    /* remove the flag of failed transaction */
    txn_id &= 0x7fffffffffffffff;
    /* check the transaction in the mempool */
    for( txn=plugin->mempool; txn; txn=txn->next ){
      if( txn->id==txn_id ) break;
    }
    if( !txn ){
      /* transaction not present in the local mempool */
      all_present = FALSE;
      /* to avoid making a second request for non-arrived txns */
      if( !block->downloading_txns ){
        request_transaction(plugin, txn_id);
      }
    }
  }

  block->downloading_txns = !all_present;

  if( !all_present ) return SQLITE_BUSY;

  /* start a new block */
  rc = aergolite_begin_block(this_node);
  if( rc ) goto loc_failed;

  /* execute the transactions from the local mempool */
  binn_list_foreach(list, value) {
    int64 txn_id = value.vint64 & 0x7fffffffffffffff;
    /* x */
    for( txn=plugin->mempool; txn; txn=txn->next ){
      if( txn->id==txn_id ) break;
    }
    /* x */
    rc = aergolite_execute_transaction(this_node, txn->node_id, txn->nonce, txn->log);
    if( rc==SQLITE_BUSY ){  /* try again later */
      aergolite_rollback_block(this_node);
      return rc;
    }
    if( (rc!=SQLITE_OK) != (value.vint64<0) ){
      sqlite3_log(rc, "apply_block - transaction with different result");
      aergolite_rollback_block(this_node);
      goto loc_failed;
    }
  }

  rc = aergolite_verify_block(this_node, block->header, block->body, block->id);
  if( rc ) goto loc_failed;

  /* approved by this node */
  block->ack_count++;

  /* broadcast the approved block message */
  //! it must be signed (?) - later
  map = binn_map();
  binn_map_set_int32(map, PLUGIN_CMD, PLUGIN_BLOCK_APPROVED);
  binn_map_set_int64(map, PLUGIN_HEIGHT, block->height);
  binn_map_set_blob(map, PLUGIN_HASH, block->id, 32);
  for( node=plugin->peers; node; node=node->next ){
    if( node->is_authorized ){
      send_peer_message(node, map, NULL);
    }
  }
  binn_free(map);

  SYNCTRACE("verify_block OK\n");

  /* check if it already has sufficient votes */
//  if( block->ack_count >= majority(plugin->total_authorized_nodes) ){
    /* commit the new block on this node */
//    commit_block(plugin, block);
//  }

  return SQLITE_OK;

loc_failed:
  SYNCTRACE("verify_block FAILED\n");
// close connection?
// or try again? use a timer?
  if( rc!=SQLITE_BUSY ){
    plugin->sync_down_state = DB_STATE_OUTDATED; /* it may download this block later */
    discard_block(block);
    plugin->new_block = NULL;
  }
  return rc;
}

/****************************************************************************/

SQLITE_PRIVATE int commit_block(plugin *plugin, struct block *block){
  aergolite *this_node = plugin->this_node;
  struct transaction *txn;
  binn_iter iter;
  binn value;
  void *list;
  int rc;

  SYNCTRACE("commit_block\n");

  rc = aergolite_commit_block(this_node, block->header, block->body, block->signatures);
  if( rc ) goto loc_failed;

  /* get the list of transactions ids */
  list = binn_map_list(block->body, BODY_TXN_IDS);  //  BLOCK_TRANSACTIONS);

  /* mark the used transactions on the mempool */
  binn_list_foreach(list, value) {
    for( txn=plugin->mempool; txn; txn=txn->next ){
      if( txn->id==value.vint64 ){
        txn->block_height = block->height;
      }
    }
  }

  /* remove the old transactions from the mempool */
  binn_list_foreach(list, value) {
    for( txn=plugin->mempool; txn; txn=txn->next ){
      if( txn->block_height>0 && txn->block_height <= block->height - 2 ){
        discard_mempool_transaction(plugin, txn);
        break;
      }
    }
  }

  /* remove old transactions from mempool */
  check_mempool_transactions(plugin);

  /* replace the previous block by the new one */
  if( plugin->current_block ) discard_block(plugin->current_block);
  plugin->current_block = block;
  plugin->new_block = NULL;

  /* discard old block votes */
  discard_old_block_votes(plugin);

  SYNCTRACE("commit_block OK\n");

  if( plugin->is_leader ){
    start_new_block_timer(plugin);
  }

  return SQLITE_OK;

loc_failed:
  SYNCTRACE("commit_block FAILED\n");
// close connection?
// or try again? use a timer?
  if( rc!=SQLITE_BUSY ){
    plugin->sync_down_state = DB_STATE_OUTDATED; /* it may download this block later */
    discard_block(block);
    plugin->new_block = NULL;
  }
  return rc;
}

/****************************************************************************/

SQLITE_PRIVATE int apply_block(plugin *plugin, struct block *block){
  int rc;
  rc = verify_block(plugin, block);
  if( rc==SQLITE_OK ){
    rc = commit_block(plugin, block);
  }
  return rc;
}

/****************************************************************************/

SQLITE_PRIVATE int verify_last_block(plugin *plugin) {

  return verify_block(plugin, plugin->new_block);

}

/****************************************************************************/

SQLITE_PRIVATE void on_new_block(node *node, void *msg, int size) {
  aergolite *this_node = node->this_node;
  plugin *plugin = node->plugin;
  struct block *block;
  int64 height;
  void *header, *body;
  uchar id[32]={0};
  int rc;

  header = binn_map_blob(msg, PLUGIN_HEADER, NULL);
  body   = binn_map_blob(msg, PLUGIN_BODY, NULL);

  rc = aergolite_verify_block_header(this_node, header, body, &height, id);

  SYNCTRACE("on_new_block - %s height=%" INT64_FORMAT " id=%02X%02X%02X%02X\n",
            rc ? "INVALID BLOCK -" : "",
            height, id[0], id[1], id[2], id[3]);

  if( rc ) return;

  /* if this node is not prepared to apply this block, do not acknowledge its receival */
  if( !plugin->current_block ){
    SYNCTRACE("on_new_block plugin->current_block==NULL\n");
    if( height>1 ){
      request_state_update(plugin);  //! what if the block is from an attacker?
      return;
    }
  }else if( height<=plugin->current_block->height ){
    SYNCTRACE("on_new_block OLD BLOCK plugin->current_block->height=%" INT64_FORMAT "\n",
              plugin->current_block->height);
    return;
  }else if( height!=plugin->current_block->height+1 ){
    SYNCTRACE("on_new_block OUTDATED STATE plugin->current_block->height=%" INT64_FORMAT "\n",
              plugin->current_block->height);
    request_state_update(plugin);  //! what if the block is from an attacker?
    return;
  }

  /* if another block is open */
  if( plugin->new_block ){
    if( height==plugin->new_block->height && memcmp(id,plugin->new_block->id,32)==0 ){
      /* it is the same block */
      return;
    }else{
      /* it is a different block */
      rollback_block(plugin);
    }
  }

  /* allocate a new block structure */
  block = sqlite3_malloc_zero(sizeof(struct block));
  if( !block ) return;  // SQLITE_NOMEM;

  /* store the new block data */
  block->height = height;
  block->header = sqlite3_memdup(header, binn_size(header));
  block->body   = sqlite3_memdup(body,   binn_size(body));

  if( !block->header || !block->body ){
    SYNCTRACE("on_new_block FAILED header=%p body=%p\n", block->header, block->body);
    discard_block(block);
    return;
  }

  memcpy(block->id, id, 32);

  /* are there stored votes for this block? */
  transfer_block_votes(plugin, block);

  /* verify if the block is correct */
  verify_block(plugin, block);

}

/****************************************************************************/

SQLITE_PRIVATE void on_node_approved_block(node *source_node, void *msg, int size) {
  plugin *plugin = source_node->plugin;
  struct block *block;
  int64 height;
  unsigned char *id;

  height = binn_map_int64(msg, PLUGIN_HEIGHT);
  id = binn_map_blob(msg, PLUGIN_HASH, NULL);

  SYNCTRACE("on_node_approved_block - height=%" INT64_FORMAT " id=%02X%02X%02X%02X\n",
            height, *id, *(id+1), *(id+2), *(id+3));

  /* check if we have some block to be committed */
  block = plugin->new_block;
  if( !block ){
    int64 current_height = plugin->current_block ? plugin->current_block->height : 0;
    SYNCTRACE("on_node_approved_block - no open block\n");
    if( plugin->is_leader ) return;
    if( height>current_height ){
      /* store the vote for this block */
      store_block_vote(source_node, height, id);
      //if( plugin->sync_down_state==DB_STATE_IN_SYNC ){
      //  /* the block is not on memory. request it */
      //! request_block(source_node, height, id);  //! if it fails, start a state update
      //}
    }
    if( height>current_height+1 ){
      if( plugin->sync_down_state==DB_STATE_IN_SYNC ){
        request_state_update(plugin);
      }
    }
    return;
  }

  if( !plugin->is_leader ){
    /* if this node is in a state update, ignore the block commit command */
    if( plugin->sync_down_state!=DB_STATE_IN_SYNC ) return;
  }

  /* check if the local block is the expected one */
  if( block->height!=height || id==NULL || memcmp(block->id,id,32)!=0 ){
    SYNCTRACE("on_node_approved_block - unexpected block - cached block height: %"
              INT64_FORMAT " id=%02X%02X%02X%02X\n", block->height,
              *block->id, *(block->id+1), *(block->id+2), *(block->id+3));
    if( !plugin->is_leader ){
      rollback_block(plugin);
      request_state_update(plugin);
    }
    return;
  }

  /* check if this vote was already counted */
  if( add_block_vote(block,source_node->id)==false ){
    SYNCTRACE("on_node_approved_block - block vote already stored\n");
    return;
  }

  /* increment the number of nodes that approved the block */
  block->ack_count++;

  SYNCTRACE("on_node_approved_block - ack_count=%d total_authorized_nodes=%d\n",
            block->ack_count, plugin->total_authorized_nodes);

  /* check if we reached the majority of the nodes */
  if( block->ack_count >= majority(plugin->total_authorized_nodes) ){
    /* commit the new block on this node */
    if( plugin->is_leader ){
      apply_block(plugin, block);
    }else{
      commit_block(plugin, block);
    }
  }

}

/****************************************************************************/
/****************************************************************************/

SQLITE_PRIVATE int count_mempool_unused_txns(plugin *plugin){
  struct transaction *txn;
  int count = 0;
  for( txn=plugin->mempool; txn; txn=txn->next ){
    if( txn->block_height==0 ) count++;
  }
  return count;
}

/****************************************************************************/

SQLITE_PRIVATE bool is_next_nonce(plugin *plugin, int node_id, int64 nonce){
  struct node_nonce *item;
  int count, i;
  bool node_found = false;

  SYNCTRACE("is_next_nonce node_id=%d nonce=%" INT64_FORMAT "\n",
            node_id, nonce);

  count = array_count(plugin->nonces);
  for( i=0; i<count; i++ ){
    item = array_get(plugin->nonces, i);
    //if( item->node_id==node_id && nonce==item->last_nonce+1 ){
    if( item->node_id==node_id ){
      node_found = true;
      SYNCTRACE("is_next_nonce node_id=%d last_nonce=%" INT64_FORMAT "\n",
                node_id, item->last_nonce);
      if( nonce==item->last_nonce+1 ) return true;
    }
  }

  if( !node_found && nonce==1 ) return true;  //! workaround. remove it later!
  return false;
}

/****************************************************************************/

SQLITE_PRIVATE struct block * create_new_block(plugin *plugin) {
  aergolite *this_node = plugin->this_node;
  struct transaction *txn;
  struct block *block;
  int64 block_height;
  int rc, count;

  SYNCTRACE("create_new_block\n");

  /* are there unused transactions on mempool? */
  if( count_mempool_unused_txns(plugin)==0 ||
      !has_nodes_for_consensus(plugin)
  ){
    return (struct block *) -1;
  }

  /* get the list of last_nonce for each node */
  build_last_nonce_array(plugin);

  /* get the next block height */
  if( plugin->current_block ){
    block_height = plugin->current_block->height + 1;
  }else{
    SYNCTRACE("create_new_block plugin->current_block==NULL\n");
    block_height = 1;
  }

  /* allocate a new block object */
  block = sqlite3_malloc_zero(sizeof(struct block));
  if( !block ) return NULL;

  /* start the block creation */
  rc = aergolite_begin_block(this_node);
  if( rc ) goto loc_failed2;

  /* execute the transactions from the local mempool */
  count = 0;
loc_again:
  for( txn=plugin->mempool; txn; txn=txn->next ){
    if( txn->block_height==0 && is_next_nonce(plugin,txn->node_id,txn->nonce) ){
      /* include this transaction on the block */
      /* no need to check the return result. if the execution failed or was rejected
      ** the nonce will be included in the block as a failed transaction */
      rc = aergolite_execute_transaction(this_node, txn->node_id, txn->nonce, txn->log);
      if( rc==SQLITE_PERM ) continue;
      update_last_nonce_array(plugin, txn->node_id, txn->nonce);
      txn->block_height = -1;
      count++;
      goto loc_again;
    }
  }
  /* reset the flag on used transactions */
  for( txn=plugin->mempool; txn; txn=txn->next ){
    if( txn->block_height==-1 ) txn->block_height = 0;
  }

  /* if no valid transactions were found */
  if( count==0 ) goto loc_failed;

  /* finalize the block creation */
  rc = aergolite_create_block(this_node, &block->height, &block->header, &block->body, block->id);
  if( rc ) goto loc_failed2;

  array_free(&plugin->nonces);
  SYNCTRACE("create_new_block OK\n");
  return block;

loc_failed:
  aergolite_rollback_block(this_node);
loc_failed2:
  SYNCTRACE("create_new_block FAILED\n");
  if( block ) sqlite3_free(block);
  array_free(&plugin->nonces);
  return NULL;
}

/****************************************************************************/

/*
** Used by the leader.
** -start a db transaction
** -execute the transactions from the local mempool (without the BEGIN and COMMIT)
** -track which db pages were modified and their hashes
** -create a "block" with the transactions ids (and page hashes)
** -roll back the database transaction
** -reset the block ack_count
** -broadcast the block to the peers
*/
SQLITE_PRIVATE void new_block_timer_cb(uv_timer_t* handle) {
  plugin *plugin = (struct plugin *) handle->loop->data;
  aergolite *this_node = plugin->this_node;
  struct block *block;

  SYNCTRACE("new_block_timer_cb\n");

  if( !plugin->is_leader ){
    SYNCTRACE("new_block_timer_cb not longer the leader node\n");
    return;
  }

  /* if there is an open non-commited block */
  if( plugin->new_block ){
    SYNCTRACE("new_block_timer_cb OPEN BLOCK\n");
    return;
  }

  block = create_new_block(plugin);
  if( !block ){
    SYNCTRACE("create_new_block FAILED. restarting the timer\n");
    /* restart the timer */
    uv_timer_start(&plugin->new_block_timer, new_block_timer_cb, NEW_BLOCK_WAIT_INTERVAL, 0);
    return;
  }
  if( block==(struct block *)-1 ) return;

  /* store the new block */
  //llist_add(&plugin->blocks, block);
  plugin->new_block = block;

  add_block_vote(block, plugin->node_id);

  /* reset the block ack_count */
  block->ack_count = 1;  /* ack by this node */

  /* broadcast the block to the peers */
  broadcast_new_block(plugin);

}

/****************************************************************************/

SQLITE_PRIVATE void start_new_block_timer(plugin *plugin) {
  //if( plugin->new_block ) return;
  if( count_mempool_unused_txns(plugin)==0 ) return;
  if( !has_nodes_for_consensus(plugin) ) return;
  if( !uv_is_active((uv_handle_t*)&plugin->new_block_timer) ){
    SYNCTRACE("start_new_block_timer\n");
    uv_timer_start(&plugin->new_block_timer, new_block_timer_cb, plugin->block_interval, 0);
  }
}

/****************************************************************************/

SQLITE_PRIVATE binn* encode_new_block(plugin *plugin) {
  struct block *block;
  binn *map;
  if( !plugin->new_block ) return NULL;
  block = plugin->new_block;
  map = binn_map();
  if( binn_map_set_int32(map, PLUGIN_CMD, PLUGIN_NEW_BLOCK)==FALSE ) goto loc_failed;
  if( binn_map_set_blob(map, PLUGIN_HEADER, block->header, binn_size(block->header))==FALSE ) goto loc_failed;
  if( binn_map_set_blob(map, PLUGIN_BODY, block->body, binn_size(block->body))==FALSE ) goto loc_failed;
  return map;
loc_failed:
  if( map ) binn_free(map);
  return NULL;
}

/****************************************************************************/

/*
** Used by the leader.
*/
SQLITE_PRIVATE void send_new_block(plugin *plugin, node *node) {
  binn *map;

  if( !plugin->new_block ) return;

  SYNCTRACE("send_new_block - height=%" INT64_FORMAT "\n",
            plugin->new_block->height);

  map = encode_new_block(plugin);
  if( map ){
    send_peer_message(node, map, NULL);
    binn_free(map);
  }

}

/****************************************************************************/

/*
** Used by the leader.
*/
SQLITE_PRIVATE int broadcast_new_block(plugin *plugin) {
  struct node *node;
  binn *map;

  SYNCTRACE("broadcast_new_block - height=%" INT64_FORMAT "\n",
            plugin->new_block->height);

  /* signal other peers that there is a new transaction */
  map = encode_new_block(plugin);
  if( !map ) return SQLITE_BUSY;  /* flag to retry the command later */

  for( node=plugin->peers; node; node=node->next ){
    if( node->is_authorized ){
      send_peer_message(node, map, NULL);
    }
  }

  binn_free(map);
  return SQLITE_OK;
}
