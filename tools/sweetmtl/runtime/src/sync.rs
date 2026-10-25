// SPDX-License-Identifier: MIT
//! Events and fences (`MTLEvent`, `MTLSharedEvent`, `MTLSharedEventListener`, `MTLFence`), in one
//! process.
//!
//! An event's value moves when the GPU work encoded before its signal completes (the command
//! buffer is flushed at the signal and the value set once that submission's fence signals). A
//! wait holds the queue's later work until the value is reached. A fence orders encoders across
//! queues: an update counts at the commit of its command buffer and is flushed with it; a wait
//! holds its command buffer's replay until the updates committed before it were flushed (the GPU
//! then runs them first: one ring, in submission order).

use crate::device::Device;
use crate::error::Error;
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

type Notification = Box<dyn FnOnce(&SharedEvent, u64) + Send>;

pub(crate) struct EventInner {
    value: Mutex<u64>,
    cond: Condvar,
    waiting: Mutex<Vec<(u64, Notification, Sender<Job>)>>,
}

impl EventInner {
    fn new() -> Arc<EventInner> {
        Arc::new(EventInner { value: Mutex::new(0), cond: Condvar::new(), waiting: Mutex::new(Vec::new()) })
    }

    pub(crate) fn value(&self) -> u64 {
        *self.value.lock().unwrap_or_else(|p| p.into_inner())
    }

    /// Sets the value and runs the notifications it reaches (on their listeners).
    pub(crate) fn set(self: &Arc<EventInner>, value: u64) {
        *self.value.lock().unwrap_or_else(|p| p.into_inner()) = value;
        self.cond.notify_all();
        let mut ready = Vec::new();
        {
            let mut w = self.waiting.lock().unwrap_or_else(|p| p.into_inner());
            let mut i = 0;
            while i < w.len() {
                if w[i].0 <= value {
                    ready.push(w.swap_remove(i));
                } else {
                    i += 1;
                }
            }
        }
        for (_, notify, listener) in ready {
            let _ = listener.send(Job { event: self.clone(), value, notify });
        }
    }

    /// Waits until the value reaches `value`; false on timeout.
    pub(crate) fn wait(&self, value: u64, timeout: Option<Duration>) -> bool {
        let deadline = timeout.map(|t| Instant::now() + t);
        let mut v = self.value.lock().unwrap_or_else(|p| p.into_inner());
        while *v < value {
            match deadline {
                None => v = self.cond.wait(v).unwrap_or_else(|p| p.into_inner()),
                Some(d) => {
                    let now = Instant::now();
                    if now >= d {
                        return false;
                    }
                    v = self.cond.wait_timeout(v, d - now).unwrap_or_else(|p| p.into_inner()).0;
                }
            }
        }
        true
    }
}

/// `MTLEvent`: GPU-to-GPU ordering in one device.
#[derive(Clone)]
pub struct Event(pub(crate) Arc<EventInner>);

/// `MTLSharedEvent`: an event the CPU can read, set and wait on.
#[derive(Clone)]
pub struct SharedEvent(pub(crate) Arc<EventInner>);

/// Either kind, as the command buffer's signal and wait take them.
pub enum EventRef<'a> {
    Event(&'a Event),
    Shared(&'a SharedEvent),
}

impl EventRef<'_> {
    pub(crate) fn inner(&self) -> Arc<EventInner> {
        match self {
            EventRef::Event(e) => e.0.clone(),
            EventRef::Shared(s) => s.0.clone(),
        }
    }
}

impl Device {
    pub fn new_event(&self) -> Result<Event, Error> {
        Ok(Event(EventInner::new()))
    }
    pub fn new_shared_event(&self) -> Result<SharedEvent, Error> {
        Ok(SharedEvent(EventInner::new()))
    }
    pub fn new_fence(&self) -> Fence {
        Fence(Arc::new(FenceInner { committed: Mutex::new(0), flushed: Mutex::new(0), cond: Condvar::new() }))
    }
}

impl SharedEvent {
    pub fn signaled_value(&self) -> u64 {
        self.0.value()
    }
    pub fn set_signaled_value(&self, value: u64) {
        self.0.set(value)
    }
    /// `waitUntilSignaledValue:timeoutMS:`: true once reached, false on timeout.
    pub fn wait_until_signaled_value(&self, value: u64, timeout: Duration) -> bool {
        self.0.wait(value, Some(timeout))
    }
    /// `notifyListener:atValue:block:`: `block` runs on the listener's thread once the value
    /// reaches `value` (at once where it has).
    pub fn notify_listener(&self, listener: &SharedEventListener, value: u64, block: Notification) {
        let reached = {
            let v = self.0.value.lock().unwrap_or_else(|p| p.into_inner());
            if *v >= value {
                Some(*v)
            } else {
                self.0.waiting.lock().unwrap_or_else(|p| p.into_inner()).push((value, block, listener.tx.clone()));
                return;
            }
        };
        if let Some(v) = reached {
            let _ = listener.tx.send(Job { event: self.0.clone(), value: v, notify: block });
        }
    }
}

struct Job {
    event: Arc<EventInner>,
    value: u64,
    notify: Notification,
}

/// `MTLSharedEventListener`: one thread that runs notifications in order.
pub struct SharedEventListener {
    tx: Sender<Job>,
}

impl SharedEventListener {
    pub fn new() -> Result<SharedEventListener, Error> {
        let (tx, rx) = channel::<Job>();
        std::thread::Builder::new()
            .name("sweetmtl-listener".into())
            .spawn(move || {
                for job in rx {
                    (job.notify)(&SharedEvent(job.event), job.value);
                }
            })
            .map_err(|e| Error::Gallium(format!("listener thread: {e}")))?;
        Ok(SharedEventListener { tx })
    }
}

pub(crate) struct FenceInner {
    /// Updates committed so far (the generation of the latest).
    pub(crate) committed: Mutex<u64>,
    /// The latest generation whose command buffer was flushed.
    pub(crate) flushed: Mutex<u64>,
    pub(crate) cond: Condvar,
}

impl FenceInner {
    pub(crate) fn commit_update(&self) -> u64 {
        let mut c = self.committed.lock().unwrap_or_else(|p| p.into_inner());
        *c += 1;
        *c
    }
    pub(crate) fn committed(&self) -> u64 {
        *self.committed.lock().unwrap_or_else(|p| p.into_inner())
    }
    pub(crate) fn flushed(&self, generation: u64) {
        let mut f = self.flushed.lock().unwrap_or_else(|p| p.into_inner());
        if generation > *f {
            *f = generation;
        }
        self.cond.notify_all();
    }
    pub(crate) fn wait(&self, generation: u64) {
        let mut f = self.flushed.lock().unwrap_or_else(|p| p.into_inner());
        while *f < generation {
            f = self.cond.wait(f).unwrap_or_else(|p| p.into_inner());
        }
    }
}

/// `MTLFence`.
#[derive(Clone)]
pub struct Fence(pub(crate) Arc<FenceInner>);
