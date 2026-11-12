/**
  ******************************************************************************
  * @file    video_forward.c
  * @brief   0x0310 video frame forwarder: USB CDC -> VT03 UART.
  ******************************************************************************
  */
#include "video_forward.h"
#include "usbd_cdc_if.h"
#include <string.h>

/* ========================== configuration ================================= */

/* VT03 UART = USART6 (PG14) = 板上丝印 "UART1" 的 3pin 图传口。
 *
 * 开发板 C 型用户手册原文：「开发板的外壳丝印（UART1 与 UART2）与 STM32 的实际串口配置
 * 并不对应，外壳丝印 UART1 对应 STM32 的 UART6，外壳丝印 UART2 对应 STM32 的 UART1。」
 * 所以：3pin（丝印 UART1）= USART6 = huart6 ← 视频走这个；
 *       4pin（丝印 UART2）= USART1 = huart1。
 * 电控说「UART1 是 4pin」是按 STM32 侧命名，指的是 4pin 那个口，两者不矛盾。
 * 别再把这里改成 huart1。
 */
extern UART_HandleTypeDef huart6;
#define VIDEO_VT03_HANDLE      (&huart6)

/* DMA runs independently of RTOS preemption. This is a fault watchdog,
 * not a blocking per-packet allowance. No loop waits for DMA or UART flags. */
#define VIDEO_DMA_WATCHDOG_MS       100u
#define VIDEO_ABORT_WATCHDOG_MS     100u
#define VIDEO_PENDING_MAX_AGE_MS    200u
/* DWT CYCCNT gates real 20ms gaps without touching its shared timeline state.
 * If the existing clock is unavailable/changes, quantized HAL ticks fall back
 * to 21ms conservatively. Neither clock is initialized or reconfigured here. */
#define VIDEO_FALLBACK_START_GAP_MS 21u
#define VIDEO_BUILD_TAG             0x261112D1u
#define VIDEO_DMA_STATE_ERROR       0x80000000u
#define VIDEO_TX_RETRY              2u
#define VIDEO_QUEUE_DEPTH           8u
#define VIDEO_FRAME_BYTES           309u
#define VIDEO_DATA_BYTES            300u
#define VIDEO_HEAD_BYTES            7u
#define VIDEO_SOF                   0xA5u
#define VIDEO_CMD_ID                0x0310u

/* ========================== checksum tables =============================== */

/* Same tables as the Jetson side (autoaim/tools/crc.cpp) and the ACE
 * reference implementation. Do not replace them with the CRC8_CRC16.c
 * helpers: that file uses a different CRC16 table for its own protocol. */
