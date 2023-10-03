use std::time::Instant;

use anyhow::Result;
use clap::Parser;
use rand::distributions::Uniform;
use tracing::info;

use gups::*;

/// GUPS random version with all updates going to the entire region uniformly
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
}

fn main() -> Result<()> {
    tracing_subscriber::fmt::init();
    let args = Args::parse();
    info!("gups args {args:?}");

    let end = args.len / args.threads / args.granularity;
    let dist = Uniform::new(0, end);
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
