// SPDX-License-Identifier: MIT
//! Command queues and buffers (`MTLCommandQueue`, `MTLCommandBuffer`): recording, replay into the
//! Gallium context, submission and completion.
//!
//! Encoders record `Cmd`s (Gallium-ready: shaders, bindings, push bytes, state) into their command
//! buffer. `commit` hands the buffer to its queue's thread, which replays the commands in order
//! into the device's one pipe_context under the device lock and flushes with a fence; event waits
//! and fence waits release the lock and block that queue only. The device's completion thread
//! waits on the fences in submission order, copies no-copy buffers' shadows back to their pages,
//! sets events, marks the buffer completed and runs its handlers.

use crate::bind::{apply, Bind};
use crate::device::{Device, Gpu, GpuLock};
use crate::error::{invalid, Error};
use crate::ffi;
use crate::pipeline::Shader;
use crate::resource::{Buffer, BufferInner, ResourceRef, SgTexture};
use crate::sync::{EventInner, EventRef, FenceInner};
use std::collections::HashSet;
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

/// Seconds on the host's monotonic clock (Metal's GPU times are host times).
pub(crate) fn host_seconds() -> f64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: a valid out-parameter.
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as f64 + ts.tv_nsec as f64 * 1e-9
}

// ---- recorded commands ----

pub(crate) struct DispatchRegion {
    pub(crate) shader: Arc<Shader>,
    pub(crate) push: Vec<u8>,
    pub(crate) groups: [u32; 3],
}

pub(crate) struct DrawCmd {
    pub(crate) vs: Arc<Shader>,
    pub(crate) fs: Arc<Shader>,
    pub(crate) binds: Vec<Bind>,
    pub(crate) push: Vec<u8>,
    pub(crate) blend: Vec<ffi::sg_blend_rt>,
    pub(crate) alpha_to_coverage: bool,
    pub(crate) alpha_to_one: bool,
    pub(crate) blend_color: [f32; 4],
    pub(crate) raster: ffi::sg_raster,
    pub(crate) depth_stencil: Option<ffi::sg_depth_stencil>,
    pub(crate) stencil_ref: [u32; 2],
    pub(crate) viewport: [f64; 6],
    pub(crate) scissor: [u32; 4],
    pub(crate) elements: Vec<ffi::sg_vertex_element>,
    pub(crate) vertex_buffers: Vec<Option<Arc<BufferInner>>>,
    pub(crate) vertex_offsets: Vec<u64>,
    pub(crate) primitive: u32,
    pub(crate) primitive_restart: bool,
    pub(crate) first_vertex: u32,
    pub(crate) count: u32,
    pub(crate) first_instance: u32,
    pub(crate) instances: u32,
    pub(crate) index: Option<(Arc<BufferInner>, u32, u64)>,
    pub(crate) base_vertex: i32,
    pub(crate) texture_barrier: bool,
}

pub(crate) struct PassCmd {
    pub(crate) pass: ffi::sg_pass,
    /// What the pass's raw texture pointers point at, kept alive.
    #[allow(dead_code)] // held, not read
    pub(crate) textures: Vec<Arc<SgTexture>>,
}

// SAFETY: the raw pointers in `pass` are kept valid by `textures`.
unsafe impl Send for PassCmd {}

/// A pass attachment's multisample resolve (its store action), replayed after the pass ends.
pub(crate) struct ResolveCmd {
    pub(crate) src: Arc<SgTexture>,
    pub(crate) src_format: u32,
    pub(crate) src_level: u32,
    pub(crate) src_layer: u32,
    pub(crate) dst: Arc<SgTexture>,
    pub(crate) dst_format: u32,
    pub(crate) dst_level: u32,
    pub(crate) dst_layer: u32,
    pub(crate) width: u32,
    pub(crate) height: u32,
    /// ffi::SG_ASPECT_*
    pub(crate) aspects: u32,
}

