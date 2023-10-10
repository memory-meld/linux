#![allow(missing_docs)]

use crate::helper::{helper_install_hook, helper_remove_hook};
use kernel::prelude::*;

pub mod alloc {
    pub use {
        alloc::alloc::Global,
        core::alloc::{AllocError, Allocator},
    };
}

pub mod event;
pub mod hagent;
pub mod helper;
pub mod iheap;
pub mod migrator;
pub mod sdh;
pub mod spsc;

module! {
    type: HeteroModule,
    name: "hagent",
    author: "Junliang HU",
    description: "Heterogeneous memory management guest agent",
    license: "GPL",
}

// This should be defined by module!()
// const __LOG_PREFIX: &[u8] = b"hagent\0";

struct HeteroModule;

impl kernel::Module for HeteroModule {
    fn init(_module: &'static ThisModule) -> Result<Self> {
        pr_info!("Rust heterogeneous memory management guest agent (init)\n");
        pr_info!("Am I built-in? {}\n", !cfg!(MODULE));
        unsafe { helper_install_hook() }
        Ok(Self)
    }
}

impl Drop for HeteroModule {
    fn drop(&mut self) {
        unsafe { helper_remove_hook() }
        pr_info!("Rust heterogeneous memory management guest agent (exit)\n");
    }
}
