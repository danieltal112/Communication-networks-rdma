#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/time.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <infiniband/verbs.h>
#include "bw_template.h"
#include "db.h"

const int RX_DEPTH = 100;
const int TX_DEPTH = 100;
const int SL = 0;
const enum ibv_mtu MTU = IBV_MTU_2048;

//init
int init_dev_list(KvHandle* my_kv){
    my_kv->dev_list = ibv_get_device_list(NULL);
    if (!my_kv->dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }
    return 0;
}

int init_context(KvHandle* my_kv){
    struct ibv_device* ib_dev = *my_kv->dev_list;
    if (!ib_dev) {
        fprintf(stderr, "No IB devices found\n");
        return 1;
    }

    my_kv->context = ibv_open_device(ib_dev);
    if (!my_kv->context) {
        fprintf(stderr, "Couldn't get context for %s\n", ibv_get_device_name(ib_dev));
        return 1;
    }
    return 0;
}

struct ibv_cq* init_cq(KvHandle *kv_handle)
{
    int numOfCq = kv_handle->if_server ? NUM_OF_CLIENTS : 1;
    struct ibv_cq* cq = ibv_create_cq(kv_handle->context, numOfCq * (RX_DEPTH + TX_DEPTH), NULL,
                                      NULL, 0);
    if (!cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return NULL;
    }

    return cq;
}

int init_pd(struct pingpong_context *ctx, KvHandle* kv_handle){
    ctx->pd = ibv_alloc_pd(kv_handle->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        return 1;
    }
    return 0;
}

int init_buf(struct pingpong_context *ctx, KvHandle* kv_handle, int page_size){
    ctx->buf = malloc(roundup(kv_handle->size, page_size));
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        return 1;
    }
    return 0;
}

struct ibv_mr* init_mr(struct ibv_pd* pd, void* buf, size_t size, enum ibv_access_flags access)
{
    struct ibv_mr* mr = ibv_reg_mr(pd, buf, size, access);
    if (!mr) {
        fprintf(stderr, "Couldn't register MR\n");
        return NULL;
    }

    return mr;
}

struct ibv_qp* init_qp(struct pingpong_context* ctx)
{
    struct ibv_qp* qp;
    {
        struct ibv_qp_init_attr attr = {
                .send_cq = ctx->cq,
                .recv_cq = ctx->cq,
                .cap     = {
                        .max_send_wr  = TX_DEPTH,
                        .max_recv_wr  = RX_DEPTH,
                        .max_send_sge = 1,
                        .max_recv_sge = 1
                },
                .qp_type = IBV_QPT_RC
        };

        qp = ibv_create_qp(ctx->pd, &attr);
        if (qp == NULL) {
            fprintf(stderr, "Couldn't create QP\n");
            return NULL;
        }
    }

    {
        struct ibv_qp_attr attr = {
                .qp_state        = IBV_QPS_INIT,
                .pkey_index      = 0,
                .port_num        = IB_PORT,
                .qp_access_flags = IBV_ACCESS_REMOTE_READ |
                                   IBV_ACCESS_REMOTE_WRITE
        };

        if (ibv_modify_qp(qp, &attr,
                          IBV_QP_STATE     |
                          IBV_QP_PKEY_INDEX         |
                          IBV_QP_PORT               |
                          IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "Failed to modify QP to INIT\n");
            return NULL;
        }
    }

    return qp;
}

struct pingpong_context *pp_init_ctx(KvHandle* kv_handle, struct ibv_cq* cq)
{
    struct pingpong_context *ctx;
    int page_size = sysconf(_SC_PAGESIZE);

    ctx = calloc(1, sizeof *ctx);
    if (!ctx) {return NULL;}

    ctx->cq = cq;

    // Init multiple client resurces
    for (int i = 0; i < MAX_RESOURCES; ++i) {
        ctx->resources[i].mr = NULL;
        ctx->resources[i].buf = NULL;
    }

    ctx->size     = kv_handle->size;
    ctx->rx_depth = RX_DEPTH;
//    ctx->routs    = RX_DEPTH;
    ctx->channel = NULL;

    if(init_buf(ctx, kv_handle, page_size)){return NULL;}
    memset(ctx->buf, 0, kv_handle->size);

    if(init_pd(ctx, kv_handle)){return NULL;}

    ctx->mr = init_mr(ctx->pd, ctx->buf, kv_handle->size, IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->mr) {return NULL;}

    ctx->qp = init_qp(ctx);
    if (!ctx->qp) {return NULL;}
    return ctx;
}

int get_port_info(KvHandle* kv_handle){
    if (ibv_query_port(kv_handle->context, IB_PORT, &kv_handle->ctx->portinfo)) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }
    return 0;
}

int get_local_lid(KvHandle* kv_handle){
    kv_handle->my_dest.lid = kv_handle->ctx->portinfo.lid;
    if (kv_handle->ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !kv_handle->my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return 1;
    }
    return 0;
}

