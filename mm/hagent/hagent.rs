use kernel::{
    new_mutex,
    prelude::*,
    sync::{Arc, Mutex},
};
use lazy_static::lazy_static;

struct Inner {
    tracking: Option<Pid>,
    info: HashMap<Pid, Arc<Migrator>, BuildHasherDefault<FnvHasher>>,
}

impl Inner {
    fn new() -> Self {
        pr_info!("hagent initializing");
        Self {
            tracking: None,
            info: HashMap::with_hasher(BuildHasherDefault::<FnvHasher>::default()),
        }
    }

    // only track the process with a visze larger than 1/3 of RAM size
    fn track(&mut self, process: Pid) {
        let vsize = task_vsize(process);
        if vsize > self.tracking.map(task_vsize).unwrap_or(0) && vsize > ram_size() / 3 {
            self.switch(Some(process))
        }
    }

    fn untrack(&mut self, process: Pid) {
        self.tracking
            .map(|tracking| (tracking == process).then(|| self.switch(None)));
    }

    fn switch(&mut self, to: Option<Pid>) {
        // TODO: stop PEBS instead of destroy
        self.tracking.take().map(|ref p| self.info.remove(p));
        to.map(|p| {
            self.tracking.replace(p);
            self.info
                .insert(p, Arc::pin_init(Migrator::new(p)).unwrap());
        });
    }
}

lazy_static! {
    pub static ref HAGENT: Hagent = Hagent::new();
}

pub struct Hagent {
    inner: Arc<Mutex<Inner>>,
}

impl Hagent {
    fn new() -> Self {
        Self {
            inner: Arc::pin_init(new_mutex!(Inner::new(), "Hagent::inner")).unwrap(),
        }
    }

    pub fn track(&self, p: Pid) {
        let mut inner = self.inner.lock();
        inner.track(p)
    }

    pub fn untrack(&self, p: Pid) {
        let mut inner = self.inner.lock();
        inner.untrack(p)
    }
}

pub mod ffi {
    use super::HAGENT;
    use crate::helper::*;
    use kernel::prelude::*;

    #[no_mangle]
    pub extern "C" fn hagent_callback_mmap(_regs: Option<&PtRegs>) {
        HAGENT.track(current!().group_leader().pid())
    }

    #[no_mangle]
    pub extern "C" fn hagent_callback_exit_group(_regs: Option<&PtRegs>) {
        HAGENT.untrack(current!().group_leader().pid())
    }
}
