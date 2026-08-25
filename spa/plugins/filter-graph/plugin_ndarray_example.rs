// SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors
// SPDX-License-Identifier: MIT

//! Minimal Rust `cdylib` implementation of the ndarray filter-graph C ABI.
//!
//! Calculon should generate these declarations from `ndarray-plugin.h`; they
//! are written out here so this repository can test the language boundary
//! without adding a Rust package dependency to PipeWireAO.

use std::ffi::{CStr, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

const ABI_VERSION: u32 = 0;
const DIRECTION_INPUT: u32 = 0;
const DIRECTION_OUTPUT: u32 = 1;
const PROPERTY_READONLY: u32 = 1;
const PROPERTY_RANGE: u32 = 2;
const TYPE_LONG: u32 = 5;
const TYPE_FLOAT: u32 = 6;
const ELEMENT_F32_LE: u32 = 18;
const LAYOUT_ROW_MAJOR: u32 = 1;
const META_HEADER: u32 = 1;
const EINVAL: i32 = 22;
const ENOENT: i32 = 2;
const EFAULT: i32 = 14;

#[repr(C)]
struct SpaChunk {
    offset: u32,
    size: u32,
    stride: i32,
    flags: i32,
}

#[repr(C)]
struct SpaData {
    type_: u32,
    flags: u32,
    fd: i64,
    mapoffset: u32,
    maxsize: u32,
    data: *mut c_void,
    chunk: *mut SpaChunk,
}

#[repr(C)]
struct SpaMeta {
    type_: u32,
    size: u32,
    data: *mut c_void,
}

#[repr(C)]
struct SpaBuffer {
    n_metas: u32,
    n_datas: u32,
    metas: *mut SpaMeta,
    datas: *mut SpaData,
}

#[repr(C)]
struct Format {
    element_type: u32,
    layout: u32,
    rate_num: u32,
    rate_denom: u32,
    n_dimensions: u32,
    shape: *const u32,
    schema: *const c_char,
    profile: *const c_char,
}

unsafe impl Sync for Format {}

#[repr(C)]
struct PortInfo {
    struct_size: u32,
    index: u32,
    direction: u32,
    flags: u32,
    name: *const c_char,
}

unsafe impl Sync for PortInfo {}

#[repr(C)]
#[derive(Clone, Copy)]
union ValueBody {
    boolean: i32,
    integer: i32,
    long_integer: i64,
    float_value: f32,
    double_value: f64,
    id: u32,
    string: *const c_char,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Value {
    type_: u32,
    reserved: u32,
    value: ValueBody,
}

#[repr(C)]
struct PropertyInfo {
    struct_size: u32,
    id: u32,
    flags: u32,
    reserved: u32,
    name: *const c_char,
    description: *const c_char,
    default_value: Value,
    minimum: Value,
    maximum: Value,
}

#[repr(C)]
struct Property {
    id: u32,
    reserved: u32,
    value: Value,
}

#[repr(C)]
struct FgnBuffer {
    buffer: *mut SpaBuffer,
    format: *const Format,
}

type Instantiate = unsafe extern "C" fn(*const Descriptor, *const c_char, *mut *mut c_void) -> i32;
type Cleanup = unsafe extern "C" fn(*mut c_void);
type GetPortFormat = unsafe extern "C" fn(*mut c_void, u32, *mut *const Format) -> i32;
type EnumPropInfo = unsafe extern "C" fn(*mut c_void, u32, *mut PropertyInfo) -> i32;
type GetProp = unsafe extern "C" fn(*mut c_void, u32, *mut Value) -> i32;
type PrepareProps =
    unsafe extern "C" fn(*mut c_void, *const Property, u32, *mut *mut c_void) -> i32;
type PropsAction = unsafe extern "C" fn(*mut c_void, *mut c_void);
type Lifecycle = unsafe extern "C" fn(*mut c_void) -> i32;
type Process = unsafe extern "C" fn(*mut c_void, *const FgnBuffer, u32, *mut FgnBuffer, u32) -> i32;

#[repr(C)]
struct Descriptor {
    struct_size: u32,
    version: u32,
    name: *const c_char,
    n_ports: u32,
    ports: *const PortInfo,
    instantiate: Option<Instantiate>,
    cleanup: Option<Cleanup>,
    get_port_format: Option<GetPortFormat>,
    enum_prop_info: Option<EnumPropInfo>,
    get_prop: Option<GetProp>,
    prepare_props: Option<PrepareProps>,
    commit_props: Option<PropsAction>,
    discard_props: Option<PropsAction>,
    activate: Option<Lifecycle>,
    deactivate: Option<Lifecycle>,
    reset: Option<Lifecycle>,
    process: Option<Process>,
}

unsafe impl Sync for Descriptor {}

type FindDescriptor = unsafe extern "C" fn(*const c_char) -> *const Descriptor;

#[repr(C)]
struct Plugin {
    struct_size: u32,
    abi_version: u32,
    name: *const c_char,
    find_descriptor: Option<FindDescriptor>,
}

unsafe impl Sync for Plugin {}

struct Instance {
    requested_state: AtomicU64,
    active_state: AtomicU64,
}

struct Prepared {
    gain: f32,
}

fn pack_state(generation: u32, gain: f32) -> u64 {
    (u64::from(generation) << 32) | u64::from(gain.to_bits())
}

fn state_generation(state: u64) -> u32 {
    (state >> 32) as u32
}

fn state_gain(state: u64) -> f32 {
    f32::from_bits(state as u32)
}

static SHAPE: [u32; 2] = [2, 2];
static FORMAT: Format = Format {
    element_type: ELEMENT_F32_LE,
    layout: LAYOUT_ROW_MAJOR,
    rate_num: 0,
    rate_denom: 0,
    n_dimensions: 2,
    shape: SHAPE.as_ptr(),
    schema: c"test.matrix/1".as_ptr(),
    profile: ptr::null(),
};

static PORTS: [PortInfo; 2] = [
    PortInfo {
        struct_size: size_of::<PortInfo>() as u32,
        index: 0,
        direction: DIRECTION_INPUT,
        flags: 0,
        name: c"in".as_ptr(),
    },
    PortInfo {
        struct_size: size_of::<PortInfo>() as u32,
        index: 1,
        direction: DIRECTION_OUTPUT,
        flags: 0,
        name: c"out".as_ptr(),
    },
];

fn float_value(value: f32) -> Value {
    Value {
        type_: TYPE_FLOAT,
        reserved: 0,
        value: ValueBody { float_value: value },
    }
}

fn long_value(value: i64) -> Value {
    Value {
        type_: TYPE_LONG,
        reserved: 0,
        value: ValueBody {
            long_integer: value,
        },
    }
}

unsafe extern "C" fn instantiate(
    _descriptor: *const Descriptor,
    _config: *const c_char,
    result: *mut *mut c_void,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if result.is_null() {
            return -EINVAL;
        }
        let instance = Box::new(Instance {
            requested_state: AtomicU64::new(pack_state(0, 1.0)),
            active_state: AtomicU64::new(pack_state(0, 1.0)),
        });
        // SAFETY: result was validated above and ownership crosses the ABI.
        unsafe { *result = Box::into_raw(instance).cast() };
        0
    }))
    .unwrap_or(-EFAULT)
}

