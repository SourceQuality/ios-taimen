// SPDX-License-Identifier: MIT
//! The runtime's errors. Every failure names its reason; `NotYet` marks what this runtime has not
//! built (it never substitutes a different behavior).

use std::fmt;

/// A runtime failure, typed by who stops the call.
#[derive(Debug)]
pub enum Error {
    /// Not implemented in sweetmtl-runtime yet (the call does nothing).
    NotYet(String),
    /// Metal's validation would stop this call.
    Invalid(String),
    /// The device (Adreno 540 through Gallium freedreno) cannot do this.
    Unsupported(String),
    /// The metal2vulkan translator refused or failed.
    Compile(metal2vulkan::error::Error),
    /// The translator's binder refused a draw or dispatch.
    Bind(metal2vulkan::error::BindError),
    /// The Gallium shim failed (its reason).
    Gallium(String),
    /// A wait ran out of time.
    Timeout,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotYet(what) => write!(f, "not yet implemented in sweetmtl: {what}"),
            Error::Invalid(what) => write!(f, "invalid: {what}"),
            Error::Unsupported(what) => write!(f, "unsupported on the Adreno 540: {what}"),
            Error::Compile(e) => write!(f, "translator: {e:?}"),
            Error::Bind(e) => write!(f, "binder: {e:?}"),
            Error::Gallium(why) => write!(f, "gallium: {why}"),
            Error::Timeout => write!(f, "timed out"),
        }
    }
}

impl std::error::Error for Error {}

impl From<metal2vulkan::error::Error> for Error {
    fn from(e: metal2vulkan::error::Error) -> Error {
        Error::Compile(e)
    }
}

impl From<metal2vulkan::error::BindError> for Error {
    fn from(e: metal2vulkan::error::BindError) -> Error {
        Error::Bind(e)
    }
}

pub(crate) fn not_yet(what: impl Into<String>) -> Error {
    Error::NotYet(what.into())
}

pub(crate) fn invalid(what: impl Into<String>) -> Error {
    Error::Invalid(what.into())
}

/// The shim's last error, as this runtime's.
pub(crate) fn gallium(what: &str) -> Error {
    Error::Gallium(format!("{what}: {}", crate::ffi::last_error()))
}

/// A device query the runtime cannot answer exactly (the bridge reports it as such).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RuntimeGap(pub &'static str);
