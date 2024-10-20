/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "d3d12_video_enc.h"
#include "d3d12_video_enc_h264.h"
#include "util/u_video.h"
#include "d3d12_screen.h"
#include "d3d12_format.h"

#include <cmath>
#include <algorithm>
#include <numeric>

void
d3d12_video_encoder_update_current_rate_control_h264(struct d3d12_video_encoder *pD3D12Enc,
                                                     pipe_h264_enc_picture_desc *picture)
{
   struct pipe_h264_enc_picture_desc *h264Pic = (struct pipe_h264_enc_picture_desc *) picture;

   assert(h264Pic->pic_ctrl.temporal_id < ARRAY_SIZE(pipe_h264_enc_picture_desc::rate_ctrl));
   assert(h264Pic->pic_ctrl.temporal_id < std::max(1u, pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificSequenceStateDescH264.num_temporal_layers));
   assert(h264Pic->pic_ctrl.temporal_id < ARRAY_SIZE(D3D12EncodeConfiguration::m_encoderRateControlDesc));

   struct D3D12EncodeRateControlState m_prevRCState = pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[picture->pic_ctrl.temporal_id];
   pD3D12Enc->m_currentEncodeConfig.m_activeRateControlIndex = h264Pic->pic_ctrl.temporal_id;
   pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id] = {};
   pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_FrameRate.Numerator =
      picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].frame_rate_num;
   pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_FrameRate.Denominator =
      picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].frame_rate_den;
   pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE;

   if (picture->roi.num > 0)
      pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
         D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_DELTA_QP;

   switch (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].rate_ctrl_method) {
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE_SKIP:
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE:
      {
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_VBR;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR.TargetAvgBitRate =
            picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].target_bitrate;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR.PeakBitRate =
            picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].peak_bitrate;

         if (D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE) {
            debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE environment variable is set, "
                       ", forcing VBV Size = VBV Initial Capacity = Target Bitrate = %" PRIu64 " (bits)\n", pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.TargetBitRate);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.VBVCapacity =
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.TargetBitRate;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.InitialVBVFullness =
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.TargetBitRate;
         } else if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].app_requested_hrd_buffer) {
            debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 HRD required by app,"
                       " setting VBV Size = %d (bits) - VBV Initial Capacity %d (bits)\n", picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buffer_size, picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buf_initial_size);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR.VBVCapacity =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buffer_size;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR.InitialVBVFullness =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buf_initial_size;
         }

         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].max_frame_size = picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size;
         if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size > 0) {
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR.MaxFrameBitSize =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size;

            debug_printf(
               "[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 "
               "Upper layer requested explicit MaxFrameBitSize: %" PRIu64 "\n",
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR.MaxFrameBitSize);
         }

         if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].app_requested_qp_range) {
            debug_printf(
               "[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 "
               "Upper layer requested explicit MinQP: %d MaxQP: %d\n",
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].min_qp, picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_qp);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR.MinQP =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].min_qp;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR.MaxQP =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_qp;
         }

         if (picture->quality_modes.level > 0) {
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QUALITY_VS_SPEED;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT;

            // Convert between D3D12 definition and PIPE definition
            // D3D12: QualityVsSpeed must be in the range [0, D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1.MaxQualityVsSpeed]
            // The lower the value, the fastest the encode operation
            // PIPE: The quality level range can be queried through the VAConfigAttribEncQualityRange attribute. 
            // A lower value means higher quality, and a value of 1 represents the highest quality. 
            // The quality level setting is used as a trade-off between quality and speed/power 
            // consumption, with higher quality corresponds to lower speed and higher power consumption.

            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_VBR1.QualityVsSpeed =
               pD3D12Enc->max_quality_levels - picture->quality_modes.level;
         }

      } break;
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_QUALITY_VARIABLE:
      {
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR.TargetAvgBitRate =
            picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].target_bitrate;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR.PeakBitRate =
            picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].peak_bitrate;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR.ConstantQualityTarget =
            picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbr_quality_factor;

         if (D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE) {
            debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE environment variable is set, "
                       ", forcing VBV Size = VBV Initial Capacity = Target Bitrate = %" PRIu64 " (bits)\n", pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR1.TargetAvgBitRate);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR1.VBVCapacity =
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR1.TargetAvgBitRate;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR1.InitialVBVFullness =
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR1.TargetAvgBitRate;
         } else if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].app_requested_hrd_buffer) {
            debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 HRD required by app,"
                       " setting VBV Size = %d (bits) - VBV Initial Capacity %d (bits)\n", picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buffer_size, picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buf_initial_size);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT;               
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR1.VBVCapacity =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buffer_size;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR1.InitialVBVFullness =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buf_initial_size;
         }
      pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].max_frame_size = picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size;
      if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size > 0) {
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR.MaxFrameBitSize =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size;

            debug_printf(
               "[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 "
               "Upper layer requested explicit MaxFrameBitSize: %" PRIu64 "\n",
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR.MaxFrameBitSize);
         }

         if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].app_requested_qp_range) {
            debug_printf(
               "[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 "
               "Upper layer requested explicit MinQP: %d MaxQP: %d\n",
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].min_qp, picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_qp);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR.MinQP =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].min_qp;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR.MaxQP =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_qp;
         }
         if (picture->quality_modes.level > 0) {
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QUALITY_VS_SPEED;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT;

            // Convert between D3D12 definition and PIPE definition
            // D3D12: QualityVsSpeed must be in the range [0, D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1.MaxQualityVsSpeed]
            // The lower the value, the fastest the encode operation
            // PIPE: The quality level range can be queried through the VAConfigAttribEncQualityRange attribute. 
            // A lower value means higher quality, and a value of 1 represents the highest quality. 
            // The quality level setting is used as a trade-off between quality and speed/power 
            // consumption, with higher quality corresponds to lower speed and higher power consumption.

            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_QVBR1.QualityVsSpeed =
               pD3D12Enc->max_quality_levels - picture->quality_modes.level;
         }
      } break;
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT_SKIP:
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT:
      {
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.TargetBitRate =
            picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].target_bitrate;

         /* For CBR mode, to guarantee bitrate of generated stream complies with
          * target bitrate (e.g. no over +/-10%), vbv_buffer_size and initial capacity should be same
          * as target bitrate. Controlled by OS env var D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE
          */
         if (D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE) {
            debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE environment variable is set, "
                       ", forcing VBV Size = VBV Initial Capacity = Target Bitrate = %" PRIu64 " (bits)\n", pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.TargetBitRate);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.VBVCapacity =
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.TargetBitRate;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.InitialVBVFullness =
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.TargetBitRate;
         } else if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].app_requested_hrd_buffer) {
            debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 HRD required by app,"
                       " setting VBV Size = %d (bits) - VBV Initial Capacity %d (bits)\n", picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buffer_size, picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buf_initial_size);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.VBVCapacity =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buffer_size;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.InitialVBVFullness =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].vbv_buf_initial_size;
         }

         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].max_frame_size = picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size;
         if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size > 0) {
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.MaxFrameBitSize =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_au_size;

            debug_printf(
               "[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 "
               "Upper layer requested explicit MaxFrameBitSize: %" PRIu64 "\n",
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.MaxFrameBitSize);
         }

         if (picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].app_requested_qp_range) {
            debug_printf(
               "[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 "
               "Upper layer requested explicit MinQP: %d MaxQP: %d\n",
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].min_qp, picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_qp);

            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.MinQP =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].min_qp;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR.MaxQP =
               picture->rate_ctrl[h264Pic->pic_ctrl.temporal_id].max_qp;
         }

         if (picture->quality_modes.level > 0) {
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QUALITY_VS_SPEED;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT;

            // Convert between D3D12 definition and PIPE definition
            // D3D12: QualityVsSpeed must be in the range [0, D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1.MaxQualityVsSpeed]
            // The lower the value, the fastest the encode operation
            // PIPE: The quality level range can be queried through the VAConfigAttribEncQualityRange attribute. 
            // A lower value means higher quality, and a value of 1 represents the highest quality. 
            // The quality level setting is used as a trade-off between quality and speed/power 
            // consumption, with higher quality corresponds to lower speed and higher power consumption.

            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CBR1.QualityVsSpeed =
               pD3D12Enc->max_quality_levels - picture->quality_modes.level;
         }
      } break;
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_DISABLE:
      {
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;

         // Load previous RC state for all frames and only update the current frame
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CQP =
                  m_prevRCState.m_Config.m_Configuration_CQP;
         switch (picture->picture_type) {
            case PIPE_H2645_ENC_PICTURE_TYPE_P:
            {
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CQP
                  .ConstantQP_InterPredictedFrame_PrevRefOnly = picture->quant_p_frames;
            } break;
            case PIPE_H2645_ENC_PICTURE_TYPE_B:
            {
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CQP
                  .ConstantQP_InterPredictedFrame_BiDirectionalRef = picture->quant_b_frames;
            } break;
            case PIPE_H2645_ENC_PICTURE_TYPE_I:
            case PIPE_H2645_ENC_PICTURE_TYPE_IDR:
            {
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CQP
                  .ConstantQP_FullIntracodedFrame = picture->quant_i_frames;
            } break;
            default:
            {
               unreachable("Unsupported pipe_h2645_enc_picture_type");
            } break;
         }

         if (picture->quality_modes.level > 0) {
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QUALITY_VS_SPEED;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_EXTENSION1_SUPPORT;

            // Convert between D3D12 definition and PIPE definition
            // D3D12: QualityVsSpeed must be in the range [0, D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1.MaxQualityVsSpeed]
            // The lower the value, the fastest the encode operation
            // PIPE: The quality level range can be queried through the VAConfigAttribEncQualityRange attribute. 
            // A lower value means higher quality, and a value of 1 represents the highest quality. 
            // The quality level setting is used as a trade-off between quality and speed/power 
            // consumption, with higher quality corresponds to lower speed and higher power consumption.

            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CQP1.QualityVsSpeed =
               pD3D12Enc->max_quality_levels - picture->quality_modes.level;
         }
      } break;
      default:
      {
         debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 invalid RC "
                       "config, using default RC CQP mode\n");
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CQP
            .ConstantQP_FullIntracodedFrame = 30;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CQP
            .ConstantQP_InterPredictedFrame_PrevRefOnly = 30;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Config.m_Configuration_CQP
            .ConstantQP_InterPredictedFrame_BiDirectionalRef = 30;
      } break;
   }
}

