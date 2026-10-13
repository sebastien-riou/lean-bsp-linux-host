/* linux-host glue: the contract surface of main(), and the BSP's weak default hooks. */
#include <stdlib.h>
#include "bsp_api.h"
#include "bsp_linux.h"

__attribute__((weak)) void app_init(void) {}

/* An alias rather than a plain weak definition, so bsp_start_tick() can tell whether the
 * application overrides app_tick and skip the tick thread when it does not. */
void lean_bsp_default_app_tick(uint32_t tick_ms) { (void)tick_ms; }
void app_tick(uint32_t tick_ms) __attribute__((weak, alias("lean_bsp_default_app_tick")));

__attribute__((weak)) void app_on_fault(uint32_t reason, uintptr_t pc) {
    (void)reason;
    (void)pc;
}

__attribute__((weak)) void app_on_exit(int code) { exit(code); }

int main(int argc, char *argv[]) {
    bsp_init(argc, argv);
    app_init();
    bsp_start_tick();
    int rc = app_main();
    app_on_exit(rc);
    return rc;
}
