use core::{
    convert::Infallible,
    default::Default,
    iter::{IntoIterator, Iterator},
    mem::size_of,
    ptr,
    sync::atomic::{AtomicU64, Ordering},
};

use crate::{
    event::Sample,
    helper::*,
    sdh::SDH,
    spsc::{self, Receiver, Sender},
};
use kernel::{error::to_result, init::*, new_mutex, prelude::*, sync::*};

#[repr(C)]
#[pin_data(PinnedDrop)]
struct CollectionContext {
    // sample collection
    // Put it the first element, so that the address of the struct
    // can be known when creating perf counter
    #[pin]
    event: &'static PerfEvent,
    tx: Sender<Sample>,
    identification: Arc<IdentificationContext>,
}

impl CollectionContext {
    fn new(
        cpu: i32,
        tx: Sender<Sample>,
        identification: Arc<IdentificationContext>,
    ) -> impl PinInit<Self> {
        let attr = EventAttr {
            typ: PERF_TYPE_RAW,
            size: size_of::<EventAttr>() as _,
            config: unsafe { hagent_event_config },
            config1: unsafe { hagent_event_threshold },
            sample_type: PERF_SAMPLE_TID | PERF_SAMPLE_ADDR | PERF_SAMPLE_WEIGHT,
            sample_period: unsafe { hagent_event_period },
            flags: EVENT_ATTR_SAMPLE_IP_ZERO_SKID
                | EVENT_ATTR_EXCLUDE_KERNEL
                | EVENT_ATTR_EXCLUDE_CALLCHAIN_KERNEL,
            ..Default::default()
        };
        unsafe {
            pin_init!(Self {
                event <- pin_init_from_closure::<_, Infallible>(move |slot| {
                    let event = perf_event_create_kernel_counter(&attr, cpu, None, Self::ffi_handler, slot as _);
                    ptr::write(slot, event);
                    Ok(())
                }),
                tx,
                identification,
            })
        }
    }

    fn ffi_handler(event: &PerfEvent, data: &SampleData, _regs: &PtRegs) {
        static NTH: AtomicU64 = AtomicU64::new(0);
        let mut sample = Sample::from(data);
        sample.id = NTH.fetch_add(1, Ordering::Relaxed);
        // ignore if full
        let this = unsafe { &mut *(event.overflow_handler_context as *mut Self) };
        this.tx.send(sample).ok();
        if sample.id % 1024 == 0 {
            this.identification.queue_on(CPU_IDENTIFICATION);
        }
    }
}

#[pinned_drop]
impl PinnedDrop for CollectionContext {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("collection context exiting");
        let err = unsafe { perf_event_release_kernel(self.event) };
        to_result(err).unwrap();
        pr_info!("kernel event {:?} released", self.event as *const _);
        pr_info!("collection context exited");
    }
}

#[repr(C)]
#[pin_data(PinnedDrop)]
struct IdentificationContext {
    #[pin]
    irq_work: IrqWork,
    rx: Vec<Receiver<Sample>>,
    sdh: SDH,
    tx: Sender<Vec<u64>>,
    migration: Arc<MigrationContext>,
}

impl IdentificationContext {
    fn queue_on(&self, cpu: i32) -> bool {
        irq_work_queue_on(&self.irq_work as *const _ as *mut _, cpu)
    }

    fn drain_pebs(&mut self) {
        for rx in self.rx.iter() {
            while let Some(sample) = rx.recv() {
                self.sdh.add(sample.va & HPAGE_MASK);
                let n = sample.id;
                if n % 4096 == 0 {
                    pr_info!("drained {n} samples");
                }
                if n % u64::MAX == 0 {
                    self.migration.queue_on(CPU_MIGRATION);
                }
            }
        }
    }

    fn ffi_handler(irq_work: &IrqWork) {
        let this = unsafe { &mut *(irq_work as *const _ as *mut Self) };
        this.drain_pebs();
    }

