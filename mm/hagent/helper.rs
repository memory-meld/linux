use kernel::bindings;

pub type Pid = bindings::pid_t;
pub type Work = bindings::work_struct;
pub type IrqWork = bindings::irq_work;
pub type WorkQueue = bindings::work_struct;
pub(crate) use inner::{system_highpri_wq, system_long_wq, system_wq};
pub(crate) use param::*;

pub fn install_hook() {
    unsafe { inner::helper_install_hook() }
}
pub fn remove_hook() {
    unsafe { inner::helper_remove_hook() }
}

pub fn task_vsize(pid: Pid) -> u64 {
    unsafe { inner::helper_task_vsize(pid) }
}
pub fn ram_size() -> u64 {
    unsafe { inner::helper_ram_size() }
}
pub fn irq_work_queue_on(work: *mut IrqWork, cpu: i32) -> bool {
    unsafe { inner::irq_work_queue_on(work, cpu) }
}
pub fn irq_work_sync(work: *mut IrqWork) {
    unsafe { inner::irq_work_sync(work) }
}
pub fn init_irq_work(work: *mut IrqWork, f: fn(&IrqWork)) {
    unsafe { inner::helper_init_irq_work(work, f) }
}
pub fn init_work(work: *mut Work, f: fn(&Work)) {
    unsafe { inner::helper_init_work(work, f) }
}
pub fn queue_work_on(cpu: i32, wq: *mut WorkQueue, work: *mut Work) -> bool {
    unsafe { inner::queue_work_on(cpu, wq, work) }
}
pub fn flush_work(work: *mut Work) -> bool {
    unsafe { inner::flush_work(work) }
}
pub fn num_online_cpus() -> u32 {
    unsafe { inner::helper_num_online_cpus() }
}

mod inner {
    use super::*;

    extern "C" {
        pub(crate) fn helper_install_hook();
        pub(crate) fn helper_remove_hook();

        pub(crate) fn helper_task_vsize(pid: Pid) -> u64;
        pub(crate) fn helper_ram_size() -> u64;

        pub(crate) fn helper_init_irq_work(work: *mut IrqWork, f: fn(&IrqWork));
        pub(crate) fn irq_work_queue_on(work: *mut IrqWork, cpu: i32) -> bool;
        pub(crate) fn irq_work_sync(work: *mut IrqWork);

        pub(crate) static system_wq: *mut WorkQueue;
        pub(crate) static system_long_wq: *mut WorkQueue;
        pub(crate) static system_highpri_wq: *mut WorkQueue;
        pub(crate) fn helper_init_work(work: *mut Work, f: fn(&Work));
        pub(crate) fn queue_work_on(cpu: i32, wq: *mut WorkQueue, work: *mut Work) -> bool;
        pub(crate) fn flush_work(work: *mut Work) -> bool;

        pub(crate) fn helper_num_online_cpus() -> u32;

    }
}

mod param {
    extern "C" {
        pub(crate) static hagent_sdh_w: usize;
        pub(crate) static hagent_sdh_d: usize;
        pub(crate) static hagent_sdh_k: usize;
        pub(crate) static hagent_event_config: u64;
        pub(crate) static hagent_event_threshold: u64;
        pub(crate) static hagent_event_period: u64;
        pub(crate) static hagent_channel_capacity: usize;
        pub(crate) static hagent_dump_topk: bool;
    }

    pub(crate) const HPAGE_MASK: u64 = !((1 << 21) - 1);
    pub(crate) const CPU_IDENTIFICATION: i32 = 0;
    pub(crate) const CPU_MIGRATION: i32 = 1;
}

pub use event::*;
mod event {
    use core::{ffi::c_void, ptr::NonNull};
    use kernel::{bindings, task::Task};

    pub type PtRegs = bindings::pt_regs;
    pub type PerfEvent = bindings::perf_event;

    pub const PERF_TYPE_RAW: u32 = 4;
    pub const PERF_SAMPLE_TID: u64 = 1 << 1;
    pub const PERF_SAMPLE_ADDR: u64 = 1 << 3;
    pub const PERF_SAMPLE_WEIGHT: u64 = 1 << 14;
    pub const PERF_SAMPLE_PHYS_ADDR: u64 = 1 << 19;

    pub const EVENT_ATTR_SAMPLE_IP_ZERO_SKID: u64 = 3 << 15;
    pub const EVENT_ATTR_EXCLUDE_USER: u64 = 1 << 4;
    pub const EVENT_ATTR_EXCLUDE_KERNEL: u64 = 1 << 5;
    pub const EVENT_ATTR_EXCLUDE_HV: u64 = 1 << 6;
    pub const EVENT_ATTR_EXCLUDE_IDLE: u64 = 1 << 7;
    pub const EVENT_ATTR_EXCLUDE_HOST: u64 = 1 << 19;
    pub const EVENT_ATTR_EXCLUDE_GUEST: u64 = 1 << 20;
    pub const EVENT_ATTR_EXCLUDE_CALLCHAIN_KERNEL: u64 = 1 << 21;
    pub const EVENT_ATTR_EXCLUDE_CALLCHAIN_USER: u64 = 1 << 22;

    #[repr(C)]
    #[derive(Default, Debug, Clone, Copy)]
    pub struct EventAttr {
        pub typ: u32,
        pub size: u32,
        pub config: u64,
        pub sample_period: u64,
        pub sample_type: u64,
        pub read_format: u64,
        pub flags: u64,
        pub wakeup: u32,
        pub bp_type: u32,
        pub config1: u64,
        pub config2: u64,
        pub branch_sample_type: u64,
        pub sample_regs_user: u64,
        pub sample_stack_user: u32,
        pub clockid: i32,
        pub sample_regs_intr: u64,
        pub aux_watermark: u32,
        pub sample_max_stack: u16,
        pub __reserved_2: u16,
        pub aux_sample_size: u32,
        pub __reserved_3: u32,
        pub sig_data: u64,
        pub config3: u64,
    }

    #[repr(C)]
    #[derive(Default, Debug, Clone, Copy)]
    pub struct SampleData {
        pub sample_flags: u64,
        pub period: u64,
        pub dyn_size: u64,
        pub typ: u64,
        pub pid: u32,
        pub tid: u32,
        pub time: u64,
        pub id: u64,
        pub cpu: u32,
        pub reserved: u32,
        pub ip: u64,
        pub callchain: Option<NonNull<c_void>>,
        pub raw: Option<NonNull<c_void>>,
        pub br_stack: Option<NonNull<c_void>>,
        pub weight: u64,
        pub data_src: u64,
        pub txn: u64,
        pub regs_user_abi: u64,
        pub regs_user_regs: Option<NonNull<c_void>>,
        pub regs_intr_abi: u64,
        pub regs_intr_regs: Option<NonNull<c_void>>,
        pub stack_user_size: u64,
        pub stream_id: u64,
        pub cgroup: u64,
        pub addr: u64,
        pub phys_addr: u64,
        pub data_page_size: u64,
        pub code_page_size: u64,
        pub aux_size: u64,
    }

    extern "C" {
        pub fn perf_event_create_kernel_counter(
            attr: &EventAttr,
            cpu: i32,
            task: Option<&Task>,
            handler: fn(&PerfEvent, &SampleData, &PtRegs),
            context: *mut c_void,
        ) -> &'static PerfEvent;

        pub fn perf_event_release_kernel(event: &PerfEvent) -> i32;
    }
}
