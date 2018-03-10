#include "jsvar.h"
#include "jswrap_spi_dma.h"
#include "esp_heap_alloc_caps.h"
#include "jsinteractive.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

/**
 * 
 * THIS IS THE MOST BASIC HARD CODED IMPLEMENTATION - HACK ALERT!!!!
 * 
 */

#define PIN_NUM_MISO 25
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5

#define PIN_NUM_DC 2
#define PIN_NUM_RST 22

#define NUM_PAGES 8
#define PAGE_SIZE 128


DRAM_ATTR static const uint8_t st_init_cmds[] = {
    0xAE,       // 0 disp off
    0xD5,       // 1 clk div
    0x50,       // 2 suggested ratio
    0xA8, 63,   // 3 set multiplex
    0xD3, 0x0,  // 5 display offset
    0x40,       // 7 start line
    0xAD, 0x8B, // 8 enable charge pump
    0xA1,       // 10 seg remap 1, pin header at the top
    0xC8,       // 11 comscandec, pin header at the top
    0xDA, 0x12, // 12 set compins
    0x81, 0x80, // 14 set contrast
    0xD9, 0x22, // 16 set precharge
    0xDB, 0x35, // 18 set vcom detect
    0xA6,       // 20 display normal (non-inverted)
    0xAF        // 21 disp on
};

void lcd_cmd(spi_device_handle_t spi, const uint8_t *data, int len)
{
  esp_err_t ret;
  spi_transaction_t t;
  if (len == 0)
    return;                           //no need to send anything
  memset(&t, 0, sizeof(t));           //Zero out the transaction
  t.length = len * 8;                 //Command is 8 bits
  t.tx_buffer = data;                 //The data is the cmd itself
  t.user = (void *)0;                 //D/C needs to be set to 0
  ret = spi_device_transmit(spi, &t); //Transmit!

  assert(ret == ESP_OK); //Should have had no issues.
}

void lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
  int dc = (int)t->user;
  gpio_set_level(PIN_NUM_DC, dc);
}

static spi_transaction_t cmd_trans[NUM_PAGES];
static spi_transaction_t data_trans[NUM_PAGES];

/*JSON{
  "type" : "class",
  "class" : "SPID"
}
This class provides basic SPI with DMA on ESP32
 */

/*JSON{
  "type" : "constructor",
  "class" : "SPID",
  "name" : "SPID",
  "generate" : "jswrap_spid_constructor",
  "params" : [
    ["clock","int32","Clock speed"]
  ]  
}
Create a software SPI port. This has limited functionality (no baud rate), but it can work on any pins.

Use `SPI.setup` to configure this port.
 */

spi_device_handle_t spi = NULL;
uint8_t *mem;

JsVar *jswrap_spid_constructor(int clock)
{
  if (spi)
  {
    jsiConsolePrintf("SPI already done init!\n");
    return;
  }

  memset(&cmd_trans, 0, sizeof(cmd_trans));
  memset(&data_trans, 0, sizeof(data_trans));

  mem = pvPortMallocCaps(1024, MALLOC_CAP_DMA);
  assert(mem);
  jsiConsolePrintf("Allocated and init'd some DMA mem - whoop!\n");

  gpio_set_direction(PIN_NUM_DC, GPIO_MODE_OUTPUT);
  gpio_set_direction(PIN_NUM_RST, GPIO_MODE_OUTPUT);

  //Reset the display
  gpio_set_level(PIN_NUM_RST, 0);
  for (int t = 0; t < 100000; t++)
    ;
  gpio_set_level(PIN_NUM_RST, 1);
  for (int t = 0; t < 100000; t++)
    ;

  esp_err_t ret;
  spi_bus_config_t buscfg = {
      .miso_io_num = PIN_NUM_MISO,
      .mosi_io_num = PIN_NUM_MOSI,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1};
  spi_device_interface_config_t devcfg = {
      .clock_speed_hz = clock * 1000 * 1000,  //Clock out at set speed
      .mode = 0,                              //SPI mode 0
      .spics_io_num = PIN_NUM_CS,             //CS pin
      .queue_size = 128,                      //We want to be able to queue 64 transactions at a time
      .pre_cb = lcd_spi_pre_transfer_callback //lcd_spi_pre_transfer_callback, //Specify pre-transfer callback to handle D/C line
  };
  //Initialize the SPI bus
  ret = spi_bus_initialize(HSPI_HOST, &buscfg, 1);
  assert(ret == ESP_OK);
  //Attach the LCD to the SPI bus
  ret = spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
  assert(ret == ESP_OK);
  //Initialize the LCD
  lcd_cmd(spi, st_init_cmds, sizeof(st_init_cmds));

  return jsvNewObject();
}

  /*JSON{
  "type" : "method",
  "class" : "SPID",
  "name" : "send",
  "generate" : "jswrap_spid_send",
  "params" : [
    ["from","JsVar","Data to send"]
  ]
}
Send array buffer to the device
*/

void jswrap_spid_send(JsVar *parent, JsVar *from)
{
  if (!jsvIsArrayBuffer(from))
  {
    jsExceptionHere(JSET_ERROR, "First argument should be array buffer");
    return;
  }

  size_t len;
  char *dataPtr = jsvGetDataPointer(from, &len);
  if (!dataPtr || len != 1024)
  {
    jsExceptionHere(JSET_ERROR, "Array buffer must be flat data area of 1024 bytes");
    return;
  }

  memcpy(mem, dataPtr, 1024);

  spi_transaction_t *rtrans;

  int buffer_offset = 0;
  int ret;
  uint8_t page = 0xB0;
  while (buffer_offset < 1024)
  {
    for (int n = 0; n < NUM_PAGES; n++)
    {
      cmd_trans[n].length = 3 * 8; //Command is 8 bits
      cmd_trans[n].tx_data[0] = page++;
      cmd_trans[n].tx_data[1] = 0x02;
      cmd_trans[n].tx_data[2] = 0x10;
      cmd_trans[n].flags = SPI_TRANS_USE_TXDATA;
      cmd_trans[n].user = 0; // cmd

      data_trans[n].length = PAGE_SIZE * 8;          //Command is 8 bits
      data_trans[n].tx_buffer = &mem[buffer_offset]; //The data is the cmd itself
      data_trans[n].user = 1;                        // data

      ret = spi_device_queue_trans(spi, &cmd_trans[n], portMAX_DELAY);
      assert(ret == ESP_OK);

      ret = spi_device_queue_trans(spi, &data_trans[n], portMAX_DELAY);
      assert(ret == ESP_OK);

      buffer_offset += PAGE_SIZE;
    }
    for (int n = 0; n < NUM_PAGES; n++)
    {
      spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
      spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
    }
  }
}