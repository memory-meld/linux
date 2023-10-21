use core::{
    convert::Infallible,
    default::Default,
    iter::{IntoIterator, Iterator},
    mem::size_of,
    ptr,
    time::Duration,
};

use crate::{
    event::Sample,
    hash::*,
    helper::*,
    sdh::SDH,
    spsc::{self, Receiver, Sender},
};
use kernel::{
    bindings,
    error::to_result,
    init::*,
    new_mutex,
    prelude::*,
    sync::*,
    task::Task,
    types::{ARef, Opaque},
};

#[repr(C)]
#[pin_data(PinnedDrop)]
struct CollectionContext {
    // sample collection
    // Put it the first element, so that the address of the struct
    // can be known when creating perf counter
    #[pin]
    event: &'static PerfEvent,
    cpu: i32,
    tx: Sender<Sample>,
    identification: Arc<IdentificationContext>,
    collected: u64,
    invalid: u64,
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
                cpu,
                tx,
                identification,
                collected: 0,
                invalid: 0,
            })
        }
    }

    extern "C" fn ffi_handler(event: &PerfEvent, data: &SampleData, _regs: &PtRegs) {
        let this = unsafe { &mut *event.overflow_handler_context.cast::<Self>() };
        this.drain_pebs(data);
    }

    fn drain_pebs(&mut self, data: &SampleData) {
        let va = data.addr;
        if !unsafe { helper_in_mmap_region(va) } {
            self.invalid += 1;
            return;
        }
        let id = self.collected;
        self.collected += 1;
        // let pa = unsafe { perf_virt_to_phys(va) };
        // if pa == 0 {
        //     self.invalid += 1;
        //     return;
        // }
        let lat = data.weight;
        // ignore if full
        self.tx.send(Sample { id, va, lat, pa: 0 }).ok();
        if (id + 1) % IDENTIFIATION_PERIOD == 0 {
            self.identification.queue();
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
    promotion_tx: Sender<u64>,
    demotion_tx: Sender<u64>,
    migration: Arc<MigrationContext>,
    received: u64,
}

impl IdentificationContext {
    fn queue(&self) -> bool {
        unsafe { irq_work_queue_on(self.irq_work.get(), CPU_IDENTIFICATION) }
    }

    extern "C" fn ffi_handler(irq_work: *mut bindings::irq_work) {
        let this = unsafe { &mut *irq_work.cast::<Self>() };
        this.identify_hotness();
    }

    fn identify_hotness(&mut self) {
        let mut sent = 0;
        let mut queued = false;
        for rx in self.rx.iter() {
            while let Some(sample) = rx.recv() {
                let va = sample.va & HPAGE_MASK;
                let (_, hot, replaced) = self.sdh.add(va);
                if hot {
                    self.promotion_tx.send(va).is_ok().then(|| sent += 1);
                }
                if let Some(va) = replaced {
                    self.demotion_tx.send(va).is_ok().then(|| sent += 1);
                }
                if !queued && sent % MIGRATION_PERIOD == 0 {
                    self.migration.queue(0);
                    queued = true;
                }
                self.received += 1;
                if self.received % DRAIN_REPORT_PERIOD == 0 {
                    pr_info!("identified {} samples", self.received);
                }
            }
        }
    }

    fn new(
        rx: Vec<Receiver<Sample>>,
        promotion_tx: Sender<u64>,
        demotion_tx: Sender<u64>,
        sdh: SDH,
        migration: Arc<MigrationContext>,
    ) -> impl PinInit<Self> {
        unsafe {
            pin_init!(Self {
                irq_work <- Opaque::ffi_init(move |slot| helper_init_irq_work(slot, Self::ffi_handler)),
                rx,
                sdh,
                promotion_tx,
                demotion_tx,
                migration,
                received: 0,
            })
        }
    }
}

#[pinned_drop]
impl PinnedDrop for IdentificationContext {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("identification context exiting");
        unsafe {
            if hagent_dump_topk {
                self.sdh.dump_topk();
            }
            irq_work_sync(self.irq_work.get())
        }
        pr_info!("identification context exited");
    }
}

#[repr(C)]
struct MovePagesArgs<const LEN: usize> {
    pid: Pid,
    pages: Box<[u64; LEN]>,
    nodes: Box<[i32; LEN]>,
    status: Box<[i32; LEN]>,
}

impl<const LEN: usize> MovePagesArgs<LEN> {
    fn new(pid: Pid, target_node: i32) -> Self {
        let mut nodes: Box<[i32; LEN]> = Box::init(init::zeroed()).unwrap();
        nodes.fill(target_node);
        Self {
            pid,
            pages: Box::init(init::zeroed()).unwrap(),
            nodes,
            status: Box::init(init::zeroed()).unwrap(),
        }
    }

    fn base_address(&mut self, address: u64) {
        (0..LEN).for_each(|i| self.pages[i] = address + PAGE_SIZE * i as u64);
    }

