#include "jsvar.h"

JsVar *jswrap_spid_constructor(int clock);
void jswrap_spid_test(JsVar *parent, int cmd, int queue);
void jswrap_spid_send(JsVar *parent, JsVar *from);
// void jswrap_spi_setup(JsVar *parent, JsVar *options);
// void jswrap_spi_write(JsVar *parent, JsVar *args);