static const uint8_t video_crc8_table[256] = {
  0x00, 0x5e, 0xbc, 0xe2, 0x61, 0x3f, 0xdd, 0x83,
  0xc2, 0x9c, 0x7e, 0x20, 0xa3, 0xfd, 0x1f, 0x41,
  0x9d, 0xc3, 0x21, 0x7f, 0xfc, 0xa2, 0x40, 0x1e,
  0x5f, 0x01, 0xe3, 0xbd, 0x3e, 0x60, 0x82, 0xdc,
  0x23, 0x7d, 0x9f, 0xc1, 0x42, 0x1c, 0xfe, 0xa0,
  0xe1, 0xbf, 0x5d, 0x03, 0x80, 0xde, 0x3c, 0x62,
  0xbe, 0xe0, 0x02, 0x5c, 0xdf, 0x81, 0x63, 0x3d,
  0x7c, 0x22, 0xc0, 0x9e, 0x1d, 0x43, 0xa1, 0xff,
  0x46, 0x18, 0xfa, 0xa4, 0x27, 0x79, 0x9b, 0xc5,
  0x84, 0xda, 0x38, 0x66, 0xe5, 0xbb, 0x59, 0x07,
  0xdb, 0x85, 0x67, 0x39, 0xba, 0xe4, 0x06, 0x58,
  0x19, 0x47, 0xa5, 0xfb, 0x78, 0x26, 0xc4, 0x9a,
  0x65, 0x3b, 0xd9, 0x87, 0x04, 0x5a, 0xb8, 0xe6,
  0xa7, 0xf9, 0x1b, 0x45, 0xc6, 0x98, 0x7a, 0x24,
  0xf8, 0xa6, 0x44, 0x1a, 0x99, 0xc7, 0x25, 0x7b,
  0x3a, 0x64, 0x86, 0xd8, 0x5b, 0x05, 0xe7, 0xb9,
  0x8c, 0xd2, 0x30, 0x6e, 0xed, 0xb3, 0x51, 0x0f,
  0x4e, 0x10, 0xf2, 0xac, 0x2f, 0x71, 0x93, 0xcd,
  0x11, 0x4f, 0xad, 0xf3, 0x70, 0x2e, 0xcc, 0x92,
  0xd3, 0x8d, 0x6f, 0x31, 0xb2, 0xec, 0x0e, 0x50,
  0xaf, 0xf1, 0x13, 0x4d, 0xce, 0x90, 0x72, 0x2c,
  0x6d, 0x33, 0xd1, 0x8f, 0x0c, 0x52, 0xb0, 0xee,
  0x32, 0x6c, 0x8e, 0xd0, 0x53, 0x0d, 0xef, 0xb1,
  0xf0, 0xae, 0x4c, 0x12, 0x91, 0xcf, 0x2d, 0x73,
  0xca, 0x94, 0x76, 0x28, 0xab, 0xf5, 0x17, 0x49,
  0x08, 0x56, 0xb4, 0xea, 0x69, 0x37, 0xd5, 0x8b,
  0x57, 0x09, 0xeb, 0xb5, 0x36, 0x68, 0x8a, 0xd4,
  0x95, 0xcb, 0x29, 0x77, 0xf4, 0xaa, 0x48, 0x16,
  0xe9, 0xb7, 0x55, 0x0b, 0x88, 0xd6, 0x34, 0x6a,
  0x2b, 0x75, 0x97, 0xc9, 0x4a, 0x14, 0xf6, 0xa8,
  0x74, 0x2a, 0xc8, 0x96, 0x15, 0x4b, 0xa9, 0xf7,
  0xb6, 0xe8, 0x0a, 0x54, 0xd7, 0x89, 0x6b, 0x35,
};

static const uint16_t video_crc16_table[256] = {
  0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
  0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
  0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
  0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
  0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
  0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
  0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
  0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
  0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
  0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
  0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
  0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
  0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
  0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
  0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
  0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
  0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
  0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
  0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
  0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
  0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
  0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
  0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
  0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
  0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
  0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
  0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
  0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
  0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
  0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
  0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
  0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78,
};

static uint8_t video_crc8(const uint8_t *data, uint16_t len)
{
  uint8_t crc = 0xFFu;
  while (len--) crc = video_crc8_table[crc ^ (*data++)];
  return crc;
}

static uint16_t video_crc16(const uint8_t *data, uint32_t len)
{
  uint16_t crc = 0xFFFFu;
  while (len--) crc = (uint16_t)((crc >> 8) ^ video_crc16_table[(crc ^ (*data++)) & 0xFFu]);
  return crc;
}

/* ========================== frame queue ================================== */

/* USB IRQ is the single producer; Self_aim_task is the single consumer. */
static uint8_t s_queue[VIDEO_QUEUE_DEPTH][VIDEO_FRAME_BYTES];
/* DMA exclusively owns this buffer until real UART TC or confirmed TX abort. */
static uint8_t s_tx_frame[VIDEO_FRAME_BYTES];
static volatile uint8_t s_head = 0u;
static volatile uint8_t s_tail = 0u;