void
d3d12_video_encoder_update_current_frame_pic_params_info_h264(struct d3d12_video_encoder *pD3D12Enc,
                                                              struct pipe_video_buffer *srcTexture,
                                                              struct pipe_picture_desc *picture,
                                                              D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA &picParams,
                                                              bool &bUsedAsReference)
{
   struct pipe_h264_enc_picture_desc *h264Pic = (struct pipe_h264_enc_picture_desc *) picture;
   d3d12_video_bitstream_builder_h264 *pH264BitstreamBuilder =
      static_cast<d3d12_video_bitstream_builder_h264 *>(pD3D12Enc->m_upBitstreamBuilder.get());
   assert(pH264BitstreamBuilder != nullptr);

   pD3D12Enc->m_currentEncodeConfig.m_bUsedAsReference = !h264Pic->not_referenced;
   bUsedAsReference = pD3D12Enc->m_currentEncodeConfig.m_bUsedAsReference;

   if (pD3D12Enc->m_currentEncodeCapabilities.m_encoderCodecSpecificConfigCaps.m_H264CodecCaps.SupportFlags &
       D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_NUM_REF_IDX_ACTIVE_OVERRIDE_FLAG_SLICE_SUPPORT)
   {
      picParams.pH264PicData->Flags |= D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAG_REQUEST_NUM_REF_IDX_ACTIVE_OVERRIDE_FLAG_SLICE;
   }

   //
   // These need to be set here so they're available for SPS/PPS header building (reference manager updates after that, for slice header params)
   //
   picParams.pH264PicData->TemporalLayerIndex = h264Pic->pic_ctrl.temporal_id;
   picParams.pH264PicData->pic_parameter_set_id = pH264BitstreamBuilder->get_active_pps().pic_parameter_set_id;
   picParams.pH264PicData->List0ReferenceFramesCount = 0;
   picParams.pH264PicData->List1ReferenceFramesCount = 0;
   if ((h264Pic->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_P) ||
       (h264Pic->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_B))
      picParams.pH264PicData->List0ReferenceFramesCount = h264Pic->num_ref_idx_l0_active_minus1 + 1;

   if (h264Pic->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_B)
      picParams.pH264PicData->List1ReferenceFramesCount = h264Pic->num_ref_idx_l1_active_minus1 + 1;

   if ((pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_DELTA_QP) != 0)
   {
      // Use 8 bit qpmap array for H264 picparams (-51, 51 range and int8_t pRateControlQPMap type)
      const int32_t h264_min_delta_qp = -51;
      const int32_t h264_max_delta_qp = 51;
      d3d12_video_encoder_update_picparams_region_of_interest_qpmap(
         pD3D12Enc,
         &h264Pic->roi,
         h264_min_delta_qp,
         h264_max_delta_qp,
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_pRateControlQPMap8Bit);
      picParams.pH264PicData->pRateControlQPMap = pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_pRateControlQPMap8Bit.data();
      picParams.pH264PicData->QPMapValuesCount = pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc[h264Pic->pic_ctrl.temporal_id].m_pRateControlQPMap8Bit.size();
   }

   // Save state snapshot from record time to resolve headers at get_feedback time
   uint64_t current_metadata_slot = (pD3D12Enc->m_fenceValue % D3D12_VIDEO_ENC_METADATA_BUFFERS_COUNT);
   pD3D12Enc->m_spEncodedFrameMetadata[current_metadata_slot].m_associatedEncodeCapabilities =
      pD3D12Enc->m_currentEncodeCapabilities;
   pD3D12Enc->m_spEncodedFrameMetadata[current_metadata_slot].m_associatedEncodeConfig =
      pD3D12Enc->m_currentEncodeConfig;
}

