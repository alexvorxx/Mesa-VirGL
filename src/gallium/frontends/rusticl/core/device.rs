use crate::api::icd::*;
use crate::api::util::*;
use crate::core::format::*;
use crate::core::platform::*;
use crate::core::util::*;
use crate::core::version::*;
use crate::impl_cl_type_trait_base;

use mesa_rust::compiler::clc::*;
use mesa_rust::compiler::nir::*;
use mesa_rust::pipe::context::*;
use mesa_rust::pipe::device::load_screens;
use mesa_rust::pipe::fence::*;
use mesa_rust::pipe::resource::*;
use mesa_rust::pipe::screen::*;
use mesa_rust_gen::*;
use mesa_rust_util::math::SetBitIndices;
use mesa_rust_util::static_assert;
use rusticl_opencl_gen::*;

use std::cmp::max;
use std::cmp::min;
use std::collections::HashMap;
use std::convert::TryInto;
use std::env;
use std::ffi::CString;
use std::mem::transmute;
use std::os::raw::*;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::MutexGuard;

pub struct Device {
    pub base: CLObjectBase<CL_INVALID_DEVICE>,
    pub screen: Arc<PipeScreen>,
    pub cl_version: CLVersion,
    pub clc_version: CLVersion,
    pub clc_versions: Vec<cl_name_version>,
    pub custom: bool,
    pub embedded: bool,
    pub extension_string: String,
    pub extensions: Vec<cl_name_version>,
    pub spirv_extensions: Vec<CString>,
    pub clc_features: Vec<cl_name_version>,
    pub formats: HashMap<cl_image_format, HashMap<cl_mem_object_type, cl_mem_flags>>,
    pub lib_clc: NirShader,
    pub caps: DeviceCaps,
    helper_ctx: Mutex<PipeContext>,
    reusable_ctx: Mutex<Vec<PipeContext>>,
}

#[derive(Default)]
pub struct DeviceCaps {
    pub has_3d_image_writes: bool,
    pub has_images: bool,
    pub has_rw_images: bool,
    pub has_timestamp: bool,
    pub image_2d_size: u32,
    pub max_read_images: u32,
    pub max_write_images: u32,
    pub timer_resolution: u32,
}

impl DeviceCaps {
    fn new(screen: &PipeScreen) -> Self {
        let cap_timestamp = screen.param(pipe_cap::PIPE_CAP_QUERY_TIMESTAMP) != 0;
        let timer_resolution = screen.param(pipe_cap::PIPE_CAP_TIMER_RESOLUTION) as u32;

        let max_write_images =
            Self::shader_param(screen, pipe_shader_cap::PIPE_SHADER_CAP_MAX_SHADER_IMAGES) as u32;
        let max_read_images =
            Self::shader_param(screen, pipe_shader_cap::PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS) as u32;
        let image_2d_size = screen.param(pipe_cap::PIPE_CAP_MAX_TEXTURE_2D_SIZE) as u32;

        let has_images = screen.param(pipe_cap::PIPE_CAP_TEXTURE_SAMPLER_INDEPENDENT) != 0 &&
            screen.param(pipe_cap::PIPE_CAP_IMAGE_STORE_FORMATTED) != 0 &&
            // The minimum value is 8 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            max_read_images >= 8 &&
            // The minimum value is 8 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            max_write_images >= 8 &&
            // The minimum value is 2048 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            image_2d_size >= 2048;

        Self {
            has_images: has_images,
            has_timestamp: cap_timestamp && timer_resolution > 0,
            image_2d_size: has_images.then_some(image_2d_size).unwrap_or_default(),
            max_read_images: has_images.then_some(max_read_images).unwrap_or_default(),
            max_write_images: has_images.then_some(max_write_images).unwrap_or_default(),
            timer_resolution: timer_resolution,
            ..Default::default()
        }
    }

    fn shader_param(screen: &PipeScreen, cap: pipe_shader_cap) -> i32 {
        screen.shader_param(pipe_shader_type::PIPE_SHADER_COMPUTE, cap)
    }
}

pub trait HelperContextWrapper {
    #[must_use]
    fn exec<F>(&self, func: F) -> PipeFence
    where
        F: Fn(&HelperContext);

    fn create_compute_state(&self, nir: &NirShader, static_local_mem: u32) -> *mut c_void;
    fn delete_compute_state(&self, cso: *mut c_void);
    fn compute_state_info(&self, state: *mut c_void) -> pipe_compute_state_object_info;
    fn compute_state_subgroup_size(&self, state: *mut c_void, block: &[u32; 3]) -> u32;

    fn is_create_fence_fd_supported(&self) -> bool;
    fn import_fence(&self, fence_fd: &FenceFd) -> PipeFence;
}

pub struct HelperContext<'a> {
    lock: MutexGuard<'a, PipeContext>,
}

