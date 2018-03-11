#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <endian.h>

#if __BYTE_ORDER != __LITTLE_ENDIAN
#error Byte order swapping is currently not implemented.
#endif

#include "redisraft.h"

/* ------------------------------------ RaftRedisCommand ------------------------------------ */

/* Serialize a RaftRedisCommand into a Raft entry */
void RaftRedisCommandSerialize(raft_entry_data_t *target, RaftRedisCommand *source)
{
    size_t sz = sizeof(size_t) * (source->argc + 1);
    size_t len;
    int i;
    char *p;

    /* Compute sizes */
    for (i = 0; i < source->argc; i++) {
        RedisModule_StringPtrLen(source->argv[i], &len);
        sz += len;
    }

    /* Serialize argc */
    p = target->buf = RedisModule_Alloc(sz);
    target->len = sz;

    *(size_t *)p = source->argc;
    p += sizeof(size_t);

    /* Serialize argumnets */
    for (i = 0; i < source->argc; i++) {
        const char *d = RedisModule_StringPtrLen(source->argv[i], &len);
        *(size_t *)p = len;
        p += sizeof(size_t);
        memcpy(p, d, len);
        p += len;
    }
}

/* Deserialize a RaftRedisCommand from a Raft entry */
bool RaftRedisCommandDeserialize(RedisModuleCtx *ctx,
        RaftRedisCommand *target, raft_entry_data_t *source)
{
    char *p = source->buf;
    size_t argc = *(size_t *)p;
    p += sizeof(size_t);

    target->argv = RedisModule_Calloc(argc, sizeof(RedisModuleString *));
    target->argc = argc;

    int i;
    for (i = 0; i < argc; i++) {
        size_t len = *(size_t *)p;
        p += sizeof(size_t);

        target->argv[i] = RedisModule_CreateString(ctx, p, len);
        p += len;
    }

    return true;
}

/* Free a RaftRedisCommand */
void RaftRedisCommandFree(RedisModuleCtx *ctx, RaftRedisCommand *r)
{
    int i;

    for (i = 0; i < r->argc; i++) {
        RedisModule_FreeString(ctx, r->argv[i]);
    }
    RedisModule_Free(r->argv);
}


/* ------------------------------------ RaftRedisCommand ------------------------------------ */

/*
 * Execution of Raft log on the local instance.  There are two variants:
 * 1) Execution of a raft entry received from another node.
 * 2) Execution of a locally initiated command.
 */

static void executeLogEntry(RedisRaftCtx *rr, raft_entry_t *entry)
{
    RaftRedisCommand rcmd;
    RaftRedisCommandDeserialize(rr->ctx, &rcmd, &entry->data);
    RaftReq *req = entry->user_data;
    RedisModuleCtx *ctx = req ? req->ctx : rr->ctx;

    size_t cmdlen;
    const char *cmd = RedisModule_StringPtrLen(rcmd.argv[0], &cmdlen);

    RedisModule_ThreadSafeContextLock(ctx);
    RedisModuleCallReply *reply = RedisModule_Call(
            ctx, cmd, "v",
            &rcmd.argv[1],
            rcmd.argc - 1);
    RedisModule_ThreadSafeContextUnlock(ctx);
    if (req) {
        if (reply) {
            RedisModule_ReplyWithCallReply(ctx, reply);
        } else {
            RedisModule_ReplyWithError(ctx, "Unknown command/arguments");
        }
        RedisModule_UnblockClient(req->client, NULL);
    }

    if (reply) {
        RedisModule_FreeCallReply(reply);
    }

    RaftRedisCommandFree(rr->ctx, &rcmd);
}

/*
 * Callbacks to handle async Redis commands we send to remote peers.
 */