static volatile uint32_t s_forwarded = 0u;
static volatile uint32_t s_crc_error = 0u;
static volatile uint32_t s_overflow = 0u;
static volatile uint32_t s_tx_error = 0u;
static volatile uint32_t s_tx_retry = 0u;
static volatile uint32_t s_tx_stuck = 0u;
static volatile uint32_t s_usb_bytes = 0u;
static volatile uint32_t s_frames = 0u;
static volatile uint32_t s_task_calls = 0u;

static volatile VideoForwardTxState_t s_tx_state = VIDEO_TX_IDLE;
static volatile uint8_t s_owns_tx = 0u;
static volatile uint8_t s_tc_event = 0u;
static volatile uint8_t s_dma_error_event = 0u;
static uint8_t s_attempts = 0u;
static uint8_t s_have_start_tick = 0u;
static uint32_t s_last_start_tick = 0u;
static uint32_t s_last_start_cycle = 0u;
static uint32_t s_last_cycle_clock_hz = 0u;
static uint8_t s_have_start_cycle = 0u;
static uint32_t s_pending_tick = 0u;
static uint32_t s_abort_tick = 0u;

static volatile uint32_t s_dma_started = 0u;
static volatile uint32_t s_dma_completed = 0u;
static volatile uint32_t s_dma_timeout = 0u;
static volatile uint32_t s_dma_start_fail = 0u;
static volatile uint32_t s_dma_busy = 0u;
static volatile uint32_t s_dma_abort = 0u;
static volatile uint32_t s_dma_abort_fail = 0u;
static volatile uint32_t s_dma_error = 0u;
static volatile uint32_t s_pending_expired = 0u;
static volatile uint32_t s_init_rejected = 0u;
static volatile uint32_t s_last_uart_status = HAL_OK;
static volatile uint32_t s_last_dma_error = 0u;
static volatile uint32_t s_last_tx_bytes = 0u;
static volatile uint32_t s_timing_mode = 0u;
static volatile uint32_t s_timing_fallback = 0u;

volatile VideoForwardDebug_t VideoForward_Debug;

static void video_refresh_debug(void)
{
  VideoForward_Debug.usb_bytes = s_usb_bytes;
  VideoForward_Debug.frames = s_frames;
  VideoForward_Debug.forwarded = s_forwarded;
  VideoForward_Debug.crc_error = s_crc_error;
  VideoForward_Debug.overflow = s_overflow;
  VideoForward_Debug.tx_error = s_tx_error;
  VideoForward_Debug.tx_retry = s_tx_retry;
  VideoForward_Debug.tx_stuck = s_tx_stuck;
  VideoForward_Debug.task_calls = s_task_calls;
  VideoForward_Debug.tx_state = (uint32_t)s_tx_state;
  VideoForward_Debug.dma_started = s_dma_started;
  VideoForward_Debug.dma_completed = s_dma_completed;
  VideoForward_Debug.dma_timeout = s_dma_timeout;
  VideoForward_Debug.dma_start_fail = s_dma_start_fail;
  VideoForward_Debug.dma_busy = s_dma_busy;
  VideoForward_Debug.dma_abort = s_dma_abort;
  VideoForward_Debug.dma_abort_fail = s_dma_abort_fail;
  VideoForward_Debug.dma_error = s_dma_error;
  VideoForward_Debug.pending_expired = s_pending_expired;
  VideoForward_Debug.init_rejected = s_init_rejected;
  VideoForward_Debug.last_uart_status = s_last_uart_status;
  VideoForward_Debug.last_dma_error = s_last_dma_error;
  VideoForward_Debug.last_tx_bytes = s_last_tx_bytes;
  VideoForward_Debug.queue_depth = (uint32_t)((s_head + VIDEO_QUEUE_DEPTH - s_tail) % VIDEO_QUEUE_DEPTH);
  VideoForward_Debug.attempts = s_attempts;
  VideoForward_Debug.timing_mode = s_timing_mode;
  VideoForward_Debug.timing_fallback = s_timing_fallback;
  VideoForward_Debug.build_tag = VIDEO_BUILD_TAG;
}