pub(crate) enum Cmd {
    Dispatch { binds: Vec<Bind>, regions: Vec<DispatchRegion>, shared: u32 },
    BeginPass(Box<PassCmd>),
    Draw(Box<DrawCmd>),
    EndPass,
    Resolve(Box<ResolveCmd>),
    CopyBuffer { src: Arc<BufferInner>, src_offset: u64, dst: Arc<BufferInner>, dst_offset: u64, size: u64 },
    FillBuffer { buffer: Arc<BufferInner>, offset: u64, size: u64, value: u8 },
    CopyTexture {
        src: Arc<SgTexture>,
        src_level: u32,
        src_layer: u32,
        src_box: ffi::sg_box,
        dst: Arc<SgTexture>,
        dst_level: u32,
        dst_layer: u32,
        dst_origin: [u32; 3],
    },
    TextureToBuffer {
        src: Arc<SgTexture>,
        level: u32,
        layer: u32,
        b: ffi::sg_box,
        dst: Arc<BufferInner>,
        offset: u64,
        stride: u32,
        layer_stride: u32,
    },
    BufferToTexture {
        src: Arc<BufferInner>,
        offset: u64,
        stride: u32,
        layer_stride: u32,
        dst: Arc<SgTexture>,
        level: u32,
        layer: u32,
        b: ffi::sg_box,
    },
    GenerateMipmaps(Arc<SgTexture>),
    SignalEvent(Arc<EventInner>, u64),
    WaitEvent(Arc<EventInner>, u64),
    /// generation assigned at commit
    UpdateFence(Arc<FenceInner>, u64),
    /// the generation to wait for, assigned at commit
    WaitFence(Arc<FenceInner>, u64),
}

/// Replays one command into the locked context.
fn replay(lock: &GpuLock, cmd: &Cmd) -> Result<(), String> {
    let sg = lock.sg();
    let fail = |what: &str| format!("{what}: {}", ffi::last_error());
    // SAFETY (every call below): the device lock is held; every handle is kept alive by `cmd`.
    unsafe {
        match cmd {
            Cmd::Dispatch { binds, regions, shared } => {
                ffi::sg_ctx_reset_bindings(sg);
                apply(sg, binds);
                for r in regions {
                    ffi::sg_ctx_set_push(sg, ffi::SG_COMPUTE, r.push.as_ptr() as *const _, r.push.len() as u32);
                    if !ffi::sg_ctx_dispatch(sg, r.shader.sg, r.groups.as_ptr(), *shared) {
                        return Err(fail("dispatch"));
                    }
                }
            }
            Cmd::BeginPass(p) => {
                if !ffi::sg_ctx_begin_pass(sg, &p.pass) {
                    return Err(fail("render pass"));
                }
            }
            Cmd::Draw(d) => {
                ffi::sg_ctx_reset_bindings(sg);
                apply(sg, &d.binds);
                for stage in [ffi::SG_VERTEX, ffi::SG_FRAGMENT] {
                    ffi::sg_ctx_set_push(sg, stage, d.push.as_ptr() as *const _, d.push.len() as u32);
                }
                let vbs: Vec<*mut ffi::sg_buffer> =
                    d.vertex_buffers.iter().map(|b| b.as_ref().map_or(std::ptr::null_mut(), |b| b.sg.sg)).collect();
                let (index_buffer, index_size, index_offset) =
                    d.index.as_ref().map_or((std::ptr::null_mut(), 0, 0), |(b, s, o)| (b.sg.sg, *s, *o));
                let draw = ffi::sg_draw {
                    vs: d.vs.sg,
                    fs: d.fs.sg,
                    blend: d.blend.as_ptr(),
                    blend_count: d.blend.len() as u32,
                    alpha_to_coverage: d.alpha_to_coverage as u32,
                    alpha_to_one: d.alpha_to_one as u32,
                    blend_color: d.blend_color,
                    raster: &d.raster,
                    depth_stencil: d.depth_stencil.as_ref().map_or(std::ptr::null(), |p| p as *const _),
                    stencil_ref: d.stencil_ref,
                    viewport: d.viewport,
                    scissor: d.scissor,
                    elements: d.elements.as_ptr(),
                    element_count: d.elements.len() as u32,
                    vertex_buffers: vbs.as_ptr(),
                    vertex_offsets: d.vertex_offsets.as_ptr(),
                    vertex_buffer_count: vbs.len() as u32,
                    primitive: d.primitive,
                    primitive_restart: d.primitive_restart as u32,
                    first_vertex: d.first_vertex,
                    vertex_count: d.count,
                    first_instance: d.first_instance,
                    instance_count: d.instances,
                    index_size,
                    index_buffer,
                    index_offset,
                    base_vertex: d.base_vertex,
                    texture_barrier: d.texture_barrier as u32,
                };
                if !ffi::sg_ctx_draw(sg, &draw) {
                    return Err(fail("draw"));
                }
            }
            Cmd::EndPass => ffi::sg_ctx_end_pass(sg),
            Cmd::Resolve(r) => {
                let resolve = ffi::sg_resolve {
                    src: r.src.sg,
                    dst: r.dst.sg,
                    src_format: r.src_format,
                    src_level: r.src_level,
                    src_layer: r.src_layer,
                    dst_format: r.dst_format,
                    dst_level: r.dst_level,
                    dst_layer: r.dst_layer,
                    width: r.width,
                    height: r.height,
                    aspects: r.aspects,
                };
                if !ffi::sg_ctx_resolve(sg, &resolve) {
                    return Err(fail("multisample resolve"));
                }
            }
            Cmd::CopyBuffer { src, src_offset, dst, dst_offset, size } => {
                if !ffi::sg_ctx_copy_buffer(sg, dst.sg.sg, *dst_offset, src.sg.sg, *src_offset, *size) {
                    return Err(fail("buffer copy"));
                }
            }
            Cmd::FillBuffer { buffer, offset, size, value } => {
                if !ffi::sg_ctx_fill_buffer(sg, buffer.sg.sg, *offset, *size, *value) {
                    return Err(fail("buffer fill"));
                }
            }
            Cmd::CopyTexture { src, src_level, src_layer, src_box, dst, dst_level, dst_layer, dst_origin } => {
                if !ffi::sg_ctx_copy_texture(sg, dst.sg, *dst_level, *dst_layer, dst_origin.as_ptr(), src.sg, *src_level,
                                             *src_layer, src_box) {
                    return Err(fail("texture copy"));
                }
            }
            Cmd::TextureToBuffer { src, level, layer, b, dst, offset, stride, layer_stride } => {
                if !ffi::sg_ctx_texture_to_buffer(sg, src.sg, *level, *layer, b, dst.sg.sg, *offset, *stride, *layer_stride) {
                    return Err(fail("texture to buffer copy"));
                }
            }
            Cmd::BufferToTexture { src, offset, stride, layer_stride, dst, level, layer, b } => {
                if !ffi::sg_ctx_buffer_to_texture(sg, src.sg.sg, *offset, *stride, *layer_stride, dst.sg, *level, *layer, b) {
                    return Err(fail("buffer to texture copy"));
                }
            }
            Cmd::GenerateMipmaps(t) => {
                if !ffi::sg_ctx_generate_mipmaps(sg, t.sg) {
                    return Err(fail("mipmaps"));
                }
            }
            Cmd::SignalEvent(..) | Cmd::WaitEvent(..) | Cmd::UpdateFence(..) | Cmd::WaitFence(..) => {}
        }
    }
    Ok(())
}

