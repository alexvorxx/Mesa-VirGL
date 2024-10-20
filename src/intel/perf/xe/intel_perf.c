/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "perf/xe/intel_perf.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "perf/intel_perf.h"
#include "intel_perf_common.h"
#include "intel/common/intel_gem.h"

#include "drm-uapi/xe_drm.h"

#define FIELD_PREP_ULL(_mask, _val) (((_val) << (ffsll(_mask) - 1)) & (_mask))

uint64_t xe_perf_get_oa_format(struct intel_perf_config *perf)
{
   uint64_t fmt;

   if (perf->devinfo->verx10 >= 200) {
      /* BSpec: 60942
       * PEC64u64
       */
      fmt = FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_FMT_TYPE, DRM_XE_OA_FMT_TYPE_PEC);
      fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SEL, 1);
      fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SIZE, 1);
      fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_BC_REPORT, 0);
   } else {
      /* BSpec: 52198
       * same as I915_OA_FORMAT_A24u40_A14u32_B8_C8 and
       * I915_OA_FORMAT_A32u40_A4u32_B8_C8 returned for gfx 125+ and gfx 120
       * respectively.
       */
      fmt = FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_FMT_TYPE, DRM_XE_OA_FMT_TYPE_OAG);
      fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SEL, 5);
      fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_COUNTER_SIZE, 0);
      fmt |= FIELD_PREP_ULL(DRM_XE_OA_FORMAT_MASK_BC_REPORT, 0);
   }

   return fmt;
}

bool
xe_oa_metrics_available(struct intel_perf_config *perf, int fd, bool use_register_snapshots)
{
   bool perf_oa_available = false;
   struct stat sb;

   /* The existence of this file implies that this Xe KMD version supports
    * observation interface.
    */
   if (stat("/proc/sys/dev/xe/observation_paranoid", &sb) == 0) {
      uint64_t paranoid = 1;

      /* Now we need to check if application has privileges to access observation
       * interface.
       *
       * TODO: this approach does not takes into account applications running
       * with CAP_PERFMON privileges.
       */
      read_file_uint64("/proc/sys/dev/xe/observation_paranoid", &paranoid);
      if (paranoid == 0 || geteuid() == 0)
         perf_oa_available = true;
   }

   if (!perf_oa_available)
      return perf_oa_available;

   perf->features_supported |= INTEL_PERF_FEATURE_HOLD_PREEMPTION;

   return perf_oa_available;
}

uint64_t
xe_add_config(struct intel_perf_config *perf, int fd,
              const struct intel_perf_registers *config,
              const char *guid)
{
   struct drm_xe_oa_config xe_config = {};
   struct drm_xe_observation_param observation_param = {
      .observation_type = DRM_XE_OBSERVATION_TYPE_OA,
      .observation_op = DRM_XE_OBSERVATION_OP_ADD_CONFIG,
      .param = (uintptr_t)&xe_config,
   };
   uint32_t *regs;
   int ret;

   memcpy(xe_config.uuid, guid, sizeof(xe_config.uuid));

   xe_config.n_regs = config->n_mux_regs + config->n_b_counter_regs + config->n_flex_regs;
   assert(xe_config.n_regs > 0);

   regs = malloc(sizeof(uint64_t) * xe_config.n_regs);
   xe_config.regs_ptr = (uintptr_t)regs;

   memcpy(regs, config->mux_regs, config->n_mux_regs * sizeof(uint64_t));
   regs += 2 * config->n_mux_regs;
   memcpy(regs, config->b_counter_regs, config->n_b_counter_regs * sizeof(uint64_t));
   regs += 2 * config->n_b_counter_regs;
   memcpy(regs, config->flex_regs, config->n_flex_regs * sizeof(uint64_t));

   ret = intel_ioctl(fd, DRM_IOCTL_XE_OBSERVATION, &observation_param);
   free((void*)(uintptr_t)xe_config.regs_ptr);
   return ret > 0 ? ret : 0;
}

void
xe_remove_config(struct intel_perf_config *perf, int fd, uint64_t config_id)
{
   struct drm_xe_observation_param observation_param = {
      .observation_type = DRM_XE_OBSERVATION_TYPE_OA,
      .observation_op = DRM_XE_OBSERVATION_OP_REMOVE_CONFIG,
      .param = (uintptr_t)&config_id,
   };

   intel_ioctl(fd, DRM_IOCTL_XE_OBSERVATION, &observation_param);
}

static void
oa_prop_set(struct drm_xe_ext_set_property *props, uint32_t *index,
            enum drm_xe_oa_property_id prop_id, uint64_t value)
{
   if (*index > 0)
      props[*index - 1].base.next_extension = (uintptr_t)&props[*index];

   props[*index].base.name = DRM_XE_OA_EXTENSION_SET_PROPERTY;
   props[*index].property = prop_id;
   props[*index].value = value;
   *index = *index + 1;
}

