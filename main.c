#include <stdio.h>
#include <infiniband/verbs.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include "Client.h"
#include "Server.h"
#include "bw_template.h"

int main(int argc, char *argv[])
{
        char* servername = NULL;
        srand48(getpid() * time(NULL));

        if (optind == argc - 1)
            servername = strdup(argv[optind]);
        else if (optind < argc) {
            return 1;
        }

        KvHandle handler;
        memset(&handler, 0, sizeof(KvHandle));
        KvHandle* kv_handle = &handler;

        if (init_network_context(kv_handle, servername) != 0)
        {
            fprintf(stderr, "failed to initialize KvHandle");
            return 1;
        }

        // Client
        if (servername)
        {
            if(init_client_post_recv(kv_handle)){
                return 1;
            }
            // Initialize Work Request sent.
            kv_handle->ctx->count_send = 0;

            if (kv_open(servername, (void **) &kv_handle) != 0)
            {
                fprintf(stderr, "Client couldn't connect\n");
                return 1;
            }

            const char *key = "key";
            const char *value = "value";

            if (kv_set((void *)kv_handle, key, value) != 0)
            {
                return 1;
            }

            char *received_value = NULL;
            if (kv_get((void *)kv_handle, key, &received_value) != 0)
            {
                fprintf(stderr, "Client failed to preform get\n");
                return 1;
            }

            kv_release(received_value);
        }

        // Server
        else
        {
            start_server((void *) kv_handle);
            for (int i = 0; i < NUM_OF_CLIENTS; ++i) {
                pp_close_ctx(kv_handle->clients_ctx[i]);
            }
            release_db(kv_handle->hashTable);
        }
        kv_close(kv_handle);
        release_kv_handler((KvHandle **) &kv_handle);
        return 0;
    }