static void requestvote_response_handler(redisAsyncContext *c, void *r, void *privdata)
{
    Node *node = privdata;
    RedisRaftCtx *rr = node->rr;

    redisReply *reply = r;
    assert(reply != NULL);

    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        NODE_LOG_ERROR(node, "RAFT.REQUESTVOTE failed: %s\n", reply ? reply->str : "connection dropped.");
        return;
    }
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 ||
            reply->element[0]->type != REDIS_REPLY_INTEGER ||
            reply->element[1]->type != REDIS_REPLY_INTEGER) {
        NODE_LOG_ERROR(node, "invalid RAFT.REQUESTVOTE reply\n");
        return;
    }

    msg_requestvote_response_t response = {
        .term = reply->element[0]->integer,
        .vote_granted = reply->element[1]->integer
    };

    raft_node_t *raft_node = raft_get_node(rr->raft, node->id);
    assert(raft_node != NULL);

    int ret;
    if ((ret = raft_recv_requestvote_response(
            rr->raft,
            raft_node,
            &response)) != 0) {
        LOG_ERROR("raft_recv_requestvote_response failed, error %d\n", ret);
    }
    NODE_LOG_INFO(node, "received requestvote response\n");
}


static void appendentries_response_handler(redisAsyncContext *c, void *r, void *privdata)
{
    Node *node = privdata;
    RedisRaftCtx *rr = node->rr;

    redisReply *reply = r;
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        NODE_LOG_ERROR(node, "RAFT.APPENDENTRIES failed: %s\n", reply ? reply->str : "connection dropped.");
        return;
    }

    if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 4 ||
            reply->element[0]->type != REDIS_REPLY_INTEGER ||
            reply->element[1]->type != REDIS_REPLY_INTEGER ||
            reply->element[2]->type != REDIS_REPLY_INTEGER ||
            reply->element[3]->type != REDIS_REPLY_INTEGER) {
        NODE_LOG_ERROR(node, "invalid RAFT.APPENDENTRIES reply\n");
        return;
    }

    msg_appendentries_response_t response = {
        .term = reply->element[0]->integer,
        .success = reply->element[1]->integer,
        .current_idx = reply->element[2]->integer,
        .first_idx = reply->element[3]->integer
    };

    raft_node_t *raft_node = raft_get_node(rr->raft, node->id);
    assert(raft_node != NULL);

    int ret;
    if ((ret = raft_recv_appendentries_response(
            rr->raft,
            raft_node,
            &response)) != 0) {
        NODE_LOG_ERROR(node, "raft_recv_appendentries_response failed, error %d\n", ret);
    }

    /* Maybe we have pending stuff to apply now */
    raft_apply_all(rr->raft);
}

/*
 * Callbacks we provide to the Raft library
 */

static int raftSendRequestVote(raft_server_t *raft, void *user_data,
        raft_node_t *raft_node, msg_requestvote_t *msg)
{
    Node *node = (Node *) raft_node_get_udata(raft_node);
    RedisRaftCtx *rr = user_data;

    if (!(node->state & NODE_CONNECTED)) {
        NodeConnect(node, rr);
        NODE_LOG_DEBUG(node, "not connected, state=%u\n", node->state);
        return 0;
    }

    /* RAFT.REQUESTVOTE <src_node_id> <term> <candidate_id> <last_log_idx> <last_log_term> */
    if (redisAsyncCommand(node->rc, requestvote_response_handler,
                node, "RAFT.REQUESTVOTE %d %d:%d:%d:%d",
                raft_get_nodeid(raft),
                msg->term,
                msg->candidate_id,
                msg->last_log_idx,
                msg->last_log_term) != REDIS_OK) {
        NODE_LOG_ERROR(node, "failed requestvote");
    }

    return 0;
}

