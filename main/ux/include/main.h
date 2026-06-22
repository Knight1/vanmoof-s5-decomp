/*
 * main.h — UX service program entry.
 *
 * Reconstructed from program "ux" (AArch64, image base 0x100000),
 * devices/main/ux/src/main.cpp. main() parses CLI args, builds the
 * "ux-service" application framework, then constructs/runs/destroys the
 * UXService. The framework (arg parser + message-bus/MQTT handles) is vendor;
 * modelled as opaque handles + externs.
 */
#ifndef UX_MAIN_H
#define UX_MAIN_H

#include "ux_common.h"
#include "ux_service.h"

/* Opaque app-framework + CLI-option-set objects (the i.MX8 common::App glue). */
typedef struct ux_app  { unsigned char _opaque[512]; } ux_app;
typedef struct ux_opts { unsigned char _opaque[256]; } ux_opts;
typedef struct ux_cfg  { unsigned char _opaque[512]; } ux_cfg;
void ux_service_ctor(ux_service *svc, void *bus, void *a, void *b, void *c);
void ux_service_run (ux_service *svc);
void ux_service_dtor(ux_service *svc);
void ux_cfg_init(ux_cfg *c, int v);
void ux_app_init(ux_app *a);

/* App-framework lifecycle (vendor common::App). */
void  ux_cfg_init   (ux_cfg *cfg, int dummy);                 /* FUN_00117b00 */
void  ux_app_init   (ux_app *app);                            /* FUN_00183130 */
void  ux_app_parse_args(ux_app *app, int argc, void *argv,
                        void *name, void *opt_vec, void *help,
                        void *cb);                            /* FUN_00183db0 */
void  ux_app_build  (ux_opts *opts, ux_app *app, const char *name); /* FUN_001835f0 */
void *ux_app_bus_a  (ux_opts *opts);                          /* FUN_00182ef0 */
void *ux_app_bus_b  (ux_opts *opts);                          /* FUN_00182f00 */
void *ux_app_cfg    (ux_opts *opts);                          /* FUN_00182ee0 */
void  ux_app_run    (ux_opts *opts);                          /* FUN_00182eb0 (blocks) */
void  ux_app_stop   (ux_opts *opts);                          /* FUN_00182c80 */
void  ux_app_destroy(ux_app *app);                            /* FUN_00182bf0 */
void  ux_cfg_destroy(ux_cfg *cfg);                            /* FUN_00117630 */

int main(int argc, char **argv);

#endif /* UX_MAIN_H */