void VideoForward_Init(void)
{
  /* Init is normally boot-only. Never release an active DMA buffer on re-init. */
  if (s_owns_tx)
  {
    s_init_rejected++;
    video_refresh_debug();
    return;
  }
  s_head = 0u;
  s_tail = 0u;
  s_forwarded = 0u;
  s_crc_error = 0u;
  s_overflow = 0u;
  s_tx_error = 0u;
  s_tx_retry = 0u;
  s_tx_stuck = 0u;
  s_usb_bytes = 0u;
  s_frames = 0u;
  s_task_calls = 0u;
  s_tx_state = VIDEO_TX_IDLE;
  s_tc_event = 0u;
  s_dma_error_event = 0u;
  s_attempts = 0u;
  s_have_start_tick = 0u;
  s_last_start_tick = 0u;
  s_last_start_cycle = 0u;
  s_last_cycle_clock_hz = 0u;
  s_have_start_cycle = 0u;
  s_pending_tick = 0u;
  s_abort_tick = 0u;
  s_dma_started = 0u;
  s_dma_completed = 0u;
  s_dma_timeout = 0u;
  s_dma_start_fail = 0u;
  s_dma_busy = 0u;
  s_dma_abort = 0u;
  s_dma_abort_fail = 0u;
  s_dma_error = 0u;
  s_pending_expired = 0u;
  s_init_rejected = 0u;
  s_last_uart_status = HAL_OK;
  s_last_dma_error = 0u;
  s_last_tx_bytes = 0u;
  s_timing_mode = 0u;
  s_timing_fallback = 0u;
  video_refresh_debug();
}

void VideoForward_NoteUsbBytes(uint16_t len)
{
  s_usb_bytes += len;
  video_refresh_debug();
}

static uint8_t video_frame_is_valid(const uint8_t *frame)
{
  uint16_t crc_recv;
  if (frame[0] != VIDEO_SOF) return 0u;
  if (frame[1] != (uint8_t)(VIDEO_DATA_BYTES & 0xFFu)) return 0u;
  if (frame[2] != (uint8_t)((VIDEO_DATA_BYTES >> 8) & 0xFFu)) return 0u;
  if (frame[5] != 0x10u || frame[6] != 0x03u) return 0u;
  if (frame[4] != video_crc8(frame, 4u)) return 0u;
  crc_recv = (uint16_t)((uint16_t)frame[307] | ((uint16_t)frame[308] << 8));
  if (crc_recv != video_crc16(frame, VIDEO_HEAD_BYTES + VIDEO_DATA_BYTES)) return 0u;
  return 1u;
}

uint8_t VideoForward_PushFrame(const uint8_t *frame)
{
  uint8_t next;
  if (frame == 0) return 0u;
  s_frames++;
  if (!video_frame_is_valid(frame))
  {
    s_crc_error++;
    return 0u;
  }
  next = (uint8_t)((s_head + 1u) % VIDEO_QUEUE_DEPTH);
  if (next == s_tail)
  {
    s_tail = (uint8_t)((s_tail + 1u) % VIDEO_QUEUE_DEPTH);
    s_overflow++;
  }
  memcpy(s_queue[s_head], frame, VIDEO_FRAME_BYTES);
  __DMB();
  s_head = next;
  return 1u;
}

/* These callbacks are TX-only counterparts of local HAL UART
 * UART_DMATransmitCplt / UART_DMAError. Never use the latter: its RX error
 * branch can stop DMAR, which belongs to the existing remote-control parser. */