static int raftSendAppendEntries(raft_server_t *raft, void *user_data,
        raft_node_t *raft_node, msg_appendentries_t *msg)
{
    Node *node = (Node *) raft_node_get_udata(raft_node);
    RedisRaftCtx *rr = user_data;

    int argc = 4 + msg->n_entries * 2;
    char *argv[argc];
    size_t argvlen[argc];

    if (!(node->state & NODE_CONNECTED)) {
        NodeConnect(node, rr);
        NODE_LOG_ERROR(node, "not connected, state=%u\n", node->state);
        return 0;
    }

    char argv1_buf[12];
    char argv2_buf[50];
    char argv3_buf[12];
    argv[0] = "RAFT.APPENDENTRIES";
    argvlen[0] = strlen(argv[0]);
    argv[1] = argv1_buf;
    argvlen[1] = snprintf(argv1_buf, sizeof(argv1_buf)-1, "%d", raft_get_nodeid(raft));
    argv[2] = argv2_buf;
    argvlen[2] = snprintf(argv2_buf, sizeof(argv2_buf)-1, "%d:%d:%d:%d",
            msg->term,
            msg->prev_log_idx,
            msg->prev_log_term,
            msg->leader_commit);
    argv[3] = argv3_buf;
    argvlen[3] = snprintf(argv3_buf, sizeof(argv3_buf)-1, "%d", msg->n_entries);

    int i;
    for (i = 0; i < msg->n_entries; i++) {
        raft_entry_t *e = &msg->entries[i];
        argv[4 + i*2] = RedisModule_Alloc(64);
        argvlen[4 + i*2] = snprintf(argv[4 + i*2], 63, "%d:%d:%d", e->term, e->id, e->type);
        argvlen[5 + i*2] = e->data.len;
        argv[5 + i*2] = e->data.buf;
    }

    if (redisAsyncCommandArgv(node->rc, appendentries_response_handler,
                node, argc, (const char **)argv, argvlen) != REDIS_OK) {
        NODE_LOG_ERROR(node, "failed appendentries");
    }

    for (i = 0; i < msg->n_entries; i++) {
        RedisModule_Free(argv[4 + i*2]);
    }
    return 0;
}

static int raftPersistVote(raft_server_t *raft, void *user_data, int vote)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) user_data;

    rr->log->header->vote = vote;
    if (!RaftLogUpdate(rr, rr->log)) {
        return RAFT_ERR_SHUTDOWN;
    }

    return 0;
}

static int raftPersistTerm(raft_server_t *raft, void *user_data, int term, int vote)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) user_data;

    rr->log->header->term = term;
    if (!RaftLogUpdate(rr, rr->log)) {
        return RAFT_ERR_SHUTDOWN;
    }

    return 0;
}

static void raftLog(raft_server_t *raft, raft_node_t *node, void *user_data, const char *buf)
{
    if (node) {
        Node *n = raft_node_get_udata(node);
        if (n) {
            NODE_LOG_DEBUG(n, "[raft] %s\n", buf);
            return;
        }
    }
    LOG_DEBUG("[raft] %s\n", buf);
}

static int raftLogOffer(raft_server_t *raft, void *user_data, raft_entry_t *entry, int entry_idx)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) user_data;
    raft_node_t *raft_node;
    Node *node;

    if (!RaftLogAppend(rr, rr->log, entry)) {
        return RAFT_ERR_SHUTDOWN;
    }

    if (!raft_entry_is_cfg_change(entry)) {
        return 0;
    }

    RaftCfgChange *req = (RaftCfgChange *) entry->data.buf;

    switch (entry->type) {
        case RAFT_LOGTYPE_REMOVE_NODE:
            raft_node = raft_get_node(raft, req->id);
            assert(raft_node != NULL);
            raft_remove_node(raft, raft_node);
            break;

        case RAFT_LOGTYPE_ADD_NODE:
        case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
            node = NodeInit(req->id, &req->addr);

            int is_self = req->id == raft_get_nodeid(raft);
            if (entry->type == RAFT_LOGTYPE_ADD_NODE) {
                raft_node = raft_add_node(raft, node, node->id, is_self);
                assert(raft_node_is_voting(raft_node));
            } else {
                raft_node = raft_add_non_voting_node(raft, node, node->id, is_self);
            }
            if (!raft_node) {
                TRACE("Failed to add node, id=%d, log type=%d\n", node->id, entry->type);
                return 0;
            }
            break;
    }

    return 0;
}

static int raftLogPop(raft_server_t *raft, void *user_data, raft_entry_t *entry, int entry_idx)
{
    return 0;
}