// ---- completion ----

pub(crate) struct Fenced(*mut ffi::sg_fence);
// SAFETY: a fence handle is waited on and freed by one thread at a time.
unsafe impl Send for Fenced {}

enum Action {
    Signal(Arc<EventInner>, u64),
    Complete(Arc<CbInner>),
}

struct CompletionJob {
    fence: Fenced,
    actions: Vec<Action>,
}

/// The device's completion thread: waits on fences in submission order.
pub(crate) struct Completion {
    tx: Mutex<Sender<CompletionJob>>,
}

/// How long a GPU submission may take before the command buffer fails (a hang on the phone).
const GPU_TIMEOUT: Duration = Duration::from_secs(10);

impl Completion {
    pub(crate) fn start(gpu: Arc<Gpu>) -> Completion {
        let (tx, rx) = channel::<CompletionJob>();
        std::thread::Builder::new()
            .name("sweetmtl-completion".into())
            .spawn(move || completion_thread(gpu, rx))
            .expect("completion thread");
        Completion { tx: Mutex::new(tx) }
    }

    fn push(&self, job: CompletionJob) {
        let _ = self.tx.lock().unwrap_or_else(|p| p.into_inner()).send(job);
    }
}

fn completion_thread(gpu: Arc<Gpu>, rx: Receiver<CompletionJob>) {
    for job in rx {
        let began = Instant::now();
        let mut timed_out = false;
        // SAFETY: a fence from sg_ctx_flush, waited on and freed here only.
        unsafe {
            loop {
                match ffi::sg_fence_wait(gpu.sg, job.fence.0, 100_000_000) {
                    1 => break,
                    0 if began.elapsed() < GPU_TIMEOUT => continue,
                    _ => {
                        timed_out = true;
                        break;
                    }
                }
            }
            ffi::sg_fence_free(gpu.sg, job.fence.0);
        }
        for action in job.actions {
            match action {
                Action::Signal(event, value) => event.set(value),
                Action::Complete(cb) => cb.complete(timed_out),
            }
        }
    }
}

