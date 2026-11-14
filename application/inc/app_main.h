/*
 * Application entry points, called from USER CODE sections of the CubeMX generated files:
 *   main.c          USER CODE 2                  -> app_early_init()
 *   eth.c           USER CODE ETH_Init 0         -> board_eth_phy_reset()
 *   eth.c           USER CODE MACADDRESS         -> board_get_mac_address()
 *   app_netxduo.c   USER CODE MX_NetXDuo_Init    -> app_start()
 */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include "nx_api.h"

/* Before the scheduler starts: safe pin states and the microsecond timer. */
void app_early_init(void);

/* From tx_application_define() context, after the NetX IP instance has been created. */
void app_start(NX_IP *ip, NX_PACKET_POOL *pool);

#endif /* APP_MAIN_H */
