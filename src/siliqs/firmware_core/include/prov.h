/* prov.h — provisioning command processor over a hal_link_t (BLE or USB-C).
   Transport-agnostic: same commands on both. */
#pragma once
#include "config.h"
#include "hal/hal_link.h"

/* Service one provisioning session: read newline commands from link, mutate cfg,
   write responses, until the link disconnects. Commands:
     cfg <hex>   – DOWNLOAD a binary config blob (hex-decoded, CRC-checked, stored).
                   This is the normal config path — no text parsing of fields.
     get         – short summary (debug)
     export      – read-plan dump: device/baud/interval + polls (debug)
     poll        – run the poll list once over RS485, print the raw payload (debug)
     bus         – raw RS485 dump of poll[0]'s transaction (debug)
     reset       – factory reset
   Per-poll type/scale are cloud metadata and are NOT in the device blob.
*/
void prov_service(sq_config_t *cfg, hal_link_t *link);