// ---- command buffers ----

/// `MTLCommandBufferStatus`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CommandBufferStatus {
    NotEnqueued,
    Enqueued,
    Committed,
    Scheduled,
    Completed,
    Error,
}

impl CommandBufferStatus {
    pub fn raw(self) -> u64 {
        self as u64
    }
}

/// A completed or scheduled handler.
pub type Handler = Box<dyn FnOnce(&CommandBuffer) + Send>;

pub(crate) struct CbState {
    pub(crate) status: CommandBufferStatus,
    pub(crate) cmds: Vec<Cmd>,
    pub(crate) encoder_open: bool,
    completed: Vec<Handler>,
    scheduled: Vec<Handler>,
    pub(crate) error: Option<String>,
    pub(crate) used: HashSet<u64>,
    /// No-copy buffers the commands use (shadowed: copied in before, out after).
    pub(crate) no_copy: Vec<Buffer>,
    times: (f64, f64),
    /// The upload arena of set*Bytes: chunks and the next free offset of the last.
    pub(crate) arena: Vec<Buffer>,
    pub(crate) arena_used: u64,
}

pub(crate) struct CbInner {
    pub(crate) device: Device,
    queue: Arc<QueueInner>,
    pub(crate) state: Mutex<CbState>,
    cond: Condvar,
}

/// `MTLCommandBuffer`.
#[derive(Clone)]
pub struct CommandBuffer(pub(crate) Arc<CbInner>);

/// What a command buffer uses, until it completes.
pub struct UsedResources(HashSet<u64>);

impl UsedResources {
    pub fn references_resource(&self, resource: ResourceRef) -> bool {
        self.0.contains(&resource.0)
    }
}

const ARENA_CHUNK: u64 = 64 * 1024;

impl CbInner {
    fn complete(self: &Arc<CbInner>, timed_out: bool) {
        let (handlers, no_copy, cmds) = {
            let mut s = self.state.lock().unwrap_or_else(|p| p.into_inner());
            if timed_out && s.error.is_none() {
                s.error = Some(format!("the GPU did not finish the command buffer in {} s", GPU_TIMEOUT.as_secs()));
            }
            s.status = if s.error.is_some() { CommandBufferStatus::Error } else { CommandBufferStatus::Completed };
            s.times.1 = host_seconds();
            if crate::device::trace_enabled() {
                let st = self.device.gallium_stats();
                crate::device::trace(format_args!(
                    "command buffer {:?}{} ({} commands, {:.3} ms); device: {} flushes, {} dispatches, {} draws, \
                     {} clears, {} GPU blits, {} CPU copies, {} shaders",
                    s.status,
                    s.error.as_ref().map_or(String::new(), |e| format!(": {e}")),
                    s.cmds.len(),
                    (s.times.1 - s.times.0) * 1000.0,
                    st.flushes,
                    st.dispatches,
                    st.draws,
                    st.clears,
                    st.blits,
                    st.cpu_copies,
                    st.shaders
                ));
            }
            s.used.clear();
            (std::mem::take(&mut s.completed), std::mem::take(&mut s.no_copy), std::mem::take(&mut s.cmds))
        };
        // The GPU is done: the shadows of no-copy buffers go back to their pages.
        for b in &no_copy {
            if let Some(host) = b.0.host {
                // SAFETY: the shadow's mapping and the caller's pages hold `length` bytes each.
                unsafe {
                    std::ptr::copy_nonoverlapping(ffi::sg_buffer_map(b.0.sg.sg) as *const u8, host.as_ptr(), b.0.length as usize)
                };
            }
        }
        drop(cmds);
        self.cond.notify_all();
        let cb = CommandBuffer(self.clone());
        for h in handlers {
            h(&cb);
        }
    }