int
xe_perf_stream_open(struct intel_perf_config *perf_config, int drm_fd,
                    uint32_t exec_id, uint64_t metrics_set_id,
                    uint64_t report_format, uint64_t period_exponent,
                    bool hold_preemption, bool enable)
{
   struct drm_xe_ext_set_property props[DRM_XE_OA_PROPERTY_NO_PREEMPT + 1] = {};
   struct drm_xe_observation_param observation_param = {
      .observation_type = DRM_XE_OBSERVATION_TYPE_OA,
      .observation_op = DRM_XE_OBSERVATION_OP_STREAM_OPEN,
      .param = (uintptr_t)&props,
   };
   uint32_t i = 0;
   int fd, flags;

   if (exec_id)
      oa_prop_set(props, &i, DRM_XE_OA_PROPERTY_EXEC_QUEUE_ID, exec_id);
   oa_prop_set(props, &i, DRM_XE_OA_PROPERTY_OA_DISABLED, !enable);
   oa_prop_set(props, &i, DRM_XE_OA_PROPERTY_SAMPLE_OA, true);
   oa_prop_set(props, &i, DRM_XE_OA_PROPERTY_OA_METRIC_SET, metrics_set_id);
   oa_prop_set(props, &i, DRM_XE_OA_PROPERTY_OA_FORMAT, report_format);
   oa_prop_set(props, &i, DRM_XE_OA_PROPERTY_OA_PERIOD_EXPONENT, period_exponent);
   if (hold_preemption)
      oa_prop_set(props, &i, DRM_XE_OA_PROPERTY_NO_PREEMPT, hold_preemption);

   fd = intel_ioctl(drm_fd, DRM_IOCTL_XE_OBSERVATION, &observation_param);
   if (fd < 0)
      return fd;

   flags = fcntl(fd, F_GETFL, 0);
   flags |= O_CLOEXEC | O_NONBLOCK;
   if (fcntl(fd, F_SETFL, flags)) {
      close(fd);
      return -1;
   }

   return fd;
}

int
xe_perf_stream_set_state(int perf_stream_fd, bool enable)
{
   unsigned long uapi = enable ? DRM_XE_OBSERVATION_IOCTL_ENABLE :
                                 DRM_XE_OBSERVATION_IOCTL_DISABLE;

   return intel_ioctl(perf_stream_fd, uapi, 0);
}

int
xe_perf_stream_set_metrics_id(int perf_stream_fd, uint64_t metrics_set_id)
{
   struct drm_xe_ext_set_property prop = {};
   uint32_t index = 0;

   oa_prop_set(&prop, &index, DRM_XE_OA_PROPERTY_OA_METRIC_SET,
                 metrics_set_id);
   return intel_ioctl(perf_stream_fd, DRM_XE_OBSERVATION_IOCTL_CONFIG,
                      (void *)(uintptr_t)&prop);
}

static int
xe_perf_stream_read_error(int perf_stream_fd, uint8_t *buffer, size_t buffer_len)
{
   struct drm_xe_oa_stream_status status = {};
   struct intel_perf_record_header *header;
   int ret;

   ret = intel_ioctl(perf_stream_fd, DRM_XE_OBSERVATION_IOCTL_STATUS, &status);
   if (ret)
      return -errno;

   header = (struct intel_perf_record_header *)buffer;
   header->pad = 0;
   header->type = 0;
   header->size = sizeof(*header);
   ret = header->size;

   if (status.oa_status & INTEL_PERF_RECORD_TYPE_OA_BUFFER_LOST)
      header->type = INTEL_PERF_RECORD_TYPE_OA_BUFFER_LOST;
   else if (status.oa_status & DRM_XE_OASTATUS_REPORT_LOST)
      header->type = INTEL_PERF_RECORD_TYPE_OA_REPORT_LOST;
   else if (status.oa_status & DRM_XE_OASTATUS_COUNTER_OVERFLOW)
      header->type = INTEL_PERF_RECORD_TYPE_COUNTER_OVERFLOW;
   else if (status.oa_status & DRM_XE_OASTATUS_MMIO_TRG_Q_FULL)
      header->type = INTEL_PERF_RECORD_TYPE_MMIO_TRG_Q_FULL;
   else
      unreachable("missing");

   return header->type ? header->size : -1;
}

int
xe_perf_stream_read_samples(struct intel_perf_config *perf_config, int perf_stream_fd,
                            uint8_t *buffer, size_t buffer_len)
{
   const size_t sample_size = perf_config->oa_sample_size;
   const size_t sample_header_size = sample_size + sizeof(struct intel_perf_record_header);
   uint32_t num_samples = buffer_len / sample_header_size;
   const size_t max_bytes_read = num_samples * sample_size;
   uint8_t *offset, *offset_samples;
   int len, i;

   if (buffer_len < sample_header_size)
      return -ENOSPC;

   do {
      len = read(perf_stream_fd, buffer, max_bytes_read);
   } while (len < 0 && errno == EINTR);

   if (len <= 0) {
      if (errno == EIO)
         return xe_perf_stream_read_error(perf_stream_fd, buffer, buffer_len);

      return len < 0 ? -errno : 0;
   }

   num_samples = len / sample_size;
   offset = buffer;
   offset_samples = buffer + (buffer_len - len);
   /* move all samples to the end of buffer */
   memmove(offset_samples, buffer, len);

   /* setup header, then copy sample from the end of buffer */
   for (i = 0; i < num_samples; i++) {
      struct intel_perf_record_header *header = (struct intel_perf_record_header *)offset;

      /* TODO: also append REPORT_LOST and BUFFER_LOST */
      header->type = INTEL_PERF_RECORD_TYPE_SAMPLE;
      header->pad = 0;
      header->size = sample_header_size;
      offset += sizeof(*header);

      memmove(offset, offset_samples, sample_size);
      offset += sample_size;
      offset_samples += sample_size;
   }

   return offset - buffer;
}