static void video_dma_complete(DMA_HandleTypeDef *hdma)
{
  UART_HandleTypeDef *uart = VIDEO_VT03_HANDLE;
  if (!s_owns_tx || s_tx_state != VIDEO_TX_IN_FLIGHT || hdma != uart->hdmatx) return;
  if (s_dma_error_event || hdma->ErrorCode != HAL_DMA_ERROR_NONE ||
      hdma->State != HAL_DMA_STATE_READY ||
      (hdma->Instance->CR & DMA_SxCR_EN) != 0u || hdma->Instance->NDTR != 0u)
  {
    s_last_dma_error |= hdma->ErrorCode | VIDEO_DMA_STATE_ERROR;
    s_dma_error_event = 1u;
    CLEAR_BIT(uart->Instance->CR3, USART_CR3_DMAT);
    CLEAR_BIT(uart->Instance->CR1, USART_CR1_TCIE);
    return;
  }
  uart->TxXferCount = 0u;
  CLEAR_BIT(uart->Instance->CR3, USART_CR3_DMAT);
  SET_BIT(uart->Instance->CR1, USART_CR1_TCIE);
}

static void video_dma_error(DMA_HandleTypeDef *hdma)
{
  UART_HandleTypeDef *uart = VIDEO_VT03_HANDLE;
  if (!s_owns_tx || s_tx_state != VIDEO_TX_IN_FLIGHT || hdma != uart->hdmatx) return;
  s_last_dma_error = hdma->ErrorCode;
  s_dma_error_event = 1u;
  CLEAR_BIT(uart->Instance->CR3, USART_CR3_DMAT);
  CLEAR_BIT(uart->Instance->CR1, USART_CR1_TCIE);
}

void VideoForward_TxCompleteFromISR(void)
{
  UART_HandleTypeDef *uart = VIDEO_VT03_HANDLE;
  DMA_HandleTypeDef *dma = uart->hdmatx;
  if (!s_owns_tx || s_tx_state != VIDEO_TX_IN_FLIGHT) return;
  if ((uart->Instance->SR & USART_SR_TC) == 0u ||
      (uart->Instance->CR1 & USART_CR1_TCIE) == 0u) return;
  /* Once an owned TC interrupt is pending, always mask it, including invalid
   * state/error cases; otherwise an interrupt storm could starve the task. */
  CLEAR_BIT(uart->Instance->CR1, USART_CR1_TCIE);
  if (dma == 0)
  {
    s_last_dma_error |= VIDEO_DMA_STATE_ERROR;
    s_dma_error_event = 1u;
    CLEAR_BIT(uart->Instance->CR3, USART_CR3_DMAT);
    return;
  }
  if (s_dma_error_event ||
      dma->ErrorCode != HAL_DMA_ERROR_NONE ||
      (uart->Instance->CR3 & USART_CR3_DMAT) != 0u ||
      (dma->Instance->CR & DMA_SxCR_EN) != 0u || dma->Instance->NDTR != 0u ||
      dma->State != HAL_DMA_STATE_READY || uart->gState != HAL_UART_STATE_BUSY_TX)
  {
    s_last_dma_error |= dma->ErrorCode | VIDEO_DMA_STATE_ERROR;
    s_dma_error_event = 1u;
    CLEAR_BIT(uart->Instance->CR3, USART_CR3_DMAT);
    return;
  }
  /* Exact TX-only completion semantics of local HAL UART_EndTransmit_IT.
   * Do not call HAL_UART_IRQHandler: ORE+DMAR may abort the unrelated RX. */
  uart->gState = HAL_UART_STATE_READY;
  __DMB();
  s_tc_event = 1u;
}

static uint8_t video_hardware_ready(void)
{
  UART_HandleTypeDef *uart = VIDEO_VT03_HANDLE;
  DMA_HandleTypeDef *dma = uart->hdmatx;
  return (uint8_t)(dma != 0 && uart->gState == HAL_UART_STATE_READY &&
    uart->Lock == HAL_UNLOCKED && dma->State == HAL_DMA_STATE_READY &&
    dma->Lock == HAL_UNLOCKED && (dma->Instance->CR & DMA_SxCR_EN) == 0u &&
    (uart->Instance->CR3 & USART_CR3_DMAT) == 0u &&
    (uart->Instance->CR1 & (USART_CR1_TCIE | USART_CR1_TXEIE)) == 0u);
}