unsafe extern "C" fn cleanup(data: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !data.is_null() {
            // SAFETY: data came from Box::into_raw in instantiate and is freed once.
            unsafe { drop(Box::from_raw(data.cast::<Instance>())) };
        }
    }));
}

unsafe extern "C" fn get_port_format(
    _data: *mut c_void,
    port: u32,
    format: *mut *const Format,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if format.is_null() || port >= PORTS.len() as u32 {
            return -EINVAL;
        }
        // SAFETY: the output pointer is valid for this callback.
        unsafe { *format = &FORMAT };
        0
    }))
    .unwrap_or(-EFAULT)
}

unsafe extern "C" fn enum_prop_info(data: *mut c_void, index: u32, info: *mut PropertyInfo) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if data.is_null() || info.is_null() {
            return -EINVAL;
        }
        let instance = unsafe { &*data.cast::<Instance>() };
        let requested_state = instance.requested_state.load(Ordering::Acquire);
        let active_state = instance.active_state.load(Ordering::Acquire);
        let (name, description, flags, default_value, minimum, maximum) = match index {
            0 => (
                c"gain",
                c"Requested scalar gain",
                PROPERTY_RANGE,
                float_value(state_gain(requested_state)),
                float_value(-16.0),
                float_value(16.0),
            ),
            1 => (
                c"active-gain",
                c"Scalar gain adopted by process()",
                PROPERTY_READONLY,
                float_value(state_gain(active_state)),
                Value {
                    type_: 0,
                    reserved: 0,
                    value: ValueBody { long_integer: 0 },
                },
                Value {
                    type_: 0,
                    reserved: 0,
                    value: ValueBody { long_integer: 0 },
                },
            ),
            2 => (
                c"requested-generation",
                c"Generation published by the control thread",
                PROPERTY_READONLY,
                long_value(0),
                Value {
                    type_: 0,
                    reserved: 0,
                    value: ValueBody { long_integer: 0 },
                },
                Value {
                    type_: 0,
                    reserved: 0,
                    value: ValueBody { long_integer: 0 },
                },
            ),
            3 => (
                c"active-generation",
                c"Generation adopted by process()",
                PROPERTY_READONLY,
                long_value(0),
                Value {
                    type_: 0,
                    reserved: 0,
                    value: ValueBody { long_integer: 0 },
                },
                Value {
                    type_: 0,
                    reserved: 0,
                    value: ValueBody { long_integer: 0 },
                },
            ),
            _ => return 0,
        };
        let value = PropertyInfo {
            struct_size: size_of::<PropertyInfo>() as u32,
            id: index,
            flags,
            reserved: 0,
            name: name.as_ptr(),
            description: description.as_ptr(),
            default_value,
            minimum,
            maximum,
        };
        unsafe { *info = value };
        1
    }))
    .unwrap_or(-EFAULT)
}