static int raftApplyLog(raft_server_t *raft, void *user_data, raft_entry_t *entry, int entry_idx)
{
    RedisRaftCtx *rr = user_data;
    RaftCfgChange *req;

    /* Update commit index.
     * TODO: Do we want to write it now? Probably not sync though.
     */
    if (entry_idx > rr->log->header->commit_idx) {
        rr->log->header->commit_idx = entry_idx;
    }

    switch (entry->type) {
        case RAFT_LOGTYPE_REMOVE_NODE:

            req = (RaftCfgChange *) entry->data.buf;
            if (req->id == raft_get_nodeid(raft)) {
                return RAFT_ERR_SHUTDOWN;
            }
            break;

        case RAFT_LOGTYPE_NORMAL:
            executeLogEntry(rr, entry);
            break;
        default:
            break;
    }
    return 0;
}

static int raftLogGetNodeId(raft_server_t *raft, void *user_data, raft_entry_t *entry, int entry_idx)
{
    RaftCfgChange *req = (RaftCfgChange *) entry->data.buf;
    return req->id;
}

static int raftNodeHasSufficientLogs(raft_server_t *raft, void *user_data, raft_node_t *raft_node)
{
    Node *node = raft_node_get_udata(raft_node);
    assert (node != NULL);

    TRACE("node:%u has sufficient logs now", node->id);

    raft_entry_t entry = {
        .id = rand(),
        .type = RAFT_LOGTYPE_ADD_NODE
    };
    msg_entry_response_t response;

    RaftCfgChange *req;
    entry.data.len = sizeof(RaftCfgChange);
    entry.data.buf = RedisModule_Alloc(entry.data.len);
    req = (RaftCfgChange *) entry.data.buf;
    req->id = node->id;
    req->addr = node->addr;

    int e = raft_recv_entry(raft, &entry, &response);
    assert(e == 0);
    
    return 0;
}

raft_cbs_t redis_raft_callbacks = {
    .send_requestvote = raftSendRequestVote,
    .send_appendentries = raftSendAppendEntries,
    .persist_vote = raftPersistVote,
    .persist_term = raftPersistTerm,
    .log_offer = raftLogOffer,
    .log_pop = raftLogPop,
    .log = raftLog,
    .log_get_node_id = raftLogGetNodeId,
    .applylog = raftApplyLog,
    .node_has_sufficient_logs = raftNodeHasSufficientLogs,
};

/*
 * Handling of the Redis Raft context, including its own thread and
 * async I/O loop.
 */

static void callRaftPeriodic(uv_timer_t *handle)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) uv_handle_get_data((uv_handle_t *) handle);

    int ret = raft_periodic(rr->raft, 500);
    assert(ret == 0);
    raft_apply_all(rr->raft);
}

static void RedisRaftCtxhread(void *arg)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) arg;

    rr->loop = RedisModule_Alloc(sizeof(uv_loop_t));
    uv_loop_init(rr->loop);

    uv_async_init(rr->loop, &rr->rqueue_sig, RaftReqHandleQueue);
    uv_handle_set_data((uv_handle_t *) &rr->rqueue_sig, rr);

    uv_timer_init(rr->loop, &rr->ptimer);
    uv_handle_set_data((uv_handle_t *) &rr->ptimer, rr);
    uv_timer_start(&rr->ptimer, callRaftPeriodic, 500, 500);

    rr->running = true;
    uv_run(rr->loop, UV_RUN_DEFAULT);
}