int check_gidX(KvHandle* kv_handle){
    if (kv_handle->gidx >= 0) {
        if (ibv_query_gid(kv_handle->context, IB_PORT, kv_handle->gidx, &kv_handle->my_dest.gid)) {
            fprintf(stderr, "Could not get local gid for gid index %d\n", kv_handle->gidx);
            return 1;
        }
    } else {
        memset(&kv_handle->my_dest.gid, 0, sizeof kv_handle->my_dest.gid);
    }
    return 0;
}

int pp_post_recv_client(struct pingpong_context *ctx, int n)
{
    int i;
    size_t size = MAX_BUF_SIZE * sizeof(char);

    struct ibv_sge list = {
            .addr    = (uintptr_t) ctx->buf,
            .length = size,
            .lkey    = ctx->mr->lkey
    };
    struct ibv_recv_wr wr = {
            .wr_id      = CLIENT_RECEIVE,
            .sg_list    = &list,
            .num_sge    = 1,
            .next       = NULL
    };
    struct ibv_recv_wr *bad_wr;

    for (i = 0; i < n; ++i) {
        if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
            break;
    }
    return i;
}

int init_client_post_recv(KvHandle* kv_handle){
    kv_handle->ctx->routs = pp_post_recv_client(kv_handle->ctx, kv_handle->ctx->rx_depth);
    if (kv_handle->ctx->routs < kv_handle->ctx->rx_depth) {
        fprintf(stderr, "Couldn't post receive (%d)\n", kv_handle->ctx->routs);
        return 1;
    }
    return 0;
}

int init_network_context(KvHandle* kv_handle, const char* servername)
{
    kv_handle->if_server = servername ? 0 : 1 ;
    char gid[33];
    kv_handle->size = MAX_BUF_SIZE * sizeof(char);
    kv_handle->gidx = -1;

    if(init_dev_list(kv_handle)){return 1;}
    if(init_context(kv_handle)){return 1;}

    struct ibv_cq* cq = init_cq(kv_handle);
    if (cq == NULL)
    {
        return 1;
    }

    kv_handle->ctx = pp_init_ctx(kv_handle, cq);
    if (!kv_handle->ctx) {return 1;}

    if(get_port_info(kv_handle)){return 1;}
    if(get_local_lid(kv_handle)){return 1;}
    if(check_gidX(kv_handle)){return 1;}

    kv_handle->my_dest.qpn = kv_handle->ctx->qp->qp_num;
    kv_handle->my_dest.psn = lrand48() & 0xffffff;
    inet_ntop(AF_INET6, &kv_handle->my_dest.gid, gid, sizeof gid);
//    printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
//           kv_handle->my_dest.lid, kv_handle->my_dest.qpn, kv_handle->my_dest.psn, gid);

    return 0;
}

//connect
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
    char tmp[9];
    uint32_t v32;
    int i;

    for (tmp[8] = 0, i = 0; i < 4; ++i) {
        memcpy(tmp, wgid + i * 8, 8);
        sscanf(tmp, "%x", &v32);
        *(uint32_t *)(&gid->raw[i * 4]) = ntohl(v32);
    }
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
    int i;

    for (i = 0; i < 4; ++i)
        sprintf(&wgid[i * 8], "%08x", htonl(*(uint32_t *)(gid->raw + i * 4)));
}

struct pingpong_dest *pp_client_exch_dest(const char *servername, const struct pingpong_dest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_family   = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

    if (asprintf(&service, "%d", PORT) < 0)
        return NULL;

    n = getaddrinfo(servername, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, PORT);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't connect to %s:%d\n", servername, PORT);
        return NULL;
    }

    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    if (write(sockfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        goto out;
    }

    if (read(sockfd, msg, sizeof msg) != sizeof msg) {
        perror("client read");
        fprintf(stderr, "Couldn't read remote address\n");
        goto out;
    }

    write(sockfd, "done", sizeof "done");

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);

    out:
    close(sockfd);
    return rem_dest;
}

int pp_connect_ctx(struct ibv_qp* qp, int my_psn,
                   struct pingpong_dest *dest, int sgid_idx)
{
    struct ibv_qp_attr attr = {
            .qp_state		= IBV_QPS_RTR,
            .path_mtu		= MTU,
            .dest_qp_num		= dest->qpn,
            .rq_psn			= dest->psn,
            .max_dest_rd_atomic	= 1,
            .min_rnr_timer		= 12,
            .ah_attr		= {
                    .is_global	= 0,
                    .dlid		= dest->lid,
                    .sl		= SL,
                    .port_num	= IB_PORT
            }
    };


