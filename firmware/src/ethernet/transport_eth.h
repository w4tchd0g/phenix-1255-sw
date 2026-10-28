/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Ethernet host transport: TCP control channel and UDP sample channel.
 */

#ifndef PHENIX_TRANSPORT_ETH_H_
#define PHENIX_TRANSPORT_ETH_H_

#include <stdint.h>

/** Start the TCP control server and the UDP sample socket. */
int transport_eth_start(uint16_t ctrl_port, uint16_t sample_port);

#endif /* PHENIX_TRANSPORT_ETH_H_ */