    /// A slice of the upload arena holding `bytes` (256-byte aligned, the strictest offset
    /// alignment the binder asks of a buffer).
    pub(crate) fn upload(&self, bytes: &[u8]) -> Result<(Buffer, u64), Error> {
        let mut s = self.state.lock().unwrap_or_else(|p| p.into_inner());
        let len = bytes.len() as u64;
        let at = s.arena_used.div_ceil(256) * 256;
        let fits = s.arena.last().is_some_and(|b| at + len <= b.length());
        let (buffer, offset) = if fits {
            (s.arena.last().unwrap().clone(), at)
        } else {
            let b = self.device.new_buffer(ARENA_CHUNK.max(len), crate::resource::ResourceOptions::shared())?;
            s.arena.push(b.clone());
            (b, 0)
        };
        s.arena_used = offset + len;
        // SAFETY: the arena chunk's mapping holds offset + len bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), (ffi::sg_buffer_map(buffer.0.sg.sg) as *mut u8).add(offset as usize),
                                          bytes.len())
        };
        Ok((buffer, offset))
    }
}

impl CommandBuffer {
    pub fn device(&self) -> Device {
        self.0.device.clone()
    }

    pub fn status(&self) -> CommandBufferStatus {
        self.0.state.lock().unwrap_or_else(|p| p.into_inner()).status
    }

    pub fn error(&self) -> Option<String> {
        self.0.state.lock().unwrap_or_else(|p| p.into_inner()).error.clone()
    }

    pub fn error_description(&self) -> Option<String> {
        self.error()
    }

    pub fn used_resources(&self) -> UsedResources {
        UsedResources(self.0.state.lock().unwrap_or_else(|p| p.into_inner()).used.clone())
    }

    fn not_committed(s: &CbState) -> bool {
        matches!(s.status, CommandBufferStatus::NotEnqueued | CommandBufferStatus::Enqueued)
    }

    pub fn add_completed_handler(&self, handler: Handler) -> Result<(), Error> {
        let mut s = self.0.state.lock().unwrap_or_else(|p| p.into_inner());
        if !Self::not_committed(&s) {
            return Err(invalid("addCompletedHandler: after commit"));
        }
        s.completed.push(handler);
        Ok(())
    }

    pub fn add_scheduled_handler(&self, handler: Handler) -> Result<(), Error> {
        let mut s = self.0.state.lock().unwrap_or_else(|p| p.into_inner());
        if !Self::not_committed(&s) {
            return Err(invalid("addScheduledHandler: after commit"));
        }
        s.scheduled.push(handler);
        Ok(())
    }

    pub fn encode_signal_event(&self, event: EventRef, value: u64) -> Result<(), Error> {
        self.push_sync(Cmd::SignalEvent(event.inner(), value))
    }

    pub fn encode_wait_for_event(&self, event: EventRef, value: u64) -> Result<(), Error> {
        self.push_sync(Cmd::WaitEvent(event.inner(), value))
    }

    fn push_sync(&self, cmd: Cmd) -> Result<(), Error> {
        let mut s = self.0.state.lock().unwrap_or_else(|p| p.into_inner());
        if s.encoder_open {
            return Err(invalid("an event encoded while an encoder is open"));
        }
        if !Self::not_committed(&s) {
            return Err(invalid("an event encoded after commit"));
        }
        s.cmds.push(cmd);
        Ok(())
    }

    /// `commit`: hands the command buffer to its queue's thread.
    pub fn commit(&self) -> Result<(), Error> {
        {
            let mut s = self.0.state.lock().unwrap_or_else(|p| p.into_inner());
            if !Self::not_committed(&s) {
                return Err(invalid("commit of a committed command buffer"));
            }
            if s.encoder_open {
                return Err(invalid("commit with an encoder open"));
            }
            // fence generations, in commit order
            for cmd in s.cmds.iter_mut() {
                match cmd {
                    Cmd::UpdateFence(f, generation) => *generation = f.commit_update(),
                    Cmd::WaitFence(f, generation) => *generation = f.committed(),
                    _ => {}
                }
            }
            s.status = CommandBufferStatus::Committed;
        }
        let tx = self.0.queue.tx.lock().unwrap_or_else(|p| p.into_inner());
        tx.send(self.0.clone()).map_err(|_| Error::Gallium("the queue's thread has ended".into()))
    }

    fn wait_for(&self, done: impl Fn(CommandBufferStatus) -> bool, timeout: Option<Duration>) -> Result<(), Error> {
        let deadline = timeout.map(|t| Instant::now() + t);
        let mut s = self.0.state.lock().unwrap_or_else(|p| p.into_inner());
        if Self::not_committed(&s) {
            return Err(invalid("a wait on a command buffer that was not committed"));
        }
        while !done(s.status) {
            match deadline {
                None => s = self.0.cond.wait(s).unwrap_or_else(|p| p.into_inner()),
                Some(d) => {
                    let now = Instant::now();
                    if now >= d {
                        return Err(Error::Timeout);
                    }
                    s = self.0.cond.wait_timeout(s, d - now).unwrap_or_else(|p| p.into_inner()).0;
                }
            }
        }
        Ok(())
    }