int RedisRaftInit(RedisModuleCtx *ctx, RedisRaftCtx *rr, RedisRaftConfig *config)
{
    memset(rr, 0, sizeof(RedisRaftCtx));
    uv_mutex_init(&rr->rqueue_mutex);
    STAILQ_INIT(&rr->rqueue);
    rr->ctx = RedisModule_GetThreadSafeContext(NULL);

    /* Initialize raft library */
    rr->raft = raft_new();

    /* Create our own node. */
    raft_node_t *self;
    if (config->init) {
        self = raft_add_node(rr->raft, NULL, config->id, 1);
    } else {
        self = raft_add_non_voting_node(rr->raft, NULL, config->id, 1);
    }
    if (!self) {
        RedisModule_Log(ctx, REDIS_WARNING, "Failed to initialize raft_node");
        return REDISMODULE_ERR;
    }

    char default_raftlog[256];
    snprintf(default_raftlog, sizeof(default_raftlog)-1, "redisraft-log-%u.db", config->id);

    /* Initialize a new cluster? */
    if (config->init || config->join) {
        msg_entry_response_t response;
        msg_entry_t msg = {
            .id = rand(),
            .type = RAFT_LOGTYPE_ADD_NODE
        };
        msg.data.len = sizeof(RaftCfgChange);
        msg.data.buf = RedisModule_Alloc(msg.data.len);

        RaftCfgChange *req = (RaftCfgChange *) msg.data.buf;
        req->id = config->id;
        req->addr = config->addr;

        raft_become_leader(rr->raft);
        int e = raft_recv_entry(rr->raft, &msg, &response);
        assert (e == 0);

        /* Initialize log */
        rr->log = RaftLogCreate(rr, config->raftlog ? config->raftlog : default_raftlog);
        if (!rr->log) {
            RedisModule_Log(ctx, REDIS_WARNING, "Failed to initialize Raft log");
            return REDISMODULE_ERR;
        }
    } else {
        rr->log = RaftLogOpen(rr, config->raftlog ? config->raftlog : default_raftlog);
        if (!rr->log)  {
            RedisModule_Log(ctx, REDIS_WARNING, "Failed to open Raft log");
            return REDISMODULE_ERR;
        }

        int entries = RaftLogLoadEntries(rr, rr->log, raft_append_entry, rr->raft);
        if (entries < 0) {
            RedisModule_Log(ctx, REDIS_WARNING, "Failed to read Raft log");
            return REDISMODULE_ERR;
        } else {
            RedisModule_Log(ctx, REDIS_NOTICE, "%d entries loaded from Raft log", entries);
        }

        raft_set_commit_idx(rr->raft, rr->log->header->commit_idx);
        raft_apply_all(rr->raft);

        raft_vote_for_nodeid(rr->raft, rr->log->header->vote);
        raft_set_current_term(rr->raft, rr->log->header->term);
    }

    raft_set_callbacks(rr->raft, &redis_raft_callbacks, rr);
    return REDISMODULE_OK;
}

