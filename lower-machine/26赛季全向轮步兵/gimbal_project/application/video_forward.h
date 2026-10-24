/**
  ******************************************************************************
  * @file    video_forward.h
  * @brief   0x0310 video frame forwarder: USB CDC -> VT03 UART.
  *
  * The Jetson vision program sends H.264 video frames to the main controller
  * over the same USB CDC link used for gimbal control:
  *   - control frame : 0xFF header, 32 bytes (existing three-order command)
  *   - video   frame : 0xA5 header, 309 bytes (0x0310, custom data channel)
  *
  * This module only:
  *   1) splits the incoming byte stream by frame header,
  *   2) verifies the 309-byte 0x0310 frame (length / cmd_id / CRC8 / CRC16),
  *   3) forwards valid frames unchanged to the VT03 image transmitter.
  *
  * It never parses or modifies the payload.
  ******************************************************************************
  */
#ifndef __VIDEO_FORWARD_H
#define __VIDEO_FORWARD_H

#include "main.h"

/* Optional: reset state and counters. */
void VideoForward_Init(void);

/* Diagnostics hook: call once per USB CDC callback with the byte count the
 * host actually delivered. It only accumulates a counter, nothing else. */
void VideoForward_NoteUsbBytes(uint16_t len);

/* Bytes are split by the single state machine in Self_aim.c.
 * Call this with ONE complete 309-byte frame; it validates and queues it.
 * Returns 1 if the frame was valid and queued. */
uint8_t VideoForward_PushFrame(const uint8_t *frame);

/* Call from the main loop (or a task). Sends queued frames to VT03. */
void VideoForward_Task(void);

/* Watch-window mirror.
 *
 * Keil cannot evaluate a function call while the target is running, so
 * "cannot evaluate" is what you get from VideoForward_GetXxx() in Watch.
 * These counters are copied into the struct below once per
 * VideoForward_Task() call (every 5 ms). In the debugger just add
 * `VideoForward_Debug` to the Watch window and expand it - no function call
 * involved, works while the program is running. */
typedef struct
{
  uint32_t usb_bytes;   /* bytes the USB CDC callback received           */
  uint32_t frames;      /* complete 309-byte frames assembled            */
  uint32_t forwarded;   /* frames HAL_UART_Transmit accepted             */
  uint32_t crc_error;   /* frames rejected by CRC8/CRC16                 */
  uint32_t overflow;    /* frames dropped because the queue was full     */
  uint32_t tx_error;    /* frames given up after VIDEO_TX_RETRY retries  */
  uint32_t tx_retry;    /* extra transmit attempts actually used         */
  uint32_t tx_stuck;    /* times gState had to be re-armed by hand       */
  uint32_t task_calls;  /* VideoForward_Task() invocations               */
} VideoForwardDebug_t;

extern volatile VideoForwardDebug_t VideoForward_Debug;

/* Diagnostics. */
uint32_t VideoForward_GetForwarded(void);   /* frames handed to USART6 OK   */
uint32_t VideoForward_GetCrcError(void);    /* bad CRC8/CRC16, not queued   */
uint32_t VideoForward_GetOverflow(void);    /* queue full, oldest dropped   */
uint32_t VideoForward_GetTxError(void);     /* given up after retries       */
uint32_t VideoForward_GetTxRetry(void);     /* extra attempts that were used*/
uint32_t VideoForward_GetTxStuck(void);     /* gState was stuck, force reset*/
uint32_t VideoForward_GetUsbBytes(void);    /* bytes received from the host */
uint32_t VideoForward_GetFrames(void);      /* 309-byte frames assembled    */

#endif /* __VIDEO_FORWARD_H */