    pub fn wait_until_completed(&self) -> Result<(), Error> {
        self.wait_for(|s| matches!(s, CommandBufferStatus::Completed | CommandBufferStatus::Error), None)
    }

    pub fn wait_until_completed_for(&self, timeout: Duration) -> Result<(), Error> {
        self.wait_for(|s| matches!(s, CommandBufferStatus::Completed | CommandBufferStatus::Error), Some(timeout))
    }

    pub fn wait_until_scheduled(&self) -> Result<(), Error> {
        self.wait_for(
            |s| matches!(s, CommandBufferStatus::Scheduled | CommandBufferStatus::Completed | CommandBufferStatus::Error),
            None,
        )
    }

    /// `GPUStartTime`: when the queue began replaying it (host seconds).
    pub fn gpu_start_time(&self) -> Result<f64, Error> {
        Ok(self.0.state.lock().unwrap_or_else(|p| p.into_inner()).times.0)
    }

    /// `GPUEndTime`: when its last submission's fence signaled (host seconds).
    pub fn gpu_end_time(&self) -> Result<f64, Error> {
        Ok(self.0.state.lock().unwrap_or_else(|p| p.into_inner()).times.1)
    }

    /// Opens an encoder (one at a time, Metal's rule).
    pub(crate) fn open_encoder(&self) -> Result<(), Error> {
        let mut s = self.0.state.lock().unwrap_or_else(|p| p.into_inner());
        if s.encoder_open {
            return Err(invalid("an encoder while another is open"));
        }
        if !Self::not_committed(&s) {
            return Err(invalid("an encoder on a committed command buffer"));
        }
        s.encoder_open = true;
        Ok(())
    }

    /// Ends the open encoder with its commands and the resources they use.
    pub(crate) fn close_encoder(&self, cmds: Vec<Cmd>, used: &[u64], no_copy: &[Buffer]) {
        let mut s = self.0.state.lock().unwrap_or_else(|p| p.into_inner());
        s.encoder_open = false;
        s.cmds.extend(cmds);
        s.used.extend(used.iter().copied().filter(|&id| id != 0));
        for b in no_copy {
            if !s.no_copy.contains(b) {
                s.no_copy.push(b.clone());
            }
        }
    }

    /// Drops an encoder that was not ended (its commands are discarded).
    pub(crate) fn discard_encoder(&self) {
        self.0.state.lock().unwrap_or_else(|p| p.into_inner()).encoder_open = false;
    }

    pub(crate) fn upload(&self, bytes: &[u8]) -> Result<(Buffer, u64), Error> {
        self.0.upload(bytes)
    }
}

// ---- queues ----

pub(crate) struct QueueInner {
    device: Device,
    tx: Mutex<Sender<Arc<CbInner>>>,
}

/// `MTLCommandQueue`: command buffers run in commit order on the queue's thread.
#[derive(Clone)]
pub struct CommandQueue(Arc<QueueInner>);

impl Device {
    pub fn new_command_queue(&self) -> Result<CommandQueue, Error> {
        let (tx, rx) = channel::<Arc<CbInner>>();
        let device = self.clone();
        std::thread::Builder::new()
            .name("sweetmtl-queue".into())
            .spawn(move || {
                for cb in rx {
                    run(&device, &cb);
                }
            })
            .map_err(|e| Error::Gallium(format!("queue thread: {e}")))?;
        Ok(CommandQueue(Arc::new(QueueInner { device: self.clone(), tx: Mutex::new(tx) })))
    }
}

impl CommandQueue {
    pub fn command_buffer(&self) -> Result<CommandBuffer, Error> {
        Ok(CommandBuffer(Arc::new(CbInner {
            device: self.0.device.clone(),
            queue: self.0.clone(),
            state: Mutex::new(CbState {
                status: CommandBufferStatus::NotEnqueued,
                cmds: Vec::new(),
                encoder_open: false,
                completed: Vec::new(),
                scheduled: Vec::new(),
                error: None,
                used: HashSet::new(),
                no_copy: Vec::new(),
                times: (0.0, 0.0),
                arena: Vec::new(),
                arena_used: 0,
            }),
            cond: Condvar::new(),
        })))
    }

