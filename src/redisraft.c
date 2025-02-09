/*
 * This file is part of RedisRaft.
 *
 * Copyright (c) 2020-2021 Redis Ltd.
 *
 * RedisRaft is licensed under the Redis Source Available License (RSAL).
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

#include "redisraft.h"

RedisModuleCtx *redis_raft_log_ctx = NULL;

int redis_raft_trace = TRACE_OFF;
int redis_raft_loglevel = LOG_LEVEL_NOTICE;

const char *redis_raft_log_levels[] = {
    REDISMODULE_LOGLEVEL_DEBUG,
    REDISMODULE_LOGLEVEL_VERBOSE,
    REDISMODULE_LOGLEVEL_NOTICE,
    REDISMODULE_LOGLEVEL_WARNING
};

RedisRaftCtx redis_raft = { 0 };
static RedisRaftConfig config;

/* This is needed for newer pthread versions to properly link and work */
#ifdef LINUX
void *__dso_handle;
#endif

#define VALID_NODE_ID(x)    ((x) > 0)

/* Parse a node address from a RedisModuleString */
static RRStatus getNodeAddrFromArg(RedisModuleCtx *ctx, RedisModuleString *arg, NodeAddr *addr)
{
    size_t node_addr_len;
    const char *node_addr_str = RedisModule_StringPtrLen(arg, &node_addr_len);
    if (!NodeAddrParse(node_addr_str, node_addr_len, addr)) {
        RedisModule_ReplyWithError(ctx, "invalid node address");
        return RR_ERROR;
    }

    return RR_OK;
}

/* RAFT.NODE ADD [id] [address:port]
 *   Add a new node to the cluster.  The [id] can be an explicit non-zero value,
 *   or zero to let the cluster choose one.
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   -CLUSTERDOWN ||
 *   -MOVED <slot> <addr>:<port> ||
 *   *2
 *   :<new node id>
 *   :<dbid>
 *
 * RAFT.NODE REMOVE [id]
 *   Remove an existing node from the cluster.
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   -CLUSTERDOWN ||
 *   -MOVED <slot> <addr>:<port> ||
 *   +OK
 */
