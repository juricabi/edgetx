/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "lcd_driver.h"

#include "stm32_gpio.h"
#include "stm32_spi.h"

#include "delays_driver.h"
#include "hal/gpio.h"
#include "lcd.h"

#include "hal.h"
#include "board.h"

#if !defined(BOOT) && !defined(LCD_SYNC_TRANSFER)
// StickTime build: frames go to the display in the background, as on the HelloRadioSky V12
// (boards/helloradio-h750/lcd_driver.cpp, the same SPI1 and DMA stream). The flush only
// queues the frame; the FMARK (tearing effect) interrupt starts it at the next vertical blank
// and the SPI end-of-transfer interrupt chains the DMA chunks and reports the frame done. The
// CPU meanwhile runs the next frame (a Lua tool's run(), for example) instead of waiting for
// the blank (up to a whole display refresh) and then for 150 KB of SPI at 50 MHz (25 ms).
#define LCD_BACKGROUND_TRANSFER
#include "stm32_dma.h"
#include "stm32_hal_ll.h"
#include "timers_driver.h"
#endif

#define LCD_NRST_HIGH()               gpio_set(LCD_NRST)
#define LCD_NRST_LOW()                gpio_clear(LCD_NRST)
#define LCD_COMMAND_MODE()            gpio_clear(LCD_SPI_RS)
#define LCD_DATA_MODE()               gpio_set(LCD_SPI_RS)

#define CASET 0x2A
#define RASET 0x2B
#define RAMWR 0x2C

static const stm32_spi_t lcdSpi = {
  .SPIx = LCD_SPI,
  .SCK = LCD_SPI_CLK,
  .MISO = LCD_SPI_MISO,
  .MOSI = LCD_SPI_MOSI,
  .CS = LCD_SPI_CS,
  .DMA = LCD_SPI_DMA,
  .txDMA_PeriphRequest = LCD_SPI_TX_DMA,
  .rxDMA_PeriphRequest = LCD_SPI_RX_DMA,
  .txDMA_Stream = LCD_SPI_TX_DMA_STREAM,
  .rxDMA_Stream = LCD_SPI_RX_DMA_STREAM,
};

static void* initialFrameBuffer = nullptr;

static void write_start_end(uint8_t cmd, uint16_t start, uint16_t end)
{
  // big endian format
  uint8_t buf[4] = {
      (uint8_t)(start >> 8),
      (uint8_t)(start & 0xFF),
      (uint8_t)(end >> 8),
      (uint8_t)(end & 0xFF),
  };

  stm32_spi_select(&lcdSpi);

  LCD_COMMAND_MODE();
  stm32_spi_transfer_byte(&lcdSpi, cmd);

  LCD_DATA_MODE();
  stm32_spi_transfer_bytes(&lcdSpi, (uint8_t *)buf, nullptr, sizeof(buf));

  stm32_spi_unselect(&lcdSpi);
}

static inline void set_column_addr(uint16_t xs, uint16_t xe)
{
  write_start_end(CASET, xs, xe);
}

static inline void set_row_addr(uint16_t ys, uint16_t ye)
{
  write_start_end(RASET, ys, ye);
}

static void memory_write(const uint16_t* data, uint32_t length)
{
  stm32_spi_select(&lcdSpi);

  LCD_COMMAND_MODE();
  stm32_spi_transfer_byte(&lcdSpi, RAMWR);

  LCD_DATA_MODE();
  stm32_spi_set_data_width(&lcdSpi, LL_SPI_DATAWIDTH_16BIT);

  SCB_CleanDCache_by_Addr((void *)data, length * sizeof(uint16_t));
  stm32_spi_dma_transmit_words(&lcdSpi, data, length);

  stm32_spi_unselect(&lcdSpi);
  stm32_spi_set_data_width(&lcdSpi, LL_SPI_DATAWIDTH_8BIT);
}

#if defined(LCD_BACKGROUND_TRANSFER)

#if !defined(STM32H7) || defined(STM32H7RS)
#error "the background LCD transfer is written for the STM32H7 SPI and DMA"
#endif
// LCD_FMARK is PB7. Its EXTI line 7 belongs to the rotary encoder (on port A), so the FMARK
// edge comes from TIM4 instead, which the PA01 leaves free: PB7 is TIM4_CH2 (AF2), captured on
// the rising edge, with the capture interrupt. A second TIM4_IRQHandler would not link.
#define LCD_FMARK_TIMER       TIM4
#define LCD_IRQ_PRIORITY      11                  // as low as USB: nothing waits on it
#define LCD_XFER_CHUNK        (LCD_W * LCD_H / 2) // words per DMA transfer (counters are 16 bit)
#define LCD_VSYNC_TIMEOUT_US  40000               // no FMARK: send without it
#define LCD_XFER_TIMEOUT_US   100000              // a frame takes 25 ms: give up on it

