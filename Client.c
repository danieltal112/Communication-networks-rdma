#include "Client.h"
#include <stdio.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <stdlib.h>
#include <string.h>
#include "bw_template.h"

int pp_post_send_set_client(struct pingpong_context *ctx)
{
    struct ibv_sge list = {
            .addr	= (uintptr_t)ctx->resources[ctx->count_send].buf,
            .length = ctx->size,
            .lkey	= ctx->resources[ctx->count_send].mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
            .wr_id	    = ctx->count_send,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
            .next       = NULL
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

int pp_post_send_get_client(struct pingpong_context *ctx)
{
    struct ibv_sge list = {
            .addr	= (uintptr_t)ctx->buf,
            .length = ctx->size,
            .lkey	= ctx->mr->lkey
    };

    struct ibv_send_wr *bad_wr, wr = {
            .wr_id	    = I_SEND_GET,
            .sg_list    = &list,
            .num_sge    = 1,
            .opcode     = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
            .next       = NULL
    };

    return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

int eager_set(KvHandle* kv_handle, const char* key, const char* value, size_t keySize, size_t valueSize)
{
    init_resource(&kv_handle->ctx->resources[kv_handle->ctx->count_send], kv_handle->ctx->pd,
                  MAX_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);

    char* buf_pointer = kv_handle->ctx->resources[kv_handle->ctx->count_send].buf;
    buf_pointer = copy_message_data_to_buf(buf_pointer, keySize, valueSize, SET, EAGER,
                                           NULL, 0, 0);

    strcpy(buf_pointer, key);
    buf_pointer += sizeof(key);
    strcpy(buf_pointer, value);
    if(pp_post_send_set_client(kv_handle->ctx)){
        printf("error send\n");
        return 1;
    }
    kv_handle->ctx->count_send += 1 % MAX_RESOURCES;
    return 0;
}

int eager_get(MessageData* messageData, char* data, char** value)
{
    *value = malloc(messageData->valueSize);
    if (!*value) {return 1;}
    memcpy(*value, data, messageData->valueSize);
    return 0;
}

// This function creates a buffer and mr for the value
int init_value(KvHandle* kv_handle, Resource* resources, const char* value){
    size_t valSize = strlen(value) + 1;
    resources->value_buffer = malloc(valSize);
    strcpy(resources->value_buffer, value);
    resources->value_mr = init_mr(kv_handle->ctx->pd, resources->value_buffer, valSize,
                                  IBV_ACCESS_REMOTE_READ);

    if(!resources->value_mr || resources->value_buffer == NULL)
    {
        return 1;
    }
    return 0;
}

int rendezvous_set(KvHandle* kv_handle, const char* key, const char* value, size_t keySize, size_t valueSize)
{
    if (init_value(kv_handle, &kv_handle->ctx->resources[kv_handle->ctx->count_send], value)){
        return 1;
    }

    init_resource(&kv_handle->ctx->resources[kv_handle->ctx->count_send], kv_handle->ctx->pd,
                  MAX_BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    char* buf_pointer = kv_handle->ctx->resources[kv_handle->ctx->count_send].buf;
    uint32_t rkey = kv_handle->ctx->resources[kv_handle->ctx->count_send].value_mr->rkey;
    void* value_address = kv_handle->ctx->resources[kv_handle->ctx->count_send].value_buffer;

    buf_pointer = copy_message_data_to_buf(buf_pointer, keySize, valueSize, SET, RENDEZVOUS,
                                           value_address, rkey, kv_handle->ctx->count_send);

    strcpy(buf_pointer, key);
    buf_pointer += keySize;

    if(pp_post_send_set_client(kv_handle->ctx)){
        printf("error send\n");
        return 1;
    }
    kv_handle->ctx->count_send += 1 % MAX_RESOURCES;
    return 0;
}

int rendezvous_get(KvHandle* kv_handle, MessageData* messageData, char** value)
{
    *value = malloc(messageData->valueSize);
    uintptr_t buffer_address = (uintptr_t) *value;

    kv_handle->ctx->resources[kv_handle->ctx->count_send].value_mr = init_mr(
            kv_handle->ctx->pd,
            *value,
            messageData->valueSize,
            IBV_ACCESS_LOCAL_WRITE);
    messageData->wr_id = kv_handle->ctx->count_send;

    if(pp_post_rdma(kv_handle->ctx, messageData, IBV_WR_RDMA_READ, buffer_address, RDMA)){
        printf("error send\n");
        return 1;
    }
    struct ibv_wc wc;
    empty_cq(kv_handle, &wc, RDMA);

    ibv_dereg_mr(kv_handle->ctx->resources[kv_handle->ctx->count_send].value_mr);
    kv_handle->ctx->resources[kv_handle->ctx->count_send].value_mr = NULL;

    // Send FIN to server
    MessageData messageDataToServer;
    memset(&messageDataToServer, 0, sizeof(MessageData));
    messageDataToServer.value_address = messageData->value_address;
    messageDataToServer.fin = 1;
    messageDataToServer.wr_id = messageData->wr_id;
    memcpy(kv_handle->ctx->buf, &messageDataToServer, sizeof(MessageData));
    if (pp_post_send(kv_handle->ctx))
    {
        fprintf(stderr, "Failed to send FIN");
        return 1;
    }
    return 0;
}

int kv_set(void* obj, const char *key, const char *value)
{
    KvHandle* kv_handle = (KvHandle *) obj;

    // We add 1 because strlen does not cont the null byte \0
    size_t key_size = strlen(key) + 1;
    size_t value_size = strlen(value) + 1;

    struct ibv_wc wc;
    empty_cq(kv_handle, &wc, I_SEND_SET);
    if (key_size + value_size < MAX_EAGER_SIZE)
    {
        return eager_set(kv_handle, key, value, key_size, value_size);
    }

    return rendezvous_set(kv_handle, key, value, key_size, value_size);
}

int kv_get(void *obj, const char *key, char **value)
{
    size_t keySize = strlen(key) + 1;
    KvHandle* kv_handle = (KvHandle *) obj;
    char* buf_pointer = kv_handle->ctx->buf;
    buf_pointer = copy_message_data_to_buf(buf_pointer, keySize, 0, GET, EAGER,
                                           NULL, 0, 0);
    strcpy(buf_pointer, key);
    if (pp_post_send_get_client(kv_handle->ctx)){
        perror("Client failed to post send the request");
        return 1;
    }

    struct ibv_wc wc;
    empty_cq(kv_handle, &wc, CLIENT_RECEIVE);
    MessageData* messageData = malloc(sizeof(MessageData));
    char* data = get_message_data(kv_handle->ctx->buf, messageData);
    if (messageData->fin){
        Resource* resource = &kv_handle->ctx->resources[messageData->wr_id];
        free_and_reset_ptr(resource->value_buffer);
        free_and_reset_ptr(resource->buf);
        free_and_reset_mr(resource->value_mr);
        free_and_reset_mr(resource->mr);
    }

    switch (messageData->Protocol) {
        case EAGER:
            return eager_get(messageData, data, value);
        case RENDEZVOUS:
            return rendezvous_get(kv_handle, messageData, value);
        default:
            return 1;
    }
}

int kv_open(char *servername, void** obj)
{
    KvHandle* kv_handle = *((KvHandle **) obj);

    kv_handle->rem_dest = pp_client_exch_dest(servername, &kv_handle->my_dest);
    if (!kv_handle->rem_dest) {
        return 1;
    }

    inet_ntop(AF_INET6, &kv_handle->rem_dest->gid, kv_handle->gid,
              sizeof kv_handle->gid);

    if (pp_connect_ctx(kv_handle->ctx->qp, kv_handle->my_dest.psn,
                       kv_handle->rem_dest, kv_handle->gidx)) {
        return 1;
    }

    return 0;
}

void kv_release(char* value)
{
    free(value);
}

int kv_close(void *obj){
    KvHandle* kv_handle = (KvHandle *) obj;
    if (pp_close_ctx(kv_handle->ctx))
    {
        return  1;
    }
    return 0;
}