    fn stat_pages(&mut self) -> Result<()> {
        self.status.fill(NUMA_NO_NODE);
        to_result(move_pages(
            self.pid,
            None,
            &self.pages[..],
            &mut self.status[..],
            MPOL_MF_MOVE_ALL,
        ))
    }

    fn count_node(&self, node: i32) -> usize {
        self.status.iter().filter(|&&n| n == node).count()
    }

    fn count_status<P: FnMut(&&i32) -> bool>(&self, pred: P) -> usize {
        self.status.iter().filter(pred).count()
    }

    fn consolidate_left(&mut self) -> usize {
        let mut next = 0;
        for (i, (&status, &node)) in self.status.iter().zip(self.nodes.iter()).enumerate() {
            if status < 0 || status == node || i < next {
                continue;
            }
            self.pages.swap(i, next);
            next += 1;
        }
        self.status[..next].fill(NUMA_NO_NODE);
        next
    }

    fn move_pages(&mut self, len: usize) -> Result<()> {
        assert!(len <= LEN);
        self.status.fill(NUMA_NO_NODE);
        to_result(move_pages(
            self.pid,
            Some(&self.nodes[..]),
            &self.pages[..len],
            &mut self.status[..],
            MPOL_MF_MOVE_ALL,
        ))
    }
}

#[repr(C)]
#[pin_data(PinnedDrop)]
struct MigrationContext {
    #[pin]
    delayed_work: DelayedWork,
    task: ARef<Task>,
    promotion_rx: Receiver<u64>,
    demotion_rx: Receiver<u64>,
    dram: HashSet<u64, BuildHasherDefault<FnvHasher>>,
    pmem: HashSet<u64, BuildHasherDefault<FnvHasher>>,
    promotion_pending: Vec<u64>,
    demotion_pending: Vec<u64>,
    dram_node: i32,
    pmem_node: i32,
    promotion_args: MovePagesArgs<HPAGE_PAGES>,
    demotion_args: MovePagesArgs<HPAGE_PAGES>,
    migrated_pages: u64,
    start: Duration,
}
static_assert!(HPAGE_PAGES == 512);

impl MigrationContext {
    fn queue(&self, delay: u64) -> bool {
        let work = self.delayed_work.get();
        unsafe {
            // work_busy will check for pending and running.
            // queue_delayed_* will set the pending bit when called, even the work will not be
            // executed right away.
            if work_busy(work) == 0 {
                queue_delayed_work_on(CPU_MIGRATION, system_wq, work, delay)
            } else {
                false
            }
        }
    }

    // this must be called by the work itself
    fn queue_unchecked(&mut self, delay: u64) -> bool {
        unsafe { queue_delayed_work_on(CPU_MIGRATION, system_wq, self.delayed_work.get(), delay) }
    }

    extern "C" fn ffi_handler(work: *mut bindings::delayed_work) {
        let this = unsafe { &mut *work.cast::<Self>() };
        this.migrate();
    }

    fn speed_mbps(pages: u64, elapsed: Duration) -> u64 {
        pages * PAGE_SIZE / (elapsed.as_micros() + 1) as u64
    }

    fn migrate(&mut self) {
        let start = native_sched_clock();
        let mut promotion_success = 0;
        let mut demotion_success = 0;
        while let Some(addr) = self.promotion_rx.recv() {
            self.promotion_pending.try_push(addr).unwrap();
        }
        while let Some(addr) = self.demotion_rx.recv() {
            self.demotion_pending.try_push(addr).unwrap();
        }
        while let Some(promotion) = node_has_space(self.pmem_node)
            .then_some(0)
            .and_then(|_| self.promotion_pending.pop())
        {
            if self.dram.get(&promotion).is_some() {
                continue;
            }
            self.promotion_args.base_address(promotion);
            self.promotion_args.stat_pages().unwrap();
            let promotion_todo = self.promotion_args.consolidate_left();
            if promotion_todo == 0 {
                self.dram.insert(promotion);
                continue;
            }
            let mut demotion_needed = node_has_space(self.dram_node)
                .then_some(0)
                .unwrap_or(promotion_todo);
            while demotion_needed != 0 {
                let demotion = self.demotion_pending.pop().unwrap_or_else(|| {
                    self.demotion_pending.try_resize(BATCH_SIZE, 0).unwrap();
                    // FIXME: deadloop on small DRAM
                    let failed = unsafe {
                        helper_find_random_candidate(
                            &self.task,
                            self.demotion_pending.as_mut_ptr(),
                            self.demotion_pending.len() as _,
                        )
                    };
                    assert_eq!(failed, 0);
                    self.demotion_pending.pop().unwrap()
                });
                // cannot find a suitable demotion address
                assert_ne!(demotion, 0);
                if self.pmem.get(&demotion).is_some() {
                    continue;
                }
                self.demotion_args.base_address(demotion);
                self.demotion_args.stat_pages().unwrap();
                let demotion_todo = self.demotion_args.consolidate_left();
                if demotion_todo == 0 {
                    // try again
                    self.pmem.insert(demotion);
                    continue;
                }
                self.demotion_args.move_pages(demotion_todo).unwrap();
                let demotion_done = self.demotion_args.count_node(self.pmem_node);
                assert_eq!(demotion_todo, demotion_done);
                self.pmem.insert(demotion);
                self.migrated_pages += demotion_done as u64;
                demotion_success += demotion_done;

                demotion_needed = demotion_needed.saturating_sub(demotion_done);
            }

            self.promotion_args.move_pages(promotion_todo).unwrap();
            let promotion_done = self.promotion_args.count_node(self.dram_node);
            assert_eq!(promotion_todo, promotion_done);
            self.dram.insert(promotion);
            self.migrated_pages += promotion_done as u64;
            promotion_success += promotion_done;

            // throttle
            let average_speed =
                Self::speed_mbps(self.migrated_pages, native_sched_clock() - self.start);
            if average_speed > THROTTLE_MBPS {
                self.queue_unchecked(200);
                break;
            }
        }

        let elapsed = native_sched_clock() - start;
        let burst_speed = Self::speed_mbps((promotion_success + demotion_success) as _, elapsed);
        let average_speed =
            Self::speed_mbps(self.migrated_pages, native_sched_clock() - self.start);
        pr_info!("migration work for pid {} finished with {promotion_success} promoted and {demotion_success} demoted elapsed {elapsed:?} burst speed {burst_speed}MB/s average speed {average_speed}MB/s pending {} promotion and {} demotion",
            self.task.pid(), self.promotion_pending.len(), self.demotion_pending.len());
    }

