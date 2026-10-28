/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Minimal HTTP/1.1 server on port 80 for the network configuration.
 * One client at a time, no authentication (LAN use).
 *
 *   GET  /             status and configuration form
 *   POST /config       save the configuration and reboot
 *   GET  /status.json  status as JSON
 */

#ifndef PHENIX_WEB_UI_H_
#define PHENIX_WEB_UI_H_

int web_ui_start(void);

#endif /* PHENIX_WEB_UI_H_ */
