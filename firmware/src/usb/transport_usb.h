/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * HS USB host transport: vendor specific interface with one bulk endpoint
 * pair for the control channel and one for the sample channel. See
 * firmware/src/usb/_doc/usb.md.
 */

#ifndef PHENIX_TRANSPORT_USB_H_
#define PHENIX_TRANSPORT_USB_H_

/** Register the USB function and enable the USB device controller. */
int transport_usb_start(void);

#endif /* PHENIX_TRANSPORT_USB_H_ */