static int cmdRaftNode(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (checkRaftState(rr, ctx) == RR_ERROR ||
        checkLeader(rr, ctx, NULL) == RR_ERROR) {
        return REDISMODULE_OK;
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);

    if (!strncasecmp(cmd, "ADD", cmd_len)) {
        if (argc != 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        /* Validate node id */
        long long node_id;
        if (RedisModule_StringToLongLong(argv[2], &node_id) != REDISMODULE_OK ||
                (node_id && !VALID_NODE_ID(node_id))) {
            RedisModule_ReplyWithError(ctx, "invalid node id");
            return REDISMODULE_OK;
        }

        if (hasNodeIdBeenUsed(rr, (raft_node_id_t) node_id)) {
            RedisModule_ReplyWithError(ctx, "node id has already been used in this cluster");
            return REDISMODULE_OK;
        }

        if (node_id == 0) {
            node_id = makeRandomNodeId(rr);
        }

        RaftCfgChange cfg = {
            .id = (raft_node_id_t) node_id
        };

        /* Parse address */
        if (getNodeAddrFromArg(ctx, argv[3], &cfg.addr) == RR_ERROR) {
            /* Error already produced */
            return REDISMODULE_OK;
        }

        msg_entry_response_t resp;
        RaftReq *req = RaftReqInit(ctx, RR_CFGCHANGE_ADDNODE);

        msg_entry_t *entry = raft_entry_new(sizeof(cfg));
        entry->id = rand();
        entry->type = RAFT_LOGTYPE_ADD_NONVOTING_NODE;
        memcpy(entry->data, &cfg, sizeof(cfg));
        entryAttachRaftReq(rr, entry, req);

        int e = raft_recv_entry(rr->raft, entry, &resp);
        if (e != 0) {
            entryDetachRaftReq(rr, entry);
            replyRaftError(req->ctx, e);
            raft_entry_release(entry);
            RaftReqFree(req);
            return REDISMODULE_OK;
        }

        raft_entry_release(entry);

    } else if (!strncasecmp(cmd, "REMOVE", cmd_len)) {
        if (argc != 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        long long node_id;
        if (RedisModule_StringToLongLong(argv[2], &node_id) != REDISMODULE_OK ||
            !VALID_NODE_ID(node_id)) {
                RedisModule_ReplyWithError(ctx, "invalid node id");
                return REDISMODULE_OK;
        }

        /* Validate it exists */
        if (!raft_get_node(rr->raft, (raft_node_id_t) node_id)) {
            RedisModule_ReplyWithError(ctx, "node id does not exist");
            return REDISMODULE_OK;
        }

        RaftCfgChange cfg = {
            .id = (raft_node_id_t) node_id
        };

        msg_entry_response_t resp;
        RaftReq *req = RaftReqInit(ctx, RR_CFGCHANGE_REMOVENODE);

        msg_entry_t *entry = raft_entry_new(sizeof(cfg));
        entry->id = rand();
        entry->type = RAFT_LOGTYPE_REMOVE_NODE;
        memcpy(entry->data, &cfg, sizeof(cfg));
        entryAttachRaftReq(rr, entry, req);

        int e = raft_recv_entry(rr->raft, entry, &resp);
        if (e != 0) {
            entryDetachRaftReq(rr, entry);
            replyRaftError(req->ctx, e);
            raft_entry_release(entry);
            RaftReqFree(req);
            return REDISMODULE_OK;
        }

        raft_entry_release(entry);

        /* Special case, we are the only node and our node has been removed */
        if (cfg.id == raft_get_nodeid(rr->raft) &&
            raft_get_num_voting_nodes(rr->raft) == 0) {
            shutdownAfterRemoval(rr);
        }
    } else {
        RedisModule_ReplyWithError(ctx, "RAFT.NODE supports ADD / REMOVE only");
    }

    return REDISMODULE_OK;
}

/* RAFT.TRANSFER_LEADER [target_node_id]
  *   Attempt to transfer raft cluster leadership to targeted node
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   -ERR
 *   +OK
 */
static int cmdRaftTransferLeader(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (checkRaftState(rr, ctx) == RR_ERROR) {
        return REDISMODULE_OK;
    }

    int target_node_id;
    if (RedisModuleStringToInt(argv[1], &target_node_id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "invalid target node id");
        return REDISMODULE_OK;
    }

    if (target_node_id == rr->config->id) {
        RedisModule_ReplyWithError(ctx, "can't transfer to self");
        return REDISMODULE_OK;
    }

    int err = raft_transfer_leader(rr->raft, target_node_id, 0);
    if (err != 0) {
        replyRaftError(ctx, err);
        return REDISMODULE_OK;
    }

    rr->transfer_req = RaftReqInit(ctx, RR_TRANSFER_LEADER);

    return REDISMODULE_OK;
}

/* RAFT.TIMEOUT_NOW
 *   instruct this node to force an election
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   +OK
 */
static int cmdRaftTimeoutNow(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 1) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (checkRaftState(rr, ctx) == RR_ERROR) {
        return REDISMODULE_OK;
    }

    raft_set_timeout_now(rr->raft);
    RedisModule_ReplyWithSimpleString(ctx, "OK");

    return REDISMODULE_OK;
}

/* RAFT.REQUESTVOTE [target_node_id] [src_node_id] [term]:[candidate_id]:[last_log_idx]:[last_log_term]:[transfer_leader]
 *   Request a node's vote (per Raft paper).
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   *2
 *   :<term>
 *   :<granted> (0 or 1)
 */
static int cmdRaftRequestVote(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (checkRaftState(rr, ctx) == RR_ERROR) {
        return REDISMODULE_OK;
    }

    int target_node_id;
    if (RedisModuleStringToInt(argv[1], &target_node_id) == REDISMODULE_ERR ||
        target_node_id != rr->config->id) {

        RedisModule_ReplyWithError(ctx, "invalid or incorrect target node id");
        return REDISMODULE_OK;
    }

    int src_node_id;
    if (RedisModuleStringToInt(argv[2], &src_node_id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "invalid source node id");
        return REDISMODULE_OK;
    }


    const char *tmpstr = RedisModule_StringPtrLen(argv[3], NULL);
    msg_requestvote_t req = {0};

    if (sscanf(tmpstr, "%d:%ld:%d:%ld:%ld:%d",
                &req.prevote,
                &req.term,
                &req.candidate_id,
                &req.last_log_idx,
                &req.last_log_term,
                &req.transfer_leader) != 6) {
        RedisModule_ReplyWithError(ctx, "invalid message");
        return REDISMODULE_OK;
    }

    msg_requestvote_response_t resp = {0};
    raft_node_t *node = raft_get_node(rr->raft, src_node_id);

    if (raft_recv_requestvote(rr->raft, node, &req, &resp) != 0) {
        RedisModule_ReplyWithError(ctx, "ERR operation failed");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithArray(ctx, 4);
    RedisModule_ReplyWithLongLong(ctx, resp.prevote);
    RedisModule_ReplyWithLongLong(ctx, resp.request_term);
    RedisModule_ReplyWithLongLong(ctx, resp.term);
    RedisModule_ReplyWithLongLong(ctx, resp.vote_granted);

    return REDISMODULE_OK;
}

/* RAFT [Redis command to execute]
 *   Submit a Redis command to be appended to the Raft log and applied.
 *   The command blocks until it has been committed to the log by the majority
 *   and applied locally.
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   -CLUSTERDOWN ||
 *   -MOVED <slot> <addr>:<port> ||
 *   Any standard Redis reply, depending on the command.
 */

static int cmdRaft(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RaftReq *req = RaftReqInit(ctx, RR_REDISCOMMAND);
    RaftRedisCommand *cmd = RaftRedisCommandArrayExtend(&req->r.redis.cmds);

    cmd->argc = argc - 1;
    cmd->argv = RedisModule_Alloc((argc - 1) * sizeof(RedisModuleString *));

    int i;
    for (i = 0; i < argc - 1; i++) {
        cmd->argv[i] =  argv[i + 1];
        RedisModule_RetainString(req->ctx, cmd->argv[i]);
    }
    handleRedisCommand(&redis_raft, req);

    return REDISMODULE_OK;
}

/* RAFT.ENTRY [Serialized Entry]
 *   Receive a serialized batch of Redis commands (like a Raft entry) and
 *   process them, as if received as individual RAFT commands.
 *
 *   This is used to simplify the proxying of MULTI/EXEC commands.
 * Reply:
 *   -MOVED <addr> ||
 *   Any standard Redis reply
 */
static int cmdRaftEntry(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    size_t data_len;
    const char *data = RedisModule_StringPtrLen(argv[1], &data_len);

    RaftReq *req = RaftReqInit(ctx, RR_REDISCOMMAND);
    if (RaftRedisCommandArrayDeserialize(&req->r.redis.cmds, data, data_len) != RR_OK) {
        RedisModule_ReplyWithError(ctx, "ERR invalid argument");
        RaftReqFree(req);
    } else {
        handleRedisCommand(&redis_raft, req);
    }

    return REDISMODULE_OK;
}

/* RAFT.INFO
 *   Display Raft module specific info.
 * Reply:
 *   Raw text output, formatted like INFO.
 */
static int cmdRaftInfo(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RaftReq *req = RaftReqInit(ctx, RR_INFO);
    handleInfo(&redis_raft, req);

    return REDISMODULE_OK;
}


/* RAFT.AE [target_node_id] [src_node_id] [term]:[prev_log_idx]:[prev_log_term]:[leader_commit]
 *      [n_entries] [<term>:<id>:<type> <entry>]...
 *   A leader request to append entries to the Raft log (per Raft paper).
 * Reply:
 *   -NOCLUSTER ||
 *   -LOADING ||
 *   *4
 *   :<term>
 *   :<success> (0 or 1)
 *   :<current_idx>
 *   :<first_idx>
 */

static int cmdRaftAppendEntries(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 5) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    int target_node_id;
    if (RedisModuleStringToInt(argv[1], &target_node_id) == REDISMODULE_ERR ||
        target_node_id != rr->config->id) {
            RedisModule_ReplyWithError(ctx, "invalid or incorrect target node id");
            return REDISMODULE_OK;
    }

    long long n_entries;
    if (RedisModule_StringToLongLong(argv[4], &n_entries) != REDIS_OK) {
        RedisModule_ReplyWithError(ctx, "invalid n_entries value");
        return REDISMODULE_OK;
    }
    if (argc != 5 + 2 * n_entries) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    RaftReq *req = RaftReqInit(ctx, RR_APPENDENTRIES);
    if (RedisModuleStringToInt(argv[2], &req->r.appendentries.src_node_id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "invalid source node id");
        goto error_cleanup;
    }

    size_t tmplen;
    const char *tmpstr = RedisModule_StringPtrLen(argv[3], &tmplen);
    if (sscanf(tmpstr, "%d:%ld:%ld:%ld:%ld:%lu",
                &req->r.appendentries.msg.leader_id,
                &req->r.appendentries.msg.term,
                &req->r.appendentries.msg.prev_log_idx,
                &req->r.appendentries.msg.prev_log_term,
                &req->r.appendentries.msg.leader_commit,
                &req->r.appendentries.msg.msg_id) != 6) {
        RedisModule_ReplyWithError(ctx, "invalid message");
        goto error_cleanup;
    }

    req->r.appendentries.msg.n_entries = (int)n_entries;
    if (n_entries > 0) {
        req->r.appendentries.msg.entries = RedisModule_Calloc(n_entries, sizeof(msg_entry_t));
    }

    for (int i = 0; i < n_entries; i++) {
        /* Create entry with payload */
        tmpstr = RedisModule_StringPtrLen(argv[6 + 2*i], &tmplen);
        msg_entry_t *e = raft_entry_new(tmplen);
        memcpy(e->data, tmpstr, tmplen);

        /* Parse additional entry fields */
        tmpstr = RedisModule_StringPtrLen(argv[5 + 2*i], &tmplen);
        if (sscanf(tmpstr, "%ld:%d:%hd",
                    &e->term,
                    &e->id,
                    &e->type) != 3) {
            RedisModule_ReplyWithError(ctx, "invalid entry");
            raft_entry_release(e);
            goto error_cleanup;
        }

        req->r.appendentries.msg.entries[i] = e;
    }

    handleAppendEntries(rr, req);
    return REDISMODULE_OK;

error_cleanup:
    RaftReqFree(req);
    return REDISMODULE_OK;
}

/* RAFT.CONFIG GET [wildcard]
 *   Query Raft configuration parameters.
 *
 * RAFT.CONFIG SET [param] [value]
 *   Set a Raft configuration parameter.
 *
 * This is basically identical to Redis CONFIG GET / CONFIG SET, for
 * Raft specific configuration.
 */
static int cmdRaftConfig(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);

    if (!strncasecmp(cmd, "SET", cmd_len)) {
        if (argc != 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        ConfigSet(rr, ctx, argv, argc);
    } else if (!strncasecmp(cmd, "GET", cmd_len)) {
        if (argc != 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        ConfigGet(rr, ctx, argv, argc);
    } else {
        RedisModule_ReplyWithError(ctx, "ERR Unknown RAFT.CONFIG subcommand or wrong number of arguments");
    }

    return REDISMODULE_OK;
}

/* RAFT.SNAPSHOT [target-node-id] [src_node_id]
 *               [term]:[leader_id]:[msg_id]:[snapshot_index]:[snapshot_term]:[chunk_offset]:[last_chunk]
 *               [chunk_data]
 *   Store the specified snapshot chunk (e.g. Raft paper's InstallSnapshot RPC).
 *
 *  Reply:
 *    -NOCLUSTER ||
 *    -LOADING ||
 *    *5
 *    :<term>
 *    :<msg_id>
 *    :<offset>
 *    :<success>
 *    :<last_chunk>
 */
static int cmdRaftSnapshot(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 5) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (checkRaftState(rr, ctx) == RR_ERROR) {
        return REDISMODULE_OK;
    }

    int target_node_id;
    if (RedisModuleStringToInt(argv[1], &target_node_id) != REDISMODULE_OK ||
        target_node_id != rr->config->id) {

        RedisModule_ReplyWithError(ctx, "ERR invalid or incorrect target node id");
        return REDISMODULE_OK;
    }

    int src_node_id;
    if (RedisModuleStringToInt(argv[2], &src_node_id) == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "invalid source node id");
        return REDISMODULE_OK;
    }

    msg_snapshot_t req = {0};
    const char *tmpstr = RedisModule_StringPtrLen(argv[3], NULL);

    if (sscanf(tmpstr, "%lu:%d:%lu:%lu:%lu:%llu:%d",
               &req.term,
               &req.leader_id,
               &req.msg_id,
               &req.snapshot_index,
               &req.snapshot_term,
               &req.chunk.offset,
               &req.chunk.last_chunk) != 7) {
        RedisModule_ReplyWithError(ctx, "invalid message");
        return REDISMODULE_OK;
    }

    size_t len;
    void *data = (void*) RedisModule_StringPtrLen(argv[4], &len);

    req.chunk.data = data;
    req.chunk.len = len;

    msg_snapshot_response_t resp = {0};
    raft_node_t *node = raft_get_node(rr->raft, (raft_node_id_t) src_node_id);

    if (raft_recv_snapshot(rr->raft, node, &req, &resp) != 0) {
        RedisModule_ReplyWithError(ctx, "ERR operation failed");
        return REDISMODULE_OK;
    }

    RedisModule_ReplyWithArray(ctx, 5);
    RedisModule_ReplyWithLongLong(ctx, resp.term);
    RedisModule_ReplyWithLongLong(ctx, resp.msg_id);
    RedisModule_ReplyWithLongLong(ctx, resp.offset);
    RedisModule_ReplyWithLongLong(ctx, resp.success);
    RedisModule_ReplyWithLongLong(ctx, resp.last_chunk);

    return REDISMODULE_OK;
}