impl<'a> HelperContext<'a> {
    pub fn resource_copy_region(
        &self,
        src: &PipeResource,
        dst: &PipeResource,
        dst_offset: &[u32; 3],
        bx: &pipe_box,
    ) {
        self.lock.resource_copy_region(src, dst, dst_offset, bx);
    }

    pub fn buffer_subdata(
        &self,
        res: &PipeResource,
        offset: c_uint,
        data: *const c_void,
        size: c_uint,
    ) {
        self.lock.buffer_subdata(res, offset, data, size)
    }

    pub fn texture_subdata(
        &self,
        res: &PipeResource,
        bx: &pipe_box,
        data: *const c_void,
        stride: u32,
        layer_stride: usize,
    ) {
        self.lock
            .texture_subdata(res, bx, data, stride, layer_stride)
    }
}

impl<'a> HelperContextWrapper for HelperContext<'a> {
    fn exec<F>(&self, func: F) -> PipeFence
    where
        F: Fn(&HelperContext),
    {
        func(self);
        self.lock.flush()
    }

    fn create_compute_state(&self, nir: &NirShader, static_local_mem: u32) -> *mut c_void {
        self.lock.create_compute_state(nir, static_local_mem)
    }

    fn delete_compute_state(&self, cso: *mut c_void) {
        self.lock.delete_compute_state(cso)
    }

    fn compute_state_info(&self, state: *mut c_void) -> pipe_compute_state_object_info {
        self.lock.compute_state_info(state)
    }

    fn compute_state_subgroup_size(&self, state: *mut c_void, block: &[u32; 3]) -> u32 {
        self.lock.compute_state_subgroup_size(state, block)
    }

    fn is_create_fence_fd_supported(&self) -> bool {
        self.lock.is_create_fence_fd_supported()
    }

    fn import_fence(&self, fd: &FenceFd) -> PipeFence {
        self.lock.import_fence(fd)
    }
}

impl_cl_type_trait_base!(cl_device_id, Device, [Device], CL_INVALID_DEVICE);

impl Device {
    fn new(screen: PipeScreen) -> Option<Device> {
        if !Self::check_valid(&screen) {
            return None;
        }

        let screen = Arc::new(screen);
        // Create before loading libclc as llvmpipe only creates the shader cache with the first
        // context being created.
        let helper_ctx = screen.create_context()?;
        let lib_clc = spirv::SPIRVBin::get_lib_clc(&screen);
        if lib_clc.is_none() {
            eprintln!("Libclc failed to load. Please make sure it is installed and provides spirv-mesa3d-.spv and/or spirv64-mesa3d-.spv");
        }

        let mut d = Self {
            caps: DeviceCaps::new(&screen),
            base: CLObjectBase::new(RusticlTypes::Device),
            helper_ctx: Mutex::new(helper_ctx),
            screen: screen,
            cl_version: CLVersion::Cl3_0,
            clc_version: CLVersion::Cl3_0,
            clc_versions: Vec::new(),
            custom: false,
            embedded: false,
            extension_string: String::from(""),
            extensions: Vec::new(),
            spirv_extensions: Vec::new(),
            clc_features: Vec::new(),
            formats: HashMap::new(),
            lib_clc: lib_clc?,
            reusable_ctx: Mutex::new(Vec::new()),
        };

        // check if we are embedded or full profile first
        d.embedded = d.check_embedded_profile();

        // check if we have to report it as a custom device
        d.custom = d.check_custom();

        d.fill_format_tables();

        // query supported extensions
        d.fill_extensions();

        // now figure out what version we are
        d.check_version();

        Some(d)
    }

