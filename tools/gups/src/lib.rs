use std::{
    hint, ptr,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc, Barrier,
    },
    thread::{self, sleep, JoinHandle},
    time::{Duration, SystemTime},
};
// We use SystemTime here because its backed by TSC, which is reliable under virtualization.
// SystemTime calls clock_gettime (Realtime Clock) and in turns calls ktime_get_real_ts64,
// which is the same as gettimeofday

use anyhow::Result;
use rand::{distributions::Distribution, thread_rng};
use tracing::info;

#[derive(Debug, Default)]
pub struct GupsSharedContex {
    /// Total time in ns spent perform gups updates
    elapsed_ns: AtomicUsize,
    /// How many updates finished currently
    finished: AtomicUsize,
    /// Signal stats thread to exit
    should_exit: AtomicBool,
}

#[derive(Debug, Default, Clone, Copy)]
pub struct GupsCommonContex<D: Distribution<usize>> {
    /// Number of worker threads
    threads: usize,
    /// Number of updates per thread
    updates: usize,
    /// Length of the entire memory region
    len: usize,
    /// Granularity of each update
    granularity: usize,
    /// Element index distribution
    distribution: D,
}

#[derive(Debug)]
pub struct Gups<D: Distribution<usize>> {
    /// Mmapped memory region to perform updates
    memory: memmap::MmapMut,
    /// Common parameters
    common: GupsCommonContex<D>,
    /// Shared states for sync across threads
    shared: Arc<GupsSharedContex>,
    /// stats thread handle
    stats_handle: Option<JoinHandle<()>>,
}

impl<D: Distribution<usize> + Clone + Send + 'static> Gups<D> {
    pub fn new(
        threads: usize,
        updates: usize,
        len: usize,
        granularity: usize,
        distribution: D,
    ) -> Result<Self> {
        let start = SystemTime::now();
        let mut memory = memmap::MmapOptions::new().len(len).map_anon()?;
        memory.fill(0);
        info!("mmap init took: {:?}", start.elapsed());
        info!(
            "global memory region [{:?}, {:?}) len 0x{:x}",
            memory.as_ptr(),
            memory.as_ptr().wrapping_add(memory.len()),
            memory.len(),
        );
        let mut gups = Self {
            memory,
            common: GupsCommonContex {
                threads,
                updates,
                len,
                granularity,
                distribution,
            },
            shared: Arc::new(GupsSharedContex {
                elapsed_ns: AtomicUsize::new(0),
                finished: AtomicUsize::new(0),
                should_exit: AtomicBool::new(false),
            }),
            stats_handle: None,
        };
        let handle = gups.start_stats()?;
        gups.stats_handle.replace(handle);
        Ok(gups)
    }

    fn start_stats(&self) -> Result<JoinHandle<()>> {
        let shared = self.shared.clone();
        let f = move || {
            let mut prev = shared.finished.load(Ordering::SeqCst);
            let mut start = SystemTime::now();
            while !shared.should_exit.load(Ordering::Relaxed) {
                sleep(Duration::from_secs(1));
                let current = shared.finished.load(Ordering::SeqCst);
                let now = SystemTime::now();
                // giga updates per second == updates per nano second
                let gups =
                    (current - prev) as f64 / now.duration_since(start).unwrap().as_nanos() as f64;
                info!("last second gups: {gups}");
                prev = current;
                start = now;
            }
        };
        let handle = thread::Builder::new()
            .name("gups stats".to_string())
            .spawn(f)?;
        Ok(handle)
    }

    pub fn start_workers(&mut self) -> Result<()> {
        let fire = Arc::new(Barrier::new(self.common.threads));
        let goal = Arc::new(Barrier::new(self.common.threads));
        let _ = crossbeam::scope(|scope| {
            let handles: Vec<_> = self
                .memory
                .chunks_exact_mut(self.common.len / self.common.threads)
                .enumerate()
                .map(|(tid, thread_mem)| {
                    // let mut thread_mem = thread_mem.to_owned();
                    let common = self.common.clone();
                    let shared = self.shared.clone();
                    let fire = fire.clone();
                    let goal = goal.clone();
                    info!(
                        "thread {tid} memory region [{:?}, {:?}) len 0x{:x}",
                        thread_mem.as_ptr(),
                        thread_mem.as_ptr().wrapping_add(thread_mem.len()),
                        thread_mem.len()
                    );
                    scope.spawn(move |_| {
                        // all threads should start together
                        fire.wait();
                        let start = SystemTime::now();
                        let mut read = vec![0; common.granularity];
                        for (finished, i) in common
                            .distribution
                            .sample_iter(&mut thread_rng())
                            .take(common.updates)
                            .enumerate()
                        {
                            let elem = &mut thread_mem
                                [i * common.granularity..(i + 1) * common.granularity];
                            if common.granularity <= 8 {
                                unsafe {
                                    let ptr = elem.as_mut_ptr() as *mut u64;
                                    let read = ptr::read(ptr);
                                    ptr::write(ptr, read + 1);
                                }
                            } else {
                                // update
                                read.copy_from_slice(elem);
                                hint::black_box(&mut read);
                                elem.copy_from_slice(&read);
                            }
                            if finished % 10000 == 0 {
                                shared.finished.fetch_add(10000, Ordering::SeqCst);
                            }
                        }
                        goal.wait();
                        if tid == 0 {
                            // only the last thread should do the book-keeping
                            let elapsed = start.elapsed().unwrap();
                            info!("iteration took {elapsed:?}");
                            let gups = (common.updates * common.threads) as f64
                                / elapsed.as_nanos() as f64;
                            info!("iteration gups {gups}");
                            shared
                                .elapsed_ns
                                .fetch_add(elapsed.as_nanos() as _, Ordering::SeqCst);
                        }
                    })
                })
                .collect();
            // wait for all worker threads to complete
            drop(handles);
        })
        .expect("failed to start workers");

        Ok(())
    }
}

impl<D: Distribution<usize>> Gups<D> {
    /// TODO: support changing distribution online
    pub fn reset_distribution(&mut self, distribution: D) {
        self.common.distribution = distribution;
    }

    fn signal_exit(&self) {
        self.shared.should_exit.store(true, Ordering::Relaxed);
    }
}

impl<D: Distribution<usize>> Drop for Gups<D> {
    fn drop(&mut self) {
        self.signal_exit();
        if let Some(h) = self.stats_handle.take() {
            h.join().expect("failed waiting for stats thread to exit");
        }
        let finished = self.shared.finished.load(Ordering::SeqCst);
        let elapsed_ns = self.shared.elapsed_ns.load(Ordering::SeqCst);
        let gups = finished as f64 / elapsed_ns as f64;
        info!("overall gups: {gups}");
        info!(
            "overall elapsed: {:?}",
            Duration::from_nanos(elapsed_ns as _)
        );
    }
}

// pub fn add(left: usize, right: usize) -> usize {
//     left + right
// }

// #[cfg(test)]
// mod tests {
//     use super::*;

//     #[test]
//     fn it_works() {
//         let result = add(2, 2);
//         assert_eq!(result, 4);
//     }
// }