static void clusterInit(const char *cluster_id)
{
    RedisRaftCtx *rr = &redis_raft;

    /* Initialize dbid */
    memcpy(rr->snapshot_info.dbid, cluster_id, RAFT_DBID_LEN);
    rr->snapshot_info.dbid[RAFT_DBID_LEN] = '\0';

    /* This is the first node, so there are no used node ids yet */
    rr->snapshot_info.used_node_ids = NULL;

    /* If node id was not specified, make up one */
    if (!rr->config->id) {
        rr->config->id = makeRandomNodeId(rr);
    }

    rr->log = RaftLogCreate(rr->config->raft_log_filename,
                            rr->snapshot_info.dbid, 1, 0, rr->config);
    if (!rr->log) {
        PANIC("Failed to initialize Raft log");
    }

    addUsedNodeId(rr, rr->config->id);
    RaftLibraryInit(rr, true);
    initSnapshotTransferData(rr);
    AddBasicLocalShardGroup(rr);

    LOG_NOTICE("Raft Cluster initialized, node id: %d, dbid: %s",
               rr->config->id, rr->snapshot_info.dbid);
}

static void clusterJoinCompleted(RaftReq *req)
{
    RedisRaftCtx *rr = &redis_raft;

    /* Initialize Raft log.  We delay this operation as we want to create the
     * log with the proper dbid which is only received now.
     */
    rr->log = RaftLogCreate(rr->config->raft_log_filename,
                            rr->snapshot_info.dbid,
                            rr->snapshot_info.last_applied_term,
                            rr->snapshot_info.last_applied_idx,
                            rr->config);
    if (!rr->log) {
        PANIC("Failed to initialize Raft log");
    }

    AddBasicLocalShardGroup(rr);
    RaftLibraryInit(rr, false);
    initSnapshotTransferData(rr);

    RedisModule_ReplyWithSimpleString(req->ctx, "OK");
    RaftReqFree(req);
}