    /// Converts a temporary reference to a static if and only if this device lives inside static
    /// memory.
    pub fn to_static(&self) -> Option<&'static Self> {
        devs().iter().find(|&dev| self == dev)
    }

    fn fill_format_tables(&mut self) {
        // no need to do this if we don't support images
        if !self.caps.has_images {
            return;
        }

        for f in FORMATS {
            let mut fs = HashMap::new();
            for t in CL_IMAGE_TYPES {
                // the CTS doesn't test them, so let's not advertize them by accident if they are
                // broken
                if t == CL_MEM_OBJECT_IMAGE1D_BUFFER
                    && [CL_RGB, CL_RGBx].contains(&f.cl_image_format.image_channel_order)
                    && ![CL_UNORM_SHORT_565, CL_UNORM_SHORT_555]
                        .contains(&f.cl_image_format.image_channel_data_type)
                {
                    continue;
                }

                let mut flags: cl_uint = 0;
                if self.screen.is_format_supported(
                    f.pipe,
                    cl_mem_type_to_texture_target(t),
                    PIPE_BIND_SAMPLER_VIEW,
                ) {
                    flags |= CL_MEM_READ_ONLY;
                }

                // TODO: cl_khr_srgb_image_writes
                if !f.is_srgb
                    && self.screen.is_format_supported(
                        f.pipe,
                        cl_mem_type_to_texture_target(t),
                        PIPE_BIND_SHADER_IMAGE,
                    )
                {
                    flags |= CL_MEM_WRITE_ONLY | CL_MEM_KERNEL_READ_AND_WRITE;
                }

                // TODO: cl_khr_srgb_image_writes
                if !f.is_srgb
                    && self.screen.is_format_supported(
                        f.pipe,
                        cl_mem_type_to_texture_target(t),
                        PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHADER_IMAGE,
                    )
                {
                    flags |= CL_MEM_READ_WRITE;
                }

                fs.insert(t, flags as cl_mem_flags);
            }

            // Restrict supported formats with 1DBuffer images. This is an OpenCL CTS workaround.
            // See https://github.com/KhronosGroup/OpenCL-CTS/issues/1889
            let image1d_mask = fs[&CL_MEM_OBJECT_IMAGE1D];
            if let Some(entry) = fs.get_mut(&CL_MEM_OBJECT_IMAGE1D_BUFFER) {
                *entry &= image1d_mask;
            }

            self.formats.insert(f.cl_image_format, fs);
        }

        // now enable some caps based on advertized formats
        self.caps.has_3d_image_writes = !FORMATS
            .iter()
            .filter(|f| {
                if self.embedded {
                    f.req_for_embeded_read_or_write
                } else {
                    f.req_for_full_read_or_write
                }
            })
            .map(|f| self.formats[&f.cl_image_format][&CL_MEM_OBJECT_IMAGE3D])
            .any(|f| f & cl_mem_flags::from(CL_MEM_WRITE_ONLY) == 0);

        // if we can't advertize 3d image write ext, we have to disable them all
        if !self.caps.has_3d_image_writes {
            for f in &mut self.formats.values_mut() {
                *f.get_mut(&CL_MEM_OBJECT_IMAGE3D).unwrap() &= !cl_mem_flags::from(
                    CL_MEM_WRITE_ONLY | CL_MEM_READ_WRITE | CL_MEM_KERNEL_READ_AND_WRITE,
                );
            }
        }

        // we require formatted loads
        if self.screen.param(pipe_cap::PIPE_CAP_IMAGE_LOAD_FORMATTED) != 0 {
            // "For embedded profiles devices that support reading from and writing to the same
            // image object from the same kernel instance (see CL_DEVICE_MAX_READ_WRITE_IMAGE_ARGS)
            // there is no required minimum list of supported image formats."
            self.caps.has_rw_images = if self.embedded {
                FORMATS
                    .iter()
                    .flat_map(|f| self.formats[&f.cl_image_format].values())
                    .any(|f| f & cl_mem_flags::from(CL_MEM_KERNEL_READ_AND_WRITE) != 0)
            } else {
                !FORMATS
                    .iter()
                    .filter(|f| f.req_for_full_read_and_write)
                    .flat_map(|f| &self.formats[&f.cl_image_format])
                    // maybe? things being all optional is kinda a mess
                    .filter(|(target, _)| **target != CL_MEM_OBJECT_IMAGE3D)
                    .any(|(_, mask)| mask & cl_mem_flags::from(CL_MEM_KERNEL_READ_AND_WRITE) == 0)
            }
        }

        // if we can't advertize read_write images, disable them all
        if !self.caps.has_rw_images {
            self.formats
                .values_mut()
                .flat_map(|f| f.values_mut())
                .for_each(|f| *f &= !cl_mem_flags::from(CL_MEM_KERNEL_READ_AND_WRITE));
        }
    }

    fn check_valid(screen: &PipeScreen) -> bool {
        if screen.param(pipe_cap::PIPE_CAP_COMPUTE) == 0
            || screen.shader_param(
                pipe_shader_type::PIPE_SHADER_COMPUTE,
                pipe_shader_cap::PIPE_SHADER_CAP_SUPPORTED_IRS,
            ) & (1 << (pipe_shader_ir::PIPE_SHADER_IR_NIR as i32))
                == 0
        {
            return false;
        }

        // CL_DEVICE_MAX_PARAMETER_SIZE
        // For this minimum value, only a maximum of 128 arguments can be passed to a kernel
        if (screen.shader_param(
            pipe_shader_type::PIPE_SHADER_COMPUTE,
            pipe_shader_cap::PIPE_SHADER_CAP_MAX_CONST_BUFFER0_SIZE,
        ) as u32)
            < 128
        {
            return false;
        }
        true
    }

    fn check_custom(&self) -> bool {
        // Max size of memory object allocation in bytes. The minimum value is
        // max(min(1024 × 1024 × 1024, 1/4th of CL_DEVICE_GLOBAL_MEM_SIZE), 32 × 1024 × 1024)
        // for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
        let mut limit = min(1024 * 1024 * 1024, self.global_mem_size() / 4);
        limit = max(limit, 32 * 1024 * 1024);
        if self.max_mem_alloc() < limit {
            return true;
        }

        // CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS
        // The minimum value is 3 for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
        if self.max_grid_dimensions() < 3 {
            return true;
        }

        if self.embedded {
            // CL_DEVICE_MAX_PARAMETER_SIZE
            // The minimum value is 256 bytes for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.param_max_size() < 256 {
                return true;
            }

            // CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE
            // The minimum value is 1 KB for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.const_max_size() < 1024 {
                return true;
            }

            // TODO
            // CL_DEVICE_MAX_CONSTANT_ARGS
            // The minimum value is 4 for devices that are not of type CL_DEVICE_TYPE_CUSTOM.

            // CL_DEVICE_LOCAL_MEM_SIZE
            // The minimum value is 1 KB for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.local_mem_size() < 1024 {
                return true;
            }
        } else {
            // CL 1.0 spec:
            // CL_DEVICE_MAX_PARAMETER_SIZE
            // The minimum value is 256 for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.param_max_size() < 256 {
                return true;
            }

            // CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE
            // The minimum value is 64 KB for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.const_max_size() < 64 * 1024 {
                return true;
            }

            // TODO
            // CL_DEVICE_MAX_CONSTANT_ARGS
            // The minimum value is 8 for devices that are not of type CL_DEVICE_TYPE_CUSTOM.

            // CL 1.0 spec:
            // CL_DEVICE_LOCAL_MEM_SIZE
            // The minimum value is 16 KB for devices that are not of type CL_DEVICE_TYPE_CUSTOM.
            if self.local_mem_size() < 16 * 1024 {
                return true;
            }
        }

        false
    }

    fn check_embedded_profile(&self) -> bool {
        if self.caps.has_images {
            // The minimum value is 16 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            if self.max_samplers() < 16 ||
            // The minimum value is 128 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.caps.max_read_images < 128 ||
            // The minimum value is 64 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.caps.max_write_images < 64 ||
            // The minimum value is 16384 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.caps.image_2d_size < 16384 ||
            // The minimum value is 2048 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_array_size() < 2048 ||
            // The minimum value is 65536 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_buffer_max_size_pixels() < 65536
            {
                return true;
            }

            // TODO check req formats
        }
        !self.int64_supported()
    }

    fn parse_env_device_type() -> Option<cl_device_type> {
        let mut val = env::var("RUSTICL_DEVICE_TYPE").ok()?;
        val.make_ascii_lowercase();
        Some(
            match &*val {
                "accelerator" => CL_DEVICE_TYPE_ACCELERATOR,
                "cpu" => CL_DEVICE_TYPE_CPU,
                "custom" => CL_DEVICE_TYPE_CUSTOM,
                "gpu" => CL_DEVICE_TYPE_GPU,
                _ => return None,
            }
            .into(),
        )
    }

    fn parse_env_version() -> Option<CLVersion> {
        let val = env::var("RUSTICL_CL_VERSION").ok()?;
        let (major, minor) = val.split_once('.')?;
        let major = major.parse().ok()?;
        let minor = minor.parse().ok()?;
        mk_cl_version(major, minor, 0).try_into().ok()
    }

    // TODO add CLC checks
    fn check_version(&mut self) {
        let exts: Vec<&str> = self.extension_string.split(' ').collect();
        let mut res = CLVersion::Cl3_0;

        if self.embedded {
            if self.caps.has_images {
                let supports_array_writes = !FORMATS
                    .iter()
                    .filter(|f| f.req_for_embeded_read_or_write)
                    .map(|f| self.formats.get(&f.cl_image_format).unwrap())
                    .map(|f| f.get(&CL_MEM_OBJECT_IMAGE2D_ARRAY).unwrap())
                    .any(|f| *f & cl_mem_flags::from(CL_MEM_WRITE_ONLY) == 0);
                if self.image_3d_size() < 2048 || !supports_array_writes {
                    res = CLVersion::Cl1_2;
                }
            }
        }

        // TODO: check image 1D, 1Dbuffer, 1Darray and 2Darray support explicitly
        if self.caps.has_images {
            // The minimum value is 256 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            if self.image_array_size() < 256 ||
            // The minimum value is 2048 if CL_DEVICE_IMAGE_SUPPORT is CL_TRUE
            self.image_buffer_max_size_pixels() < 2048
            {
                res = CLVersion::Cl1_1;
            }
        }

        if self.embedded {
            // The minimum value for the EMBEDDED profile is 1 KB.
            if self.printf_buffer_size() < 1024 {
                res = CLVersion::Cl1_1;
            }
        } else {
            // The minimum value for the FULL profile is 1 MB.
            if self.printf_buffer_size() < 1024 * 1024 {
                res = CLVersion::Cl1_1;
            }
        }

        if !exts.contains(&"cl_khr_byte_addressable_store")
         || !exts.contains(&"cl_khr_global_int32_base_atomics")
         || !exts.contains(&"cl_khr_global_int32_extended_atomics")
         || !exts.contains(&"cl_khr_local_int32_base_atomics")
         || !exts.contains(&"cl_khr_local_int32_extended_atomics")
         // The following modifications are made to the OpenCL 1.1 platform layer and runtime (sections 4 and 5):
         // The minimum FULL_PROFILE value for CL_DEVICE_MAX_PARAMETER_SIZE increased from 256 to 1024 bytes
         || self.param_max_size() < 1024
         // The minimum FULL_PROFILE value for CL_DEVICE_LOCAL_MEM_SIZE increased from 16 KB to 32 KB.
         || self.local_mem_size() < 32 * 1024
        {
            res = CLVersion::Cl1_0;
        }

        if let Some(val) = Self::parse_env_version() {
            res = val;
        }

        if res >= CLVersion::Cl3_0 {
            self.clc_versions
                .push(mk_cl_version_ext(3, 0, 0, "OpenCL C"));
        }

        if res >= CLVersion::Cl1_2 {
            self.clc_versions
                .push(mk_cl_version_ext(1, 2, 0, "OpenCL C"));
        }

        if res >= CLVersion::Cl1_1 {
            self.clc_versions
                .push(mk_cl_version_ext(1, 1, 0, "OpenCL C"));
        }

        if res >= CLVersion::Cl1_0 {
            self.clc_versions
                .push(mk_cl_version_ext(1, 0, 0, "OpenCL C"));
        }

        self.cl_version = res;
        self.clc_version = min(CLVersion::Cl1_2, res);
    }

    fn fill_extensions(&mut self) {
        let mut exts_str: Vec<String> = Vec::new();
        let mut exts = PLATFORM_EXTENSIONS.to_vec();
        let mut feats = Vec::new();
        let mut spirv_exts = Vec::new();
        let mut add_ext = |major, minor, patch, ext: &str| {
            exts.push(mk_cl_version_ext(major, minor, patch, ext));
            exts_str.push(ext.to_owned());
        };
        let mut add_feat = |major, minor, patch, feat: &str| {
            feats.push(mk_cl_version_ext(major, minor, patch, feat));
        };
        let mut add_spirv = |ext: &str| {
            spirv_exts.push(CString::new(ext).unwrap());
        };

        // add extensions all drivers support for now
        add_ext(1, 0, 0, "cl_khr_global_int32_base_atomics");
        add_ext(1, 0, 0, "cl_khr_global_int32_extended_atomics");
        add_ext(2, 0, 0, "cl_khr_integer_dot_product");
        add_feat(
            2,
            0,
            0,
            "__opencl_c_integer_dot_product_input_4x8bit_packed",
        );
        add_feat(2, 0, 0, "__opencl_c_integer_dot_product_input_4x8bit");
        add_ext(1, 0, 0, "cl_khr_local_int32_base_atomics");
        add_ext(1, 0, 0, "cl_khr_local_int32_extended_atomics");

        add_spirv("SPV_KHR_expect_assume");
        add_spirv("SPV_KHR_float_controls");
        add_spirv("SPV_KHR_integer_dot_product");
        add_spirv("SPV_KHR_no_integer_wrap_decoration");

        if self.fp16_supported() {
            add_ext(1, 0, 0, "cl_khr_fp16");
        }

        if self.fp64_supported() {
            add_ext(1, 0, 0, "cl_khr_fp64");
            add_feat(1, 0, 0, "__opencl_c_fp64");
        }

        if self.is_gl_sharing_supported() {
            add_ext(1, 0, 0, "cl_khr_gl_sharing");
        }

        if self.int64_supported() {
            if self.embedded {
                add_ext(1, 0, 0, "cles_khr_int64");
            };

            add_feat(1, 0, 0, "__opencl_c_int64");
        }

        if self.caps.has_images {
            add_feat(1, 0, 0, "__opencl_c_images");

            if self.image2d_from_buffer_supported() {
                add_ext(1, 0, 0, "cl_khr_image2d_from_buffer");
            }

            if self.caps.has_rw_images {
                add_feat(1, 0, 0, "__opencl_c_read_write_images");
            }

            if self.caps.has_3d_image_writes {
                add_ext(1, 0, 0, "cl_khr_3d_image_writes");
                add_feat(1, 0, 0, "__opencl_c_3d_image_writes");
            }
        }

        if self.pci_info().is_some() {
            add_ext(1, 0, 0, "cl_khr_pci_bus_info");
        }

        if self.screen().device_uuid().is_some() && self.screen().driver_uuid().is_some() {
            static_assert!(PIPE_UUID_SIZE == CL_UUID_SIZE_KHR);
            static_assert!(PIPE_LUID_SIZE == CL_LUID_SIZE_KHR);

            add_ext(1, 0, 0, "cl_khr_device_uuid");
        }

        if self.subgroups_supported() {
            // requires CL_DEVICE_SUB_GROUP_INDEPENDENT_FORWARD_PROGRESS
            //add_ext(1, 0, 0, "cl_khr_subgroups");
            add_feat(1, 0, 0, "__opencl_c_subgroups");

            // we have lowering in `nir_lower_subgroups`, drivers can just use that
            add_ext(1, 0, 0, "cl_khr_subgroup_shuffle");
            add_ext(1, 0, 0, "cl_khr_subgroup_shuffle_relative");
        }

        if self.svm_supported() {
            add_ext(1, 0, 0, "cl_arm_shared_virtual_memory");
        }

        self.extensions = exts;
        self.clc_features = feats;
        self.extension_string = format!("{} {}", PLATFORM_EXTENSION_STR, exts_str.join(" "));
        self.spirv_extensions = spirv_exts;
    }

    fn shader_param(&self, cap: pipe_shader_cap) -> i32 {
        self.screen
            .shader_param(pipe_shader_type::PIPE_SHADER_COMPUTE, cap)
    }

    pub fn all() -> impl Iterator<Item = Device> {
        load_screens().filter_map(Device::new)
    }

    pub fn address_bits(&self) -> cl_uint {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_ADDRESS_BITS)
    }

    pub fn const_max_size(&self) -> cl_ulong {
        min(
            // Needed to fix the `api min_max_constant_buffer_size` CL CTS test as it can't really
            // handle arbitrary values here. We might want to reconsider later and figure out how to
            // advertize higher values without tripping of the test.
            // should be at least 1 << 16 (native UBO size on NVidia)
            // advertising more just in case it benefits other hardware
            1 << 26,
            min(
                self.max_mem_alloc(),
                self.screen
                    .param(pipe_cap::PIPE_CAP_MAX_SHADER_BUFFER_SIZE_UINT) as u64,
            ),
        )
    }

    pub fn const_max_count(&self) -> cl_uint {
        self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_MAX_CONST_BUFFERS) as cl_uint
    }

    pub fn device_type(&self, internal: bool) -> cl_device_type {
        if let Some(env) = Self::parse_env_device_type() {
            return env;
        }

        if self.custom {
            return CL_DEVICE_TYPE_CUSTOM as cl_device_type;
        }
        let mut res = match self.screen.device_type() {
            pipe_loader_device_type::PIPE_LOADER_DEVICE_SOFTWARE => CL_DEVICE_TYPE_CPU,
            pipe_loader_device_type::PIPE_LOADER_DEVICE_PCI => CL_DEVICE_TYPE_GPU,
            pipe_loader_device_type::PIPE_LOADER_DEVICE_PLATFORM => CL_DEVICE_TYPE_GPU,
            pipe_loader_device_type::NUM_PIPE_LOADER_DEVICE_TYPES => CL_DEVICE_TYPE_CUSTOM,
        };

        if internal && res == CL_DEVICE_TYPE_GPU && self.screen.driver_name() != "zink" {
            res |= CL_DEVICE_TYPE_DEFAULT;
        }

        res as cl_device_type
    }

    pub fn fp16_supported(&self) -> bool {
        if !Platform::features().fp16 {
            return false;
        }

        self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_FP16) != 0
    }

    pub fn fp64_supported(&self) -> bool {
        if !Platform::features().fp64 {
            return false;
        }

        self.screen.param(pipe_cap::PIPE_CAP_DOUBLES) == 1
    }

    pub fn is_gl_sharing_supported(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_CL_GL_SHARING) != 0
            && self.screen.param(pipe_cap::PIPE_CAP_DMABUF) != 0
            && !self.is_device_software()
            && self.screen.is_res_handle_supported()
            && self.screen.device_uuid().is_some()
            && self.helper_ctx().is_create_fence_fd_supported()
    }

    pub fn is_device_software(&self) -> bool {
        self.screen.device_type() == pipe_loader_device_type::PIPE_LOADER_DEVICE_SOFTWARE
    }

    pub fn get_nir_options(&self) -> nir_shader_compiler_options {
        unsafe {
            *self
                .screen
                .nir_shader_compiler_options(pipe_shader_type::PIPE_SHADER_COMPUTE)
        }
    }

    pub fn sdot_4x8_supported(&self) -> bool {
        self.get_nir_options().has_sdot_4x8
    }

    pub fn udot_4x8_supported(&self) -> bool {
        self.get_nir_options().has_udot_4x8
    }

    pub fn sudot_4x8_supported(&self) -> bool {
        self.get_nir_options().has_sudot_4x8
    }

    pub fn pack_32_4x8_supported(&self) -> bool {
        self.get_nir_options().has_pack_32_4x8
    }

    pub fn sdot_4x8_sat_supported(&self) -> bool {
        self.get_nir_options().has_sdot_4x8_sat
    }

    pub fn udot_4x8_sat_supported(&self) -> bool {
        self.get_nir_options().has_udot_4x8_sat
    }

    pub fn sudot_4x8_sat_supported(&self) -> bool {
        self.get_nir_options().has_sudot_4x8_sat
    }

    pub fn fp64_is_softfp(&self) -> bool {
        bit_check(
            self.get_nir_options().lower_doubles_options as u32,
            nir_lower_doubles_options::nir_lower_fp64_full_software as u32,
        )
    }

    pub fn int64_supported(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_INT64) == 1
    }

    pub fn global_mem_size(&self) -> cl_ulong {
        if let Some(memory_info) = self.screen().query_memory_info() {
            let memory: cl_ulong = if memory_info.total_device_memory != 0 {
                memory_info.total_device_memory.into()
            } else {
                memory_info.total_staging_memory.into()
            };
            memory * 1024
        } else {
            self.screen
                .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_GLOBAL_SIZE)
        }
    }

    pub fn image_3d_size(&self) -> usize {
        if self.caps.has_images {
            1 << (self.screen.param(pipe_cap::PIPE_CAP_MAX_TEXTURE_3D_LEVELS) - 1)
        } else {
            0
        }
    }

    pub fn image_3d_supported(&self) -> bool {
        self.caps.has_images && self.screen.param(pipe_cap::PIPE_CAP_MAX_TEXTURE_3D_LEVELS) != 0
    }

    pub fn image_array_size(&self) -> usize {
        if self.caps.has_images {
            self.screen
                .param(pipe_cap::PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS) as usize
        } else {
            0
        }
    }

    pub fn image_pitch_alignment(&self) -> cl_uint {
        if self.caps.has_images {
            self.screen
                .param(pipe_cap::PIPE_CAP_LINEAR_IMAGE_PITCH_ALIGNMENT) as u32
        } else {
            0
        }
    }

    pub fn image_base_address_alignment(&self) -> cl_uint {
        if self.caps.has_images {
            self.screen
                .param(pipe_cap::PIPE_CAP_LINEAR_IMAGE_BASE_ADDRESS_ALIGNMENT) as u32
        } else {
            0
        }
    }

    pub fn image_buffer_max_size_pixels(&self) -> usize {
        if self.caps.has_images {
            min(
                // The CTS requires it to not exceed `CL_MAX_MEM_ALLOC_SIZE`, also we need to divide
                // by the max pixel size, because this cap is in pixels, not bytes.
                //
                // The CTS also casts this to int in a couple of places,
                // see: https://github.com/KhronosGroup/OpenCL-CTS/issues/2056
                min(
                    self.max_mem_alloc() / MAX_PIXEL_SIZE_BYTES,
                    c_int::MAX as cl_ulong,
                ),
                self.screen
                    .param(pipe_cap::PIPE_CAP_MAX_TEXEL_BUFFER_ELEMENTS_UINT)
                    as cl_ulong,
            ) as usize
        } else {
            0
        }
    }

    pub fn image2d_from_buffer_supported(&self) -> bool {
        self.image_pitch_alignment() != 0 && self.image_base_address_alignment() != 0
    }

    pub fn little_endian(&self) -> bool {
        let endianness = self.screen.param(pipe_cap::PIPE_CAP_ENDIANNESS);
        endianness == (pipe_endian::PIPE_ENDIAN_LITTLE as i32)
    }

    pub fn local_mem_size(&self) -> cl_ulong {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_LOCAL_SIZE)
    }

    pub fn max_block_sizes(&self) -> Vec<usize> {
        let v: Vec<u64> = self
            .screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_BLOCK_SIZE);
        v.into_iter().map(|v| v as usize).collect()
    }

    pub fn max_grid_size(&self) -> Vec<u64> {
        let v: Vec<u64> = self
            .screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_GRID_SIZE);

        v.into_iter()
            .map(|a| min(a, Platform::dbg().max_grid_size))
            .collect()
    }

    pub fn max_clock_freq(&self) -> cl_uint {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_CLOCK_FREQUENCY)
    }

    pub fn max_compute_units(&self) -> cl_uint {
        self.screen
            .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_COMPUTE_UNITS)
    }

    pub fn max_grid_dimensions(&self) -> cl_uint {
        ComputeParam::<u64>::compute_param(
            self.screen.as_ref(),
            pipe_compute_cap::PIPE_COMPUTE_CAP_GRID_DIMENSION,
        ) as cl_uint
    }

    pub fn max_mem_alloc(&self) -> cl_ulong {
        // TODO: at the moment gallium doesn't support bigger buffers
        min(
            self.screen
                .compute_param(pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_MEM_ALLOC_SIZE),
            0x80000000,
        )
    }

    pub fn max_samplers(&self) -> cl_uint {
        self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS) as cl_uint
    }

    pub fn max_threads_per_block(&self) -> usize {
        ComputeParam::<u64>::compute_param(
            self.screen.as_ref(),
            pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_THREADS_PER_BLOCK,
        ) as usize
    }

    pub fn param_max_size(&self) -> usize {
        min(
            self.shader_param(pipe_shader_cap::PIPE_SHADER_CAP_MAX_CONST_BUFFER0_SIZE) as u32,
            4 * 1024,
        ) as usize
    }

    pub fn printf_buffer_size(&self) -> usize {
        1024 * 1024
    }

    pub fn pci_info(&self) -> Option<cl_device_pci_bus_info_khr> {
        if self.screen.device_type() != pipe_loader_device_type::PIPE_LOADER_DEVICE_PCI {
            return None;
        }

        let pci_domain = self.screen.param(pipe_cap::PIPE_CAP_PCI_GROUP) as cl_uint;
        let pci_bus = self.screen.param(pipe_cap::PIPE_CAP_PCI_BUS) as cl_uint;
        let pci_device = self.screen.param(pipe_cap::PIPE_CAP_PCI_DEVICE) as cl_uint;
        let pci_function = self.screen.param(pipe_cap::PIPE_CAP_PCI_FUNCTION) as cl_uint;

        Some(cl_device_pci_bus_info_khr {
            pci_domain,
            pci_bus,
            pci_device,
            pci_function,
        })
    }

    fn reusable_ctx(&self) -> MutexGuard<Vec<PipeContext>> {
        self.reusable_ctx.lock().unwrap()
    }

    pub fn screen(&self) -> &Arc<PipeScreen> {
        &self.screen
    }

    pub fn create_context(&self) -> Option<PipeContext> {
        self.reusable_ctx()
            .pop()
            .or_else(|| self.screen.create_context())
    }

    pub fn recycle_context(&self, ctx: PipeContext) {
        if Platform::dbg().reuse_context {
            self.reusable_ctx().push(ctx);
        }
    }

    pub fn subgroup_sizes(&self) -> Vec<usize> {
        let subgroup_size = ComputeParam::<u32>::compute_param(
            self.screen.as_ref(),
            pipe_compute_cap::PIPE_COMPUTE_CAP_SUBGROUP_SIZES,
        );

        SetBitIndices::from_msb(subgroup_size)
            .map(|bit| 1 << bit)
            .collect()
    }

    pub fn max_subgroups(&self) -> u32 {
        ComputeParam::<u32>::compute_param(
            self.screen.as_ref(),
            pipe_compute_cap::PIPE_COMPUTE_CAP_MAX_SUBGROUPS,
        )
    }

    pub fn subgroups_supported(&self) -> bool {
        let subgroup_sizes = self.subgroup_sizes().len();

        // we need to be able to query a CSO for subgroup sizes if multiple sub group sizes are
        // supported, doing it without shareable shaders isn't practical
        self.max_subgroups() > 0
            && (subgroup_sizes == 1 || (subgroup_sizes > 1 && self.shareable_shaders()))
    }

    pub fn svm_supported(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_SYSTEM_SVM) == 1
    }

    pub fn unified_memory(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_UMA) == 1
    }

    pub fn vendor_id(&self) -> cl_uint {
        let id = self.screen.param(pipe_cap::PIPE_CAP_VENDOR_ID);
        if id == -1 {
            return 0;
        }
        id as u32
    }

    pub fn prefers_real_buffer_in_cb0(&self) -> bool {
        self.screen
            .param(pipe_cap::PIPE_CAP_PREFER_REAL_BUFFER_IN_CONSTBUF0)
            == 1
    }

    pub fn shareable_shaders(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_SHAREABLE_SHADERS) == 1
    }

    pub fn images_as_deref(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_NIR_IMAGES_AS_DEREF) == 1
    }

    pub fn samplers_as_deref(&self) -> bool {
        self.screen.param(pipe_cap::PIPE_CAP_NIR_SAMPLERS_AS_DEREF) == 1
    }

    pub fn helper_ctx(&self) -> impl HelperContextWrapper + '_ {
        HelperContext {
            lock: self.helper_ctx.lock().unwrap(),
        }
    }

    pub fn cl_features(&self) -> clc_optional_features {
        let subgroups_supported = self.subgroups_supported();
        clc_optional_features {
            fp16: self.fp16_supported(),
            fp64: self.fp64_supported(),
            int64: self.int64_supported(),
            images: self.caps.has_images,
            images_read_write: self.caps.has_rw_images,
            images_write_3d: self.caps.has_3d_image_writes,
            integer_dot_product: true,
            subgroups: subgroups_supported,
            subgroups_shuffle: subgroups_supported,
            subgroups_shuffle_relative: subgroups_supported,
            ..Default::default()
        }
    }
}

pub fn devs() -> &'static Vec<Device> {
    &Platform::get().devs
}

pub fn get_devs_for_type(device_type: cl_device_type) -> Vec<&'static Device> {
    devs()
        .iter()
        .filter(|d| device_type & d.device_type(true) != 0)
        .collect()
}

pub fn get_dev_for_uuid(uuid: [c_char; UUID_SIZE]) -> Option<&'static Device> {
    devs().iter().find(|d| {
        let uuid: [c_uchar; UUID_SIZE] = unsafe { transmute(uuid) };
        uuid == d.screen().device_uuid().unwrap()
    })
}