int RedisRaftStart(RedisModuleCtx *ctx, RedisRaftCtx *rr)
{
    /* Start Raft thread */
    if (uv_thread_create(&rr->thread, RedisRaftCtxhread, rr) < 0) {
        RedisModule_Log(ctx, REDIS_WARNING, "Failed to initialize redis_raft thread");
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}


/*
 * Raft Requests, which are exchanged between the Redis main thread
 * and the Raft thread over the requests queue.
 */

void RaftReqFree(RaftReq *req)
{
    int i;

    switch (req->type) {
        case RR_APPENDENTRIES:
            if (req->r.appendentries.msg.entries) {
                RedisModule_Free(req->r.appendentries.msg.entries);
                req->r.appendentries.msg.entries = NULL;
            }
            break;
        case RR_REDISCOMMAND:
            if (req->ctx) {
                for (i = 0; i < req->r.redis.cmd.argc; i++) {
                    RedisModule_FreeString(req->ctx, req->r.redis.cmd.argv[i]);
                }
                RedisModule_Free(req->r.redis.cmd.argv);
            }
            break;
    }
    RedisModule_Free(req);
}

RaftReq *RaftReqInit(RedisModuleCtx *ctx, enum RaftReqType type)
{
    RaftReq *req = RedisModule_Calloc(1, sizeof(RaftReq));
    if (ctx != NULL) {
        req->client = RedisModule_BlockClient(ctx, NULL, NULL, NULL, 0);
        req->ctx = RedisModule_GetThreadSafeContext(req->client);
    }
    req->type = type;

    return req;
}

void RaftReqSubmit(RedisRaftCtx *rr, RaftReq *req)
{
    uv_mutex_lock(&rr->rqueue_mutex);
    STAILQ_INSERT_TAIL(&rr->rqueue, req, entries);
    uv_mutex_unlock(&rr->rqueue_mutex);
    if (rr->running) {
        uv_async_send(&rr->rqueue_sig);
    }
}

static RaftReq *raft_req_fetch(RedisRaftCtx *rr)
{
    RaftReq *r = NULL;

    uv_mutex_lock(&rr->rqueue_mutex);
    if (!STAILQ_EMPTY(&rr->rqueue)) {
        r = STAILQ_FIRST(&rr->rqueue);
        STAILQ_REMOVE_HEAD(&rr->rqueue, entries);
    }
    uv_mutex_unlock(&rr->rqueue_mutex);

    return r;
}

void RaftReqHandleQueue(uv_async_t *handle)
{
    RedisRaftCtx *rr = (RedisRaftCtx *) uv_handle_get_data((uv_handle_t *) handle);
    RaftReq *req;

    while ((req = raft_req_fetch(rr))) {
        g_RaftReqHandlers[req->type](rr, req);
        if (!(req->flags & RR_PENDING_COMMIT)) {
            RaftReqFree(req);
        }
    }
}


/*
 * Implementation of specific request types.
 */

static int handleRequestVote(RedisRaftCtx *rr, RaftReq *req)
{
    msg_requestvote_response_t response;

    if (raft_recv_requestvote(rr->raft,
                raft_get_node(rr->raft, req->r.requestvote.src_node_id),
                &req->r.requestvote.msg,
                &response) != 0) {
        RedisModule_ReplyWithError(req->ctx, "operation failed"); // TODO: Identify cases
        goto exit;
    }

    RedisModule_ReplyWithArray(req->ctx, 2);
    RedisModule_ReplyWithLongLong(req->ctx, response.term);
    RedisModule_ReplyWithLongLong(req->ctx, response.vote_granted);

exit:
    RedisModule_FreeThreadSafeContext(req->ctx);
    RedisModule_UnblockClient(req->client, NULL);
    req->ctx = NULL;

    return REDISMODULE_OK;
}


static int handleAppendEntries(RedisRaftCtx *rr, RaftReq *req)
{
    msg_appendentries_response_t response;
    int err;

    if ((err = raft_recv_appendentries(rr->raft,
                raft_get_node(rr->raft, req->r.appendentries.src_node_id),
                &req->r.appendentries.msg,
                &response)) != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg)-1, "operation failed, error %d", err);
        RedisModule_ReplyWithError(req->ctx, msg);
        goto exit;
    }

    RedisModule_ReplyWithArray(req->ctx, 4);
    RedisModule_ReplyWithLongLong(req->ctx, response.term);
    RedisModule_ReplyWithLongLong(req->ctx, response.success);
    RedisModule_ReplyWithLongLong(req->ctx, response.current_idx);
    RedisModule_ReplyWithLongLong(req->ctx, response.first_idx);

exit:
    RedisModule_FreeThreadSafeContext(req->ctx);
    RedisModule_UnblockClient(req->client, NULL);
    req->ctx = NULL;

    return REDISMODULE_OK;
}

static int handleCfgChange(RedisRaftCtx *rr, RaftReq *req)
{
    raft_entry_t entry;

    memset(&entry, 0, sizeof(entry));
    entry.id = rand();

    switch (req->type) {
        case RR_CFGCHANGE_ADDNODE:
            entry.type = RAFT_LOGTYPE_ADD_NONVOTING_NODE;
            break;
        case RR_CFGCHANGE_REMOVENODE:
            entry.type = RAFT_LOGTYPE_REMOVE_NODE;
            break;
        default:
            assert(0);
    }

    entry.data.len = sizeof(req->r.cfgchange);
    entry.data.buf = RedisModule_Alloc(sizeof(req->r.cfgchange));
    memcpy(entry.data.buf, &req->r.cfgchange, sizeof(req->r.cfgchange));

    int e = raft_recv_entry(rr->raft, &entry, &req->r.redis.response);
    if (e) {
        // todo handle errors
        RedisModule_Free(entry.data.buf);
        RedisModule_ReplyWithSimpleString(req->ctx, "ERROR");
    } else {
        RedisModule_ReplyWithSimpleString(req->ctx, "OK");
    }

    RedisModule_FreeThreadSafeContext(req->ctx);
    RedisModule_UnblockClient(req->client, NULL);
    req->ctx = NULL;

    return REDISMODULE_OK;
}