unsafe extern "C" fn get_prop(data: *mut c_void, id: u32, value: *mut Value) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if data.is_null() || value.is_null() {
            return -EINVAL;
        }
        let instance = unsafe { &*data.cast::<Instance>() };
        let requested_state = instance.requested_state.load(Ordering::Acquire);
        let active_state = instance.active_state.load(Ordering::Acquire);
        let current = match id {
            0 => float_value(state_gain(requested_state)),
            1 => float_value(state_gain(active_state)),
            2 => long_value(i64::from(state_generation(requested_state))),
            3 => long_value(i64::from(state_generation(active_state))),
            _ => return -ENOENT,
        };
        unsafe { *value = current };
        0
    }))
    .unwrap_or(-EFAULT)
}

unsafe extern "C" fn prepare_props(
    data: *mut c_void,
    properties: *const Property,
    n_properties: u32,
    result: *mut *mut c_void,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if data.is_null() || result.is_null() || (n_properties != 0 && properties.is_null()) {
            return -EINVAL;
        }
        let instance = unsafe { &*data.cast::<Instance>() };
        let mut gain = state_gain(instance.requested_state.load(Ordering::Acquire));
        for index in 0..n_properties as usize {
            let property = unsafe { &*properties.add(index) };
            if property.id != 0 || property.value.type_ != TYPE_FLOAT {
                return -EINVAL;
            }
            let value = unsafe { property.value.value.float_value };
            if !value.is_finite() || !(-16.0..=16.0).contains(&value) {
                return -EINVAL;
            }
            gain = value;
        }
        unsafe { *result = Box::into_raw(Box::new(Prepared { gain })).cast() };
        0
    }))
    .unwrap_or(-EFAULT)
}

unsafe extern "C" fn commit_props(data: *mut c_void, prepared: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if data.is_null() || prepared.is_null() {
            return;
        }
        let instance = unsafe { &*data.cast::<Instance>() };
        let prepared = unsafe { Box::from_raw(prepared.cast::<Prepared>()) };
        let current = instance.requested_state.load(Ordering::Relaxed);
        instance.requested_state.store(
            pack_state(state_generation(current).wrapping_add(1), prepared.gain),
            Ordering::Release,
        );
    }));
}

unsafe extern "C" fn discard_props(_data: *mut c_void, prepared: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !prepared.is_null() {
            unsafe { drop(Box::from_raw(prepared.cast::<Prepared>())) };
        }
    }));
}

