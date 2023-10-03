use std::time::Instant;

use anyhow::Result;
use clap::Parser;
use mix_distribution::Mix;
use rand::distributions::Uniform;
use tracing::info;

use gups::*;

/// GUPS hotset version with `weight` times as more updates going to the hot region than to the rest.
#[derive(Parser, Debug)]
#[command(author, version, about)]
struct Args {
    /// Number of worker threads
    threads: usize,
    /// Number of updates per thread
    updates: usize,
    /// Length of the entire memory region
    len: usize,
    /// Granularity of each update
    granularity: usize,
    /// Length of the hot memory region
    hot_len: usize,
    /// Weight ratio of hot region to the rest
    weight: usize,
}

fn main() -> Result<()> {
    tracing_subscriber::fmt::init();
    let args = Args::parse();
    info!("gups args {args:?}");

    let split = args.hot_len / args.threads / args.granularity;
    let end = args.len / args.threads / args.granularity;
    let dist = Mix::new(
        [Uniform::new(0, split), Uniform::new(split, end)],
        [args.weight, 1],
    )
    .unwrap();
    let mut gups = Gups::new(args.threads, args.updates, args.len, args.granularity, dist)?;

    // warm up
    let start = Instant::now();
    gups.start_workers()?;
    info!("warm up took: {:?}", start.elapsed());

    // timed iteration
    let start = Instant::now();
    gups.start_workers()?;
    info!("timed iteration took: {:?}", start.elapsed());

    Ok(())
}