    fn new(
        task: ARef<Task>,
        promotion_rx: Receiver<u64>,
        demotion_rx: Receiver<u64>,
    ) -> impl PinInit<Self> {
        unsafe {
            let dram = HashSet::with_capacity_and_hasher(
                2 * hagent_sdh_k,
                BuildHasherDefault::<FnvHasher>::default(),
            );
            let pmem = HashSet::with_capacity_and_hasher(
                2 * hagent_sdh_k,
                BuildHasherDefault::<FnvHasher>::default(),
            );
            let promotion_pending = Vec::try_with_capacity(hagent_channel_capacity).unwrap();
            let demotion_pending = Vec::try_with_capacity(hagent_channel_capacity).unwrap();
            let dram_node = helper_dram_node();
            let pmem_node = dram_node + 1;
            let promotion_args = MovePagesArgs::new(task.pid(), dram_node);
            let demotion_args = MovePagesArgs::new(task.pid(), pmem_node);
            pin_init!(Self {
                delayed_work <- Opaque::ffi_init(move |slot| helper_init_delayed_work(slot, Self::ffi_handler)),
                task,
                promotion_rx,
                demotion_rx,
                dram,
                pmem,
                promotion_pending,
                demotion_pending,
                dram_node,
                pmem_node,
                promotion_args,
                demotion_args,
                migrated_pages: 0,
                start: native_sched_clock(),
            })
        }
    }
}

#[pinned_drop]
impl PinnedDrop for MigrationContext {
    fn drop(self: Pin<&mut Self>) {
        pr_info!("migration context exiting");
        unsafe { cancel_delayed_work_sync(self.delayed_work.get()) };
        pr_info!(
            "total {} bytes of data migrated",
            self.migrated_pages * PAGE_SIZE
        );
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
    fn new(task: ARef<Task>) -> Self {
        let channel_capacity = unsafe { hagent_channel_capacity };
        let (promotion_tx, promotion_rx) = spsc::channel(channel_capacity);
        let (demotion_tx, demotion_rx) = spsc::channel(channel_capacity);
        let migration =
            Arc::pin_init(MigrationContext::new(task, promotion_rx, demotion_rx)).unwrap();

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
            promotion_tx,
            demotion_tx,
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

#[pin_data(PinnedDrop)]
pub struct Migrator {
    #[pin]
    inner: Mutex<Inner>,
    pid: Pid,
    task: ARef<Task>,
}

impl Migrator {
    pub fn new(pid: Pid) -> impl PinInit<Self> {
        pr_info!("enabling PEBS based heterogeneous memory management for pid {pid}");
        // this reference will not outlive the task itself, because we will be destroyed by
        // then via the hooked exit_group
        let task = ARef::from(unsafe { &*helper_pid_task(pid) });
        pin_init!(Self {
            inner <- new_mutex!(Inner::new(task.clone()), "Migrator::inner"),
            pid,
            task,
        })
    }
}

#[pinned_drop]
impl PinnedDrop for Migrator {
    fn drop(self: Pin<&mut Self>) {
        let pid = self.pid;
        pr_info!("disabling PEBS based heterogeneous memory management for pid {pid}");
    }
}
