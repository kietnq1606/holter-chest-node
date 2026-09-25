#include "services/ble_service/nus.h"

int main(void){

    static struct bt_nus_cb nus_callbacks = {
    .received = rx_received,
    .sent = NULL,
    .send_enabled = NULL,
    };

    bt_init(&nus_callbacks
    while(1){
        printf("Hello, Thight Node!\n\r");
        // Main loop code here
    }
}