    fn new(
        rx: Vec<Receiver<Sample>>,
        tx: Sender<Vec<u64>>,
        sdh: SDH,
        migration: Arc<MigrationContext>,
    ) -> impl PinInit<Self> {
        unsafe {
            pin_init!(Self {
                irq_work <- pin_init_from_closure::<_, Infallible>(move |slot| {
                    init_irq_work(slot, Self::ffi_handler);
                    Ok(())
                }),
                rx,
                sdh,
                tx,
                migration
            })
        }
    }
}

#[pinned_drop]
impl PinnedDrop for IdentificationContext {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("identification context exiting");
        if unsafe { hagent_dump_topk } {
            self.sdh.dump_topk();
        }
        irq_work_sync(self.get_mut() as *mut _ as *mut IrqWork);
        pr_info!("identification context exited");
    }
}

#[repr(C)]
#[pin_data(PinnedDrop)]
struct MigrationContext {
    #[pin]
    work: Work,
    rx: Receiver<Vec<u64>>,
}

impl MigrationContext {
    fn queue_on(&self, cpu: i32) -> bool {
        queue_work_on(cpu, unsafe { system_wq }, &self.work as *const _ as *mut _)
    }

    fn ffi_handler(work: &Work) {
        let this = unsafe { &mut *(work as *const _ as *mut Self) };
        pr_info!("calling migration handler {:?}", this as *mut _);
    }

    fn new(rx: Receiver<Vec<u64>>) -> impl PinInit<Self> {
        unsafe {
            pin_init!(Self {
                work <- pin_init_from_closure::<_, Infallible>(move |slot| {
                    init_work(slot, Self::ffi_handler);
                    Ok(())
                }),
                rx,
            })
        }
    }
}

#[pinned_drop]
impl PinnedDrop for MigrationContext {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("migration context exiting");
        flush_work(self.get_mut() as *mut _ as *mut Work);
        pr_info!("migration context exited");
    }
}

struct Inner {
    collection: Vec<Arc<CollectionContext>>,
    identification: Arc<IdentificationContext>,
    migration: Arc<MigrationContext>,
}

// Inner is pinned, it's ownership will never be send across threads
unsafe impl Send for Inner {}

impl Inner {
    fn new() -> Self {
        let channel_capacity = unsafe { hagent_channel_capacity };
        let (migration_tx, migration_rx) = spsc::channel(channel_capacity);
        let migration = Arc::pin_init(MigrationContext::new(migration_rx)).unwrap();

        let ncpu = num_online_cpus() as usize;
        let mut collection_tx = Vec::try_with_capacity(ncpu).unwrap();
        let mut collection_rx = Vec::try_with_capacity(ncpu).unwrap();
        for _ in 0..ncpu {
            let (tx, rx) = spsc::channel(channel_capacity);
            collection_tx.try_push(tx).unwrap();
            collection_rx.try_push(rx).unwrap();
        }
        let sdh = unsafe { SDH::new(hagent_sdh_w, hagent_sdh_d, hagent_sdh_k) };
        let identification = Arc::pin_init(IdentificationContext::new(
            collection_rx,
            migration_tx,
            sdh,
            migration.clone(),
        ))
        .unwrap();

        let mut collection = Vec::try_with_capacity(ncpu).unwrap();
        collection_tx.into_iter().enumerate().for_each(|(cpu, tx)| {
            collection
                .try_push(
                    Arc::pin_init(CollectionContext::new(cpu as _, tx, identification.clone()))
                        .unwrap(),
                )
                .unwrap()
        });

        Self {
            collection,
            identification,
            migration,
        }
    }
}

#[pin_data]
pub struct Migrator {
    #[pin]
    inner: Mutex<Inner>,
}

impl Migrator {
    pub fn new(p: Pid) -> impl PinInit<Self> {
        pr_info!("enabling PEBS based heterogeneous memory management for pid {p}");
        pin_init!(Self {
            inner <- new_mutex!(Inner::new(), "Migrator::inner"),
        })
    }
}
