/* A stand-in Unix half for the PE half's loading test (bridge/pe/test/run.sh):
 * VB_VERSION_CALL answers VB_VERSION with no features, every other call is
 * refused. The real Unix half is bridge/unix/ (WS1). */
#include <stdint.h>
#include <unistd.h>
#include "abi.h"

typedef int32_t NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *);

#define STATUS_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)

static NTSTATUS version(void *args)
{
    struct vb_version *v = args;
    v->version = VB_VERSION;
    v->pad = (uint32_t)getpid(); /* lets the test show it ran on the Linux side */
    v->features = 0;
    return 0;
}

static NTSTATUS refused(void *args)
{
    (void)args;
    return STATUS_NOT_IMPLEMENTED;
}

__attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[VB_CALLS] = {
    version, refused, refused, refused, refused, refused,
    refused, refused, refused, refused, refused,
};
