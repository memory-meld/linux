# MEMTIS: Efficient Memory Tiering with Dynamic Page Classification and Page Size Determination

## System configuration
* Fedora 33 server
* Two 20-core Intel(R) Xeon(R) Gold 5218R CPU @ 2.10GHz
* 6 x 16GB DRAM per socket
* 6 x 128GB Intel Optane DC Persistent Memory per socket

MEMTIS currently supports two system configurations
* DRAM + Intel DCPMM (used only single socket)
* local DRAM + remote DRAM (used two socket, CXL emulation mode)

## Source code information
See linux/

You have to enable CONFIG\_HTMM when compiling the linux source.
```bash
scripts/config --enable CONFIG_HTMM
```

Changes made to the original [v5.15.19](https://cdn.kernel.org/pub/linux/kernel/v5.x/linux-5.15.19.tar.xz)
kernel is listed in `memtis.path`, which can be obtained via:
```bash
git diff 8ae72487 cb79fa4d -- ':linux' ':!linux/5'
# summary
git diff --stat=120 --compact-summary 8ae72487 cb79fa4d -- ':linux' ':!linux/5'
 linux/Makefile                               |    2 +-
 linux/arch/x86/entry/syscalls/syscall_64.tbl |    2 +
 linux/arch/x86/include/asm/pgtable.h         |   11 +
 linux/arch/x86/include/asm/pgtable_64.h      |    4 +
 linux/arch/x86/include/asm/pgtable_types.h   |    4 +
 linux/arch/x86/mm/init_64.c                  |   17 +
 linux/arch/x86/mm/pgtable.c                  |   23 +-
 linux/include/asm-generic/pgalloc.h          |    3 +
 linux/include/linux/cgroup-defs.h            |    4 +
 linux/include/linux/htmm.h (new)             |  212 ++++++++++
 linux/include/linux/huge_mm.h                |    4 +
 linux/include/linux/memcontrol.h             |   56 ++-
 linux/include/linux/mempolicy.h              |   22 +-
 linux/include/linux/migrate.h                |    7 +
 linux/include/linux/mm.h                     |    4 +
 linux/include/linux/mm_types.h               |   28 ++
 linux/include/linux/mmzone.h                 |   11 +
 linux/include/linux/node.h                   |    5 +
 linux/include/linux/page-flags.h             |   11 +
 linux/include/linux/perf_event.h             |    6 +
 linux/include/linux/rmap.h                   |   25 +-
 linux/include/linux/swap.h                   |    2 +
 linux/include/linux/syscalls.h               |    4 +
 linux/include/linux/vm_event_item.h          |   10 +
 linux/include/trace/events/htmm.h (new)      |   57 +++
 linux/include/trace/events/mmflags.h         |   12 +-
 linux/kernel/cgroup/cgroup.c                 |    3 +-
 linux/kernel/events/core.c                   |  521 ++++++++++++++++++++++++
 linux/kernel/exit.c                          |    3 +
 linux/kernel/fork.c                          |   10 +
 linux/kernel/sched/sched.h                   |    2 +
 linux/mm/Kconfig                             |   11 +
 linux/mm/Makefile                            |    1 +
 linux/mm/htmm_core.c (new)                   | 1439 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 linux/mm/htmm_migrater.c (new)               | 1128 +++++++++++++++++++++++++++++++++++++++++++++++++++
 linux/mm/htmm_sampler.c (new)                |  430 ++++++++++++++++++++
 linux/mm/huge_memory.c                       |  222 ++++++++++-
 linux/mm/khugepaged.c                        |   97 ++++-
 linux/mm/memcontrol.c                        |  301 +++++++++++++-
 linux/mm/memory.c                            |   24 +-
 linux/mm/memory_hotplug.c                    |    9 +-
 linux/mm/mempolicy.c                         |  622 +++++++++++++++++++++++++++++
 linux/mm/migrate.c                           |  139 ++++++-
 linux/mm/page_alloc.c                        |   13 +
 linux/mm/rmap.c                              |  231 +++++++++++
 linux/mm/vmscan.c                            |    6 +-
 linux/mm/vmstat.c                            |   10 +
 47 files changed, 5721 insertions(+), 47 deletions(-)
```

### Dependencies
There are nothing special libraries for MEMTIS itself.

(You just need to install libraries for Linux compilation.)

## For experiments
### Userspace scripts
See memtis-userspace/

Please read memtis-userspace/README.md for detailed explanations

### Setting tiered memory systems with Intel DCPMM
* Reconfigures a namespace with devdax mode
```
sudo ndctl create-namespace -f -e namespace0.0 --mode=devdax
...
```
* Reconfigures a dax device with system-ram mode (KMEM DAX)
```
sudo daxctl reconfigure-device dax0.0 --mode=system-ram
...
```

### Preparing benchmarks
We used open-sourced benchmarks except SPECCPU2017.

We provided links to each benchmark source in memtis-userspace/bench\_dir/README.md

### Running benchmarks
It is necessary to create/update a simple script for each benchmark.
If you want to execute *XSBench*, for instance, you have to create memtis-userspace/bench\_cmds/XSBench.sh.

This is a sample.
```
# memtis-userspace/bench_cmds/XSBench.sh

BIN=/path/to/benchmark
BENCH_RUN="${BIN}/XSBench [Options]"

# Provide the DRAM size for each memory configuration setting.
# You must first check the resident set size of a benchmark.
if [[ "x${NVM_RATIO}" == "x1:16" ]]; then
    BENCH_DRAM="3850MB"
elif [[ "x${NVM_RATIO}" == "x1:8" ]]; then
    BENCH_DRAM="7200MB"
elif [[ "x${NVM_RATIO}" == "x1:2" ]]; then
    BENCH_DRAM="21800MB"
fi

# required
export BENCH_RUN
export BENCH_DRAM

```

#### Test
```
cd memtis-userspace/

# check running options
./scripts/run_bench.sh --help

# create an executable binary file
make

# run
sudo ./scripts/run_bench.sh -B ${BENCH} -R ${MEM_CONFIG} -V ${TEST_NAME}
## or use scripts
sudo ./run-fig5-6-10.sh
sudo ./run-fig7.sh
...
```

#### Tips for setting other tiered memory systems
See memtis-userspace/README.md

## Commit number used for artifact evaluation
174ca88

## License
<a rel="license" href="http://creativecommons.org/licenses/by-nc/4.0/"><img alt="Creative Commons License" style="border-width:0" src="https://i.creativecommons.org/l/by-nc/4.0/88x31.png" /></a><br />This work is licensed under a <a rel="license" href="http://creativecommons.org/licenses/by-nc/4.0/">Creative Commons Attribution-NonCommercial 4.0 International License</a>.

## Bibtex
To be updated 

## Authors
- Taehyung Lee (Sungkyunkwan University, SKKU) <taehyunggg@skku.edu>, <taehyung.tlee@gmail.com>
- Sumit Kumar Monga (Virginia Tech) <sumitkm@vt.edu>
- Changwoo Min (Virginia Tech) <changwoo@vt.edu>
- Young Ik Eom (Sungkyunkwan University, SKKU) <yieom@skku.edu>