enum { LCD_IDLE, LCD_WAIT_VSYNC, LCD_SENDING };
static volatile uint8_t lcdState = LCD_IDLE;
static const uint16_t* lcdXferData;
static uint32_t lcdXferLeft;
static volatile uint32_t lcdStateSince;

// (the PA01 uses the V12's stream: LCD_SPI_TX_DMA_STREAM is DMA1 stream 4, LCD_SPI is SPI1)
static_assert(LCD_SPI_TX_DMA_STREAM == LL_DMA_STREAM_4, "LCD TX DMA is not stream 4");

// the FMARK capture interrupt, on while a frame waits for the vertical blank
static void lcdFmarkArm()
{
  LL_TIM_ClearFlag_CC2(LCD_FMARK_TIMER);
  LL_TIM_EnableIT_CC2(LCD_FMARK_TIMER);
}

static void lcdFmarkDisarm()
{
  LL_TIM_DisableIT_CC2(LCD_FMARK_TIMER);
  LL_TIM_ClearFlag_CC2(LCD_FMARK_TIMER);
}

static void lcdDmaClearFlags()
{
  // a stream must not be enabled while any of its flags is set
  LL_DMA_ClearFlag_TC4(LCD_SPI_DMA);
  LL_DMA_ClearFlag_HT4(LCD_SPI_DMA);
  LL_DMA_ClearFlag_TE4(LCD_SPI_DMA);
  LL_DMA_ClearFlag_DME4(LCD_SPI_DMA);
  LL_DMA_ClearFlag_FE4(LCD_SPI_DMA);
}

// next chunk of the frame: DMA into the SPI, end-of-transfer interrupt when it is out
static void lcdChunkStart()
{
  uint32_t n = lcdXferLeft > LCD_XFER_CHUNK ? LCD_XFER_CHUNK : lcdXferLeft;
  lcdDmaClearFlags();
  LL_DMA_SetMemoryAddress(LCD_SPI_DMA, LCD_SPI_TX_DMA_STREAM, (uintptr_t)lcdXferData);
  LL_DMA_SetDataLength(LCD_SPI_DMA, LCD_SPI_TX_DMA_STREAM, n);
  LL_DMA_EnableStream(LCD_SPI_DMA, LCD_SPI_TX_DMA_STREAM);
  lcdXferData += n;
  lcdXferLeft -= n;

  LL_SPI_SetTransferSize(LCD_SPI, n);
  LL_SPI_EnableDMAReq_TX(LCD_SPI);
  LL_SPI_Enable(LCD_SPI);
  LL_SPI_EnableIT_EOT(LCD_SPI);
  LL_SPI_StartMasterTransfer(LCD_SPI);
}

// the frame starts: memory write command, then the pixels by DMA (window set by the flush)
static void lcdStartFrame()
{
  lcdState = LCD_SENDING;
  lcdStateSince = timersGetUsTick();

  stm32_spi_select(&lcdSpi);
  LCD_COMMAND_MODE();
  stm32_spi_transfer_byte(&lcdSpi, RAMWR);
  LCD_DATA_MODE();
  stm32_spi_set_data_width(&lcdSpi, LL_SPI_DATAWIDTH_16BIT);
  LL_DMA_SetPeriphSize(LCD_SPI_DMA, LCD_SPI_TX_DMA_STREAM, LL_DMA_PDATAALIGN_HALFWORD);
  LL_DMA_SetMemorySize(LCD_SPI_DMA, LCD_SPI_TX_DMA_STREAM, LL_DMA_MDATAALIGN_HALFWORD);
  lcdChunkStart();
}

static void lcdEndFrame()
{
  stm32_spi_unselect(&lcdSpi);
  stm32_spi_set_data_width(&lcdSpi, LL_SPI_DATAWIDTH_8BIT);
  lcdState = LCD_IDLE;
  lcdFlushed();
}

// end of a chunk (SPI end-of-transfer): next chunk, or the frame is out
static void lcdChunkDone()
{
  LL_SPI_DisableIT_EOT(LCD_SPI);
  LL_SPI_ClearFlag_EOT(LCD_SPI);
  LL_SPI_ClearFlag_TXTF(LCD_SPI);
  LL_SPI_Disable(LCD_SPI);
  CLEAR_BIT(LCD_SPI->CFG1, SPI_CFG1_TXDMAEN | SPI_CFG1_RXDMAEN);
  if (lcdXferLeft > 0) {
    lcdChunkStart();
  } else {
    lcdEndFrame();
  }
}

