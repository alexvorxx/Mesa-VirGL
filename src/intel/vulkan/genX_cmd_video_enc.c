/*
 * Copyright © 2024 Igalia
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "anv_private.h"

#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

static int
anv_get_max_vmv_range(StdVideoH264LevelIdc level)
{
   int max_vmv_range;

   switch(level) {
   case STD_VIDEO_H264_LEVEL_IDC_1_0:
      max_vmv_range = 256;
      break;
   case STD_VIDEO_H264_LEVEL_IDC_1_1:
   case STD_VIDEO_H264_LEVEL_IDC_1_2:
   case STD_VIDEO_H264_LEVEL_IDC_1_3:
   case STD_VIDEO_H264_LEVEL_IDC_2_0:
      max_vmv_range = 512;
      break;
   case STD_VIDEO_H264_LEVEL_IDC_2_1:
   case STD_VIDEO_H264_LEVEL_IDC_2_2:
   case STD_VIDEO_H264_LEVEL_IDC_3_0:
      max_vmv_range = 1024;
      break;

   case STD_VIDEO_H264_LEVEL_IDC_3_1:
   case STD_VIDEO_H264_LEVEL_IDC_3_2:
   case STD_VIDEO_H264_LEVEL_IDC_4_0:
   case STD_VIDEO_H264_LEVEL_IDC_4_1:
   case STD_VIDEO_H264_LEVEL_IDC_4_2:
   case STD_VIDEO_H264_LEVEL_IDC_5_0:
   case STD_VIDEO_H264_LEVEL_IDC_5_1:
   case STD_VIDEO_H264_LEVEL_IDC_5_2:
   case STD_VIDEO_H264_LEVEL_IDC_6_0:
   case STD_VIDEO_H264_LEVEL_IDC_6_1:
   case STD_VIDEO_H264_LEVEL_IDC_6_2:
   default:
      max_vmv_range = 2048;
      break;
   }

   return max_vmv_range;
}

static bool
anv_post_deblock_enable(const StdVideoH264PictureParameterSet *pps, const VkVideoEncodeH264PictureInfoKHR *frame_info)
{

   if (!pps->flags.deblocking_filter_control_present_flag)
      return true;

   for (uint32_t slice_id = 0; slice_id < frame_info->naluSliceEntryCount; slice_id++) {
      const VkVideoEncodeH264NaluSliceInfoKHR *nalu = &frame_info->pNaluSliceEntries[slice_id];
      const StdVideoEncodeH264SliceHeader *slice_header = nalu->pStdSliceHeader;

      if (slice_header->disable_deblocking_filter_idc != 1)
         return true;
   }

   return false;
}

static uint8_t
anv_vdenc_h264_picture_type(StdVideoH264PictureType pic_type)
{
   if (pic_type == STD_VIDEO_H264_PICTURE_TYPE_I || pic_type == STD_VIDEO_H264_PICTURE_TYPE_IDR) {
      return 0;
   } else {
      return 1;
   }
}

static const uint8_t vdenc_const_qp_lambda[42] = {
   0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02,
   0x02, 0x03, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x07,
   0x07, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0f, 0x11, 0x13, 0x15,
   0x17, 0x1a, 0x1e, 0x21, 0x25, 0x2a, 0x2f, 0x35, 0x3b, 0x42,
   0x4a, 0x53,
};

/* P frame */
static const uint8_t vdenc_const_qp_lambda_p[42] = {
   0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02,
   0x02, 0x03, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x07,
   0x07, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0f, 0x11, 0x13, 0x15,
   0x17, 0x1a, 0x1e, 0x21, 0x25, 0x2a, 0x2f, 0x35, 0x3b, 0x42,
   0x4a, 0x53,
};

static const uint16_t vdenc_const_skip_threshold_p[27] = {
   0x0000, 0x0000, 0x0000, 0x0000, 0x0002, 0x0004, 0x0007, 0x000b,
   0x0011, 0x0019, 0x0023, 0x0032, 0x0044, 0x005b, 0x0077, 0x0099,
   0x00c2, 0x00f1, 0x0128, 0x0168, 0x01b0, 0x0201, 0x025c, 0x02c2,
   0x0333, 0x03b0, 0x0000,
};

static const uint16_t vdenc_const_sic_forward_transform_coeff_threshold_0_p[27] = {
   0x02, 0x02, 0x03, 0x04, 0x04, 0x05, 0x07, 0x09, 0x0b, 0x0e,
   0x12, 0x14, 0x18, 0x1d, 0x20, 0x25, 0x2a, 0x34, 0x39, 0x3f,
   0x4e, 0x51, 0x5b, 0x63, 0x6f, 0x7f, 0x00,
};

static const uint8_t vdenc_const_sic_forward_transform_coeff_threshold_1_p[27] = {
   0x03, 0x04, 0x05, 0x05, 0x07, 0x09, 0x0b, 0x0e, 0x12, 0x17,
   0x1c, 0x21, 0x27, 0x2c, 0x33, 0x3b, 0x41, 0x51, 0x5c, 0x1a,
   0x1e, 0x21, 0x22, 0x26, 0x2c, 0x30, 0x00,
};

static const uint8_t vdenc_const_sic_forward_transform_coeff_threshold_2_p[27] = {
   0x02, 0x02, 0x03, 0x04, 0x04, 0x05, 0x07, 0x09, 0x0b, 0x0e,
   0x12, 0x14, 0x18, 0x1d, 0x20, 0x25, 0x2a, 0x34, 0x39, 0x0f,
   0x13, 0x14, 0x16, 0x18, 0x1b, 0x1f, 0x00,
};