    if (dest->gid.global.interface_id) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.dgid = dest->gid;
        attr.ah_attr.grh.sgid_index = sgid_idx;
    }
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE              |
                      IBV_QP_AV                 |
                      IBV_QP_PATH_MTU           |
                      IBV_QP_DEST_QPN           |
                      IBV_QP_RQ_PSN             |
                      IBV_QP_MAX_DEST_RD_ATOMIC |
                      IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state	    = IBV_QPS_RTS;
    attr.timeout	    = 14;
    attr.retry_cnt	    = 7;
    attr.rnr_retry	    = 7;
    attr.sq_psn	    = my_psn;
    attr.max_rd_atomic  = 1;
    if (ibv_modify_qp(qp, &attr,
                      IBV_QP_STATE              |
                      IBV_QP_TIMEOUT            |
                      IBV_QP_RETRY_CNT          |
                      IBV_QP_RNR_RETRY          |
                      IBV_QP_SQ_PSN             |
                      IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }
    return 0;
}

int pull_cq(KvHandle * kv_handle, struct ibv_wc *wc, int iters)
{
    printf("-------------Pull_CQ-------------\n");
    printf("Iters: %d\n", iters);
    for (int i = 0; i < iters; ++i)
    {
        int ne;
        do {
            ne = ibv_poll_cq(kv_handle->ctx->cq, 1, wc);
            if (ne != 0)
            {
                printf("ne = %d\nwc->wr_id = %lu\n", ne, wc->wr_id);
            }
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                wc = NULL;
                return 1;
            }
        } while (ne < 1);

        if (wc->status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                    ibv_wc_status_str(wc->status),
                    wc->status, (int) wc->wr_id);
            return 1;
        }
    }
    return 0;
}

int restore_post_receive_queue(struct pingpong_context* ctx)
{
    printf("restore_post_receive_queue\n");
    ctx->routs = pp_post_recv_client(ctx, ctx->rx_depth);
    if (ctx->routs < ctx->rx_depth)
    {
        fprintf(stderr, "Failed to post receive %d messages\n", ctx->rx_depth - ctx->routs);
        return 1;
    }
    return 0;
}

int empty_cq(KvHandle* kv_handle, struct ibv_wc *wc, int stopCondition)
{
    printf("-------------empty_cq-------------\n");
    int wr_id = -1;
    printf("Stop condition: %d\n", stopCondition);
    while (wr_id != stopCondition)
    {
        int ne;
        do {
            ne = ibv_poll_cq(kv_handle->ctx->cq, 1, wc);
            if (ne != 0)
            {
                printf("ne = %d\nwc->wr_id = %lu\n", ne, wc->wr_id);
            }
            if (ne < 0) {
                fprintf(stderr, "poll CQ failed %d\n", ne);
                wc = NULL;
                return 1;
            }


            if (stopCondition == I_SEND_SET && ne == 0)
            {
                return 0;
            }
        } while (ne < 1);

        if (wc->opcode == IBV_WC_RECV && --kv_handle->ctx->routs == 0)
        {
            if (restore_post_receive_queue(kv_handle->ctx))
            {
                return 1;
            }
        }

        if (wc->status != IBV_WC_SUCCESS) {
            fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
                    ibv_wc_status_str(wc->status),
                    wc->status, (int) wc->wr_id);
            return 1;
        }

        wr_id = wc->wr_id;
        // The wc is the message received from the server through the post receive queue
        if (wr_id < MAX_RESOURCES)
        {
            free(kv_handle->ctx->resources[wr_id].buf);
            ibv_dereg_mr(kv_handle->ctx->resources[wr_id].mr);
        }
    }
    return 0;
}

