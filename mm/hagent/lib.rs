#![allow(missing_docs)]

pub mod alloc {
    pub use {
        alloc::alloc::Global,
        core::alloc::{AllocError, Allocator},
    };
}

pub mod iheap;
pub mod sdh;

// This should be defined by module!()
const __LOG_PREFIX: &[u8] = b"hagent\0";