// a transfer that never ends (it should not happen): stop it and drop the frame
static void lcdAbortFrame()
{
  LL_SPI_DisableIT_EOT(LCD_SPI);
  LL_DMA_DisableStream(LCD_SPI_DMA, LCD_SPI_TX_DMA_STREAM);
  while (LL_DMA_IsEnabledStream(LCD_SPI_DMA, LCD_SPI_TX_DMA_STREAM));
  LL_SPI_Disable(LCD_SPI);
  CLEAR_BIT(LCD_SPI->CFG1, SPI_CFG1_TXDMAEN | SPI_CFG1_RXDMAEN);
  LL_SPI_ClearFlag_EOT(LCD_SPI);
  LL_SPI_ClearFlag_TXTF(LCD_SPI);
  lcdDmaClearFlags();
  lcdXferLeft = 0;
  lcdEndFrame();
}

extern "C" void SPI1_IRQHandler(void)
{
  if (LL_SPI_IsEnabledIT_EOT(LCD_SPI) && LL_SPI_IsActiveFlag_EOT(LCD_SPI)) lcdChunkDone();
}

extern "C" void TIM4_IRQHandler(void)
{
  if (LL_TIM_IsEnabledIT_CC2(LCD_FMARK_TIMER) && LL_TIM_IsActiveFlag_CC2(LCD_FMARK_TIMER)) {
    lcdFmarkDisarm();
    if (lcdState == LCD_WAIT_VSYNC) lcdStartFrame();
  }
}

// Called while something waits for the transfer (LVGL before it draws the next frame, or
// the next flush). The interrupts normally do everything; this covers the cases where they
// cannot run (interrupts masked) and a display that stops sending FMARK.
static void lcdService()
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  uint32_t age = timersGetUsTick() - lcdStateSince;
  if (lcdState == LCD_WAIT_VSYNC) {
    if (gpio_read(LCD_FMARK) || age > LCD_VSYNC_TIMEOUT_US) {
      lcdFmarkDisarm();
      lcdStartFrame();
    }
  } else if (lcdState == LCD_SENDING) {
    if (LL_SPI_IsEnabledIT_EOT(LCD_SPI) && LL_SPI_IsActiveFlag_EOT(LCD_SPI)) {
      lcdChunkDone();
    } else if (age > LCD_XFER_TIMEOUT_US) {
      lcdAbortFrame();
    }
  }
  __set_PRIMASK(primask);
}

static void lcdWaitCb(lv_disp_drv_t* disp_drv)
{
  (void)disp_drv;
  lcdService();
}

static void startLcdRefresh(lv_disp_drv_t *disp_drv, uint16_t *buffer,
                            const rect_t &copy_area)
{
  (void)disp_drv;

  // LVGL only draws a frame once the previous one is out, so this normally returns at once
  while (lcdState != LCD_IDLE) lcdService();

  coord_t x1 = copy_area.x;
  coord_t x2 = x1 + copy_area.w - 1;
  set_column_addr(x1, x2);

  coord_t y1 = copy_area.y;
  coord_t y2 = y1 + copy_area.h - 1;
  set_row_addr(y1, y2);

  uint32_t length = copy_area.w * copy_area.h;
  SCB_CleanDCache_by_Addr((void *)buffer, length * sizeof(uint16_t));
  lcdXferData = buffer;
  lcdXferLeft = length;

  // send at the next vertical blank (FMARK high), like the original busy wait did
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  lcdState = LCD_WAIT_VSYNC;
  lcdStateSince = timersGetUsTick();
  lcdFmarkArm();
  if (gpio_read(LCD_FMARK)) {
    // already in the blank
    lcdFmarkDisarm();
    lcdStartFrame();
  }
  __set_PRIMASK(primask);
}