static int handleRedisCommand(RedisRaftCtx *rr,RaftReq *req)
{
    raft_node_t *leader = raft_get_current_leader_node(rr->raft);
    if (!leader) {
        RedisModule_ReplyWithError(req->ctx, "-NOLEADER");
        goto exit;
    }
    if (raft_node_get_id(leader) != raft_get_nodeid(rr->raft)) {
        Node *l = raft_node_get_udata(leader);
        char *reply;

        asprintf(&reply, "LEADERIS %s:%u", l->addr.host, l->addr.port);

        RedisModule_ReplyWithError(req->ctx, reply);
        free(reply);
        goto exit;
    }

    raft_entry_t entry = {
        .id = rand(),
        .type = RAFT_LOGTYPE_NORMAL,
        .user_data = req,
    };

    RaftRedisCommandSerialize(&entry.data, &req->r.redis.cmd);
    int e = raft_recv_entry(rr->raft, &entry, &req->r.redis.response);
    if (e) {
        // todo handle errors
        RedisModule_Free(entry.data.buf);
        RedisModule_ReplyWithSimpleString(req->ctx, "ERROR");
        goto exit;
    }

    // We're now waiting
    req->flags |= RR_PENDING_COMMIT;

    return REDISMODULE_OK;

exit:
    RedisModule_FreeThreadSafeContext(req->ctx);
    RedisModule_UnblockClient(req->client, NULL);
    req->ctx = NULL;
    return REDISMODULE_OK;

}

static int handleInfo(RedisRaftCtx *rr, RaftReq *req)
{
    size_t slen = 1024;
    char *s = RedisModule_Calloc(1, slen);

    char role[10];
    switch (raft_get_state(rr->raft)) {
        case RAFT_STATE_FOLLOWER:
            strcpy(role, "follower");
            break;
        case RAFT_STATE_LEADER:
            strcpy(role, "leader");
            break;
        case RAFT_STATE_CANDIDATE:
            strcpy(role, "candidate");
            break;
        default:
            strcpy(role, "(none)");
            break;
    }

    s = catsnprintf(s, &slen,
            "# Nodes\n"
            "node_id:%d\n"
            "role:%s\n"
            "leader_id:%d\n"
            "current_term:%d\n",
            raft_get_nodeid(rr->raft),
            role,
            raft_get_current_leader(rr->raft),
            raft_get_current_term(rr->raft));

    int i;
    for (i = 0; i < raft_get_num_nodes(rr->raft); i++) {
        raft_node_t *rnode = raft_get_node_from_idx(rr->raft, i);
        Node *node = raft_node_get_udata(rnode);
        if (!node) {
            continue;
        }

        char state[20] = {0};

        if (node->state & NODE_CONNECTING) {
            strcat(state, "c");
        }
        if (node->state & NODE_CONNECTED) {
            strcat(state, "C");
        }

        s = catsnprintf(s, &slen,
                "node%d:id=%d,state=%s,addr=%s,port=%d\n",
                i, node->id, state, node->addr.host, node->addr.port);
    }

    s = catsnprintf(s, &slen,
            "\n# Log\n"
            "log_entries:%d\n"
            "current_index:%d\n"
            "commit_index:%d\n"
            "last_applied_index:%d\n",
            raft_get_log_count(rr->raft),
            raft_get_current_idx(rr->raft),
            raft_get_commit_idx(rr->raft),
            raft_get_last_applied_idx(rr->raft));

    RedisModule_ReplyWithSimpleString(req->ctx, s);
    RedisModule_FreeThreadSafeContext(req->ctx);
    RedisModule_UnblockClient(req->client, NULL);
    req->ctx = NULL;

    return REDISMODULE_OK;
}


RaftReqHandler g_RaftReqHandlers[] = {
    NULL,
    handleCfgChange,        /* RR_CFGCHANGE_ADDNODE */
    handleCfgChange,        /* RR_CFGCHANGE_REMOVENODE */
    handleAppendEntries,    /* RR_APPENDENTRIES */
    handleRequestVote,      /* RR_REQUESTVOTE */
    handleRedisCommand,     /* RR_REDISOCMMAND */
    handleInfo,             /* RR_INFO */
    NULL
};


