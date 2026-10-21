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

/* Feed raw USB CDC bytes. Any length is acceptable (streaming). */
void VideoForward_Feed(const uint8_t *data, uint16_t len);

/* Call from the main loop (or a task). Sends queued frames to VT03. */
void VideoForward_Task(void);

/* Diagnostics. */
uint32_t VideoForward_GetForwarded(void);
uint32_t VideoForward_GetCrcError(void);
uint32_t VideoForward_GetOverflow(void);

#endif /* __VIDEO_FORWARD_H */