static void lcdBackgroundTransferInit()
{
  // FMARK rising edge (start of the vertical blank): TIM4 channel 2 captures it on PB7, its
  // interrupt masked until a frame waits for it. gpio_read() still sees the pin in AF mode.
  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM4);
  gpio_init_af(LCD_FMARK, GPIO_AF2, GPIO_PIN_SPEED_HIGH);
  LL_TIM_DisableCounter(LCD_FMARK_TIMER);
  LL_TIM_SetPrescaler(LCD_FMARK_TIMER, 0);
  LL_TIM_SetAutoReload(LCD_FMARK_TIMER, 0xFFFF);
  LL_TIM_IC_SetActiveInput(LCD_FMARK_TIMER, LL_TIM_CHANNEL_CH2, LL_TIM_ACTIVEINPUT_DIRECTTI);
  LL_TIM_IC_SetPrescaler(LCD_FMARK_TIMER, LL_TIM_CHANNEL_CH2, LL_TIM_ICPSC_DIV1);
  LL_TIM_IC_SetFilter(LCD_FMARK_TIMER, LL_TIM_CHANNEL_CH2, LL_TIM_IC_FILTER_FDIV1);
  LL_TIM_IC_SetPolarity(LCD_FMARK_TIMER, LL_TIM_CHANNEL_CH2, LL_TIM_IC_POLARITY_RISING);
  LL_TIM_CC_EnableChannel(LCD_FMARK_TIMER, LL_TIM_CHANNEL_CH2);
  lcdFmarkDisarm();
  LL_TIM_EnableCounter(LCD_FMARK_TIMER);
  NVIC_SetPriority(TIM4_IRQn, LCD_IRQ_PRIORITY);
  NVIC_EnableIRQ(TIM4_IRQn);

  NVIC_SetPriority(SPI1_IRQn, LCD_IRQ_PRIORITY);
  NVIC_EnableIRQ(SPI1_IRQn);

  lcdSetWaitCb(lcdWaitCb);
}

#else // LCD_BACKGROUND_TRANSFER

static void startLcdRefresh(lv_disp_drv_t *disp_drv, uint16_t *buffer,
                            const rect_t &copy_area)
{  
  (void)disp_drv;

  // TODO: replace through some smarter mechanism without busy wait
  while (!gpio_read(LCD_FMARK));

  coord_t x1 = copy_area.x;
  coord_t x2 = x1 + copy_area.w - 1;
  set_column_addr(x1, x2);

  coord_t y1 = copy_area.y;
  coord_t y2 = y1 + copy_area.h - 1;
  set_row_addr(y1, y2);

  memory_write(buffer, copy_area.w * copy_area.h);

  lcdFlushed();
}

#endif // LCD_BACKGROUND_TRANSFER

lcdSpiInitFucPtr lcdInitFunction;
lcdSpiInitFucPtr lcdOffFunction;
lcdSpiInitFucPtr lcdOnFunction;
uint32_t lcdPixelClock;

static void lcdSpiConfig(void)
{
  stm32_spi_init(&lcdSpi, LL_SPI_DATAWIDTH_8BIT);
  stm32_spi_set_max_baudrate(&lcdSpi, LCD_SPI_BAUD);

  gpio_init(LCD_FMARK, GPIO_IN, GPIO_PIN_SPEED_HIGH);
  gpio_init(LCD_NRST, GPIO_OUT, GPIO_PIN_SPEED_HIGH);
  gpio_init(LCD_SPI_RS, GPIO_OUT, GPIO_PIN_SPEED_HIGH);
}

void lcdDelay() {
  delay_01us(1);
}

static void lcdReset() {
  LCD_NRST_HIGH();
  delay_ms(1);

  LCD_NRST_LOW(); // RESET();
  delay_ms(100);

  LCD_NRST_HIGH();
  delay_ms(120);
}

static void lcdWriteCommand(uint8_t cmd)
{
  LCD_COMMAND_MODE();
  stm32_spi_select(&lcdSpi);
  stm32_spi_transfer_byte(&lcdSpi, cmd);
  stm32_spi_unselect(&lcdSpi);
}

// static void lcdWriteReg(uint8_t reg, uint8_t data)
// {
//   LCD_COMMAND_MODE();
//   stm32_spi_select(&lcdSpi);
//   stm32_spi_transfer_byte(&lcdSpi, reg);
//   LCD_DATA_MODE();
//   stm32_spi_transfer_byte(&lcdSpi, data);
//   stm32_spi_unselect(&lcdSpi);
// }

static void lcdWriteData(uint8_t data)
{
  LCD_DATA_MODE();
  stm32_spi_select(&lcdSpi);
  stm32_spi_transfer_byte(&lcdSpi, data);
  stm32_spi_unselect(&lcdSpi);
}

// static uint8_t lcdReadReg(uint8_t reg)
// {
//   uint8_t data = 0;
//   LCD_COMMAND_MODE();
//   stm32_spi_select(&lcdSpi);
//   stm32_spi_transfer_byte(&lcdSpi, reg);
//   LCD_DATA_MODE();
//   data = stm32_spi_transfer_byte(&lcdSpi, 0xFF);
//   stm32_spi_unselect(&lcdSpi);
//   return data;
// }

extern "C" void lcdSetInitalFrameBuffer(void *fbAddress)
{
  initialFrameBuffer = fbAddress;
}

