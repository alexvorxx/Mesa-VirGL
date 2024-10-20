use crate::api::event::create_and_queue;
use crate::api::icd::*;
use crate::api::util::*;
use crate::core::context::*;
use crate::core::device::*;
use crate::core::event::*;
use crate::core::queue::*;

use mesa_rust_util::properties::*;
use rusticl_opencl_gen::*;
use rusticl_proc_macros::cl_entrypoint;
use rusticl_proc_macros::cl_info_entrypoint;

use std::mem::MaybeUninit;
use std::ptr;
use std::sync::Arc;

#[cl_info_entrypoint(clGetCommandQueueInfo)]
impl CLInfo<cl_command_queue_info> for cl_command_queue {
    fn query(&self, q: cl_command_queue_info, _: &[u8]) -> CLResult<Vec<MaybeUninit<u8>>> {
        let queue = Queue::ref_from_raw(*self)?;
        Ok(match q {
            CL_QUEUE_CONTEXT => {
                // Note we use as_ptr here which doesn't increase the reference count.
                let ptr = Arc::as_ptr(&queue.context);
                cl_prop::<cl_context>(cl_context::from_ptr(ptr))
            }
            CL_QUEUE_DEVICE => cl_prop::<cl_device_id>(cl_device_id::from_ptr(queue.device)),
            CL_QUEUE_DEVICE_DEFAULT => cl_prop::<cl_command_queue>(ptr::null_mut()),
            CL_QUEUE_PROPERTIES => cl_prop::<cl_command_queue_properties>(queue.props),
            CL_QUEUE_PROPERTIES_ARRAY => {
                cl_prop::<&Option<Properties<cl_queue_properties>>>(&queue.props_v2)
            }
            CL_QUEUE_REFERENCE_COUNT => cl_prop::<cl_uint>(Queue::refcnt(*self)?),
            // clGetCommandQueueInfo, passing CL_QUEUE_SIZE Returns CL_INVALID_COMMAND_QUEUE since
            // command_queue cannot be a valid device command-queue.
            CL_QUEUE_SIZE => return Err(CL_INVALID_COMMAND_QUEUE),
            // CL_INVALID_VALUE if param_name is not one of the supported values
            _ => return Err(CL_INVALID_VALUE),
        })
    }
}

#[cl_entrypoint(clSetCommandQueueProperty)]
fn set_command_queue_property(
    _command_queue: cl_command_queue,
    _properties: cl_command_queue_properties,
    _enable: cl_bool,
    _old_properties: *mut cl_command_queue_properties,
) -> CLResult<()> {
    // clSetCommandQueueProperty may unconditionally return an error if no devices in the context
    // associated with command_queue support modifying the properties of a command-queue. Support
    // for modifying the properties of a command-queue is required only for OpenCL 1.0 devices.
    //
    // CL_INVALID_OPERATION if no devices in the context associated with command_queue support
    // modifying the properties of a command-queue.
    Err(CL_INVALID_OPERATION)
}

fn valid_command_queue_properties(properties: cl_command_queue_properties) -> bool {
    let valid_flags = cl_bitfield::from(
        CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE
            | CL_QUEUE_PROFILING_ENABLE
            | CL_QUEUE_ON_DEVICE
            | CL_QUEUE_ON_DEVICE_DEFAULT,
    );
    properties & !valid_flags == 0
}

fn supported_command_queue_properties(
    dev: &Device,
    properties: cl_command_queue_properties,
) -> bool {
    let profiling = cl_bitfield::from(CL_QUEUE_PROFILING_ENABLE);
    let valid_flags = profiling;
    if properties & !valid_flags != 0 {
        return false;
    }

    if properties & profiling != 0 && !dev.caps.has_timestamp {
        return false;
    }

    true
}

pub fn create_command_queue_impl(
    context: cl_context,
    device: cl_device_id,
    properties: cl_command_queue_properties,
    properties_v2: Option<Properties<cl_queue_properties>>,
) -> CLResult<cl_command_queue> {
    let c = Context::arc_from_raw(context)?;
    let d = Device::ref_from_raw(device)?
        .to_static()
        .ok_or(CL_INVALID_DEVICE)?;

    // CL_INVALID_DEVICE if device [...] is not associated with context.
    if !c.devs.contains(&d) {
        return Err(CL_INVALID_DEVICE);
    }

    // CL_INVALID_VALUE if values specified in properties are not valid.
    if !valid_command_queue_properties(properties) {
        return Err(CL_INVALID_VALUE);
    }

    // CL_INVALID_QUEUE_PROPERTIES if values specified in properties are valid but are not supported by the device.
    if !supported_command_queue_properties(d, properties) {
        return Err(CL_INVALID_QUEUE_PROPERTIES);
    }

    Ok(Queue::new(c, d, properties, properties_v2)?.into_cl())
}