static const uint8_t vdenc_const_sic_forward_transform_coeff_threshold_3_p[27] = {
   0x04, 0x05, 0x06, 0x09, 0x0b, 0x0d, 0x12, 0x16, 0x1b, 0x23,
   0x2c, 0x33, 0x3d, 0x45, 0x4f, 0x5b, 0x66, 0x7f, 0x8e, 0x2a,
   0x2f, 0x32, 0x37, 0x3c, 0x45, 0x4c, 0x00,
};

static void
anv_h264_encode_video(struct anv_cmd_buffer *cmd, const VkVideoEncodeInfoKHR *enc_info)
{
   ANV_FROM_HANDLE(anv_buffer, dst_buffer, enc_info->dstBuffer);

   struct anv_video_session *vid = cmd->video.vid;
   struct anv_video_session_params *params = cmd->video.params;

   const struct VkVideoEncodeH264PictureInfoKHR *frame_info =
      vk_find_struct_const(enc_info->pNext, VIDEO_ENCODE_H264_PICTURE_INFO_KHR);

   const StdVideoH264SequenceParameterSet *sps = vk_video_find_h264_enc_std_sps(&params->vk, frame_info->pStdPictureInfo->seq_parameter_set_id);
   const StdVideoH264PictureParameterSet *pps = vk_video_find_h264_enc_std_pps(&params->vk, frame_info->pStdPictureInfo->pic_parameter_set_id);
   const StdVideoEncodeH264ReferenceListsInfo *ref_list_info = frame_info->pStdPictureInfo->pRefLists;

   const struct anv_image_view *iv = anv_image_view_from_handle(enc_info->srcPictureResource.imageViewBinding);
   const struct anv_image *src_img = iv->image;
   bool post_deblock_enable = anv_post_deblock_enable(pps, frame_info);
   bool rc_disable = cmd->video.params->rc_mode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
   uint8_t dpb_idx[ANV_VIDEO_H264_MAX_NUM_REF_FRAME] = { 0,};

   const struct anv_image_view *base_ref_iv;
   if (enc_info->pSetupReferenceSlot) {
      base_ref_iv = anv_image_view_from_handle(enc_info->pSetupReferenceSlot->pPictureResource->imageViewBinding);
   } else {
      base_ref_iv = iv;
   }

   const struct anv_image *base_ref_img = base_ref_iv->image;

   anv_batch_emit(&cmd->batch, GENX(MI_FLUSH_DW), flush) {
      flush.VideoPipelineCacheInvalidate = 1;
   };

#if GFX_VER >= 12
   anv_batch_emit(&cmd->batch, GENX(MI_FORCE_WAKEUP), wake) {
      wake.MFXPowerWellControl = 1;
      wake.MaskBits = 768;
   }

   anv_batch_emit(&cmd->batch, GENX(MFX_WAIT), mfx) {
      mfx.MFXSyncControlFlag = 1;
   }
#endif

   anv_batch_emit(&cmd->batch, GENX(MFX_PIPE_MODE_SELECT), pipe_mode) {
      pipe_mode.StandardSelect = SS_AVC;
      pipe_mode.CodecSelect = Encode;
      pipe_mode.FrameStatisticsStreamOutEnable = true;
      pipe_mode.ScaledSurfaceEnable = false;
      pipe_mode.PreDeblockingOutputEnable = !post_deblock_enable;
      pipe_mode.PostDeblockingOutputEnable = post_deblock_enable;
      pipe_mode.StreamOutEnable = false;
      pipe_mode.VDEncMode = VM_VDEncMode;
      pipe_mode.DecoderShortFormatMode = LongFormatDriverInterface;
   }

#if GFX_VER >= 12
   anv_batch_emit(&cmd->batch, GENX(MFX_WAIT), mfx) {
      mfx.MFXSyncControlFlag = 1;
   }
#endif

   for (uint32_t i = 0; i < 2; i++) {
      anv_batch_emit(&cmd->batch, GENX(MFX_SURFACE_STATE), surface) {
         const struct anv_image *img_ = i == 0 ? base_ref_img : src_img;

         surface.Width = img_->vk.extent.width - 1;
         surface.Height = img_->vk.extent.height - 1;
         /* TODO. add a surface for MFX_ReconstructedScaledReferencePicture */
         surface.SurfaceID = i == 0 ? MFX_ReferencePicture : MFX_SourceInputPicture;
         surface.TileWalk = TW_YMAJOR;
         surface.TiledSurface = img_->planes[0].primary_surface.isl.tiling != ISL_TILING_LINEAR;
         surface.SurfacePitch = img_->planes[0].primary_surface.isl.row_pitch_B - 1;
         surface.InterleaveChroma = true;
         surface.SurfaceFormat = MFX_PLANAR_420_8;

         surface.YOffsetforUCb = img_->planes[1].primary_surface.memory_range.offset /
            img_->planes[0].primary_surface.isl.row_pitch_B;
         surface.YOffsetforVCr = img_->planes[1].primary_surface.memory_range.offset /
            img_->planes[0].primary_surface.isl.row_pitch_B;
      }
   }

   anv_batch_emit(&cmd->batch, GENX(MFX_PIPE_BUF_ADDR_STATE), buf) {
      if (post_deblock_enable) {
         buf.PostDeblockingDestinationAddress =
            anv_image_address(base_ref_img, &base_ref_img->planes[0].primary_surface.memory_range);
      } else {
         buf.PreDeblockingDestinationAddress =
            anv_image_address(base_ref_img, &base_ref_img->planes[0].primary_surface.memory_range);
      }
      buf.PreDeblockingDestinationAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, buf.PreDeblockingDestinationAddress.bo, 0),
      };
      buf.PostDeblockingDestinationAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, buf.PostDeblockingDestinationAddress.bo, 0),
      };

      buf.OriginalUncompressedPictureSourceAddress =
         anv_image_address(src_img, &src_img->planes[0].primary_surface.memory_range);
      buf.OriginalUncompressedPictureSourceAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, buf.OriginalUncompressedPictureSourceAddress.bo, 0),
      };

      buf.StreamOutDataDestinationAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      buf.IntraRowStoreScratchBufferAddress = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_INTRA_ROW_STORE].offset
      };
      buf.IntraRowStoreScratchBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, buf.IntraRowStoreScratchBufferAddress.bo, 0),
      };

      buf.DeblockingFilterRowStoreScratchAddress = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_DEBLOCK_FILTER_ROW_STORE].offset
      };
      buf.DeblockingFilterRowStoreScratchAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, buf.DeblockingFilterRowStoreScratchAddress.bo, 0),
      };

      struct anv_bo *ref_bo = NULL;

      for (unsigned i = 0; i < enc_info->referenceSlotCount; i++) {
         const struct anv_image_view *ref_iv =
            anv_image_view_from_handle(enc_info->pReferenceSlots[i].pPictureResource->imageViewBinding);
         int slot_idx = enc_info->pReferenceSlots[i].slotIndex;
         assert(slot_idx < ANV_VIDEO_H264_MAX_NUM_REF_FRAME);

         dpb_idx[slot_idx] = i;

         buf.ReferencePictureAddress[i] =
            anv_image_address(ref_iv->image, &ref_iv->image->planes[0].primary_surface.memory_range);

         if (i == 0)
            ref_bo = ref_iv->image->bindings[0].address.bo;
      }

      buf.ReferencePictureAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, ref_bo, 0),
      };

      buf.MBStatusBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      buf.MBILDBStreamOutBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      buf.SecondMBILDBStreamOutBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      /* TODO. Add for scaled reference surface */
      buf.ScaledReferenceSurfaceAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, buf.ScaledReferenceSurfaceAddress.bo, 0),
      };
   }

   anv_batch_emit(&cmd->batch, GENX(MFX_IND_OBJ_BASE_ADDR_STATE), index_obj) {
      index_obj.MFXIndirectBitstreamObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      index_obj.MFXIndirectMVObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      index_obj.MFDIndirectITCOEFFObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      index_obj.MFDIndirectITDBLKObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      index_obj.MFCIndirectPAKBSEObjectAddress = anv_address_add(dst_buffer->address, 0);

      index_obj.MFCIndirectPAKBSEObjectAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, index_obj.MFCIndirectPAKBSEObjectAddress.bo, 0),
      };
   }

   anv_batch_emit(&cmd->batch, GENX(MFX_BSP_BUF_BASE_ADDR_STATE), bsp) {
      bsp.BSDMPCRowStoreScratchBufferAddress = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_BSD_MPC_ROW_SCRATCH].offset
      };

      bsp.BSDMPCRowStoreScratchBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, bsp.BSDMPCRowStoreScratchBufferAddress.bo, 0),
      };

      bsp.MPRRowStoreScratchBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      bsp.BitplaneReadBufferAttributes = (struct GENX(MEMORYADDRESSATTRIBUTES)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
   }

   anv_batch_emit(&cmd->batch, GENX(VDENC_PIPE_MODE_SELECT), vdenc_pipe_mode) {
      vdenc_pipe_mode.StandardSelect = SS_AVC;
      vdenc_pipe_mode.PAKChromaSubSamplingType = _420;
#if GFX_VER >= 12
      //vdenc_pipe_mode.HMERegionPrefetchEnable = !vdenc_pipe_mode.TLBPrefetchEnable;
      vdenc_pipe_mode.SourceLumaPackedDataTLBPrefetchEnable = true;
      vdenc_pipe_mode.SourceChromaTLBPrefetchEnable = true;
      vdenc_pipe_mode.HzShift32Minus1Src = 3;
      vdenc_pipe_mode.PrefetchOffsetforSource = 4;
#endif
   }

   anv_batch_emit(&cmd->batch, GENX(VDENC_SRC_SURFACE_STATE), vdenc_surface) {
      vdenc_surface.SurfaceState.Width = src_img->vk.extent.width - 1;
      vdenc_surface.SurfaceState.Height = src_img->vk.extent.height - 1;
      vdenc_surface.SurfaceState.SurfaceFormat = VDENC_PLANAR_420_8;
      vdenc_surface.SurfaceState.SurfacePitch = src_img->planes[0].primary_surface.isl.row_pitch_B - 1;

#if GFX_VER == 9
      vdenc_surface.SurfaceState.InterleaveChroma = true;
#endif

      vdenc_surface.SurfaceState.TileWalk = TW_YMAJOR;
      vdenc_surface.SurfaceState.TiledSurface = src_img->planes[0].primary_surface.isl.tiling != ISL_TILING_LINEAR;
      vdenc_surface.SurfaceState.YOffsetforUCb = src_img->planes[1].primary_surface.memory_range.offset /
         src_img->planes[0].primary_surface.isl.row_pitch_B;
      vdenc_surface.SurfaceState.YOffsetforVCr = src_img->planes[1].primary_surface.memory_range.offset /
         src_img->planes[0].primary_surface.isl.row_pitch_B;
      vdenc_surface.SurfaceState.Colorspaceselection = 1;
   }

   anv_batch_emit(&cmd->batch, GENX(VDENC_REF_SURFACE_STATE), vdenc_surface) {
      vdenc_surface.SurfaceState.Width = base_ref_img->vk.extent.width - 1;
      vdenc_surface.SurfaceState.Height = base_ref_img->vk.extent.height - 1;
      vdenc_surface.SurfaceState.SurfaceFormat = VDENC_PLANAR_420_8;
#if GFX_VER == 9
      vdenc_surface.SurfaceState.InterleaveChroma = true;
#endif
      vdenc_surface.SurfaceState.SurfacePitch = base_ref_img->planes[0].primary_surface.isl.row_pitch_B - 1;

      vdenc_surface.SurfaceState.TileWalk = TW_YMAJOR;
      vdenc_surface.SurfaceState.TiledSurface = base_ref_img->planes[0].primary_surface.isl.tiling != ISL_TILING_LINEAR;
      vdenc_surface.SurfaceState.YOffsetforUCb = base_ref_img->planes[1].primary_surface.memory_range.offset /
         base_ref_img->planes[0].primary_surface.isl.row_pitch_B;
      vdenc_surface.SurfaceState.YOffsetforVCr = base_ref_img->planes[1].primary_surface.memory_range.offset /
         base_ref_img->planes[0].primary_surface.isl.row_pitch_B;
   }

   /* TODO. add a cmd for VDENC_DS_REF_SURFACE_STATE */

   anv_batch_emit(&cmd->batch, GENX(VDENC_PIPE_BUF_ADDR_STATE), vdenc_buf) {
      /* TODO. add DSFWDREF and FWDREF */
      vdenc_buf.DSFWDREF0.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      vdenc_buf.DSFWDREF1.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      vdenc_buf.OriginalUncompressedPicture.Address =
         anv_image_address(src_img, &src_img->planes[0].primary_surface.memory_range);
      vdenc_buf.OriginalUncompressedPicture.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, vdenc_buf.OriginalUncompressedPicture.Address.bo, 0),
      };

      vdenc_buf.StreamInDataPicture.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      vdenc_buf.RowStoreScratchBuffer.Address = (struct anv_address) {
         vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].mem->bo,
         vid->vid_mem[ANV_VID_MEM_H264_MPR_ROW_SCRATCH].offset
      };

      vdenc_buf.RowStoreScratchBuffer.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, vdenc_buf.RowStoreScratchBuffer.Address.bo, 0),
      };

      const struct anv_image_view *ref_iv[2] = { 0, };
      for (unsigned i = 0; i < enc_info->referenceSlotCount && i < 2; i++)
         ref_iv[i] = anv_image_view_from_handle(enc_info->pReferenceSlots[i].pPictureResource->imageViewBinding);

      if (ref_iv[0]) {
         vdenc_buf.ColocatedMVReadBuffer.Address =
               anv_image_address(ref_iv[0]->image, &ref_iv[0]->image->vid_dmv_top_surface);
         vdenc_buf.FWDREF0.Address =
               anv_image_address(ref_iv[0]->image, &ref_iv[0]->image->planes[0].primary_surface.memory_range);
      }

      vdenc_buf.ColocatedMVReadBuffer.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, vdenc_buf.ColocatedMVReadBuffer.Address.bo, 0),
      };

      vdenc_buf.FWDREF0.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, vdenc_buf.FWDREF0.Address.bo, 0),
      };

      if (ref_iv[1])
         vdenc_buf.FWDREF1.Address =
               anv_image_address(ref_iv[1]->image, &ref_iv[1]->image->planes[0].primary_surface.memory_range);

      vdenc_buf.FWDREF1.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, vdenc_buf.FWDREF1.Address.bo, 0),
      };

      vdenc_buf.FWDREF2.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      vdenc_buf.BWDREF0.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

      vdenc_buf.VDEncStatisticsStreamOut.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };

#if GFX_VER >= 11
      vdenc_buf.DSFWDREF04X.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.DSFWDREF14X.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.VDEncCURecordStreamOutBuffer.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.VDEncLCUPAK_OBJ_CMDBuffer.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.ScaledReferenceSurface8X.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.ScaledReferenceSurface4X.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.VP9SegmentationMapStreamInBuffer.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.VP9SegmentationMapStreamOutBuffer.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
#endif
#if GFX_VER >= 12
      vdenc_buf.VDEncTileRowStoreBuffer.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.VDEncCumulativeCUCountStreamOutSurface.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
      vdenc_buf.VDEncPaletteModeStreamOutSurface.PictureFields = (struct GENX(VDENC_SURFACE_CONTROL_BITS)) {
         .MOCS = anv_mocs(cmd->device, NULL, 0),
      };
#endif
   }

   StdVideoH264PictureType pic_type;

   pic_type = frame_info->pStdPictureInfo->primary_pic_type;

   anv_batch_emit(&cmd->batch, GENX(VDENC_CONST_QPT_STATE), qpt) {
      if (pic_type == STD_VIDEO_H264_PICTURE_TYPE_IDR || pic_type == STD_VIDEO_H264_PICTURE_TYPE_I) {
         for (uint32_t i = 0; i < 42; i++) {
            qpt.QPLambdaArrayIndex[i] = vdenc_const_qp_lambda[i];
         }
      } else {
         for (uint32_t i = 0; i < 42; i++) {
            qpt.QPLambdaArrayIndex[i] = vdenc_const_qp_lambda_p[i];
         }

         for (uint32_t i = 0; i < 27; i++) {
            qpt.SkipThresholdArrayIndex[i] = vdenc_const_skip_threshold_p[i];
            qpt.SICForwardTransformCoeffThresholdMatrix0ArrayIndex[i] = vdenc_const_sic_forward_transform_coeff_threshold_0_p[i];
            qpt.SICForwardTransformCoeffThresholdMatrix135ArrayIndex[i] = vdenc_const_sic_forward_transform_coeff_threshold_1_p[i];
            qpt.SICForwardTransformCoeffThresholdMatrix2ArrayIndex[i] = vdenc_const_sic_forward_transform_coeff_threshold_2_p[i];
            qpt.SICForwardTransformCoeffThresholdMatrix46ArrayIndex[i] = vdenc_const_sic_forward_transform_coeff_threshold_3_p[i];
         }

         if (!pps->flags.transform_8x8_mode_flag) {
            for (uint32_t i = 0; i < 27; i++) {
               qpt.SkipThresholdArrayIndex[i] /= 2;
            }
         }
      }
   }

   anv_batch_emit(&cmd->batch, GENX(MFX_AVC_IMG_STATE), avc_img) {
      avc_img.FrameWidth = sps->pic_width_in_mbs_minus1;
      avc_img.FrameHeight = sps->pic_height_in_map_units_minus1;
      avc_img.FrameSize = (avc_img.FrameWidth + 1) * (avc_img.FrameHeight + 1);
      avc_img.ImageStructure = FramePicture;

      avc_img.WeightedBiPredictionIDC = pps->weighted_bipred_idc;
      avc_img.WeightedPredictionEnable = pps->flags.weighted_pred_flag;
      avc_img.RhoDomainRateControlEnable = false;
      avc_img.FirstChromaQPOffset = pps->chroma_qp_index_offset;
      avc_img.SecondChromaQPOffset = pps->second_chroma_qp_index_offset;

      avc_img.FieldPicture = false;
      avc_img.MBAFFMode = sps->flags.mb_adaptive_frame_field_flag;
      avc_img.FrameMBOnly = sps->flags.frame_mbs_only_flag;
      avc_img._8x8IDCTTransformMode = pps->flags.transform_8x8_mode_flag;
      avc_img.Direct8x8Inference = sps->flags.direct_8x8_inference_flag;
      avc_img.ConstrainedIntraPrediction = pps->flags.constrained_intra_pred_flag;
      avc_img.NonReferencePicture = false;
      avc_img.EntropyCodingSyncEnable = pps->flags.entropy_coding_mode_flag;
      avc_img.MBMVFormat = FOLLOW;
      avc_img.ChromaFormatIDC = sps->chroma_format_idc;
      avc_img.MVUnpackedEnable = true;

      avc_img.IntraMBMaxBitControl = true;
      avc_img.InterMBMaxBitControl = true;
      avc_img.FrameBitrateMaxReport = true;
      avc_img.FrameBitrateMinReport = true;
      avc_img.ForceIPCMControl = true;
      avc_img.TrellisQuantizationChromaDisable = true;

      avc_img.IntraMBConformanceMaxSize = 2700;
      avc_img.InterMBConformanceMaxSize = 4095;

      avc_img.FrameBitrateMin = 0;
      avc_img.FrameBitrateMinUnitMode = 1;
      avc_img.FrameBitrateMinUnit = 1;
      avc_img.FrameBitrateMax = (1 << 14) - 1;
      avc_img.FrameBitrateMaxUnitMode = 1;
      avc_img.FrameBitrateMaxUnit = 1;

      avc_img.NumberofReferenceFrames = enc_info->referenceSlotCount;
      if (pic_type != STD_VIDEO_H264_PICTURE_TYPE_IDR && pic_type != STD_VIDEO_H264_PICTURE_TYPE_I) {
         avc_img.NumberofActiveReferencePicturesfromL0 = pps->num_ref_idx_l0_default_active_minus1 + 1;
         avc_img.NumberofActiveReferencePicturesfromL1 = pps->num_ref_idx_l1_default_active_minus1 + 1;
      }
      avc_img.PicOrderPresent = pps->flags.bottom_field_pic_order_in_frame_present_flag;
      avc_img.DeltaPicOrderAlwaysZero = sps->flags.delta_pic_order_always_zero_flag;
      avc_img.PicOrderCountType = sps->pic_order_cnt_type;
      avc_img.DeblockingFilterControlPresent = pps->flags.deblocking_filter_control_present_flag;
      avc_img.RedundantPicCountPresent = pps->flags.redundant_pic_cnt_present_flag;
      avc_img.Log2MaxFrameNumber = sps->log2_max_frame_num_minus4;
      avc_img.Log2MaxPicOrderCountLSB = sps->log2_max_pic_order_cnt_lsb_minus4;
   }

   anv_batch_emit(&cmd->batch, GENX(VDENC_IMG_STATE), vdenc_img) {
      uint32_t slice_qp = 0;
      for (uint32_t slice_id = 0; slice_id < frame_info->naluSliceEntryCount; slice_id++) {
         const VkVideoEncodeH264NaluSliceInfoKHR *nalu = &frame_info->pNaluSliceEntries[slice_id];
         slice_qp = rc_disable ? nalu->constantQp : pps->pic_init_qp_minus26 + 26;
      }

      if (pic_type == STD_VIDEO_H264_PICTURE_TYPE_IDR || pic_type == STD_VIDEO_H264_PICTURE_TYPE_I) {
         vdenc_img.IntraSADMeasureAdjustment = 2;
         vdenc_img.SubMBSubPartitionMask = 0x70;
         vdenc_img.CREPrefetchEnable = true;
         vdenc_img.Mode0Cost = 10;
         vdenc_img.Mode1Cost = 0;
         vdenc_img.Mode2Cost = 3;
         vdenc_img.Mode3Cost = 30;

      } else {
         vdenc_img.BidirectionalWeight = 0x20;
         vdenc_img.SubPelMode = 3;
         vdenc_img.BmeDisableForFbrMessage = true;
         vdenc_img.InterSADMeasureAdjustment = 2;
         vdenc_img.IntraSADMeasureAdjustment = 2;
         vdenc_img.SubMBSubPartitionMask = 0x70;
         vdenc_img.CREPrefetchEnable = true;

         vdenc_img.NonSkipZeroMVCostAdded = 1;
         vdenc_img.NonSkipMBModeCostAdded = 1;
         vdenc_img.RefIDCostModeSelect = 1;

         vdenc_img.Mode0Cost = 7;
         vdenc_img.Mode1Cost = 26;
         vdenc_img.Mode2Cost = 30;
         vdenc_img.Mode3Cost = 57;
         vdenc_img.Mode4Cost = 8;
         vdenc_img.Mode5Cost = 2;
         vdenc_img.Mode6Cost = 4;
         vdenc_img.Mode7Cost = 6;
         vdenc_img.Mode8Cost = 5;
         vdenc_img.Mode9Cost = 0;
         vdenc_img.RefIDCost = 4;
         vdenc_img.ChromaIntraModeCost = 0;

         vdenc_img.MVCost.MV0Cost = 0;
         vdenc_img.MVCost.MV1Cost = 6;
         vdenc_img.MVCost.MV2Cost = 6;
         vdenc_img.MVCost.MV3Cost = 9;
         vdenc_img.MVCost.MV4Cost = 10;
         vdenc_img.MVCost.MV5Cost = 13;
         vdenc_img.MVCost.MV6Cost = 14;
         vdenc_img.MVCost.MV7Cost = 24;

         vdenc_img.SadHaarThreshold0 = 800;
         vdenc_img.SadHaarThreshold1 = 1600;
         vdenc_img.SadHaarThreshold2 = 2400;
      }

      vdenc_img.PenaltyforIntra16x16NonDCPrediction = 36;
      vdenc_img.PenaltyforIntra8x8NonDCPrediction = 12;
      vdenc_img.PenaltyforIntra4x4NonDCPrediction = 4;
      vdenc_img.MaxQP = 0x33;
      vdenc_img.MinQP = 0x0a;
      vdenc_img.MaxDeltaQP = 0x0f;
      vdenc_img.MaxHorizontalMVRange = 0x2000;
      vdenc_img.MaxVerticalMVRange = 0x200;
      vdenc_img.SmallMbSizeInWord = 0xff;
      vdenc_img.LargeMbSizeInWord = 0xff;

      vdenc_img.Transform8x8 = pps->flags.transform_8x8_mode_flag;
      vdenc_img.VDEncExtendedPAK_OBJ_CMDEnable = true;
      vdenc_img.PictureWidth = sps->pic_width_in_mbs_minus1 + 1;
      vdenc_img.ForwardTransformSkipCheckEnable = true;
      vdenc_img.BlockBasedSkipEnable = true;
      vdenc_img.PictureHeight = sps->pic_height_in_map_units_minus1;
      vdenc_img.PictureType = anv_vdenc_h264_picture_type(pic_type);
      vdenc_img.ConstrainedIntraPrediction = pps->flags.constrained_intra_pred_flag;

      if (pic_type == STD_VIDEO_H264_PICTURE_TYPE_P) {
         vdenc_img.HMERef1Disable =
            (ref_list_info->num_ref_idx_l1_active_minus1 + 1) == 1 ? true : false;
      }

      vdenc_img.SliceMBHeight = sps->pic_height_in_map_units_minus1;

      if (vdenc_img.Transform8x8) {
         vdenc_img.LumaIntraPartitionMask = 0;
      } else {
         vdenc_img.LumaIntraPartitionMask = (1 << 1);
      }

      vdenc_img.QpPrimeY = slice_qp;
      vdenc_img.MaxVerticalMVRange = anv_get_max_vmv_range(sps->level_idc);

      /* TODO. Update Mode/MV cost */
   }

   if (pps->flags.pic_scaling_matrix_present_flag) {
      /* TODO. */
      assert(0);
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = pps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = pps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_8x8_Intra_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            qm.ForwardQuantizerMatrix[q] = pps->pScalingLists->ScalingList8x8[0][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_8x8_Inter_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            qm.ForwardQuantizerMatrix[q] = pps->pScalingLists->ScalingList8x8[3][q];
      }
   } else if (sps->flags.seq_scaling_matrix_present_flag) {
      /* TODO. */
      assert(0);
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = sps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               qm.ForwardQuantizerMatrix[m * 16 + q] = sps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_8x8_Intra_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            qm.ForwardQuantizerMatrix[q] = sps->pScalingLists->ScalingList8x8[0][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.DWordLength = 16;
         qm.AVC = AVC_8x8_Inter_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            qm.ForwardQuantizerMatrix[q] = sps->pScalingLists->ScalingList8x8[3][q];
      }
   } else {
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned q = 0; q < 3 * 16; q++)
            qm.ForwardQuantizerMatrix[q] = 0x10;
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned q = 0; q < 3 * 16; q++)
            qm.ForwardQuantizerMatrix[q] = 0x10;
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.AVC = AVC_8x8_Intra_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            qm.ForwardQuantizerMatrix[q] = 0x10;
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_QM_STATE), qm) {
         qm.AVC = AVC_8x8_Inter_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            qm.ForwardQuantizerMatrix[q] = 0x10;
      }
   }

   if (pps->flags.pic_scaling_matrix_present_flag) {
      /* TODO. */
      assert(0);
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               fqm.QuantizerMatrix8x8[m * 16 + q] = pps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               fqm.QuantizerMatrix8x8[m * 16 + q] = pps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_8x8_Intra_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            fqm.QuantizerMatrix8x8[q] = pps->pScalingLists->ScalingList8x8[0][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_8x8_Inter_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            fqm.QuantizerMatrix8x8[q] = pps->pScalingLists->ScalingList8x8[3][q];
      }
   } else if (sps->flags.seq_scaling_matrix_present_flag) {
      /* TODO. */
      assert(0);
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               fqm.QuantizerMatrix8x8[m * 16 + q] = sps->pScalingLists->ScalingList4x4[m][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned m = 0; m < 3; m++)
            for (unsigned q = 0; q < 16; q++)
               fqm.QuantizerMatrix8x8[m * 16 + q] = sps->pScalingLists->ScalingList4x4[m + 3][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_8x8_Intra_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            fqm.QuantizerMatrix8x8[q] = sps->pScalingLists->ScalingList8x8[0][q];
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_8x8_Inter_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            fqm.QuantizerMatrix8x8[q] = sps->pScalingLists->ScalingList8x8[3][q];
      }
   } else {
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_4x4_Intra_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            if (q % 2 == 1)
              fqm.QuantizerMatrix8x8[q] = 0x10;
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_4x4_Inter_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            if (q % 2 == 1)
              fqm.QuantizerMatrix8x8[q] = 0x10;
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_8x8_Intra_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            if (q % 2 == 1)
               fqm.QuantizerMatrix8x8[q] = 0x10;
      }
      anv_batch_emit(&cmd->batch, GENX(MFX_FQM_STATE), fqm) {
         fqm.AVC = AVC_8x8_Inter_MATRIX;
         for (unsigned q = 0; q < 64; q++)
            if (q % 2 == 1)
               fqm.QuantizerMatrix8x8[q] = 0x10;
      }
   }

   for (uint32_t slice_id = 0; slice_id < frame_info->naluSliceEntryCount; slice_id++) {
      const VkVideoEncodeH264NaluSliceInfoKHR *nalu = &frame_info->pNaluSliceEntries[slice_id];
      const StdVideoEncodeH264SliceHeader *slice_header = nalu->pStdSliceHeader;
      const StdVideoEncodeH264SliceHeader *next_slice_header = NULL;

      bool is_last = (slice_id == frame_info->naluSliceEntryCount - 1);
      uint32_t slice_type = slice_header->slice_type % 5;
      uint32_t slice_qp = rc_disable ? nalu->constantQp : pps->pic_init_qp_minus26 + 26;

      if (!is_last)
         next_slice_header = slice_header + 1;

      if (slice_type != STD_VIDEO_H264_SLICE_TYPE_I) {
         anv_batch_emit(&cmd->batch, GENX(MFX_AVC_REF_IDX_STATE), ref) {
            ref.ReferencePictureListSelect = 0;

            for (uint32_t i = 0; i < ref_list_info->num_ref_idx_l0_active_minus1 + 1; i++) {
               const VkVideoReferenceSlotInfoKHR ref_slot = enc_info->pReferenceSlots[i];
               ref.ReferenceListEntry[i] = dpb_idx[ref_slot.slotIndex];
            }
         }
      }

      if (slice_type == STD_VIDEO_H264_SLICE_TYPE_B) {
         anv_batch_emit(&cmd->batch, GENX(MFX_AVC_REF_IDX_STATE), ref) {
            ref.ReferencePictureListSelect = 1;

            for (uint32_t i = 0; i < ref_list_info->num_ref_idx_l1_active_minus1 + 1; i++) {
               const VkVideoReferenceSlotInfoKHR ref_slot = enc_info->pReferenceSlots[i];
               ref.ReferenceListEntry[i] = dpb_idx[ref_slot.slotIndex];
            }
         }
      }

      if (pps->flags.weighted_pred_flag && slice_type == STD_VIDEO_H265_SLICE_TYPE_P) {
         /* TODO. */
         assert(0);
         anv_batch_emit(&cmd->batch, GENX(MFX_AVC_WEIGHTOFFSET_STATE), w) {
         }
      }

      if (pps->flags.weighted_pred_flag && slice_type == STD_VIDEO_H265_SLICE_TYPE_B) {
         /* TODO. */
         assert(0);
         anv_batch_emit(&cmd->batch, GENX(MFX_AVC_WEIGHTOFFSET_STATE), w) {
         }
      }

      const StdVideoEncodeH264WeightTable*      weight_table =  slice_header->pWeightTable;

      unsigned w_in_mb = align(src_img->vk.extent.width, ANV_MB_WIDTH) / ANV_MB_WIDTH;
      unsigned h_in_mb = align(src_img->vk.extent.height, ANV_MB_HEIGHT) / ANV_MB_HEIGHT;

      uint8_t slice_header_data[256] = { 0, };
      size_t slice_header_data_len_in_bytes = 0;
      vk_video_encode_h264_slice_header(frame_info->pStdPictureInfo,
                                        sps,
                                        pps,
                                        slice_header,
                                        slice_qp - (pps->pic_init_qp_minus26 + 26),
                                        &slice_header_data_len_in_bytes,
                                        &slice_header_data);
      uint32_t slice_header_data_len_in_bits = slice_header_data_len_in_bytes * 8;

      anv_batch_emit(&cmd->batch, GENX(MFX_AVC_SLICE_STATE), avc_slice) {
         avc_slice.SliceType = slice_type;

         if (slice_type != STD_VIDEO_H264_SLICE_TYPE_I && weight_table) {
            avc_slice.Log2WeightDenominatorLuma = weight_table->luma_log2_weight_denom;
            avc_slice.Log2WeightDenominatorChroma = weight_table->chroma_log2_weight_denom;
         }

         avc_slice.NumberofReferencePicturesinInterpredictionList0 =
            slice_type == STD_VIDEO_H264_SLICE_TYPE_I ? 0 : ref_list_info->num_ref_idx_l0_active_minus1 + 1;
         avc_slice.NumberofReferencePicturesinInterpredictionList1 =
            (slice_type == STD_VIDEO_H264_SLICE_TYPE_I ||
             slice_type == STD_VIDEO_H264_SLICE_TYPE_P) ? 0 : ref_list_info->num_ref_idx_l1_active_minus1 + 1;

         avc_slice.SliceAlphaC0OffsetDiv2 = slice_header->slice_alpha_c0_offset_div2 & 0x7;
         avc_slice.SliceBetaOffsetDiv2 = slice_header->slice_beta_offset_div2 & 0x7;
         avc_slice.SliceQuantizationParameter = slice_qp;
         avc_slice.CABACInitIDC = slice_header->cabac_init_idc;
         avc_slice.DisableDeblockingFilterIndicator =
            pps->flags.deblocking_filter_control_present_flag ? slice_header->disable_deblocking_filter_idc : 0;
         avc_slice.DirectPredictionType = slice_header->flags.direct_spatial_mv_pred_flag;

         avc_slice.SliceStartMBNumber = slice_header->first_mb_in_slice;
         avc_slice.SliceHorizontalPosition =
            slice_header->first_mb_in_slice % (w_in_mb);
         avc_slice.SliceVerticalPosition =
            slice_header->first_mb_in_slice / (w_in_mb);

         if (is_last) {
            avc_slice.NextSliceHorizontalPosition = 0;
            avc_slice.NextSliceVerticalPosition = h_in_mb;
         } else {
            avc_slice.NextSliceHorizontalPosition = next_slice_header->first_mb_in_slice % w_in_mb;
            avc_slice.NextSliceVerticalPosition = next_slice_header->first_mb_in_slice / w_in_mb;
         }

         avc_slice.SliceID = slice_id;
         avc_slice.CABACZeroWordInsertionEnable = 1;
         avc_slice.EmulationByteSliceInsertEnable = 1;
         avc_slice.SliceDataInsertionPresent = 1;
         avc_slice.HeaderInsertionPresent = 1;
         avc_slice.LastSliceGroup = is_last;
         avc_slice.RateControlCounterEnable = false;

         /* TODO. Available only when RateControlCounterEnable is true. */
         avc_slice.RateControlPanicType = CBPPanic;
         avc_slice.RateControlPanicEnable = false;
         avc_slice.RateControlTriggleMode = LooseRateControl;
         avc_slice.ResetRateControlCounter = true;
         avc_slice.IndirectPAKBSEDataStartAddress = enc_info->dstBufferOffset;

         avc_slice.RoundIntra = 5;
         avc_slice.RoundIntraEnable = true;
         /* TODO. Needs to get a different value of rounding inter under various conditions. */
         avc_slice.RoundInter = 2;
         avc_slice.RoundInterEnable = false;

         if (slice_type == STD_VIDEO_H264_SLICE_TYPE_P) {
            avc_slice.WeightedPredictionIndicator = pps->flags.weighted_pred_flag;
            avc_slice.NumberofReferencePicturesinInterpredictionList0 = ref_list_info->num_ref_idx_l0_active_minus1 + 1;
         } else if (slice_type == STD_VIDEO_H264_SLICE_TYPE_B) {
            avc_slice.WeightedPredictionIndicator = pps->weighted_bipred_idc;
            avc_slice.NumberofReferencePicturesinInterpredictionList0 = ref_list_info->num_ref_idx_l0_active_minus1 + 1;
            avc_slice.NumberofReferencePicturesinInterpredictionList1 = ref_list_info->num_ref_idx_l1_active_minus1 + 1;
         }
      }

      uint32_t length_in_dw, data_bits_in_last_dw;
      uint32_t *dw;

      /* Insert zero slice data */
      unsigned int insert_zero[] = { 0, };
      length_in_dw = 1;
      data_bits_in_last_dw = 8;

      dw = anv_batch_emitn(&cmd->batch, length_in_dw + 2, GENX(MFX_PAK_INSERT_OBJECT),
            .DataBitsInLastDW = data_bits_in_last_dw > 0 ? data_bits_in_last_dw : 32,
            .HeaderLengthExcludedFromSize =  ACCUMULATE);

      memcpy(dw + 2, insert_zero, length_in_dw * 4);

      slice_header_data_len_in_bits -= 8;

      length_in_dw = ALIGN(slice_header_data_len_in_bits, 32) >> 5;
      data_bits_in_last_dw = slice_header_data_len_in_bits & 0x1f;

      dw = anv_batch_emitn(&cmd->batch, length_in_dw + 2, GENX(MFX_PAK_INSERT_OBJECT),
               .LastHeader = true,
               .DataBitsInLastDW = data_bits_in_last_dw > 0 ? data_bits_in_last_dw : 32,
               .SliceHeaderIndicator = true,
               .HeaderLengthExcludedFromSize =  ACCUMULATE);

      memcpy(dw + 2, slice_header_data + 1, length_in_dw * 4);

      anv_batch_emit(&cmd->batch, GENX(VDENC_WEIGHTSOFFSETS_STATE), vdenc_offsets) {
         vdenc_offsets.WeightsForwardReference0 = 1;
         vdenc_offsets.WeightsForwardReference1 = 1;
         vdenc_offsets.WeightsForwardReference2 = 1;

      }

      anv_batch_emit(&cmd->batch, GENX(VDENC_WALKER_STATE), vdenc_walker) {
         vdenc_walker.NextSliceMBStartYPosition = h_in_mb;
         vdenc_walker.Log2WeightDenominatorLuma = weight_table ? weight_table->luma_log2_weight_denom : 0;
#if GFX_VER >= 12
         vdenc_walker.TileWidth = src_img->vk.extent.width - 1;
#endif
      }

      anv_batch_emit(&cmd->batch, GENX(VD_PIPELINE_FLUSH), flush) {
         flush.MFXPipelineDone = true;
         flush.VDENCPipelineDone = true;
         flush.VDCommandMessageParserDone = true;
         flush.VDENCPipelineCommandFlush = true;
      }
   }

   anv_batch_emit(&cmd->batch, GENX(MI_FLUSH_DW), flush) {
      flush.DWordLength = 2;
      flush.VideoPipelineCacheInvalidate = 1;
   };

}

void
genX(CmdEncodeVideoKHR)(VkCommandBuffer commandBuffer,
                        const VkVideoEncodeInfoKHR *pEncodeInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   switch (cmd_buffer->video.vid->vk.op) {
   case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
      anv_h264_encode_video(cmd_buffer, pEncodeInfo);
      break;
   default:
      assert(0);
   }
}
