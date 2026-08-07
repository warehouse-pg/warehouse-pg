# Welcome to the WarehousePG Project!


<p align="center">
    <img alt="WarehousePG" src="logo-warehousepg.png" width="350px">
</p>

<p align="center">
    <a href="https://github.com/warehouse-pg/warehouse-pg/actions/workflows/whpg-ci.yml">
        <img src="https://github.com/warehouse-pg/warehouse-pg/actions/workflows/whpg-ci.yml/badge.svg" alt="CI Status">
    </a>
</p>

WarehousePG (WHPG) is an advanced, fully featured, open
source data warehouse, based on Greenplum® Database and PostgreSQL. 
WarehousePG provides powerful and rapid analytics on
petabyte scale data volumes. Uniquely geared toward big data
analytics, WarehousePG is powered by the world’s most advanced
cost-based query optimizer delivering high analytical query
performance on large data volumes.

The [Greenplum Database OSS project](https://github.com/greenplum-db/gpdb-archive) was released under the [Apache 2
license](https://github.com/greenplum-db/gpdb-archive/blob/main/LICENSE) and went closed source in May 2024.

### Build the database

Follow [appropriate Linux steps](README.RHEL-Rocky.bash) for getting your system ready.

```
# Configure build environment to install at /usr/local/whpg
./configure --with-perl --with-python --with-libxml --with-gssapi --prefix=/usr/local/whpg

# Compile and install
make -j8
make -j8 install

# Bring in greenplum environment into your running shell
source /usr/local/whpg/greenplum_path.sh

# Start demo cluster
make create-demo-cluster
# (gpdemo-env.sh contains __PGPORT__ and __COORDINATOR_DATA_DIRECTORY__ values)
source gpAux/gpdemo/gpdemo-env.sh
```

The directory, the TCP ports, the number of segments, and the existence of
standbys for segments and coordinator for the demo cluster can be changed
on the fly.
Instead of `make create-demo-cluster`, consider:

```
DATADIRS=/tmp/whpg-cluster PORT_BASE=5555 NUM_PRIMARY_MIRROR_PAIRS=1 WITH_MIRRORS=false make create-demo-cluster
```

The TCP port for the regression test can be changed on the fly:

```
PGPORT=5555 make installcheck-world
```

To turn GPORCA off and use Postgres planner for query optimization:
```
set optimizer=off;
```

If you want to clean all generated files
```
make distclean
```

### Build with AddressSanitizer (ASan)

To hunt memory bugs (out-of-bounds reads/writes, use-after-free, double
free), build with `--enable-asan`. It instruments C and C++ (including
GPORCA) alike, puts the sanitizer runtime on every link line, and defaults
optimization to `-O1`; your own `CFLAGS`/`CXXFLAGS` still override.

```
./configure --enable-asan --enable-debug --enable-cassert --prefix=/usr/local/whpg-asan
make -j8 && make -j8 install
```

Before creating a cluster or running anything, export the runtime options:

```
mkdir -p /tmp/asan
export ASAN_OPTIONS="detect_leaks=0:log_path=/tmp/asan/asan:abort_on_error=1:disable_coredump=1:print_stacktrace=1"
export PGCTLTIMEOUT=300
```

Notes:

* `detect_leaks=0` is required: management tools run short-lived
  `postgres --gp-version`-style commands whose leak-check exit code (23)
  would otherwise fail `gpinitsystem`/`gpstart`. Leak hunting needs a
  separately configured LeakSanitizer run.
* Reports land in `log_path` files (`/tmp/asan/asan.<pid>`), one per
  aborted process. A test run that "passes" while report files exist has
  still found bugs — always check the directory.
* For a single-host demo cluster the exported variables reach the server
  processes automatically. On multi-host clusters ssh strips the
  environment; either drop the exports into
  `$GPHOME/etc/environment.d/50-asan.conf` (sourced by
  `greenplum_path.sh` on every host) or start with
  `gpstart --wrapper=env --wrapper-args="ASAN_OPTIONS=..."`.
* Known limitations: the UDP interconnect's receive thread and GPORCA
  run close to their configured stack budgets under ASan's larger stack
  frames — for full test runs prefer `gp_interconnect_type=tcp` and the
  Postgres planner (`optimizer=off`) until those budgets are raised.

## Disclaimer
Greenplum® is a registered trademark of Broadcom Inc.<br>
EDB and EDB Postgres AI are not affiliated with, endorsed by, or sponsored by Broadcom Inc.<br>
Any references to Greenplum are for comparative, educational, and interoperability purposes only.


## Table of Contents
- [Source Code Provenance](PROVENANCE.md)
- [Code of Conduct](CODE-OF-CONDUCT.md)
- [Contributing](CONTRIBUTING.md)
- [License](LICENSE)


## Contributing

WarehousePG is maintained by a core team of developers with commit rights to the
[main WHPG repository](https://github.com/warehouse-pg/warehouse-pg) on GitHub. At the
same time, we are very eager to receive contributions from anybody in the wider
WarehousePG community. This section covers all you need to know if you want to see
your code or documentation changes be added to WarehousePG and appear in the
future releases.

## Getting started

WarehousePG is developed on GitHub, and anybody wishing to contribute to it will
have to [have a GitHub account](https://github.com/signup/free) and be familiar
with [Git tools and workflow](https://wiki.postgresql.org/wiki/Working_with_Git).


Once you have your GitHub account, [fork](https://github.com/warehouse-pg/warehouse-pg/fork)
this repository so that you can have your private copy to start hacking on and to
use as source of pull requests.
