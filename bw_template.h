#ifndef BW_TEMPLATE_H
#define BW_TEMPLATE_H

#include <stdbool.h>
#include "db.h"

#define IB_PORT (1)
#define PORT (12345)
#define MAX_EAGER_SIZE (4096)
#define MAX_BUF_SIZE (5120)
#define NUM_OF_CLIENTS (1)
#define MAX_RESOURCES (100)

extern const int RX_DEPTH;
extern const int TX_DEPTH;
extern const int SL;
extern const enum ibv_mtu MTU;
extern const enum ibv_mtu MTU;

// ------------------------------------------------------ Enums ------------------------------------------------------

enum Protocol {
    EAGER,
    RENDEZVOUS
};

enum OperationType {
    GET,
    SET
};

enum Wr_Id {
    I_SEND = MAX_RESOURCES,
    I_SEND_SET ,
    I_SEND_GET,
    CLIENT_RECEIVE,
    RDMA
};

// ------------------------------------------------------ Structs ------------------------------------------------------

// We use this struct when the client sends a message to the server
typedef struct MessageData
{
    enum Protocol Protocol;
    enum OperationType operationType;
    size_t keySize;
    size_t valueSize;
    int client_id;
    int wr_id;
    void* value_address;
    uint32_t rkey;
    int fin;
    int countSend;
} MessageData;

typedef struct Resource
{
    void* buf;
    struct ibv_mr* mr;
    void* value_buffer;
    struct ibv_mr* value_mr;
    void* key_buffer;
} Resource;

struct pingpong_context {
    int count_send;
    struct ibv_comp_channel *channel;
    struct ibv_pd           *pd;
    struct ibv_mr           *mr;
    struct ibv_cq           *cq;
    struct ibv_qp           *qp;
    void                    *buf;
    int                     size;
    int                     rx_depth;
    int                     routs;
    struct ibv_port_attr    portinfo;
    //todo resources
    Resource resources[MAX_RESOURCES];
    void* key;
};

struct pingpong_dest {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;
};


typedef struct KvHandle {
    int if_server;
    struct pingpong_context* ctx;
    struct pingpong_context* clients_ctx[NUM_OF_CLIENTS]; //if server
    int size;
    int gidx;
    char gid[33];
    struct ibv_device** dev_list;
    struct pingpong_dest* rem_dest;
    struct pingpong_dest my_dest;
    struct ibv_context *context;
    HashTable* hashTable;
} KvHandle;

//--------------------------------------------------------init--------------------------------------------------------
int init_dev_list(KvHandle* my_kv);
int init_context(KvHandle* my_kv);
struct ibv_cq* init_cq(KvHandle *kv_handle);
int init_pd(struct pingpong_context *ctx, KvHandle* kv_handle);
int init_buf(struct pingpong_context *ctx, KvHandle* kv_handle, int page_size);
struct ibv_mr* init_mr(struct ibv_pd* pd, void* buf, size_t size, enum ibv_access_flags access);
struct ibv_qp* init_qp(struct pingpong_context* ctx);
struct pingpong_context *pp_init_ctx(KvHandle* kv_handle, struct ibv_cq* cq);
int get_port_info(KvHandle* kv_handle);
int get_local_lid(KvHandle* kv_handle);
int check_gidX(KvHandle* kv_handle);
int pp_post_recv_client(struct pingpong_context *ctx, int n);
int init_client_post_recv(KvHandle* networkContext);
int init_network_context(KvHandle* kv_handle, const char* servername);


//------------------------------------------------------connect------------------------------------------------------
struct pingpong_dest *pp_client_exch_dest(const char *servername, const struct pingpong_dest *my_dest);
int pp_connect_ctx(struct ibv_qp* qp, int my_psn,struct pingpong_dest *dest, int sgid_idx);
void wire_gid_to_gid(const char *wgid, union ibv_gid *gid);
void gid_to_wire_gid(const union ibv_gid *gid, char wgid[]);


//set and get - helper function
int restore_post_receive_queue(struct pingpong_context* ctx);
int empty_cq(KvHandle* kv_handle, struct ibv_wc *wc, int stopCondition);
int pull_cq(KvHandle * kv_handle, struct ibv_wc *wc, int iters);
int pp_post_send(struct pingpong_context *ctx);


//-------------------------------------------------MessageData handlers-------------------------------------------------
char* copy_message_data_to_buf(char* buf_pointer, size_t keySize, size_t valueSize, enum OperationType operation,
        enum Protocol protocol, void* value_address, uint32_t rkey, int wr_id);
char* get_message_data(char* buffer, MessageData* messageData);


int pp_post_recv_server(struct pingpong_context *ctx, int n);
struct pingpong_dest *pp_server_exch_dest(struct ibv_qp* qp, const struct pingpong_dest *my_dest, int sgid_idx);
int pp_post_recv(struct pingpong_context *ctx, int resource_idx);
int pp_post_rdma(struct pingpong_context* ctx, MessageData* messageData, enum ibv_wr_opcode opcode,uintptr_t buffer_address, int wr_id);
int init_resource(Resource* resource, struct ibv_pd* pd, size_t size,enum ibv_access_flags access);

//----------------------------------------------------Release Funcs----------------------------------------------------

void free_and_reset_ptr(void* resource);
void free_and_reset_mr(struct ibv_mr* mr);
int pp_close_ctx(struct pingpong_context *ctx);
void release_kv_handler(KvHandle** kv_handle);

#endif /* BW_TEMPLATE_H */