///
/// Tries to configurate the encoder using the requested slice configuration
/// or falls back to single slice encoding.
///
bool
d3d12_video_encoder_negotiate_current_h264_slices_configuration(struct d3d12_video_encoder *pD3D12Enc,
                                                                pipe_h264_enc_picture_desc *picture)
{
   ///
   /// Initialize single slice by default
   ///
   D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE requestedSlicesMode =
      D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
   D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES requestedSlicesConfig = {};
   requestedSlicesConfig.NumberOfSlicesPerFrame = 1;

   ///
   /// Try to see if can accomodate for multi-slice request by user
   ///
   if ((picture->slice_mode == PIPE_VIDEO_SLICE_MODE_BLOCKS) && (picture->num_slice_descriptors > 1)) {
      /* Some apps send all same size slices minus 1 slice in any position in the descriptors */
      /* Lets validate that there are at most 2 different slice sizes in all the descriptors */
      std::vector<int> slice_sizes(picture->num_slice_descriptors);
      for (uint32_t i = 0; i < picture->num_slice_descriptors; i++)
         slice_sizes[i] = picture->slices_descriptors[i].num_macroblocks;
      std::sort(slice_sizes.begin(), slice_sizes.end());
      bool bUniformSizeSlices = (std::unique(slice_sizes.begin(), slice_sizes.end()) - slice_sizes.begin()) <= 2;

      uint32_t mbPerScanline =
         pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Width / D3D12_VIDEO_H264_MB_IN_PIXELS;
      bool bSliceAligned = ((picture->slices_descriptors[0].num_macroblocks % mbPerScanline) == 0);

      if (bUniformSizeSlices) {
         if (picture->intra_refresh.mode != INTRA_REFRESH_MODE_NONE) {
            /*
            * When intra-refresh is active, we must use D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME
            */
            if (d3d12_video_encoder_check_subregion_mode_support(
                     pD3D12Enc,
                     D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME)) {
               requestedSlicesMode =
                  D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME;
               requestedSlicesConfig.NumberOfSlicesPerFrame = picture->num_slice_descriptors;
               debug_printf("[d3d12_video_encoder_h264] Intra-refresh is active and per DX12 spec it requires using multi slice encoding mode: "
                              "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME "
                              "with %d slices per frame.\n",
                              requestedSlicesConfig.NumberOfSlicesPerFrame);
            } else {
               debug_printf("[d3d12_video_encoder_h264] Intra-refresh is active which requires "
                              "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME "
                              "mode but there is HW support for such mode.\n");
               return false;
            }
         } else if (bSliceAligned &&
                  d3d12_video_encoder_check_subregion_mode_support(
                     pD3D12Enc,
                     D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION)) {

            // Number of macroblocks per slice is aligned to a scanline width, in which case we can
            // use D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION
            requestedSlicesMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION;
            requestedSlicesConfig.NumberOfRowsPerSlice = (picture->slices_descriptors[0].num_macroblocks / mbPerScanline);
            debug_printf("[d3d12_video_encoder_h264] Using multi slice encoding mode: "
                           "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION with "
                           "%d macroblocks rows per slice.\n",
                           requestedSlicesConfig.NumberOfRowsPerSlice);
         } else if (d3d12_video_encoder_check_subregion_mode_support(
                     pD3D12Enc,
                     D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME)) {
               requestedSlicesMode =
                  D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME;
               requestedSlicesConfig.NumberOfSlicesPerFrame = picture->num_slice_descriptors;
               debug_printf("[d3d12_video_encoder_h264] Using multi slice encoding mode: "
                              "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME "
                              "with %d slices per frame.\n",
                              requestedSlicesConfig.NumberOfSlicesPerFrame);
         } else if (d3d12_video_encoder_check_subregion_mode_support(
                     pD3D12Enc,
                     D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED)) {
               requestedSlicesMode =
                  D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED;
               requestedSlicesConfig.NumberOfCodingUnitsPerSlice = picture->slices_descriptors[0].num_macroblocks;
               debug_printf("[d3d12_video_encoder_h264] Using multi slice encoding mode: "
                              "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED "
                              "with %d NumberOfCodingUnitsPerSlice per frame.\n",
                              requestedSlicesConfig.NumberOfCodingUnitsPerSlice);
         } else {
            debug_printf("[d3d12_video_encoder_h264] Requested slice control mode is not supported by hardware: No HW support for "
                           "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION or"
                           "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME or"
                           "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED.\n");
            return false;
         }
      } else {
         debug_printf("[d3d12_video_encoder_h264] Requested slice control mode is not supported: All slices must "
                         "have the same number of macroblocks.\n");
         return false;
      }
   } else if(picture->slice_mode == PIPE_VIDEO_SLICE_MODE_MAX_SLICE_SIZE) {
      if ((picture->max_slice_bytes > 0) &&
                 d3d12_video_encoder_check_subregion_mode_support(
                    pD3D12Enc,
                    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION )) {
            requestedSlicesMode =
               D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION;
            requestedSlicesConfig.MaxBytesPerSlice = picture->max_slice_bytes;
            debug_printf("[d3d12_video_encoder_h264] Using multi slice encoding mode: "
                           "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION  "
                           "with %d MaxBytesPerSlice per frame.\n",
                           requestedSlicesConfig.MaxBytesPerSlice);
      } else {
         debug_printf("[d3d12_video_encoder_h264] Requested slice control mode is not supported: No HW support for "
                         "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_BYTES_PER_SUBREGION.\n");
         return false;
      }
   } else {
      requestedSlicesMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
      requestedSlicesConfig.NumberOfSlicesPerFrame = 1;
      debug_printf("[d3d12_video_encoder_h264] Requested slice control mode is full frame. m_SlicesPartition_H264.NumberOfSlicesPerFrame = %d - m_encoderSliceConfigMode = %d \n",
      requestedSlicesConfig.NumberOfSlicesPerFrame, requestedSlicesMode);
   }

   if (!d3d12_video_encoder_compare_slice_config_h264_hevc(
          pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigMode,
          pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigDesc.m_SlicesPartition_H264,
          requestedSlicesMode,
          requestedSlicesConfig)) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_slices;
   }

   pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigDesc.m_SlicesPartition_H264 = requestedSlicesConfig;
   pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigMode = requestedSlicesMode;

   return true;
}