static void lcdSetOn(void) { lcdWriteCommand(0x29); }
static void lcdSetOff(void) { lcdWriteCommand(0x28); }

extern "C" void lcdInit()
{
  // Configure the LCD SPI + RESET pins
  lcdSpiConfig();
  stm32_spi_unselect(&lcdSpi);

  // hard reset
  lcdReset();

  // Init command start
  lcdWriteCommand( 0xFE );
  lcdWriteCommand( 0xEF );

  // Display orientation
  lcdWriteCommand( 0x36 );
  lcdWriteData( 0xE8 );

  // Color mode
  lcdWriteCommand( 0x3A );
  lcdWriteData( 0x05 );  // 16-bit color

  // Display control
  lcdWriteCommand( 0x86 );
  lcdWriteData( 0x98 );
  lcdWriteCommand( 0x89 );
  lcdWriteData( 0x13 );
  lcdWriteCommand( 0x8B );
  lcdWriteData( 0x80 );
  lcdWriteCommand( 0x8D );
  lcdWriteData( 0x33 );
  lcdWriteCommand( 0x8E );
  lcdWriteData( 0x0F );

  lcdWriteCommand( 0xEC );
  lcdWriteData( 0x13 );
  lcdWriteData( 0x02 );
  lcdWriteData( 0x88 );

  lcdWriteCommand( 0xED );
  lcdWriteData( 0x18 );
  lcdWriteData( 0x08 );

  lcdWriteCommand( 0xE8 );
  lcdWriteData( 0x12 );
  lcdWriteData( 0x00 );

  // Init command ends
  lcdWriteCommand( 0xFF );
  lcdWriteCommand( 0x62 );

  // Display control
  lcdWriteCommand( 0x99 );
  lcdWriteData( 0x3E );
  lcdWriteCommand( 0x9D );
  lcdWriteData( 0x4B );
  lcdWriteCommand( 0x98 );
  lcdWriteData( 0x3E );
  lcdWriteCommand( 0x9C );
  lcdWriteData( 0x4B );
  lcdWriteCommand( 0xC3 );
  lcdWriteData( 0x27 );
  lcdWriteCommand( 0xC4 );
  lcdWriteData( 0x18 );
  lcdWriteCommand( 0xC9 );
  lcdWriteData( 0x0A );

  // Gamma Control
  lcdWriteCommand( 0xF0 );
  lcdWriteData( 0x85 );
  lcdWriteData( 0x0A );
  lcdWriteData( 0x09 );
  lcdWriteData( 0x08 );
  lcdWriteData( 0x04 );
  lcdWriteData( 0x30 );
  lcdWriteCommand( 0xF1);
  lcdWriteData( 0x47 );
  lcdWriteData( 0x5B );
  lcdWriteData( 0xB0 );
  lcdWriteData( 0x3A );
  lcdWriteData( 0x3E );
  lcdWriteData( 0x7F );
  lcdWriteCommand( 0xF2 );
  lcdWriteData( 0x85 );
  lcdWriteData( 0x0A );
  lcdWriteData( 0x09 );
  lcdWriteData( 0x08 );
  lcdWriteData( 0x04 );
  lcdWriteData( 0x30 );
  lcdWriteCommand( 0xF3 );
  lcdWriteData( 0x47 );
  lcdWriteData( 0x5B );
  lcdWriteData( 0xB0 );
  lcdWriteData( 0x3A );
  lcdWriteData( 0x3F );
  lcdWriteData( 0x7F );

  // Display size
  lcdWriteCommand( 0x2A );
  lcdWriteData( 0x00 );
  lcdWriteData( 0x00 );
  lcdWriteData( 0x01 );
  lcdWriteData( 0x40 );
  lcdWriteCommand( 0x2B );
  lcdWriteData( 0x00 );
  lcdWriteData( 0x00 );
  lcdWriteData( 0x00 );
  lcdWriteData( 0xF0 );

  // No, invert color
  lcdWriteCommand( 0x20 );

  // Tear on
  lcdWriteCommand( 0x35 );
  lcdWriteData( 0x00 );

  // Tearing scan line
  lcdWriteCommand( 0x44 );
  lcdWriteData( 0x00 );
  lcdWriteData( 0x0A );

  // Exit sleep
  lcdWriteCommand( 0x11 );

  // Init LCD RAM
  memory_write((const uint16_t*)initialFrameBuffer, LCD_W * LCD_H);

  lcdSetOn();

  lcdOnFunction = lcdSetOn;
  lcdOffFunction = lcdSetOff;

#if defined(LCD_BACKGROUND_TRANSFER)
  lcdBackgroundTransferInit();
#endif
  lcdSetFlushCb(startLcdRefresh);
}
