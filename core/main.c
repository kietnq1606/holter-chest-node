#include "app_api.h"
int main(void)
{
    int err = system_init();
    if (err != 0) {
        return err;
    }
    system_run();
    return 0;
}