D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE
d3d12_video_encoder_convert_h264_motion_configuration(struct d3d12_video_encoder *pD3D12Enc,
                                                      pipe_h264_enc_picture_desc *picture)
{
   return D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
}

D3D12_VIDEO_ENCODER_LEVELS_H264
d3d12_video_encoder_convert_level_h264(uint32_t h264SpecLevel)
{
   switch (h264SpecLevel) {
      case 10:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_1;
      } break;
      case 11:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_11;
      } break;
      case 12:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_12;
      } break;
      case 13:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_13;
      } break;
      case 20:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_2;
      } break;
      case 21:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_21;
      } break;
      case 22:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_22;
      } break;
      case 30:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_3;
      } break;
      case 31:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_31;
      } break;
      case 32:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_32;
      } break;
      case 40:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_4;
      } break;
      case 41:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_41;
      } break;
      case 42:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_42;
      } break;
      case 50:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_5;
      } break;
      case 51:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_51;
      } break;
      case 52:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_52;
      } break;
      case 60:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_6;
      } break;
      case 61:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_61;
      } break;
      case 62:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_62;
      } break;
      default:
      {
         unreachable("Unsupported H264 level");
      } break;
   }
}

void
d3d12_video_encoder_convert_from_d3d12_level_h264(D3D12_VIDEO_ENCODER_LEVELS_H264 level12,
                                                  uint32_t &specLevel)
{
   specLevel = 0;

   switch (level12) {
      case D3D12_VIDEO_ENCODER_LEVELS_H264_1:
      {
         specLevel = 10;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_1b:
      {
         specLevel = 11;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_11:
      {
         specLevel = 11;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_12:
      {
         specLevel = 12;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_13:
      {
         specLevel = 13;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_2:
      {
         specLevel = 20;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_21:
      {
         specLevel = 21;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_22:
      {
         specLevel = 22;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_3:
      {
         specLevel = 30;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_31:
      {
         specLevel = 31;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_32:
      {
         specLevel = 32;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_4:
      {
         specLevel = 40;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_41:
      {
         specLevel = 41;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_42:
      {
         specLevel = 42;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_5:
      {
         specLevel = 50;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_51:
      {
         specLevel = 51;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_52:
      {
         specLevel = 52;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_6:
      {
         specLevel = 60;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_61:
      {
         specLevel = 61;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_62:
      {
         specLevel = 62;
      } break;
      default:
      {
         unreachable("Unsupported D3D12_VIDEO_ENCODER_LEVELS_H264 value");
      } break;
   }
}

bool
d3d12_video_encoder_update_h264_gop_configuration(struct d3d12_video_encoder *pD3D12Enc,
                                                  pipe_h264_enc_picture_desc *picture)
{
   // Only update GOP when it begins
   // Only update GOP when it begins
   // This triggers DPB/encoder/heap re-creation, so only check on IDR when a GOP might change
   if ((picture->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_IDR)
      || (picture->picture_type == PIPE_H2645_ENC_PICTURE_TYPE_I)) {
      uint32_t GOPLength = picture->intra_idr_period;
      uint32_t PPicturePeriod = picture->ip_period;

      if (picture->seq.pic_order_cnt_type == 1u) {
         debug_printf("[d3d12_video_encoder_h264] Upper layer is requesting pic_order_cnt_type %d but D3D12 Video "
                         "only supports pic_order_cnt_type = 0 or pic_order_cnt_type = 2\n",
                         picture->seq.pic_order_cnt_type);
         return false;
      }

      // Workaround: D3D12 needs to use the POC in the DPB to track reference frames
      // even when there's no frame reordering (picture->seq.pic_order_cnt_type == 2)
      // So in that case, derive an artificial log2_max_pic_order_cnt_lsb_minus4
      // to avoid unexpected wrapping
      if (picture->seq.pic_order_cnt_type == 2u) {
         if (GOPLength == 0) // Use max frame num to wrap on infinite GOPs
            GOPLength = 1 << (picture->seq.log2_max_frame_num_minus4 + 4);
         const uint32_t max_pic_order_cnt_lsb = 2 * GOPLength;
         picture->seq.log2_max_pic_order_cnt_lsb_minus4 = std::max(0.0, std::ceil(std::log2(max_pic_order_cnt_lsb)) - 4);
         assert(picture->seq.log2_max_pic_order_cnt_lsb_minus4 < UCHAR_MAX);
      }

      assert(picture->seq.pic_order_cnt_type < UCHAR_MAX);

      // Set dirty flag if m_H264GroupOfPictures changed
      auto previousGOPConfig = pD3D12Enc->m_currentEncodeConfig.m_encoderGOPConfigDesc.m_H264GroupOfPictures;
      pD3D12Enc->m_currentEncodeConfig.m_encoderGOPConfigDesc.m_H264GroupOfPictures = {
         GOPLength,
         PPicturePeriod,
         static_cast<uint8_t>(picture->seq.pic_order_cnt_type),
         static_cast<uint8_t>(picture->seq.log2_max_frame_num_minus4),
         static_cast<uint8_t>(picture->seq.log2_max_pic_order_cnt_lsb_minus4)
      };

      if (memcmp(&previousGOPConfig,
                 &pD3D12Enc->m_currentEncodeConfig.m_encoderGOPConfigDesc.m_H264GroupOfPictures,
                 sizeof(D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264)) != 0) {
         pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_gop;
      }
   }
   return true;
}

D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264
d3d12_video_encoder_convert_h264_codec_configuration(struct d3d12_video_encoder *pD3D12Enc,
                                                     pipe_h264_enc_picture_desc *picture,
                                                     bool &is_supported)
{
   is_supported = true;
   D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 config = {
      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_NONE,
      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED,
      // Definition of D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODES matches disable_deblocking_filter_idc syntax
      static_cast<D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODES>(picture->dbk.disable_deblocking_filter_idc),
   };

   if (picture->pic_ctrl.enc_cabac_enable) {
      config.ConfigurationFlags |= D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING;
   }

   if (picture->pic_ctrl.constrained_intra_pred_flag) {
      config.ConfigurationFlags |= D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_CONSTRAINED_INTRAPREDICTION;
   }

   if (picture->pic_ctrl.transform_8x8_mode_flag) {
      config.ConfigurationFlags |= D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM;
   }

   pD3D12Enc->m_currentEncodeCapabilities.m_encoderCodecSpecificConfigCaps.m_H264CodecCaps =
   {
      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_NONE,
      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_FLAG_NONE
   };

   D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT capCodecConfigData = { };
   capCodecConfigData.NodeIndex = pD3D12Enc->m_NodeIndex;
   capCodecConfigData.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
   D3D12_VIDEO_ENCODER_PROFILE_H264 prof = d3d12_video_encoder_convert_profile_to_d3d12_enc_profile_h264(pD3D12Enc->base.profile);
   capCodecConfigData.Profile.pH264Profile = &prof;
   capCodecConfigData.Profile.DataSize = sizeof(prof);
   capCodecConfigData.CodecSupportLimits.pH264Support = &pD3D12Enc->m_currentEncodeCapabilities.m_encoderCodecSpecificConfigCaps.m_H264CodecCaps;
   capCodecConfigData.CodecSupportLimits.DataSize = sizeof(pD3D12Enc->m_currentEncodeCapabilities.m_encoderCodecSpecificConfigCaps.m_H264CodecCaps);

   if(FAILED(pD3D12Enc->m_spD3D12VideoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT, &capCodecConfigData, sizeof(capCodecConfigData)))
      || !capCodecConfigData.IsSupported)
   {
         debug_printf("D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT call failed.");
         is_supported = false;
         return config;
   }

   if(((1 << config.DisableDeblockingFilterConfig) & capCodecConfigData.CodecSupportLimits.pH264Support->DisableDeblockingFilterSupportedModes) == 0)
   {
         debug_printf("D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION arguments not supported - DisableDeblockingFilterConfig (value %d) "
                  "not allowed by DisableDeblockingFilterSupportedModes 0x%x cap reporting.",
                  config.DisableDeblockingFilterConfig,
                  capCodecConfigData.CodecSupportLimits.pH264Support->DisableDeblockingFilterSupportedModes);
         is_supported = false;
         return config;
   }

   if(((config.ConfigurationFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING) != 0)
      && ((capCodecConfigData.CodecSupportLimits.pH264Support->SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CABAC_ENCODING_SUPPORT) == 0))
   {
      debug_printf("D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION arguments are not supported - CABAC encoding mode not supported."
         " Ignoring the request for this feature flag on this encode session");
         // Disable it and keep going with a warning
         config.ConfigurationFlags &= ~D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING;
   }

   if(((config.ConfigurationFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_CONSTRAINED_INTRAPREDICTION) != 0)
      && ((capCodecConfigData.CodecSupportLimits.pH264Support->SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_CONSTRAINED_INTRAPREDICTION_SUPPORT) == 0))
   {
      debug_printf("D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION arguments are not supported - constrained_intra_pred_flag not supported."
         " Ignoring the request for this feature flag on this encode session");
         // Disable it and keep going with a warning
         config.ConfigurationFlags &= ~D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_CONSTRAINED_INTRAPREDICTION;
   }

   if(((config.ConfigurationFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM) != 0)
      && ((capCodecConfigData.CodecSupportLimits.pH264Support->SupportFlags & D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_H264_FLAG_ADAPTIVE_8x8_TRANSFORM_ENCODING_SUPPORT) == 0))
   {
      debug_printf("D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION arguments are not supported - transform_8x8_mode_flag not supported."
         " Ignoring the request for this feature flag on this encode session");
         // Disable it and keep going with a warning
         config.ConfigurationFlags &= ~D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_USE_ADAPTIVE_8x8_TRANSFORM;
   }

   return config;
}

static bool
d3d12_video_encoder_update_intra_refresh_h264(struct d3d12_video_encoder *pD3D12Enc,
                                                        D3D12_VIDEO_SAMPLE srcTextureDesc,
                                                        struct pipe_h264_enc_picture_desc *  picture)
{
   if (picture->intra_refresh.mode != INTRA_REFRESH_MODE_NONE)
   {
      // D3D12 only supports row intra-refresh
      if (picture->intra_refresh.mode != INTRA_REFRESH_MODE_UNIT_ROWS)
      {
         debug_printf("[d3d12_video_encoder_update_intra_refresh_h264] Unsupported INTRA_REFRESH_MODE %d\n", picture->intra_refresh.mode);
         return false;
      }

      uint32_t total_frame_blocks = static_cast<uint32_t>(std::ceil(srcTextureDesc.Height / D3D12_VIDEO_H264_MB_IN_PIXELS)) *
                              static_cast<uint32_t>(std::ceil(srcTextureDesc.Width / D3D12_VIDEO_H264_MB_IN_PIXELS));
      D3D12_VIDEO_ENCODER_INTRA_REFRESH targetIntraRefresh = {
         D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_ROW_BASED,
         total_frame_blocks / picture->intra_refresh.region_size,
      };
      double ir_wave_progress = (picture->intra_refresh.offset == 0) ? 0 :
         picture->intra_refresh.offset / (double) total_frame_blocks;
      pD3D12Enc->m_currentEncodeConfig.m_IntraRefreshCurrentFrameIndex =
         static_cast<uint32_t>(std::ceil(ir_wave_progress * targetIntraRefresh.IntraRefreshDuration));

      // Set intra refresh state
      pD3D12Enc->m_currentEncodeConfig.m_IntraRefresh = targetIntraRefresh;
      // Need to send the sequence flag during all the IR duration
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_intra_refresh;
   } else {
      pD3D12Enc->m_currentEncodeConfig.m_IntraRefreshCurrentFrameIndex = 0;
      pD3D12Enc->m_currentEncodeConfig.m_IntraRefresh = {
         D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE,
         0,
      };
   }

   return true;
}

bool
d3d12_video_encoder_update_current_encoder_config_state_h264(struct d3d12_video_encoder *pD3D12Enc,
                                                             D3D12_VIDEO_SAMPLE srcTextureDesc,
                                                             struct pipe_picture_desc *picture)
{
   struct pipe_h264_enc_picture_desc *h264Pic = (struct pipe_h264_enc_picture_desc *) picture;

   // Reset reconfig dirty flags
   pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags = d3d12_video_encoder_config_dirty_flag_none;
   // Reset sequence changes flags
   pD3D12Enc->m_currentEncodeConfig.m_seqFlags = D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;

   // Set codec
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderCodecDesc != D3D12_VIDEO_ENCODER_CODEC_H264) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_codec;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderCodecDesc = D3D12_VIDEO_ENCODER_CODEC_H264;

   // Set Sequence information
   if (memcmp(&pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificSequenceStateDescH264,
              &h264Pic->seq,
              sizeof(h264Pic->seq)) != 0) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_sequence_header;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificSequenceStateDescH264 = h264Pic->seq;

   // Iterate over the headers the app requested and set flags to emit those for this frame
   util_dynarray_foreach(&h264Pic->raw_headers, struct pipe_enc_raw_header, header) {
      if (header->type == PIPE_H264_NAL_SPS)
         pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_sequence_header;
      else if (header->type == PIPE_H264_NAL_PPS)
         pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_picture_header;
      else if (header->type == PIPE_H264_NAL_AUD)
         pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_aud_header;
      else if (header->type == NAL_TYPE_SEI)
         pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_sei_header;
      else if (header->type == NAL_TYPE_PREFIX)
         pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_svcprefix_slice_header;
   }

   // Set input format
   DXGI_FORMAT targetFmt = d3d12_convert_pipe_video_profile_to_dxgi_format(pD3D12Enc->base.profile);
   if (pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo.Format != targetFmt) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_input_format;
   }

   pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo = {};
   pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo.Format = targetFmt;
   HRESULT hr = pD3D12Enc->m_pD3D12Screen->dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,
                                                          &pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo,
                                                          sizeof(pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo));
   if (FAILED(hr)) {
      debug_printf("CheckFeatureSupport failed with HR %x\n", hr);
      return false;
   }

   // Set intra-refresh config
   if(!d3d12_video_encoder_update_intra_refresh_h264(pD3D12Enc, srcTextureDesc, h264Pic)) {
      debug_printf("d3d12_video_encoder_update_intra_refresh_h264 failed!\n");
      return false;
   }

   // Set resolution
   if ((pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Width != srcTextureDesc.Width) ||
       (pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Height != srcTextureDesc.Height)) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_resolution;
   }
   pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Width = srcTextureDesc.Width;
   pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Height = srcTextureDesc.Height;

   // Set resolution codec dimensions (ie. cropping)
   if (h264Pic->seq.enc_frame_cropping_flag) {
      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig.left = h264Pic->seq.enc_frame_crop_left_offset;
      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig.right = h264Pic->seq.enc_frame_crop_right_offset;
      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig.top = h264Pic->seq.enc_frame_crop_top_offset;
      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig.bottom =
         h264Pic->seq.enc_frame_crop_bottom_offset;
   } else {
      memset(&pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig,
             0,
             sizeof(pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig));
   }

   // Set profile
   auto targetProfile = d3d12_video_encoder_convert_profile_to_d3d12_enc_profile_h264(pD3D12Enc->base.profile);
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderProfileDesc.m_H264Profile != targetProfile) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_profile;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderProfileDesc.m_H264Profile = targetProfile;

   // Set level
   auto targetLevel = d3d12_video_encoder_convert_level_h264(h264Pic->seq.level_idc);
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderLevelDesc.m_H264LevelSetting != targetLevel) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_level;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderLevelDesc.m_H264LevelSetting = targetLevel;

   // Set codec config
   bool is_supported = false;
   auto targetCodecConfig = d3d12_video_encoder_convert_h264_codec_configuration(pD3D12Enc, h264Pic, is_supported);
   if (!is_supported) {
      return false;
   }

   if (memcmp(&pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificConfigDesc.m_H264Config,
              &targetCodecConfig,
              sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264)) != 0) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_codec_config;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificConfigDesc.m_H264Config = targetCodecConfig;

   // Set rate control
   d3d12_video_encoder_update_current_rate_control_h264(pD3D12Enc, h264Pic);

   // Set slices config
   if(!d3d12_video_encoder_negotiate_current_h264_slices_configuration(pD3D12Enc, h264Pic)) {
      debug_printf("d3d12_video_encoder_negotiate_current_h264_slices_configuration failed!\n");
      return false;
   }

   // Set GOP config
   if(!d3d12_video_encoder_update_h264_gop_configuration(pD3D12Enc, h264Pic)) {
      debug_printf("d3d12_video_encoder_update_h264_gop_configuration failed!\n");
      return false;
   }

   // m_currentEncodeConfig.m_encoderPicParamsDesc pic params are set in d3d12_video_encoder_reconfigure_encoder_objects
   // after re-allocating objects if needed

   // Set motion estimation config
   auto targetMotionLimit = d3d12_video_encoder_convert_h264_motion_configuration(pD3D12Enc, h264Pic);
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderMotionPrecisionLimit != targetMotionLimit) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |=
         d3d12_video_encoder_config_dirty_flag_motion_precision_limit;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderMotionPrecisionLimit = targetMotionLimit;

   ///
   /// Check for video encode support detailed capabilities
   ///

   // Will call for d3d12 driver support based on the initial requested features, then
   // try to fallback if any of them is not supported and return the negotiated d3d12 settings
   D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 capEncoderSupportData1 = {};
   if (!d3d12_video_encoder_negotiate_requested_features_and_d3d12_driver_caps(pD3D12Enc, capEncoderSupportData1)) {
      debug_printf("[d3d12_video_encoder_h264] After negotiating caps, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1 "
                      "arguments are not supported - "
                      "ValidationFlags: 0x%x - SupportFlags: 0x%x\n",
                      capEncoderSupportData1.ValidationFlags,
                      capEncoderSupportData1.SupportFlags);
      return false;
   }

   ///
   // Calculate current settings based on the returned values from the caps query
   //
   pD3D12Enc->m_currentEncodeCapabilities.m_MaxSlicesInOutput =
      d3d12_video_encoder_calculate_max_slices_count_in_output(
         pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigMode,
         &pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigDesc.m_SlicesPartition_H264,
         pD3D12Enc->m_currentEncodeCapabilities.m_currentResolutionSupportCaps.MaxSubregionsNumber,
         pD3D12Enc->m_currentEncodeConfig.m_currentResolution,
         pD3D12Enc->m_currentEncodeCapabilities.m_currentResolutionSupportCaps.SubregionBlockPixelsSize);

   //
   // Validate caps support returned values against current settings
   //
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderProfileDesc.m_H264Profile !=
       pD3D12Enc->m_currentEncodeCapabilities.m_encoderSuggestedProfileDesc.m_H264Profile) {
      debug_printf("[d3d12_video_encoder_h264] Warning: Requested D3D12_VIDEO_ENCODER_PROFILE_H264 by upper layer: %d "
                    "mismatches UMD suggested D3D12_VIDEO_ENCODER_PROFILE_H264: %d\n",
                    pD3D12Enc->m_currentEncodeConfig.m_encoderProfileDesc.m_H264Profile,
                    pD3D12Enc->m_currentEncodeCapabilities.m_encoderSuggestedProfileDesc.m_H264Profile);
   }

   if (pD3D12Enc->m_currentEncodeConfig.m_encoderLevelDesc.m_H264LevelSetting !=
       pD3D12Enc->m_currentEncodeCapabilities.m_encoderLevelSuggestedDesc.m_H264LevelSetting) {
      debug_printf("[d3d12_video_encoder_h264] Warning: Requested D3D12_VIDEO_ENCODER_LEVELS_H264 by upper layer: %d "
                    "mismatches UMD suggested D3D12_VIDEO_ENCODER_LEVELS_H264: %d\n",
                    pD3D12Enc->m_currentEncodeConfig.m_encoderLevelDesc.m_H264LevelSetting,
                    pD3D12Enc->m_currentEncodeCapabilities.m_encoderLevelSuggestedDesc.m_H264LevelSetting);
   }

   if (pD3D12Enc->m_currentEncodeCapabilities.m_MaxSlicesInOutput >
       pD3D12Enc->m_currentEncodeCapabilities.m_currentResolutionSupportCaps.MaxSubregionsNumber) {
      debug_printf("[d3d12_video_encoder_h264] Desired number of subregions %d is not supported (higher than max "
                      "reported slice number %d in query caps) for current resolution (%d, %d)\n.",
                      pD3D12Enc->m_currentEncodeCapabilities.m_MaxSlicesInOutput,
                      pD3D12Enc->m_currentEncodeCapabilities.m_currentResolutionSupportCaps.MaxSubregionsNumber,
                      pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Width,
                      pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Height);
      return false;
   }
   return true;
}

D3D12_VIDEO_ENCODER_PROFILE_H264
d3d12_video_encoder_convert_profile_to_d3d12_enc_profile_h264(enum pipe_video_profile profile)
{
   switch (profile) {
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
      {
         return D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;

      } break;
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
      {
         return D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
      } break;
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH10:
      {
         return D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH_10;
      } break;
      default:
      {
         unreachable("Unsupported pipe_video_profile");
      } break;
   }
}

bool
d3d12_video_encoder_compare_slice_config_h264_hevc(
   D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE targetMode,
   D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES targetConfig,
   D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE otherMode,
   D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES otherConfig)
{
   return (targetMode == otherMode) &&
          (memcmp(&targetConfig,
                  &otherConfig,
                  sizeof(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES)) == 0);
}

static inline bool
d3d12_video_encoder_needs_new_pps_h264(struct d3d12_video_encoder *pD3D12Enc,
                                       bool writeNewSPS,
                                       H264_PPS &tentative_pps,
                                       const H264_PPS &active_pps)
{
   bool bUseSliceL0L1Override = (pD3D12Enc->m_currentEncodeConfig.m_encoderPicParamsDesc.m_H264PicData.Flags &
                                 D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAG_REQUEST_NUM_REF_IDX_ACTIVE_OVERRIDE_FLAG_SLICE);

   bool bDifferentL0L1Lists = !bUseSliceL0L1Override &&
         ((tentative_pps.num_ref_idx_l0_active_minus1 != active_pps.num_ref_idx_l0_active_minus1) ||
         (tentative_pps.num_ref_idx_l1_active_minus1 != active_pps.num_ref_idx_l1_active_minus1));

   bool bDidPPSChange =
      ((tentative_pps.constrained_intra_pred_flag != active_pps.constrained_intra_pred_flag) ||
       (tentative_pps.entropy_coding_mode_flag != active_pps.entropy_coding_mode_flag) ||
       bDifferentL0L1Lists ||
       (tentative_pps.pic_order_present_flag != active_pps.pic_order_present_flag) ||
       (tentative_pps.pic_parameter_set_id != active_pps.pic_parameter_set_id) ||
       (tentative_pps.seq_parameter_set_id != active_pps.seq_parameter_set_id) ||
       (tentative_pps.transform_8x8_mode_flag != active_pps.transform_8x8_mode_flag));

   return writeNewSPS || bDidPPSChange;
}

uint32_t
d3d12_video_encoder_build_codec_headers_h264(struct d3d12_video_encoder *pD3D12Enc,
                                             std::vector<uint64_t> &pWrittenCodecUnitsSizes)
{
   D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA currentPicParams =
      d3d12_video_encoder_get_current_picture_param_settings(pD3D12Enc);

   auto levelDesc = d3d12_video_encoder_get_current_level_desc(pD3D12Enc);
   auto codecConfigDesc = d3d12_video_encoder_get_current_codec_config_desc(pD3D12Enc);

   d3d12_video_bitstream_builder_h264 *pH264BitstreamBuilder =
      static_cast<d3d12_video_bitstream_builder_h264 *>(pD3D12Enc->m_upBitstreamBuilder.get());
   assert(pH264BitstreamBuilder);

   size_t writtenAUDBytesCount = 0;
   pWrittenCodecUnitsSizes.clear();

   bool forceWriteAUD = (pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags & d3d12_video_encoder_config_dirty_flag_aud_header);
   if (forceWriteAUD)
   {
      pH264BitstreamBuilder->write_aud(pD3D12Enc->m_BitstreamHeadersBuffer,
                                       pD3D12Enc->m_BitstreamHeadersBuffer.begin(),
                                       writtenAUDBytesCount);
      pWrittenCodecUnitsSizes.push_back(writtenAUDBytesCount);
   }

   bool isFirstFrame = (pD3D12Enc->m_fenceValue == 1);
   bool forceWriteSPS = (pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags & d3d12_video_encoder_config_dirty_flag_sequence_header);
   bool writeNewSPS = isFirstFrame                                         // on first frame
                      || ((pD3D12Enc->m_currentEncodeConfig.m_seqFlags &   // also on resolution change
                           D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_RESOLUTION_CHANGE) != 0)
                      || forceWriteSPS;

   uint32_t active_seq_parameter_set_id = pH264BitstreamBuilder->get_active_sps().seq_parameter_set_id;

   size_t writtenSEIBytesCount = 0;
   bool forceWriteSEI = (pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags & d3d12_video_encoder_config_dirty_flag_sei_header);
   // We only support H264_SEI_SCALABILITY_INFO, so check num_temporal_layers > 1
   if (forceWriteSEI &&
      (pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificSequenceStateDescH264.num_temporal_layers > 1))
   {
      std::vector<H264_SEI_MESSAGE> sei_messages;
      H264_SEI_MESSAGE scalability_info_sei = { };
      memset(&scalability_info_sei, 0, sizeof(scalability_info_sei));
      scalability_info_sei.payload_type = H264_SEI_SCALABILITY_INFO;
      scalability_info_sei.scalability_info.num_layers_minus1 = pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificSequenceStateDescH264.num_temporal_layers - 1;
      // Only support Identity temporal_id for now
      for (uint32_t i = 0; i <= scalability_info_sei.scalability_info.num_layers_minus1; i++)
         scalability_info_sei.scalability_info.temporal_id[i] = i;

      sei_messages.push_back(scalability_info_sei);
      pH264BitstreamBuilder->write_sei_messages(sei_messages,
                                                pD3D12Enc->m_BitstreamHeadersBuffer,
                                                pD3D12Enc->m_BitstreamHeadersBuffer.begin() + writtenAUDBytesCount,
                                                writtenSEIBytesCount);
      pWrittenCodecUnitsSizes.push_back(writtenSEIBytesCount);
   }

   size_t writtenSPSBytesCount = 0;
   if (writeNewSPS) {
      H264_SPS sps = pH264BitstreamBuilder->build_sps(pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificSequenceStateDescH264,
                                                      pD3D12Enc->base.profile,
                                                      *levelDesc.pH264LevelSetting,
                                                      pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo.Format,
                                                      *codecConfigDesc.pH264Config,
                                                      pD3D12Enc->m_currentEncodeConfig.m_encoderGOPConfigDesc.m_H264GroupOfPictures,
                                                      active_seq_parameter_set_id,
                                                      pD3D12Enc->m_currentEncodeConfig.m_currentResolution,
                                                      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig,
                                                      pD3D12Enc->m_BitstreamHeadersBuffer,
                                                      pD3D12Enc->m_BitstreamHeadersBuffer.begin() + writtenAUDBytesCount + writtenSEIBytesCount,
                                                      writtenSPSBytesCount);
      pH264BitstreamBuilder->set_active_sps(sps);
      pWrittenCodecUnitsSizes.push_back(writtenSPSBytesCount);
   }

   size_t writtenPPSBytesCount = 0;
   H264_PPS tentative_pps = pH264BitstreamBuilder->build_pps(pD3D12Enc->base.profile,
                                                             *codecConfigDesc.pH264Config,
                                                             *currentPicParams.pH264PicData,
                                                             currentPicParams.pH264PicData->pic_parameter_set_id,
                                                             active_seq_parameter_set_id,
                                                             pD3D12Enc->m_StagingHeadersBuffer,
                                                             pD3D12Enc->m_StagingHeadersBuffer.begin(),
                                                             writtenPPSBytesCount);

   const H264_PPS &active_pps = pH264BitstreamBuilder->get_active_pps();
   bool forceWritePPS = (pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags & d3d12_video_encoder_config_dirty_flag_picture_header);
   if (forceWritePPS || d3d12_video_encoder_needs_new_pps_h264(pD3D12Enc, writeNewSPS, tentative_pps, active_pps)) {
      pH264BitstreamBuilder->set_active_pps(tentative_pps);
      pD3D12Enc->m_BitstreamHeadersBuffer.resize(writtenAUDBytesCount + writtenSEIBytesCount + writtenSPSBytesCount + writtenPPSBytesCount);
      memcpy(&pD3D12Enc->m_BitstreamHeadersBuffer.data()[writtenAUDBytesCount + writtenSEIBytesCount + writtenSPSBytesCount], pD3D12Enc->m_StagingHeadersBuffer.data(), writtenPPSBytesCount);
      pWrittenCodecUnitsSizes.push_back(writtenPPSBytesCount);
   } else {
      writtenPPSBytesCount = 0;
      debug_printf("Skipping PPS (same as active PPS) for fenceValue: %" PRIu64 "\n", pD3D12Enc->m_fenceValue);
   }

   // Shrink buffer to fit the headers
   if (pD3D12Enc->m_BitstreamHeadersBuffer.size() > (writtenAUDBytesCount + writtenSEIBytesCount + writtenSPSBytesCount + writtenPPSBytesCount)) {
      pD3D12Enc->m_BitstreamHeadersBuffer.resize(writtenAUDBytesCount + writtenSEIBytesCount + writtenSPSBytesCount + writtenPPSBytesCount);
   }

   assert(std::accumulate(pWrittenCodecUnitsSizes.begin(), pWrittenCodecUnitsSizes.end(), 0u) ==
      static_cast<uint64_t>(pD3D12Enc->m_BitstreamHeadersBuffer.size()));
   return pD3D12Enc->m_BitstreamHeadersBuffer.size();
}

uint32_t
d3d12_video_encoder_build_slice_svc_prefix_nalu_h264(struct d3d12_video_encoder *   pD3D12Enc,
                                                     EncodedBitstreamResolvedMetadata& associatedMetadata,
                                                     std::vector<uint8_t> &         headerBitstream,
                                                     std::vector<uint8_t>::iterator placingPositionStart,
                                                     size_t &                       writtenSVCPrefixNalBytes)
{
   d3d12_video_bitstream_builder_h264 *pH264BitstreamBuilder =
      static_cast<d3d12_video_bitstream_builder_h264 *>(pD3D12Enc->m_upBitstreamBuilder.get());
   assert(pH264BitstreamBuilder);

   H264_SLICE_PREFIX_SVC nal_svc_prefix = {};
   memset(&nal_svc_prefix, 0, sizeof(nal_svc_prefix));
   nal_svc_prefix.idr_flag = (associatedMetadata.m_associatedEncodeConfig.m_encoderPicParamsDesc.m_H264PicData.FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME) ? 1 : 0;
   nal_svc_prefix.no_inter_layer_pred_flag = 1;
   nal_svc_prefix.output_flag = 1;
   nal_svc_prefix.discardable_flag = 1;
   nal_svc_prefix.temporal_id = associatedMetadata.m_associatedEncodeConfig.m_encoderPicParamsDesc.m_H264PicData.TemporalLayerIndex;
   nal_svc_prefix.priority_id = nal_svc_prefix.temporal_id;
   nal_svc_prefix.nal_ref_idc = associatedMetadata.m_associatedEncodeConfig.m_bUsedAsReference ? NAL_REFIDC_REF : NAL_REFIDC_NONREF;
   pH264BitstreamBuilder->write_slice_svc_prefix(nal_svc_prefix,
                                                 headerBitstream,
                                                 placingPositionStart,
                                                 writtenSVCPrefixNalBytes);

   // Shrink buffer to fit the headers
   if (headerBitstream.size() > (writtenSVCPrefixNalBytes)) {
      headerBitstream.resize(writtenSVCPrefixNalBytes);
   }

   return headerBitstream.size();
}