int pp_post_send(struct pingpong_context *ctx)
{
    struct ibv_sge list = {
            .addr	= (uintptr_t)ctx->buf,
            .length = ctx->size,
            .lkey	= ctx->mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
            .wr_id	    = I_SEND,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
            .next       = NULL
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

char* copy_message_data_to_buf(char* buf_pointer, size_t keySize, size_t valueSize, enum OperationType operation,
        enum Protocol protocol, void* value_address, uint32_t rkey, int wr_id)
{
    MessageData messageData;
    memset(&messageData, 0, sizeof(MessageData));
    messageData.operationType = operation;
    messageData.Protocol = protocol;
    messageData.keySize = keySize;
    messageData.valueSize = valueSize;
    messageData.value_address = value_address;
    messageData.rkey = rkey;
    messageData.wr_id = wr_id;
    memcpy(buf_pointer, &messageData, sizeof(MessageData));
    return buf_pointer + sizeof(MessageData);
}

char* get_message_data(char* buffer, MessageData* messageData){
    memcpy(messageData, buffer, sizeof(MessageData));
    return buffer + sizeof(MessageData);
}

//server function

int pp_post_recv(struct pingpong_context *ctx, int resource_idx)
{
    struct ibv_sge list = {
            .addr    = (uintptr_t)ctx->resources[resource_idx].buf,
            .length  = MAX_BUF_SIZE,
            .lkey    = ctx->resources[resource_idx].mr->lkey
    };
    struct ibv_recv_wr wr = {
            .wr_id      = resource_idx,
            .sg_list    = &list,
            .num_sge    = 1,
            .next       = NULL
    };
    struct ibv_recv_wr *bad_wr;

    if (ibv_post_recv(ctx->qp, &wr, &bad_wr) != 0) {
        perror("failed post_recv");
        return 1;
    }
    return 0;
}


int init_resource(Resource* resource, struct ibv_pd* pd, size_t size, enum ibv_access_flags access)
{
    void* buf = malloc(size);
    if (buf == NULL)
    {
        return 1;
    }
    struct ibv_mr* mr = init_mr(pd, buf, size, access);
    if (mr == NULL)
    {
        return 1;
    }

    resource->buf = buf;
    resource->mr = mr;
    return 0;
}

int pp_post_recv_server(struct pingpong_context *ctx, int rx)
{
    int i;
    for (i = 0; i < rx; ++i)
    {
        if (init_resource(ctx->resources + i, ctx->pd, MAX_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE) != 0)
        {
            fprintf(stderr, "Failed to init resource\n");
            return i;
        }
        if (pp_post_recv(ctx,i) != 0)
        {
            return i;
        }
    }

    return i;
}

struct pingpong_dest *pp_server_exch_dest(struct ibv_qp* qp,
                                          const struct pingpong_dest *my_dest,
                                          int sgid_idx)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_flags    = AI_PASSIVE,
            .ai_family   = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
    int n;
    int sockfd = -1, connfd;
    struct pingpong_dest *rem_dest = NULL;
    char gid[33];

    if (asprintf(&service, "%d", PORT) < 0)
        return NULL;

    n = getaddrinfo(NULL, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for port %d\n", gai_strerror(n), PORT);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            n = 1;

            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

            if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't listen to port %d\n", PORT);
        return NULL;
    }


    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, 0);
    close(sockfd);
    if (connfd < 0) {
        fprintf(stderr, "accept() failed\n");
        return NULL;
    }

    n = read(connfd, msg, sizeof msg);
    if (n != sizeof msg) {
        perror("server read");
        fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
        goto out;
    }

    rem_dest = malloc(sizeof *rem_dest);
    if (!rem_dest)
        goto out;

    sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
    wire_gid_to_gid(gid, &rem_dest->gid);
    if (pp_connect_ctx(qp, my_dest->psn,rem_dest, sgid_idx)) {
        fprintf(stderr, "Couldn't connect to remote QP\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }


    gid_to_wire_gid(&my_dest->gid, gid);
    sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
    if (write(connfd, msg, sizeof msg) != sizeof msg) {
        fprintf(stderr, "Couldn't send local address\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    read(connfd, msg, sizeof msg);

    out:
    close(connfd);
    return rem_dest;
}

int pp_post_rdma(struct pingpong_context* ctx, MessageData* messageData, enum ibv_wr_opcode opcode,
        uintptr_t buffer_address, int wr_id)
{
    struct ibv_sge list = {
            .addr   = buffer_address,
            .length = messageData->valueSize,
            .lkey   = ctx->resources[messageData->wr_id].value_mr->lkey
    };

    struct ibv_send_wr* bad_wr;
    struct ibv_send_wr wr = {
            .wr_id       = wr_id,
            .sg_list     = &list,
            .num_sge     = 1,
            .opcode      = opcode,
            .send_flags  = IBV_SEND_SIGNALED,
            .wr.rdma.remote_addr = (uintptr_t)messageData->value_address,
            .wr.rdma.rkey        = messageData->rkey,
            .next        = NULL
    };
    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

int pp_close_ctx(struct pingpong_context *ctx)
{
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr)) {
        fprintf(stderr, "Couldn't deregister MR\n");
        return 1;
    }

    if (ibv_dealloc_pd(ctx->pd)) {
        fprintf(stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ctx->channel) {
        if (ibv_destroy_comp_channel(ctx->channel)) {
            fprintf(stderr, "Couldn't destroy completion channel\n");
            return 1;
        }
    }


    free(ctx->buf);
    free(ctx);

    return 0;
}


void free_and_reset_ptr(void* resource)
{
    free(resource);
    resource = NULL;
}

void free_and_reset_mr(struct ibv_mr* mr)
{
    ibv_dereg_mr(mr);
    mr = NULL;
}

void release_kv_handler(KvHandle** kv_handle)
{
    if (kv_handle)
    {
        ibv_free_device_list((*kv_handle)->dev_list);
        free((*kv_handle)->rem_dest);
        if (ibv_close_device((*kv_handle)->context)){
            return;
        }
        *kv_handle = NULL;
    }
}