unsafe fn copy_metadata(output: *mut SpaBuffer, input: *const SpaBuffer) {
    let output = unsafe { &mut *output };
    let input = unsafe { &*input };
    for output_index in 0..output.n_metas as usize {
        let destination = unsafe { &mut *output.metas.add(output_index) };
        for input_index in 0..input.n_metas as usize {
            let source = unsafe { &*input.metas.add(input_index) };
            if source.type_ == destination.type_
                && !source.data.is_null()
                && !destination.data.is_null()
            {
                unsafe {
                    ptr::copy(
                        source.data.cast::<u8>(),
                        destination.data.cast::<u8>(),
                        source.size.min(destination.size) as usize,
                    )
                };
                break;
            }
        }
    }
}

unsafe extern "C" fn process(
    data: *mut c_void,
    inputs: *const FgnBuffer,
    n_inputs: u32,
    outputs: *mut FgnBuffer,
    n_outputs: u32,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        if data.is_null()
            || inputs.is_null()
            || outputs.is_null()
            || n_inputs != 1
            || n_outputs != 1
        {
            return -EINVAL;
        }
        let instance = unsafe { &*data.cast::<Instance>() };
        let input = unsafe { &*inputs };
        let output = unsafe { &mut *outputs };
        if input.buffer.is_null() || output.buffer.is_null() {
            return -EINVAL;
        }
        let state = instance.requested_state.load(Ordering::Acquire);
        instance.active_state.store(state, Ordering::Release);
        let gain = state_gain(state);
        let input_buffer = unsafe { &*input.buffer };
        let output_buffer = unsafe { &mut *output.buffer };
        if input_buffer.n_datas == 0 || output_buffer.n_datas == 0 {
            return -EINVAL;
        }
        let input_data = unsafe { &*input_buffer.datas };
        let output_data = unsafe { &mut *output_buffer.datas };
        if input_data.data.is_null()
            || output_data.data.is_null()
            || input_data.chunk.is_null()
            || output_data.chunk.is_null()
        {
            return -EINVAL;
        }
        let input_values = unsafe {
            input_data
                .data
                .cast::<u8>()
                .add((*input_data.chunk).offset as usize)
                .cast::<f32>()
        };
        let output_values = unsafe {
            output_data
                .data
                .cast::<u8>()
                .add((*output_data.chunk).offset as usize)
                .cast::<f32>()
        };
        for index in 0..4 {
            unsafe { *output_values.add(index) = *input_values.add(index) * gain };
        }
        unsafe { (*output_data.chunk).size = 4 * size_of::<f32>() as u32 };
        unsafe { copy_metadata(output.buffer, input.buffer) };
        0
    }))
    .unwrap_or(-EFAULT)
}

unsafe extern "C" fn find_descriptor(name: *const c_char) -> *const Descriptor {
    catch_unwind(AssertUnwindSafe(|| {
        if name.is_null() {
            return ptr::null();
        }
        let name = unsafe { CStr::from_ptr(name) };
        if name == c"scale-f32" {
            &DESCRIPTOR
        } else {
            ptr::null()
        }
    }))
    .unwrap_or(ptr::null())
}

static DESCRIPTOR: Descriptor = Descriptor {
    struct_size: size_of::<Descriptor>() as u32,
    version: ABI_VERSION,
    name: c"scale-f32".as_ptr(),
    n_ports: PORTS.len() as u32,
    ports: PORTS.as_ptr(),
    instantiate: Some(instantiate),
    cleanup: Some(cleanup),
    get_port_format: Some(get_port_format),
    enum_prop_info: Some(enum_prop_info),
    get_prop: Some(get_prop),
    prepare_props: Some(prepare_props),
    commit_props: Some(commit_props),
    discard_props: Some(discard_props),
    activate: None,
    deactivate: None,
    reset: None,
    process: Some(process),
};

static PLUGIN: Plugin = Plugin {
    struct_size: size_of::<Plugin>() as u32,
    abi_version: ABI_VERSION,
    name: c"example-rust".as_ptr(),
    find_descriptor: Some(find_descriptor),
};

#[unsafe(no_mangle)]
extern "C" fn spa_filter_graph_ndarray_plugin_get_interface(abi_version: u32) -> *const Plugin {
    if abi_version == ABI_VERSION {
        &PLUGIN
    } else {
        ptr::null()
    }
}

// Keep the constants used by the fixture tied to the C ABI on the platforms
// qualified by PipeWireAO.
const _: () = assert!(size_of::<Value>() == 16);
const _: () = assert!(size_of::<FgnBuffer>() == 16);
const _: () = assert!(META_HEADER == 1);
