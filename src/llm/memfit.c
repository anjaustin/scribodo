/* Will the local model fit in memory? See memfit.h. */
#define _DARWIN_C_SOURCE
#include "memfit.h"

#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>

long long sysmem_total(void)
{
    long long mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0) return 0;
    return mem;
}

long long sysmem_available(void)
{
    const char *env = getenv("SCRIBODO_MEM_AVAILABLE_MB");
    if (env && env[0]) return atoll(env) * 1024LL * 1024;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &n) != KERN_SUCCESS) return 0;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) return 0;
    /* Purgeable pages are already counted among active and inactive. */
    unsigned long long pages = (unsigned long long)vm.free_count + vm.inactive_count + vm.speculative_count;
    return (long long)(pages * page);
}

MemFit memfit_context(long long avail, long long other, const LlmServerConfig *cfg, int wanted, int min, int step)
{
    MemFit f = { 0 };
    int slots = cfg->slots > 0 ? cfg->slots : 1;
    if (step <= 0) step = 1;
    if (min > wanted) min = wanted;
    f.avail = avail;
    f.need_wanted = llm_server_mem_need(cfg, wanted * slots);
    long long budget = avail - other - MEMFIT_HEADROOM;
    for (int c = wanted;; c = c - step > min ? c - step : min) {
        long long need = llm_server_mem_need(cfg, c * slots);
        if (need <= budget) {
            f.slot_ctx = c;
            f.need = need;
            f.reduced = c < wanted;
            return f;
        }
        if (c <= min) break;
    }
    f.slot_ctx = min;
    f.need = llm_server_mem_need(cfg, min * slots);
    f.reduced = min < wanted;
    f.short_even_min = true;
    return f;
}

void memfit_gb(long long bytes, char *out, int outlen)
{
    snprintf(out, (size_t)outlen, "%.1f GB", (double)bytes / 1e9);
}