    pub fn device(&self) -> Device {
        self.0.device.clone()
    }
}

/// Replays and submits one committed command buffer (the queue's thread).
fn run(device: &Device, cb: &Arc<CbInner>) {
    let (cmds, no_copy) = {
        let mut s = cb.state.lock().unwrap_or_else(|p| p.into_inner());
        s.times.0 = host_seconds();
        (std::mem::take(&mut s.cmds), s.no_copy.clone())
    };
    let gpu = device.gpu().clone();
    let completion = &device.0.completion;
    let mut error: Option<String> = None;
    let mut lock = Some(gpu.lock());
    // the pages of no-copy buffers into their shadows, once earlier GPU work is done
    for b in &no_copy {
        if let Some(host) = b.0.host {
            // SAFETY: lock held; the pages and the shadow hold `length` bytes.
            if !unsafe { ffi::sg_ctx_sync_host(lock.as_ref().unwrap().sg(), b.0.sg.sg, host.as_ptr() as *mut _, b.0.length, false) } {
                error.get_or_insert_with(|| format!("no-copy buffer: {}", ffi::last_error()));
            }
        }
    }
    // Nothing stays bound while the lock is released: a binding's sampler or view may be freed once
    // its command buffer completes, and a later clear (on a5xx u_blitter's draw) emits every bound
    // sampler's border color (fd5 setup_border_colors read a freed sampler: mtlabitest on drm-shim,
    // a sampler state released after its last draw, then a pass that clears).
    let unbind = |lock: &GpuLock| {
        // SAFETY: lock held.
        unsafe { ffi::sg_ctx_reset_bindings(lock.sg()) }
    };
    let flush = |lock: &GpuLock, actions: Vec<Action>| {
        // SAFETY: lock held.
        let fence = unsafe { ffi::sg_ctx_flush(lock.sg()) };
        if fence.is_null() {
            // no fence: complete the actions now (nothing to wait for)
            for a in actions {
                match a {
                    Action::Signal(e, v) => e.set(v),
                    Action::Complete(cb) => cb.complete(false),
                }
            }
        } else {
            completion.push(CompletionJob { fence: Fenced(fence), actions });
        }
    };
    for cmd in &cmds {
        match cmd {
            Cmd::SignalEvent(event, value) => flush(lock.as_ref().unwrap(), vec![Action::Signal(event.clone(), *value)]),
            Cmd::WaitEvent(event, value) => {
                if event.value() < *value {
                    unbind(lock.as_ref().unwrap());
                    flush(lock.as_ref().unwrap(), Vec::new());
                    drop(lock.take());
                    event.wait(*value, None);
                    lock = Some(gpu.lock());
                }
            }
            Cmd::UpdateFence(fence, generation) => {
                flush(lock.as_ref().unwrap(), Vec::new());
                fence.flushed(*generation);
            }
            Cmd::WaitFence(fence, generation) => {
                if *fence.flushed.lock().unwrap_or_else(|p| p.into_inner()) < *generation {
                    unbind(lock.as_ref().unwrap());
                    drop(lock.take());
                    fence.wait(*generation);
                    lock = Some(gpu.lock());
                }
            }
            _ if error.is_some() => {}
            _ => {
                if let Err(e) = replay(lock.as_ref().unwrap(), cmd) {
                    error = Some(e);
                }
            }
        }
    }
    unbind(lock.as_ref().unwrap());
    // SAFETY: lock held.
    let fence = unsafe { ffi::sg_ctx_flush(lock.as_ref().unwrap().sg()) };
    drop(lock);
    let scheduled = {
        let mut s = cb.state.lock().unwrap_or_else(|p| p.into_inner());
        s.cmds = cmds;
        if error.is_none() && fence.is_null() {
            error = Some(format!("flush: {}", ffi::last_error()));
        }
        s.error = error;
        s.status = CommandBufferStatus::Scheduled;
        std::mem::take(&mut s.scheduled)
    };
    cb.cond.notify_all();
    // Metal runs the scheduled handlers before the completed ones.
    let handle = CommandBuffer(cb.clone());
    for h in scheduled {
        h(&handle);
    }
    if fence.is_null() {
        cb.complete(false);
    } else {
        completion.push(CompletionJob { fence: Fenced(fence), actions: vec![Action::Complete(cb.clone())] });
    }
}