static uint8_t video_cycle_clock_ready(void)
{
  /* STM32F407 here runs at 168MHz. Reject an absent/invalid clock rather
   * than silently converting a zero frequency into an unlimited channel. */
  return (uint8_t)(SystemCoreClock >= 1000000u && SystemCoreClock <= 168000000u &&
    (CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) != 0u &&
    (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0u);
}

static uint8_t video_start_gap_ready(uint32_t now, uint8_t *fallback)
{
  uint8_t cycle_ready = video_cycle_clock_ready();
  *fallback = (uint8_t)!cycle_ready;
  if (!s_have_start_tick) return 1u;
  if (cycle_ready && s_have_start_cycle && SystemCoreClock == s_last_cycle_clock_hz)
  {
    uint32_t minimum_cycles = (SystemCoreClock + 49u) / 50u;
    return (uint8_t)((uint32_t)(DWT->CYCCNT - s_last_start_cycle) >= minimum_cycles);
  }
  /* Once an unavailable/changed clock is observed, do not reuse its old
   * cycle anchor even if the clock returns before the next start. */
  s_have_start_cycle = 0u;
  /* Switching time domains is conservative; unsigned CYCCNT wrap is safe. */
  *fallback = 1u;
  return (uint8_t)((uint32_t)(now - s_last_start_tick) >= VIDEO_FALLBACK_START_GAP_MS);
}

static void video_record_start(void)
{
  s_last_start_tick = HAL_GetTick();
  s_have_start_tick = 1u;
  if (video_cycle_clock_ready())
  {
    s_last_start_cycle = DWT->CYCCNT;
    s_last_cycle_clock_hz = SystemCoreClock;
    s_have_start_cycle = 1u;
  }
  else s_have_start_cycle = 0u;
}

static void video_begin_stop(uint32_t now)
{
  UART_HandleTypeDef *uart = VIDEO_VT03_HANDLE;
  uint32_t saved_irq = __get_PRIMASK();
  __disable_irq();
  s_tx_state = VIDEO_TX_STOPPING;
  CLEAR_BIT(uart->Instance->CR1, USART_CR1_TCIE | USART_CR1_TXEIE);
  CLEAR_BIT(uart->Instance->CR3, USART_CR3_DMAT);
  if (uart->hdmatx != 0) __HAL_DMA_DISABLE(uart->hdmatx);
  __DMB();
  __set_PRIMASK(saved_irq);
  s_abort_tick = now;
  s_dma_abort++;
  s_tx_stuck++;
}

static void video_retry_or_drop(uint32_t now)
{
  s_owns_tx = 0u;
  s_tc_event = 0u;
  s_dma_error_event = 0u;
  if (s_attempts < (1u + VIDEO_TX_RETRY))
  {
    s_pending_tick = now;
    s_tx_state = VIDEO_TX_PENDING;
  }
  else
  {
    s_tx_error++;
    s_tx_state = VIDEO_TX_IDLE;
  }
}

static void video_latch_fault(void)
{
  s_dma_abort_fail++;
  s_tx_error++;
  /* Retain ownership and the buffer: hardware has not been proven safe to
   * reuse. USB input remains bounded by the existing dropping ring queue. */
  s_tx_state = VIDEO_TX_FAULT;
}

static void video_finish_stop(uint32_t now)
{
  UART_HandleTypeDef *uart = VIDEO_VT03_HANDLE;
  DMA_HandleTypeDef *dma = uart->hdmatx;
  uint32_t saved_irq;
  HAL_StatusTypeDef status = HAL_OK;
  if (dma == 0)
  {
    video_latch_fault();
    return;
  }
  if ((dma->Instance->CR & DMA_SxCR_EN) != 0u)
  {
    if ((uint32_t)(now - s_abort_tick) >= VIDEO_ABORT_WATCHDOG_MS) video_latch_fault();
    return;
  }
  saved_irq = __get_PRIMASK();
  __disable_irq();
  /* EN is already zero and this module owns TX. HAL_DMA_Abort's hardware
   * wait therefore has zero iterations. Never call it with EN still set. */
  if (dma->State == HAL_DMA_STATE_BUSY) status = HAL_DMA_Abort(dma);
  else if (dma->State != HAL_DMA_STATE_READY || dma->Lock != HAL_UNLOCKED) status = HAL_ERROR;
  if (status == HAL_OK && (dma->Instance->CR & DMA_SxCR_EN) == 0u)
  {
    /* DMAT is already zero, so local HAL_UART_AbortTransmit cannot call a
     * blocking DMA abort. It resets only UART TX, never RxState or DMAR. */
    status = HAL_UART_AbortTransmit(uart);
  }
  __DMB();
  __set_PRIMASK(saved_irq);
  s_last_uart_status = (uint32_t)status;
  if (status != HAL_OK || !video_hardware_ready())
  {
    video_latch_fault();
    return;
  }
  video_retry_or_drop(now);
}

static void video_start_dma(uint32_t now)
{
  UART_HandleTypeDef *uart = VIDEO_VT03_HANDLE;
  DMA_HandleTypeDef *dma;
  HAL_StatusTypeDef status;
  uint8_t fallback;
  uint32_t saved_irq = __get_PRIMASK();
  __disable_irq();
  if (!video_hardware_ready())
  {
    __set_PRIMASK(saved_irq);
    s_dma_busy++;
    return;
  }
  /* Task entry may have been preempted for a long time. Re-check using the
   * current tick while acquiring TX; stale task time must not allow bursts. */
  now = HAL_GetTick();
  if (!video_start_gap_ready(now, &fallback))
  {
    __set_PRIMASK(saved_irq);
    return;
  }
  dma = uart->hdmatx;
  uart->Lock = HAL_LOCKED;
  s_owns_tx = 1u;
  s_tc_event = 0u;
  s_dma_error_event = 0u;
  s_tx_state = VIDEO_TX_IN_FLIGHT;
  if (s_attempts != 0u) s_tx_retry++;
  s_attempts++;
  video_record_start();
  s_timing_mode = fallback ? 0u : 1u;
  if (fallback) s_timing_fallback++;
  s_last_tx_bytes = 0u;
  s_last_dma_error = 0u;

  /* Checked TX-only equivalent of local HAL_UART_Transmit_DMA (1279ff).
   * That version ignores HAL_DMA_Start_IT's status and installs an error
   * callback with RX side effects. Keep only its TX metadata/flag sequence. */
  uart->pTxBuffPtr = s_tx_frame;
  uart->TxXferSize = VIDEO_FRAME_BYTES;
  uart->TxXferCount = VIDEO_FRAME_BYTES;
  uart->gState = HAL_UART_STATE_BUSY_TX;
  dma->XferCpltCallback = video_dma_complete;
  dma->XferHalfCpltCallback = 0;
  dma->XferErrorCallback = video_dma_error;
  dma->XferAbortCallback = 0;
  __HAL_UART_CLEAR_FLAG(uart, UART_FLAG_TC);
  status = HAL_DMA_Start_IT(dma, (uint32_t)s_tx_frame,
                            (uint32_t)&uart->Instance->DR, VIDEO_FRAME_BYTES);
  s_last_uart_status = (uint32_t)status;
  if (status == HAL_OK && dma->State == HAL_DMA_STATE_BUSY &&
      (dma->Instance->CR & DMA_SxCR_EN) != 0u && dma->Instance->NDTR == VIDEO_FRAME_BYTES)
  {
    s_dma_started++;
    __DMB();
    SET_BIT(uart->Instance->CR3, USART_CR3_DMAT);
    /* Record after the physical request is enabled, not before preemption. */
    video_record_start();
    __HAL_UNLOCK(uart);
    __set_PRIMASK(saved_irq);
    return;
  }
  s_dma_start_fail++;
  s_last_dma_error = dma->ErrorCode;
  if (status == HAL_OK) s_last_uart_status = HAL_ERROR;
  __HAL_UNLOCK(uart);
  __set_PRIMASK(saved_irq);
  /* Even a defective/fault-injected HAL that returned OK without starting
   * cannot be counted as forwarded or let the buffer be reused too early. */
  video_begin_stop(now);
}

void VideoForward_Task(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t saved_irq;
  s_task_calls++;
  if (s_tx_state == VIDEO_TX_FAULT) goto done;
  if (s_tx_state == VIDEO_TX_IN_FLIGHT)
  {
    DMA_HandleTypeDef *dma = VIDEO_VT03_HANDLE->hdmatx;
    /* DMA/TC IRQs can complete between a timeout test and its stop request.
     * Resolve events and request a stop in one short atomic decision. */
    saved_irq = __get_PRIMASK();
    __disable_irq();
    if (dma != 0 && dma->Instance->NDTR <= VIDEO_FRAME_BYTES)
      s_last_tx_bytes = VIDEO_FRAME_BYTES - dma->Instance->NDTR;
    if (s_dma_error_event)
    {
      s_dma_error++;
      video_begin_stop(now);
      __set_PRIMASK(saved_irq);
      goto done;
    }
    if (s_tc_event)
    {
      s_dma_completed++;
      s_forwarded++;
      s_owns_tx = 0u;
      s_tc_event = 0u;
      s_tx_state = VIDEO_TX_IDLE;
      __set_PRIMASK(saved_irq);
    }
    else
    {
      if ((uint32_t)(now - s_last_start_tick) >= VIDEO_DMA_WATCHDOG_MS)
      {
        s_dma_timeout++;
        video_begin_stop(now);
      }
      __set_PRIMASK(saved_irq);
      goto done;
    }
  }
  if (s_tx_state == VIDEO_TX_STOPPING)
  {
    video_finish_stop(now);
    goto done;
  }
  if (s_tx_state == VIDEO_TX_IDLE)
  {
    saved_irq = __get_PRIMASK();
    __disable_irq();
    __DMB();
    if (s_tail == s_head)
    {
      __set_PRIMASK(saved_irq);
      goto done;
    }
    memcpy(s_tx_frame, s_queue[s_tail], VIDEO_FRAME_BYTES);
    s_tail = (uint8_t)((s_tail + 1u) % VIDEO_QUEUE_DEPTH);
    __DMB();
    __set_PRIMASK(saved_irq);
    s_attempts = 0u;
    s_pending_tick = now;
    s_tx_state = VIDEO_TX_PENDING;
  }
  if (s_tx_state == VIDEO_TX_PENDING)
  {
    if ((uint32_t)(now - s_pending_tick) >= VIDEO_PENDING_MAX_AGE_MS)
    {
      s_pending_expired++;
      s_tx_error++;
      s_tx_state = VIDEO_TX_IDLE;
      goto done;
    }
    video_start_dma(now);
  }
done:
  video_refresh_debug();
}

uint32_t VideoForward_GetForwarded(void) { return s_forwarded; }
uint32_t VideoForward_GetCrcError(void)  { return s_crc_error; }
uint32_t VideoForward_GetOverflow(void)  { return s_overflow; }
uint32_t VideoForward_GetTxError(void)   { return s_tx_error; }
uint32_t VideoForward_GetTxRetry(void)   { return s_tx_retry; }
uint32_t VideoForward_GetTxStuck(void)   { return s_tx_stuck; }
uint32_t VideoForward_GetUsbBytes(void)  { return s_usb_bytes; }
uint32_t VideoForward_GetFrames(void)    { return s_frames; }
