#include <stdlib.h>
#include <stdio.h>
#include "host_zephyr_backend.h"
#include "iio-private.h"

int main()
{

    struct iio_context_params params = {0};
    const char *args = "/dev/ttyACM0,115200,8n1n";
    struct iio_context *ctx = zephyr_create_context_from_args(&params, args);

    if (ctx){
        printf("Context created successfully\n");
        printf("About to check name field...\n");
        fflush(stdout);
        
        if (ctx->name)
            printf("name = %s\n", ctx->name);
        else
            printf("name = (null)\n");
            
        printf("About to check description field...\n");
        fflush(stdout);
            
        if (ctx->description)
            printf("description = %s\n", ctx->description);
        else
            printf("description = (null)\n");
    } else {
        printf("Context creation failed\n");
    }
    printf("HERE\n");
}