use crate::api::event::create_and_queue;
use crate::api::icd::*;
use crate::api::util::*;
use crate::core::device::*;
use crate::core::event::*;
use crate::core::kernel::*;
use crate::core::memory::*;
use crate::core::program::*;
use crate::core::queue::*;

use mesa_rust_util::ptr::*;
use mesa_rust_util::string::*;
use rusticl_opencl_gen::*;
use rusticl_proc_macros::cl_entrypoint;
use rusticl_proc_macros::cl_info_entrypoint;

use std::cmp;
use std::mem::{self, MaybeUninit};
use std::os::raw::c_void;
use std::ptr;
use std::slice;
use std::sync::Arc;

#[cl_info_entrypoint(clGetKernelInfo)]
impl CLInfo<cl_kernel_info> for cl_kernel {
    fn query(&self, q: cl_kernel_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let kernel = Kernel::ref_from_raw(*self)?;
        Ok(match q {
            CL_KERNEL_ATTRIBUTES => cl_prop::<&str>(&kernel.kernel_info.attributes_string),
            CL_KERNEL_CONTEXT => {
                let ptr = Arc::as_ptr(&kernel.prog.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_KERNEL_FUNCTION_NAME => cl_prop::<&str>(&kernel.name),
            CL_KERNEL_NUM_ARGS => cl_prop::<cl_uint>(kernel.kernel_info.args.len() as cl_uint),
            CL_KERNEL_PROGRAM => {
                let ptr = Arc::as_ptr(&kernel.prog);
                cl_prop::<cl_program>(cl_program::from_ptr(ptr))
            }
            CL_KERNEL_REFERENCE_COUNT => cl_prop::<cl_uint>(Kernel::refcnt(*self)?),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_info_entrypoint(clGetKernelArgInfo)]
impl CLInfoObj<cl_kernel_arg_info, cl_uint> for cl_kernel {
    fn query(&self, idx: cl_uint, q: cl_kernel_arg_info) -> CLResult<Vec<MaybeUninit<u8>>> {
        let kernel = Kernel::ref_from_raw(*self)?;

        // CL_INVALID_ARG_INDEX if arg_index is not a valid argument index.
        if idx as usize >= kernel.kernel_info.args.len() {
            return Err(CL_INVALID_ARG_INDEX);
        }

        Ok(match *q {
            CL_KERNEL_ARG_ACCESS_QUALIFIER => {
                cl_prop::<cl_kernel_arg_access_qualifier>(kernel.access_qualifier(idx))
            }
            CL_KERNEL_ARG_ADDRESS_QUALIFIER => {
                cl_prop::<cl_kernel_arg_address_qualifier>(kernel.address_qualifier(idx))
            }
            CL_KERNEL_ARG_NAME => cl_prop::<&str>(kernel.arg_name(idx)),
            CL_KERNEL_ARG_TYPE_NAME => cl_prop::<&str>(kernel.arg_type_name(idx)),
            CL_KERNEL_ARG_TYPE_QUALIFIER => {
                cl_prop::<cl_kernel_arg_type_qualifier>(kernel.type_qualifier(idx))
            }
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_info_entrypoint(clGetKernelWorkGroupInfo)]
impl CLInfoObj<cl_kernel_work_group_info, cl_device_id> for cl_kernel {
    fn query(
        &self,
        dev: cl_device_id,
        q: cl_kernel_work_group_info,
    ) -> CLResult<Vec<MaybeUninit<u8>>> {
        let kernel = Kernel::ref_from_raw(*self)?;

        // CL_INVALID_DEVICE [..] if device is NULL but there is more than one device associated with kernel.
        let dev = if dev.is_null() {
            if kernel.prog.devs.len() > 1 {
                return Err(CL_INVALID_DEVICE);
            } else {
                kernel.prog.devs[0]
            }
        } else {
            Device::ref_from_raw(dev)?
        };

        // CL_INVALID_DEVICE if device is not in the list of devices associated with kernel
        if !kernel.prog.devs.contains(&dev) {
            return Err(CL_INVALID_DEVICE);
        }

        Ok(match *q {
            CL_KERNEL_COMPILE_WORK_GROUP_SIZE => cl_prop::<[usize; 3]>(kernel.work_group_size()),
            CL_KERNEL_LOCAL_MEM_SIZE => cl_prop::<cl_ulong>(kernel.local_mem_size(dev)),
            CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE => {
                cl_prop::<usize>(kernel.preferred_simd_size(dev))
            }
            CL_KERNEL_PRIVATE_MEM_SIZE => cl_prop::<cl_ulong>(kernel.priv_mem_size(dev)),
            CL_KERNEL_WORK_GROUP_SIZE => cl_prop::<usize>(kernel.max_threads_per_block(dev)),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

impl CLInfoObj<cl_kernel_sub_group_info, (cl_device_id, usize, *const c_void, usize)>
    for cl_kernel
{
    fn query(
        &self,
        (dev, input_value_size, input_value, output_value_size): (
            cl_device_id,
            usize,
            *const c_void,
            usize,
        ),
        q: cl_program_build_info,
    ) -> CLResult<Vec<MaybeUninit<u8>>> {
        let kernel = Kernel::ref_from_raw(*self)?;

        // CL_INVALID_DEVICE [..] if device is NULL but there is more than one device associated
        // with kernel.
        let dev = if dev.is_null() {
            if kernel.prog.devs.len() > 1 {
                return Err(CL_INVALID_DEVICE);
            } else {
                kernel.prog.devs[0]
            }
        } else {
            Device::ref_from_raw(dev)?
        };

        // CL_INVALID_DEVICE if device is not in the list of devices associated with kernel
        if !kernel.prog.devs.contains(&dev) {
            return Err(CL_INVALID_DEVICE);
        }

        // CL_INVALID_OPERATION if device does not support subgroups.
        if !dev.subgroups_supported() {
            return Err(CL_INVALID_OPERATION);
        }

        let usize_byte = mem::size_of::<usize>();
        // first we have to convert the input to a proper thing
        let input: &[usize] = match q {
            CL_KERNEL_MAX_SUB_GROUP_SIZE_FOR_NDRANGE | CL_KERNEL_SUB_GROUP_COUNT_FOR_NDRANGE => {
                // CL_INVALID_VALUE if param_name is CL_KERNEL_MAX_SUB_GROUP_SIZE_FOR_NDRANGE,
                // CL_KERNEL_SUB_GROUP_COUNT_FOR_NDRANGE or ... and the size in bytes specified by
                // input_value_size is not valid or if input_value is NULL.
                if ![usize_byte, 2 * usize_byte, 3 * usize_byte].contains(&input_value_size) {
                    return Err(CL_INVALID_VALUE);
                }
                // SAFETY: we verified the size as best as possible, with the rest we trust the client
                unsafe { slice::from_raw_parts(input_value.cast(), input_value_size / usize_byte) }
            }
            CL_KERNEL_LOCAL_SIZE_FOR_SUB_GROUP_COUNT => {
                // CL_INVALID_VALUE if param_name is ... CL_KERNEL_LOCAL_SIZE_FOR_SUB_GROUP_COUNT
                // and the size in bytes specified by input_value_size is not valid or if
                // input_value is NULL.
                if input_value_size != usize_byte || input_value.is_null() {
                    return Err(CL_INVALID_VALUE);
                }
                // SAFETY: we trust the client here
                unsafe { slice::from_raw_parts(input_value.cast(), 1) }
            }
            _ => &[],
        };

        Ok(match q {
            CL_KERNEL_SUB_GROUP_COUNT_FOR_NDRANGE => {
                cl_prop::<usize>(kernel.subgroups_for_block(dev, input))
            }
            CL_KERNEL_MAX_SUB_GROUP_SIZE_FOR_NDRANGE => {
                cl_prop::<usize>(kernel.subgroup_size_for_block(dev, input))
            }
            CL_KERNEL_LOCAL_SIZE_FOR_SUB_GROUP_COUNT => {
                let subgroups = input[0];
                let mut res = vec![0; 3];

                for subgroup_size in kernel.subgroup_sizes(dev) {
                    let threads = subgroups * subgroup_size;

                    if threads > dev.max_threads_per_block() {
                        continue;
                    }

                    let block = [threads, 1, 1];
                    let real_subgroups = kernel.subgroups_for_block(dev, &block);

                    if real_subgroups == subgroups {
                        res = block.to_vec();
                        break;
                    }
                }

                res.truncate(output_value_size / usize_byte);
                cl_prop::<Vec<usize>>(res)
            }
            CL_KERNEL_MAX_NUM_SUB_GROUPS => {
                let threads = kernel.max_threads_per_block(dev);
                let max_groups = dev.max_subgroups();

                let mut result = 0;
                for sgs in kernel.subgroup_sizes(dev) {
                    result = cmp::max(result, threads / sgs);
                    result = cmp::min(result, max_groups as usize);
                }
                cl_prop::<usize>(result)
            }
            CL_KERNEL_COMPILE_NUM_SUB_GROUPS => cl_prop::<usize>(kernel.num_subgroups()),
            CL_KERNEL_COMPILE_SUB_GROUP_SIZE_INTEL => cl_prop::<usize>(kernel.subgroup_size()),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

const ZERO_ARR: [usize; 3] = [0; 3];

/// # Safety
///
/// This function is only safe when called on an array of `work_dim` length
unsafe fn kernel_work_arr_or_default<'a>(arr: *const usize, work_dim: cl_uint) -> &'a [usize] {
    if !arr.is_null() {
        unsafe { slice::from_raw_parts(arr, work_dim as usize) }
    } else {
        &ZERO_ARR
    }
}

/// # Safety
///
/// This function is only safe when called on an array of `work_dim` length
unsafe fn kernel_work_arr_mut<'a>(arr: *mut usize, work_dim: cl_uint) -> Option<&'a mut [usize]> {
    if !arr.is_null() {
        unsafe { Some(slice::from_raw_parts_mut(arr, work_dim as usize)) }
    } else {
        None
    }
}

#[cl_entrypoint(clCreateKernel)]
fn create_kernel(
    program: cl_program,
    kernel_name: *const ::std::os::raw::c_char,
) -> CLResult<cl_kernel> {
    let p = Program::arc_from_raw(program)?;
    let name = c_string_to_string(kernel_name);

    // CL_INVALID_VALUE if kernel_name is NULL.
    if kernel_name.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    let build = p.build_info();
    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built executable for program.
    if build.kernels().is_empty() {
        return Err(CL_INVALID_PROGRAM_EXECUTABLE);
    }

    // CL_INVALID_KERNEL_NAME if kernel_name is not found in program.
    if !build.kernels().contains(&name) {
        return Err(CL_INVALID_KERNEL_NAME);
    }

    // CL_INVALID_KERNEL_DEFINITION if the function definition for __kernel function given by
    // kernel_name such as the number of arguments, the argument types are not the same for all
    // devices for which the program executable has been built.
    if !p.has_unique_kernel_signatures(&name) {
        return Err(CL_INVALID_KERNEL_DEFINITION);
    }

    Ok(Kernel::new(name, Arc::clone(&p), &build).into_cl())
}

#[cl_entrypoint(clRetainKernel)]
fn retain_kernel(kernel: cl_kernel) -> CLResult<()> {
    Kernel::retain(kernel)
}

#[cl_entrypoint(clReleaseKernel)]
fn release_kernel(kernel: cl_kernel) -> CLResult<()> {
    Kernel::release(kernel)
}

#[cl_entrypoint(clCreateKernelsInProgram)]
fn create_kernels_in_program(
    program: cl_program,
    num_kernels: cl_uint,
    kernels: *mut cl_kernel,
    num_kernels_ret: *mut cl_uint,
) -> CLResult<()> {
    let p = Program::arc_from_raw(program)?;
    let build = p.build_info();

    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built executable for any device in
    // program.
    if build.kernels().is_empty() {
        return Err(CL_INVALID_PROGRAM_EXECUTABLE);
    }

    // CL_INVALID_VALUE if kernels is not NULL and num_kernels is less than the number of kernels
    // in program.
    if !kernels.is_null() && build.kernels().len() > num_kernels as usize {
        return Err(CL_INVALID_VALUE);
    }

    let mut num_kernels = 0;
    for name in build.kernels() {
        // Kernel objects are not created for any __kernel functions in program that do not have the
        // same function definition across all devices for which a program executable has been
        // successfully built.
        if !p.has_unique_kernel_signatures(name) {
            continue;
        }

        if !kernels.is_null() {
            // we just assume the client isn't stupid
            unsafe {
                kernels
                    .add(num_kernels as usize)
                    .write(Kernel::new(name.clone(), p.clone(), &build).into_cl());
            }
        }
        num_kernels += 1;
    }
    num_kernels_ret.write_checked(num_kernels);
    Ok(())
}

#[cl_entrypoint(clSetKernelArg)]
fn set_kernel_arg(
    kernel: cl_kernel,
    arg_index: cl_uint,
    arg_size: usize,
    arg_value: *const ::std::os::raw::c_void,
) -> CLResult<()> {
    let k = Kernel::ref_from_raw(kernel)?;
    let arg_index = arg_index as usize;

    // CL_INVALID_ARG_INDEX if arg_index is not a valid argument index.
    if let Some(arg) = k.kernel_info.args.get(arg_index) {
        // CL_INVALID_ARG_SIZE if arg_size does not match the size of the data type for an argument
        // that is not a memory object or if the argument is a memory object and
        // arg_size != sizeof(cl_mem) or if arg_size is zero and the argument is declared with the
        // local qualifier or if the argument is a sampler and arg_size != sizeof(cl_sampler).
        match arg.kind {
            KernelArgType::MemLocal => {
                if arg_size == 0 {
                    return Err(CL_INVALID_ARG_SIZE);
                }
            }
            KernelArgType::MemGlobal
            | KernelArgType::MemConstant
            | KernelArgType::Image
            | KernelArgType::RWImage
            | KernelArgType::Texture => {
                if arg_size != std::mem::size_of::<cl_mem>() {
                    return Err(CL_INVALID_ARG_SIZE);
                }
            }

            KernelArgType::Sampler => {
                if arg_size != std::mem::size_of::<cl_sampler>() {
                    return Err(CL_INVALID_ARG_SIZE);
                }
            }

            KernelArgType::Constant(size) => {
                if size as usize != arg_size {
                    return Err(CL_INVALID_ARG_SIZE);
                }
            }
        }

        // CL_INVALID_ARG_VALUE if arg_value specified is not a valid value.
        match arg.kind {
            // If the argument is declared with the local qualifier, the arg_value entry must be
            // NULL.
            KernelArgType::MemLocal => {
                if !arg_value.is_null() {
                    return Err(CL_INVALID_ARG_VALUE);
                }
            }
            // If the argument is of type sampler_t, the arg_value entry must be a pointer to the
            // sampler object.
            KernelArgType::Constant(_) | KernelArgType::Sampler => {
                if arg_value.is_null() {
                    return Err(CL_INVALID_ARG_VALUE);
                }
            }
            _ => {}
        };

        // let's create the arg now
        let arg = unsafe {
            if arg.dead {
                KernelArgValue::None
            } else {
                match arg.kind {
                    KernelArgType::Constant(_) => KernelArgValue::Constant(
                        slice::from_raw_parts(arg_value.cast(), arg_size).to_vec(),
                    ),
                    KernelArgType::MemConstant | KernelArgType::MemGlobal => {
                        let ptr: *const cl_mem = arg_value.cast();
                        if ptr.is_null() || (*ptr).is_null() {
                            KernelArgValue::None
                        } else {
                            KernelArgValue::Buffer(Buffer::arc_from_raw(*ptr)?)
                        }
                    }
                    KernelArgType::MemLocal => KernelArgValue::LocalMem(arg_size),
                    KernelArgType::Image | KernelArgType::RWImage | KernelArgType::Texture => {
                        let img: *const cl_mem = arg_value.cast();
                        KernelArgValue::Image(Image::arc_from_raw(*img)?)
                    }
                    KernelArgType::Sampler => {
                        let ptr: *const cl_sampler = arg_value.cast();
                        KernelArgValue::Sampler(Sampler::arc_from_raw(*ptr)?)
                    }
                }
            }
        };
        k.set_kernel_arg(arg_index, arg)
    } else {
        Err(CL_INVALID_ARG_INDEX)
    }

    //• CL_INVALID_DEVICE_QUEUE for an argument declared to be of type queue_t when the specified arg_value is not a valid device queue object. This error code is missing before version 2.0.
    //• CL_INVALID_ARG_VALUE if the argument is an image declared with the read_only qualifier and arg_value refers to an image object created with cl_mem_flags of CL_MEM_WRITE_ONLY or if the image argument is declared with the write_only qualifier and arg_value refers to an image object created with cl_mem_flags of CL_MEM_READ_ONLY.
    //• CL_MAX_SIZE_RESTRICTION_EXCEEDED if the size in bytes of the memory object (if the argument is a memory object) or arg_size (if the argument is declared with local qualifier) exceeds a language- specified maximum size restriction for this argument, such as the MaxByteOffset SPIR-V decoration. This error code is missing before version 2.2.
}

#[cl_entrypoint(clSetKernelArgSVMPointer)]
fn set_kernel_arg_svm_pointer(
    kernel: cl_kernel,
    arg_index: cl_uint,
    arg_value: *const ::std::os::raw::c_void,
) -> CLResult<()> {
    let kernel = Kernel::ref_from_raw(kernel)?;
    let arg_index = arg_index as usize;
    let arg_value = arg_value as usize;

    if !kernel.has_svm_devs() {
        return Err(CL_INVALID_OPERATION);
    }

    if let Some(arg) = kernel.kernel_info.args.get(arg_index) {
        if !matches!(
            arg.kind,
            KernelArgType::MemConstant | KernelArgType::MemGlobal
        ) {
            return Err(CL_INVALID_ARG_INDEX);
        }

        let arg_value = KernelArgValue::Constant(arg_value.to_ne_bytes().to_vec());
        kernel.set_kernel_arg(arg_index, arg_value)
    } else {
        Err(CL_INVALID_ARG_INDEX)
    }

    // CL_INVALID_ARG_VALUE if arg_value specified is not a valid value.
}

#[cl_entrypoint(clSetKernelExecInfo)]
fn set_kernel_exec_info(
    kernel: cl_kernel,
    param_name: cl_kernel_exec_info,
    param_value_size: usize,
    param_value: *const ::std::os::raw::c_void,
) -> CLResult<()> {
    let k = Kernel::ref_from_raw(kernel)?;

    // CL_INVALID_OPERATION if no devices in the context associated with kernel support SVM.
    if !k.prog.devs.iter().any(|dev| dev.svm_supported()) {
        return Err(CL_INVALID_OPERATION);
    }

    // CL_INVALID_VALUE ... if param_value is NULL
    if param_value.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_VALUE ... if the size specified by param_value_size is not valid.
    match param_name {
        CL_KERNEL_EXEC_INFO_SVM_PTRS | CL_KERNEL_EXEC_INFO_SVM_PTRS_ARM => {
            // it's a list of pointers
            if param_value_size % mem::size_of::<*const c_void>() != 0 {
                return Err(CL_INVALID_VALUE);
            }
        }
        CL_KERNEL_EXEC_INFO_SVM_FINE_GRAIN_SYSTEM
        | CL_KERNEL_EXEC_INFO_SVM_FINE_GRAIN_SYSTEM_ARM => {
            if param_value_size != mem::size_of::<cl_bool>() {
                return Err(CL_INVALID_VALUE);
            }
        }
        // CL_INVALID_VALUE if param_name is not valid
        _ => return Err(CL_INVALID_VALUE),
    }

    Ok(())

    // CL_INVALID_OPERATION if param_name is CL_KERNEL_EXEC_INFO_SVM_FINE_GRAIN_SYSTEM and param_value is CL_TRUE but no devices in context associated with kernel support fine-grain system SVM allocations.
}

#[cl_entrypoint(clEnqueueNDRangeKernel)]
fn enqueue_ndrange_kernel(
    command_queue: cl_command_queue,
    kernel: cl_kernel,
    work_dim: cl_uint,
    global_work_offset: *const usize,
    global_work_size: *const usize,
    local_work_size: *const usize,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = Queue::arc_from_raw(command_queue)?;
    let k = Kernel::arc_from_raw(kernel)?;
    let evs = event_list_from_cl(&q, num_events_in_wait_list, event_wait_list)?;

    // CL_INVALID_CONTEXT if context associated with command_queue and kernel are not the same
    if q.context != k.prog.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built program executable available
    // for device associated with command_queue.
    if k.prog.status(q.device) != CL_BUILD_SUCCESS as cl_build_status {
        return Err(CL_INVALID_PROGRAM_EXECUTABLE);
    }

    // CL_INVALID_KERNEL_ARGS if the kernel argument values have not been specified.
    if k.arg_values().iter().any(|v| v.is_none()) {
        return Err(CL_INVALID_KERNEL_ARGS);
    }

    // CL_INVALID_WORK_DIMENSION if work_dim is not a valid value (i.e. a value between 1 and
    // CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS).
    if work_dim == 0 || work_dim > q.device.max_grid_dimensions() {
        return Err(CL_INVALID_WORK_DIMENSION);
    }

    // we assume the application gets it right and doesn't pass shorter arrays then actually needed.
    let global_work_size = unsafe { kernel_work_arr_or_default(global_work_size, work_dim) };
    let local_work_size = unsafe { kernel_work_arr_or_default(local_work_size, work_dim) };
    let global_work_offset = unsafe { kernel_work_arr_or_default(global_work_offset, work_dim) };

    let device_bits = q.device.address_bits();
    let device_max = u64::MAX >> (u64::BITS - device_bits);

    let mut threads = 0;
    for i in 0..work_dim as usize {
        let lws = local_work_size[i];
        let gws = global_work_size[i];
        let gwo = global_work_offset[i];

        threads *= lws;

        // CL_INVALID_WORK_ITEM_SIZE if the number of work-items specified in any of
        // local_work_size[0], … local_work_size[work_dim - 1] is greater than the corresponding
        // values specified by
        // CL_DEVICE_MAX_WORK_ITEM_SIZES[0], …, CL_DEVICE_MAX_WORK_ITEM_SIZES[work_dim - 1].
        if lws > q.device.max_block_sizes()[i] {
            return Err(CL_INVALID_WORK_ITEM_SIZE);
        }

        // CL_INVALID_WORK_GROUP_SIZE if the work-group size must be uniform and the
        // local_work_size is not NULL, [...] if the global_work_size is not evenly divisible by
        // the local_work_size.
        if lws != 0 && gws % lws != 0 {
            return Err(CL_INVALID_WORK_GROUP_SIZE);
        }

        // CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and does not match the
        // required work-group size for kernel in the program source.
        if lws != 0 && k.work_group_size()[i] != 0 && lws != k.work_group_size()[i] {
            return Err(CL_INVALID_WORK_GROUP_SIZE);
        }

        // CL_INVALID_GLOBAL_WORK_SIZE if any of the values specified in global_work_size[0], …
        // global_work_size[work_dim - 1] exceed the maximum value representable by size_t on
        // the device on which the kernel-instance will be enqueued.
        if gws as u64 > device_max {
            return Err(CL_INVALID_GLOBAL_WORK_SIZE);
        }

        // CL_INVALID_GLOBAL_OFFSET if the value specified in global_work_size + the
        // corresponding values in global_work_offset for any dimensions is greater than the
        // maximum value representable by size t on the device on which the kernel-instance
        // will be enqueued
        if u64::checked_add(gws as u64, gwo as u64)
            .filter(|&x| x <= device_max)
            .is_none()
        {
            return Err(CL_INVALID_GLOBAL_OFFSET);
        }
    }

    // CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and the total number of work-items
    // in the work-group computed as local_work_size[0] × … local_work_size[work_dim - 1] is greater
    // than the value specified by CL_KERNEL_WORK_GROUP_SIZE in the Kernel Object Device Queries
    // table.
    if threads != 0 && threads > k.max_threads_per_block(q.device) {
        return Err(CL_INVALID_WORK_GROUP_SIZE);
    }

    // If global_work_size is NULL, or the value in any passed dimension is 0 then the kernel
    // command will trivially succeed after its event dependencies are satisfied and subsequently
    // update its completion event.
    let cb: EventSig = if global_work_size.contains(&0) {
        Box::new(|_, _| Ok(()))
    } else {
        k.launch(
            &q,
            work_dim,
            local_work_size,
            global_work_size,
            global_work_offset,
        )?
    };

    create_and_queue(q, CL_COMMAND_NDRANGE_KERNEL, evs, event, false, cb)

    //• CL_INVALID_WORK_GROUP_SIZE if local_work_size is specified and is not consistent with the required number of sub-groups for kernel in the program source.
    //• CL_MISALIGNED_SUB_BUFFER_OFFSET if a sub-buffer object is specified as the value for an argument that is a buffer object and the offset specified when the sub-buffer object is created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN value for device associated with queue. This error code
    //• CL_INVALID_IMAGE_SIZE if an image object is specified as an argument value and the image dimensions (image width, height, specified or compute row and/or slice pitch) are not supported by device associated with queue.
    //• CL_IMAGE_FORMAT_NOT_SUPPORTED if an image object is specified as an argument value and the image format (image channel order and data type) is not supported by device associated with queue.
    //• CL_OUT_OF_RESOURCES if there is a failure to queue the execution instance of kernel on the command-queue because of insufficient resources needed to execute the kernel. For example, the explicitly specified local_work_size causes a failure to execute the kernel because of insufficient resources such as registers or local memory. Another example would be the number of read-only image args used in kernel exceed the CL_DEVICE_MAX_READ_IMAGE_ARGS value for device or the number of write-only and read-write image args used in kernel exceed the CL_DEVICE_MAX_READ_WRITE_IMAGE_ARGS value for device or the number of samplers used in kernel exceed CL_DEVICE_MAX_SAMPLERS for device.
    //• CL_MEM_OBJECT_ALLOCATION_FAILURE if there is a failure to allocate memory for data store associated with image or buffer objects specified as arguments to kernel.
    //• CL_INVALID_OPERATION if SVM pointers are passed as arguments to a kernel and the device does not support SVM or if system pointers are passed as arguments to a kernel and/or stored inside SVM allocations passed as kernel arguments and the device does not support fine grain system SVM allocations.
}

#[cl_entrypoint(clEnqueueTask)]
fn enqueue_task(
    command_queue: cl_command_queue,
    kernel: cl_kernel,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    // clEnqueueTask is equivalent to calling clEnqueueNDRangeKernel with work_dim set to 1,
    // global_work_offset set to NULL, global_work_size[0] set to 1, and local_work_size[0] set to
    // 1.
    enqueue_ndrange_kernel(
        command_queue,
        kernel,
        1,
        ptr::null(),
        [1, 1, 1].as_ptr(),
        [1, 0, 0].as_ptr(),
        num_events_in_wait_list,
        event_wait_list,
        event,
    )
}

#[cl_entrypoint(clCloneKernel)]
fn clone_kernel(source_kernel: cl_kernel) -> CLResult<cl_kernel> {
    let k = Kernel::ref_from_raw(source_kernel)?;
    Ok(Arc::new(k.clone()).into_cl())
}

#[cl_entrypoint(clGetKernelSuggestedLocalWorkSizeKHR)]
fn get_kernel_suggested_local_work_size_khr(
    command_queue: cl_command_queue,
    kernel: cl_kernel,
    work_dim: cl_uint,
    global_work_offset: *const usize,
    global_work_size: *const usize,
    suggested_local_work_size: *mut usize,
) -> CLResult<()> {
    // CL_INVALID_GLOBAL_WORK_SIZE if global_work_size is NULL or if any of the values specified in
    // global_work_size are 0.
    if global_work_size.is_null() {
        return Err(CL_INVALID_GLOBAL_WORK_SIZE);
    }

    if global_work_offset.is_null() {
        return Err(CL_INVALID_GLOBAL_OFFSET);
    }

    // CL_INVALID_VALUE if suggested_local_work_size is NULL.
    if suggested_local_work_size.is_null() {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_COMMAND_QUEUE if command_queue is not a valid host command-queue.
    let queue = Queue::ref_from_raw(command_queue)?;

    // CL_INVALID_KERNEL if kernel is not a valid kernel object.
    let kernel = Kernel::ref_from_raw(kernel)?;

    // CL_INVALID_CONTEXT if the context associated with kernel is not the same as the context
    // associated with command_queue.
    if queue.context != kernel.prog.context {
        return Err(CL_INVALID_CONTEXT);
    }

    // CL_INVALID_PROGRAM_EXECUTABLE if there is no successfully built program executable available
    // for kernel for the device associated with command_queue.
    if kernel.prog.status(queue.device) != CL_BUILD_SUCCESS as cl_build_status {
        return Err(CL_INVALID_PROGRAM_EXECUTABLE);
    }

    // CL_INVALID_KERNEL_ARGS if all argument values for kernel have not been set.
    if kernel.arg_values().iter().any(|v| v.is_none()) {
        return Err(CL_INVALID_KERNEL_ARGS);
    }

    // CL_INVALID_WORK_DIMENSION if work_dim is not a valid value (i.e. a value between 1 and
    // CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS).
    if work_dim == 0 || work_dim > queue.device.max_grid_dimensions() {
        return Err(CL_INVALID_WORK_DIMENSION);
    }

    let mut global_work_size =
        unsafe { kernel_work_arr_or_default(global_work_size, work_dim).to_vec() };

    let suggested_local_work_size = unsafe {
        kernel_work_arr_mut(suggested_local_work_size, work_dim).ok_or(CL_INVALID_VALUE)?
    };

    let global_work_offset = unsafe { kernel_work_arr_or_default(global_work_offset, work_dim) };

    let device_bits = queue.device.address_bits();
    let device_max = u64::MAX >> (u64::BITS - device_bits);
    for i in 0..work_dim as usize {
        let gws = global_work_size[i];
        let gwo = global_work_offset[i];

        // CL_INVALID_GLOBAL_WORK_SIZE if global_work_size is NULL or if any of the values specified
        // in global_work_size are 0.
        if gws == 0 {
            return Err(CL_INVALID_GLOBAL_WORK_SIZE);
        }
        // CL_INVALID_GLOBAL_WORK_SIZE if any of the values specified in global_work_size exceed the
        // maximum value representable by size_t on the device associated with command_queue.
        if gws as u64 > device_max {
            return Err(CL_INVALID_GLOBAL_WORK_SIZE);
        }
        // CL_INVALID_GLOBAL_OFFSET if the value specified in global_work_size plus the
        // corresponding value in global_work_offset for dimension exceeds the maximum value
        // representable by size_t on the device associated with command_queue.
        if u64::checked_add(gws as u64, gwo as u64)
            .filter(|&x| x <= device_max)
            .is_none()
        {
            return Err(CL_INVALID_GLOBAL_OFFSET);
        }
    }

    kernel.suggest_local_size(
        queue.device,
        work_dim as usize,
        &mut global_work_size,
        suggested_local_work_size,
    );

    Ok(())

    // CL_MISALIGNED_SUB_BUFFER_OFFSET if a sub-buffer object is set as an argument to kernel and the offset specified when the sub-buffer object was created is not aligned to CL_DEVICE_MEM_BASE_ADDR_ALIGN for the device associated with command_queue.
    // CL_INVALID_IMAGE_SIZE if an image object is set as an argument to kernel and the image dimensions are not supported by device associated with command_queue.
    // CL_IMAGE_FORMAT_NOT_SUPPORTED if an image object is set as an argument to kernel and the image format is not supported by the device associated with command_queue.
    // CL_INVALID_OPERATION if an SVM pointer is set as an argument to kernel and the device associated with command_queue does not support SVM or the required SVM capabilities for the SVM pointer.
    // CL_OUT_OF_RESOURCES if there is a failure to allocate resources required by the OpenCL implementation on the device.
    // CL_OUT_OF_HOST_MEMORY if there is a failure to allocate resources required by the OpenCL implementation on the host.
}