#[cl_entrypoint(clCreateCommandQueue)]
fn create_command_queue(
    context: cl_context,
    device: cl_device_id,
    properties: cl_command_queue_properties,
) -> CLResult<cl_command_queue> {
    create_command_queue_impl(context, device, properties, None)
}

#[cl_entrypoint(clCreateCommandQueueWithProperties)]
fn create_command_queue_with_properties(
    context: cl_context,
    device: cl_device_id,
    properties: *const cl_queue_properties,
) -> CLResult<cl_command_queue> {
    let mut queue_properties = cl_command_queue_properties::default();
    let properties = if properties.is_null() {
        None
    } else {
        let properties = Properties::from_ptr(properties).ok_or(CL_INVALID_PROPERTY)?;

        for (k, v) in &properties.props {
            match *k as cl_uint {
                CL_QUEUE_PROPERTIES => queue_properties = *v,
                // CL_INVALID_QUEUE_PROPERTIES if values specified in properties are valid but are not
                // supported by the device.
                CL_QUEUE_SIZE => return Err(CL_INVALID_QUEUE_PROPERTIES),
                _ => return Err(CL_INVALID_PROPERTY),
            }
        }

        Some(properties)
    };

    create_command_queue_impl(context, device, queue_properties, properties)
}

#[cl_entrypoint(clEnqueueMarker)]
fn enqueue_marker(command_queue: cl_command_queue, event: *mut cl_event) -> CLResult<()> {
    let q = Queue::arc_from_raw(command_queue)?;

    // TODO marker makes sure previous commands did complete
    create_and_queue(
        q,
        CL_COMMAND_MARKER,
        Vec::new(),
        event,
        false,
        Box::new(|_, _| Ok(())),
    )
}

#[cl_entrypoint(clEnqueueMarkerWithWaitList)]
fn enqueue_marker_with_wait_list(
    command_queue: cl_command_queue,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = Queue::arc_from_raw(command_queue)?;
    let evs = event_list_from_cl(&q, num_events_in_wait_list, event_wait_list)?;

    // TODO marker makes sure previous commands did complete
    create_and_queue(
        q,
        CL_COMMAND_MARKER,
        evs,
        event,
        false,
        Box::new(|_, _| Ok(())),
    )
}

#[cl_entrypoint(clEnqueueBarrier)]
fn enqueue_barrier(command_queue: cl_command_queue) -> CLResult<()> {
    let q = Queue::arc_from_raw(command_queue)?;

    // TODO barriers make sure previous commands did complete and other commands didn't start
    let e = Event::new(&q, CL_COMMAND_BARRIER, Vec::new(), Box::new(|_, _| Ok(())));
    q.queue(e);
    Ok(())
}

#[cl_entrypoint(clEnqueueBarrierWithWaitList)]
fn enqueue_barrier_with_wait_list(
    command_queue: cl_command_queue,
    num_events_in_wait_list: cl_uint,
    event_wait_list: *const cl_event,
    event: *mut cl_event,
) -> CLResult<()> {
    let q = Queue::arc_from_raw(command_queue)?;
    let evs = event_list_from_cl(&q, num_events_in_wait_list, event_wait_list)?;

    // TODO barriers make sure previous commands did complete and other commands didn't start
    create_and_queue(
        q,
        CL_COMMAND_BARRIER,
        evs,
        event,
        false,
        Box::new(|_, _| Ok(())),
    )
}

#[cl_entrypoint(clFlush)]
fn flush(command_queue: cl_command_queue) -> CLResult<()> {
    // CL_INVALID_COMMAND_QUEUE if command_queue is not a valid host command-queue.
    Queue::ref_from_raw(command_queue)?.flush(false)
}

#[cl_entrypoint(clFinish)]
fn finish(command_queue: cl_command_queue) -> CLResult<()> {
    // CL_INVALID_COMMAND_QUEUE if command_queue is not a valid host command-queue.
    Queue::ref_from_raw(command_queue)?.flush(true)
}

#[cl_entrypoint(clRetainCommandQueue)]
fn retain_command_queue(command_queue: cl_command_queue) -> CLResult<()> {
    Queue::retain(command_queue)
}

#[cl_entrypoint(clReleaseCommandQueue)]
fn release_command_queue(command_queue: cl_command_queue) -> CLResult<()> {
    // clReleaseCommandQueue performs an implicit flush to issue any previously queued OpenCL
    // commands in command_queue.
    flush(command_queue)?;
    Queue::release(command_queue)
}