/* RAFT.CLUSTER INIT <id>
 *   Initializes a new Raft cluster.
 *   <id> is an optional 32 character string, if set, cluster will use it for the id
 * Reply:
 *   +OK [dbid]
 *
 * RAFT.CLUSTER JOIN [addr:port]
 *   Join an existing cluster.
 *   The operation is asynchronous and may take place/retry in the background.
 * Reply:
 *   +OK
 */
static int cmdRaftCluster(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (checkRaftUninitialized(rr, ctx) == RR_ERROR ||
        checkRaftNotLoading(rr, ctx) == RR_ERROR) {
        return REDISMODULE_OK;
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);

    if (!strncasecmp(cmd, "INIT", cmd_len)) {
        if (argc != 2 && argc != 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        char cluster_id[RAFT_DBID_LEN];

        if (argc == 2) {
            RedisModule_GetRandomHexChars(cluster_id, RAFT_DBID_LEN);
        } else {
            size_t len;
            const char *reqId = RedisModule_StringPtrLen(argv[2], &len);
            if (len != RAFT_DBID_LEN) {
                RedisModule_ReplyWithError(ctx, "ERR cluster id must be 32 characters");
                return REDISMODULE_OK;
            }
            memcpy(cluster_id, reqId, RAFT_DBID_LEN);
        }

        clusterInit(cluster_id);

        char reply[RAFT_DBID_LEN + 256];
        snprintf(reply, sizeof(reply) - 1, "OK %s", rr->snapshot_info.dbid);

        RedisModule_ReplyWithSimpleString(ctx, reply);
    } else if (!strncasecmp(cmd, "JOIN", cmd_len)) {
        if (argc < 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        NodeAddrListElement *addrs = NULL;

        for (int i = 2; i < argc; i++) {
            NodeAddr addr;
            if (getNodeAddrFromArg(ctx, argv[i], &addr) == RR_ERROR) {
                /* Error already produced */
                return REDISMODULE_OK;
            }
            NodeAddrListAddElement(&addrs, &addr);
        }

        RaftReq *req = RaftReqInit(ctx, RR_CLUSTER_JOIN);
        JoinCluster(rr, addrs, req, clusterJoinCompleted);

        NodeAddrListFree(addrs);
        rr->state = REDIS_RAFT_JOINING;
    } else {
        RedisModule_ReplyWithError(ctx, "RAFT.CLUSTER supports INIT / JOIN only");
    }

    return REDISMODULE_OK;
}

/* RAFT.SHARDGROUP GET
 *   Returns the current cluster's local shard group configuration, in a format
 *   that is compatible with RAFT.SHARDGROUP ADD.
 * Reply:
 *   [start-slot] [end-slot] [node-id node-addr] [node-id node-addr...]
 *
 * RAFT.SHARDGROUP ADD [shardgroup id] [num_slots] [num_nodes] ([start slot] [end slot] [slot type])* ([node-uid node-addr:node-port])*
 *   Adds a new shard group configuration.
 * Reply:
 *   +OK
 *
 * RAFT.SHARDGROUP REPLACE [num shardgroups] ([shardgroup id] [num_slots] [num_nodes] ([start slot] [end slot] [slot type])* ([node-uid node-addr:node-port])*)*
 *   Replaces all the external shardgroups with the external shardgroups listed here.  ignores the shardgroup set that is the local cluster
 * Reply:
 *   +OK
 *
 * RAFT.SHARDGROUP LINK [node-addr:port]
 *   Link cluster with a new remote shardgroup.
 * Reply:
 *   +OK
 *   -ERR error description
 */
static int cmdRaftShardGroup(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    if (!rr->config->sharding) {
        RedisModule_ReplyWithError(ctx, "ERR RedisRaft sharding not enabled");
        return REDISMODULE_OK;
    }

    if (checkRaftState(rr, ctx) == RR_ERROR ||
        checkLeader(rr, ctx, NULL) == RR_ERROR) {
        return REDISMODULE_OK;
    }

    size_t cmd_len;
    const char *cmd = RedisModule_StringPtrLen(argv[1], &cmd_len);

    if (!strncasecmp(cmd, "GET", cmd_len)) {
        ShardGroupGet(rr, ctx);
        return REDISMODULE_OK;
    } else if (!strncasecmp(cmd, "ADD", cmd_len)) {
        if (argc < 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        ShardGroupAdd(rr, ctx, argv, argc);
        return REDISMODULE_OK;
    } else if (!strncasecmp(cmd, "REPLACE", cmd_len)) {
        if (argc < 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        ShardGroupReplace(rr, ctx, argv, argc);
        return REDISMODULE_OK;
    } else if (!strncasecmp(cmd, "LINK", cmd_len)) {
        if (argc != 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        ShardGroupLink(rr, ctx, argv, argc);
        return REDISMODULE_OK;
    } else {
        RedisModule_ReplyWithError(ctx, "RAFT.SHARDGROUP supports GET/ADD/REPLACE/LINK only");
        return REDISMODULE_OK;
    }
}

/* RAFT.DEBUG COMPACT [delay]
 *   Initiate an immediate rewrite of the Raft log + snapshot.
 *   If [delay] is specified, introduce an artificial delay of [delay] seconds in
 *   the background rewrite child process.
 * Reply:
 *   +OK
 */
static int cmdRaftDebug(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    size_t cmdlen;
    const char *cmdstr = RedisModule_StringPtrLen(argv[1], &cmdlen);
    char cmd[cmdlen+1];

    memcpy(cmd, cmdstr, cmdlen);
    cmd[cmdlen] = '\0';

    if (!strncasecmp(cmd, "compact", cmdlen)) {
        long long fail = 0;
        long long delay = 0;

        if (argc == 3) {
            if (RedisModule_StringToLongLong(argv[2], &delay) != REDISMODULE_OK) {
                RedisModule_ReplyWithError(ctx, "ERR invalid compact delay value");
                return REDISMODULE_OK;
            }
        }

        if (argc == 4) {
            if (RedisModule_StringToLongLong(argv[3], &fail) != REDISMODULE_OK) {
                RedisModule_ReplyWithError(ctx, "ERR invalid compact fail value");
                return REDISMODULE_OK;
            }
        }

        RaftReq *req = RaftReqInit(ctx, RR_DEBUG);
        req->r.debug.delay = (int) delay;
        req->r.debug.fail = (int) fail;

        rr->debug_req = req;

        if (initiateSnapshot(rr) != RR_OK) {
            LOG_VERBOSE("RAFT.DEBUG COMPACT requested but failed.");
            RedisModule_ReplyWithError(req->ctx, "ERR operation failed");
            rr->debug_req = NULL;
            RaftReqFree(req);
        }
    } else if (!strncasecmp(cmd, "nodecfg", cmdlen)) {
        if (argc != 4) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        long long node_id;
        if (RedisModule_StringToLongLong(argv[2], &node_id) != REDISMODULE_OK) {
            RedisModule_ReplyWithError(ctx, "ERR invalid node id");
            return REDISMODULE_OK;
        }

        raft_node_t *node = raft_get_node(rr->raft, (raft_node_id_t) node_id);
        if (!node) {
            RedisModule_ReplyWithError(ctx, "ERR node does not exist");
            return REDISMODULE_OK;
        }

        size_t slen;
        const char *str = RedisModule_StringPtrLen(argv[3], &slen);

        char *cfg = RedisModule_Alloc(slen + 1);
        memcpy(cfg, str, slen);
        cfg[slen] = '\0';

        char *saveptr = NULL;
        char *tok;

        tok = strtok_r(cfg, " ", &saveptr);
        while (tok != NULL) {
            if (!strcasecmp(tok, "+voting")) {
                raft_node_set_voting(node, 1);
                raft_node_set_voting_committed(node, 1);
            } else if (!strcasecmp(tok, "-voting")) {
                raft_node_set_voting(node, 0);
                raft_node_set_voting_committed(node, 0);
            } else if (!strcasecmp(tok, "+active")) {
                raft_node_set_active(node, 1);
            } else if (!strcasecmp(tok, "-active")) {
                raft_node_set_active(node, 0);
            } else {
                RedisModule_ReplyWithError(ctx, "ERR invalid nodecfg option");
                RedisModule_Free(cfg);
                return REDISMODULE_OK;
            }
            tok = strtok_r(NULL, " ", &saveptr);
        }

        RedisModule_Free(cfg);
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    } else if (!strncasecmp(cmd, "sendsnapshot", cmdlen)) {
        if (argc != 3) {
            RedisModule_WrongArity(ctx);
            return REDISMODULE_OK;
        }

        long long node_id;
        if (RedisModule_StringToLongLong(argv[2], &node_id) != REDISMODULE_OK) {
            RedisModule_ReplyWithError(ctx, "ERR invalid node id");
            return REDISMODULE_OK;
        }

        raft_node_t *node = raft_get_node(rr->raft, (raft_node_id_t) node_id);
        if (!node) {
            RedisModule_ReplyWithError(ctx, "ERR node does not exist");
            return REDISMODULE_OK;
        }

        if (node_id == raft_get_nodeid(rr->raft)) {
            RedisModule_ReplyWithError(ctx, "ERR cannot send snapshot itself");
            return REDISMODULE_OK;
        }

        raft_node_set_next_idx(node, raft_get_snapshot_last_idx(rr->raft));
        RedisModule_ReplyWithSimpleString(ctx, "OK");

    } else if (!strncasecmp(cmd, "used_node_ids", cmdlen)) {
        RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);

        int len = 0;
        NodeIdEntry *e;

        for (e = rr->snapshot_info.used_node_ids; e != NULL; e = e->next) {
            RedisModule_ReplyWithLongLong(ctx, e->id);
            len++;
        }

        RedisModule_ReplySetArrayLength(ctx, len);
    } else if (!strncasecmp(cmd, "exec", cmdlen) && argc > 3) {
        RedisModuleCallReply *reply;

        const char *exec_cmd = RedisModule_StringPtrLen(argv[2], NULL);

        enterRedisModuleCall();
        reply = RedisModule_Call(ctx, exec_cmd, "v", &argv[3], argc - 3);
        exitRedisModuleCall();

        if (!reply) {
            RedisModule_ReplyWithError(ctx, "Bad command or failed to execute");
        } else {
            RedisModule_ReplyWithCallReply(ctx, reply);
            RedisModule_FreeCallReply(reply);
        }
    } else {
        RedisModule_ReplyWithError(ctx, "ERR invalid debug subcommand");
    }

    return REDISMODULE_OK;
}

static int cmdRaftNodeShutdown(RedisModuleCtx *ctx,
                               RedisModuleString **argv, int argc)
{
    RedisRaftCtx *rr = &redis_raft;

    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }

    long long node_id;
    if (RedisModule_StringToLongLong(argv[1], &node_id) != REDISMODULE_OK ||
        node_id != rr->config->id) {
        RedisModule_ReplyWithError(ctx, "invalid node id");
        return REDISMODULE_OK;
    }

    shutdownAfterRemoval(rr);
    return REDISMODULE_OK;
}

static int cmdRaftSort(RedisModuleCtx *ctx,
                       RedisModuleString **argv, int argc)
{
    if (argc < 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_OK;
    }


    argv++;
    argc--;

    handleSort(ctx, argv, argc);

    return REDISMODULE_OK;

}

static int cmdRaftRandom(RedisModuleCtx *ctx,
                         RedisModuleString **argv, int argc)
{
    RedisModule_ReplyWithError(ctx, "Cannot run a command with random results in this context");

    return REDISMODULE_OK;
}

void handleClientDisconnectEvent(RedisModuleCtx *ctx,
        RedisModuleEvent eid, uint64_t subevent, void *data)
{
    if (eid.id == REDISMODULE_EVENT_CLIENT_CHANGE &&
            subevent == REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED) {
        RedisModuleClientInfo *ci = (RedisModuleClientInfo *) data;
        RaftReq *req = RaftReqInit(NULL, RR_CLIENT_DISCONNECT);
        req->r.client_disconnect.client_id = ci->id;
        handleClientDisconnect(&redis_raft, req);
    }
}

/* Command filter callback that intercepts normal Redis commands and prefixes them
 * with a RAFT command prefix in order to divert them to execute inside RedisRaft.
 */
static void interceptRedisCommands(RedisModuleCommandFilterCtx *filter)
{
    if (checkInRedisModuleCall()) {
        /* if we are running a command in lua that has to be sorted to be deterministic across all nodes */
        if (redis_raft.entered_eval) {
            RedisModuleString *cmd = RedisModule_CommandFilterArgGet(filter, 0);
            const CommandSpec *cs;
            if ((cs = CommandSpecGet(cmd)) != NULL) {
                if (cs->flags & CMD_SPEC_SORT_REPLY) {
                    RedisModuleString *raft_str = RedisModule_CreateString(NULL, "RAFT._SORT_REPLY", 16);
                    RedisModule_CommandFilterArgInsert(filter, 0, raft_str);
                } else if (cs->flags & CMD_SPEC_RANDOM) {
                    RedisModuleString *raft_str = RedisModule_CreateString(NULL, "RAFT._REJECT_RANDOM_COMMAND", 27);
                    RedisModule_CommandFilterArgInsert(filter, 0, raft_str);
                }
            }
        }

        return;
    }

    const CommandSpec *cs = CommandSpecGet(RedisModule_CommandFilterArgGet(filter, 0));
    if (cs && (cs->flags & CMD_SPEC_DONT_INTERCEPT))
        return;

    /* Prepend RAFT to the original command */
    RedisModuleString *raft_str = RedisModule_CreateString(NULL, "RAFT", 4);
    RedisModule_CommandFilterArgInsert(filter, 0, raft_str);
}

/* Callback from Redis event loop */
static void beforeSleep(RedisModuleCtx *ctx,
                        RedisModuleEvent eid, uint64_t subevent, void *data)
{
    if (subevent == REDISMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP) {
        handleBeforeSleep(&redis_raft);
    }
}


static int registerRaftCommands(RedisModuleCtx *ctx)
{
    /* Register commands.
     *
     * NOTE: Internal RedisRaft module commands must also be set with
     * their apropriate flags in commands.c, typically with a
     * CMD_SPEC_DONT_INTERCEPT flag.
     * */
    if (RedisModule_CreateCommand(ctx, "raft",
                cmdRaft, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.entry",
                cmdRaftEntry, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.info",
                cmdRaftInfo, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.config",
                cmdRaftConfig, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.cluster",
                cmdRaftCluster, "admin",0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.shardgroup",
                cmdRaftShardGroup, "admin",0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.node",
                cmdRaftNode, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.ae",
                cmdRaftAppendEntries, "write", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.transfer_leader",
                cmdRaftTransferLeader, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.timeout_now",
                cmdRaftTimeoutNow, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.requestvote",
                cmdRaftRequestVote, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.snapshot",
                cmdRaftSnapshot, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.debug",
                cmdRaftDebug, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft.nodeshutdown",
                cmdRaftNodeShutdown, "admin", 0, 0, 0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft._sort_reply",
                cmdRaftSort, "admin", 0,0,0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }

    if (RedisModule_CreateCommand(ctx, "raft._reject_random_command",
                                  cmdRaftRandom, "admin", 0,0,0) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }


    if ((RedisRaftType = RedisModule_CreateDataType(ctx, REDIS_RAFT_DATATYPE_NAME, REDIS_RAFT_DATATYPE_ENCVER,
            &RedisRaftTypeMethods)) == NULL) {
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

__attribute__((__unused__)) int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    if (RedisModule_Init(ctx, "redisraft", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_NOTICE,
                    "RedisRaft version %s [%s]",
                    REDISRAFT_VERSION, REDISRAFT_GIT_SHA1);

    /* With https://github.com/redis/redis/pull/9968, rdbSave() function
     * prototype has changed. RedisRaft uses this function, we are dependent on
     * its prototype. Here, we check existence of a symbol which was added in
     * the same PR. So, we make sure Redis build contains this change.
     * This check should be replaced with a proper version check after Redis 7.0
     * release. */
    void *handle = dlopen(NULL, RTLD_NOW);
    if (!dlsym(handle, "rdbSaveFunctions") ) {
        RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_NOTICE,
                        "RedisRaft requires Redis build from unstable branch!");
        dlclose(handle);
        return REDISMODULE_ERR;
    }
    dlclose(handle);

    /* Sanity check Redis version */
    if (RedisModule_SubscribeToServerEvent == NULL ||
            RedisModule_RegisterCommandFilter == NULL ||
            RedisModule_GetCommandKeys == NULL ||
            RedisModule_GetDetachedThreadSafeContext == NULL ||
            RedisModule_MonotonicMicroseconds == NULL) {
        RedisModule_Log(ctx, REDISMODULE_LOGLEVEL_NOTICE,
                        "RedisRaft requires Redis build from unstable branch!");
        return REDISMODULE_ERR;
    }

    /* Create a logging context */
    redis_raft_log_ctx = RedisModule_GetDetachedThreadSafeContext(ctx);

    /* Sanity check that not running with cluster_enabled */
    RedisModuleServerInfoData *info = RedisModule_GetServerInfo(ctx, "cluster");
    int cluster_enabled = (int) RedisModule_ServerInfoGetFieldSigned(info, "cluster_enabled", NULL);
    RedisModule_FreeServerInfo(ctx, info);
    if (cluster_enabled) {
        LOG_WARNING("Redis Raft requires Redis not be started with cluster_enabled!");
        return REDISMODULE_ERR;
    }

    /* Report arguments */
    size_t str_len = 1024;
    char *str = RedisModule_Calloc(1, str_len);
    int i;
    for (i = 0; i < argc; i++) {
        size_t slen;
        const char *s = RedisModule_StringPtrLen(argv[i], &slen);
        str = catsnprintf(str, &str_len, "%s%.*s", i == 0 ? "" : " ", (int) slen, s);
    }

    LOG_NOTICE("RedisRaft starting, arguments: %s", str);
    RedisModule_Free(str);

    /* Initialize and validate configuration */
    ConfigInit(ctx, &config);
    if (ConfigParseArgs(ctx, argv, argc, &config) == RR_ERROR) {
        return REDISMODULE_ERR;
    }

    /* Configure Redis */
    if (ConfigureRedis(ctx) == RR_ERROR) {
        LOG_WARNING("Failed to set Redis configuration!");
        return REDISMODULE_ERR;
    }

    if (CommandSpecInit(ctx, &config) == RR_ERROR) {
        LOG_WARNING("Failed to initialize internal command table");
        return REDISMODULE_ERR;
    }

    if (registerRaftCommands(ctx) == RR_ERROR) {
        LOG_WARNING("Failed to register commands");
        return REDISMODULE_ERR;
    }

    raft_set_heap_functions(RedisModule_Alloc,
                            RedisModule_Calloc,
                            RedisModule_Realloc,
                            RedisModule_Free);

    if (RedisRaftInit(ctx, &redis_raft, &config) == RR_ERROR) {
        return REDISMODULE_ERR;
    }

    redis_raft.registered_filter = RedisModule_RegisterCommandFilter(ctx,
        interceptRedisCommands, 0);

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_ClientChange,
                handleClientDisconnectEvent) != REDISMODULE_OK) {
        LOG_WARNING("Failed to subscribe to server events.");
        return REDISMODULE_ERR;
    }

    if (RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_EventLoop,
                                           beforeSleep) != REDISMODULE_OK) {
        LOG_WARNING("Failed to subscribe to server events.");
        return REDISMODULE_ERR;
    }

    RedisRaftCtx *rr = &redis_raft;

#ifdef HAVE_TLS
    if (rr->config->tls_enabled) {
         redisSSLContextError ssl_error;
         rr->ssl = redisCreateSSLContext(rr->config->tls_ca_cert,
                                         NULL,
                                         rr->config->tls_cert,
                                         rr->config->tls_key,
                                         NULL,
                                         &ssl_error);
         if (!rr->ssl) {
             const char *err = redisSSLContextGetError(ssl_error);
             LOG_WARNING("Failed to create ssl context: %s", err);
             return REDISMODULE_ERR;
         }
    }
#endif

    RedisModule_CreateTimer(ctx, rr->config->raft_interval, callRaftPeriodic, rr);
    RedisModule_CreateTimer(ctx, rr->config->reconnect_interval, callHandleNodeStates, rr);

    LOG_NOTICE("Raft module loaded, state is '%s'", getStateStr(rr));

    fsyncThreadStart(&rr->fsyncThread, handleFsyncCompleted);

    return REDISMODULE_OK;
}
