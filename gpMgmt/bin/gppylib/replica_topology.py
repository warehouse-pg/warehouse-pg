#!/usr/bin/env python3
"""
  replica_topology.py:

    Recognises a whpg-dr read replica from its coordinator data directory
    and builds the GpArray of the replica's own nodes.

    On a read replica the catalog is the source cluster's, replayed from its
    WAL, so gp_segment_configuration names the SOURCE's hosts, ports and data
    directories.  whpg-dr writes whpg_replica.conf into every data directory
    (postgresql.conf includes it with include_if_exists); its
    whpg_dispatch_topology_file setting names the file that lists the
    replica's nodes, and the coordinator dispatches from that file.  The
    utilities that act on the nodes (gpconfig, gpstop, gpstart) read the same
    file instead of the catalog.
"""

import os
import re

from gppylib import gparray, pgconf

REPLICA_CONF_FILE = 'whpg_replica.conf'
TOPOLOGY_GUC = 'whpg_dispatch_topology_file'

# Limits of the coordinator's topology parser (dispatch_topology_parse):
# content and dbid are int16, a hostname or address fits MAXHOSTNAMELEN.
_INT16_MAX = 32767
_MAXHOSTNAMELEN = 64

# whpg-dr's managed header, e.g. "# cluster=prod generation=3 written=...".
# The cluster name matches lazily up to " generation=", as whpg-dr's own
# parser does: a cluster name configured before whpg-dr's naming rule may
# contain whitespace.
_HEADER_RE = re.compile(r'^# cluster=(.+?) generation=(\d+) written=', re.MULTILINE)

# Written by whpg-dr at the end of a replica enable, once the nodes are
# paused at the anchor, the anchor is published and an anchored query has
# run.  A re-enable rewrites the file without it; disable and promote remove
# the file; a restore over the replica removes the line.
_ENABLED_RE = re.compile(r'^# whpg-dr: enable completed generation=(\d+)$', re.MULTILINE)


class ReplicaTopologyError(Exception):
    pass


class ReplicaConf(object):
    """The whpg_replica.conf of a coordinator data directory."""

    def __init__(self, path, settings, content):
        self.path = path
        self.settings = settings
        header = _HEADER_RE.search(content)
        self.cluster = header.group(1) if header else None
        self.generation = int(header.group(2)) if header else None
        enabled = _ENABLED_RE.search(content)
        self.enabled_generation = int(enabled.group(1)) if enabled else None

    def managed_settings(self):
        """The settings whpg-dr manages: every one this file sets."""
        return set(self.settings.keys())

    def enable_completed(self):
        return self.generation is not None and self.enabled_generation == self.generation

    def topology_path(self):
        value = self.settings.str(TOPOLOGY_GUC, '')
        if not value:
            return None
        if os.path.isabs(value):
            return value
        return os.path.join(os.path.dirname(self.path), value)


def read_replica_conf(coordinator_datadir):
    """
    Returns the ReplicaConf of coordinator_datadir when it is a read replica
    coordinator, that is when whpg_replica.conf sets
    whpg_dispatch_topology_file, and None otherwise.
    """
    path = os.path.join(coordinator_datadir, REPLICA_CONF_FILE)
    if not os.path.exists(path):
        return None
    with open(path) as fp:
        content = fp.read()
    rc = ReplicaConf(path, pgconf.readfile(path), content)
    if rc.topology_path() is None:
        return None
    return rc


def parse_topology(text, path='topology file'):
    """
    Parses a dispatch topology file by the rules the coordinator's parser
    applies: '#' starts a comment line, blank lines are skipped, and every
    other line is "content dbid hostname address port datadir", with content
    -1..32767, dbid 1..32767, port 1..65535, a hostname and an address of at
    most 64 characters and an absolute datadir.  Exactly one row per content
    and per dbid, a coordinator row (content -1) and at least one segment
    row, the segment contents numbered 0 .. N-1 without a gap (the
    coordinator refuses any other numbering when it builds its component
    list).  Returns a list of (content, dbid, hostname, address, port,
    datadir) sorted by content.
    """
    rows = []
    contents = set()
    dbids = set()
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        fields = stripped.split()
        if len(fields) != 6:
            raise ReplicaTopologyError('%s line %d: expected 6 fields "content dbid hostname address port datadir"'
                                       % (path, lineno))
        try:
            content, dbid, port = int(fields[0]), int(fields[1]), int(fields[4])
        except ValueError:
            raise ReplicaTopologyError('%s line %d: content, dbid and port must be integers' % (path, lineno))
        if content < -1 or content > _INT16_MAX:
            raise ReplicaTopologyError('%s line %d: invalid content %d' % (path, lineno, content))
        if dbid < 1 or dbid > _INT16_MAX:
            raise ReplicaTopologyError('%s line %d: invalid dbid %d' % (path, lineno, dbid))
        if port < 1 or port > 65535:
            raise ReplicaTopologyError('%s line %d: invalid port %d' % (path, lineno, port))
        if len(fields[2]) > _MAXHOSTNAMELEN or len(fields[3]) > _MAXHOSTNAMELEN:
            raise ReplicaTopologyError('%s line %d: hostname or address exceeds %d characters'
                                       % (path, lineno, _MAXHOSTNAMELEN))
        if not os.path.isabs(fields[5]):
            raise ReplicaTopologyError('%s line %d: datadir must be an absolute path' % (path, lineno))
        if content in contents:
            raise ReplicaTopologyError('%s line %d: duplicate entry for content %d' % (path, lineno, content))
        if dbid in dbids:
            raise ReplicaTopologyError('%s line %d: duplicate dbid %d' % (path, lineno, dbid))
        contents.add(content)
        dbids.add(dbid)
        rows.append((content, dbid, fields[2], fields[3], port, fields[5]))
    if -1 not in contents:
        raise ReplicaTopologyError('%s: missing coordinator row (content -1)' % path)
    if len(contents) < 2:
        raise ReplicaTopologyError('%s: no segment rows' % path)
    if contents != set(range(-1, len(contents) - 1)):
        raise ReplicaTopologyError('%s: segment contents must be 0 .. %d without a gap, found %s'
                                   % (path, len(contents) - 2, ', '.join(str(c) for c in sorted(contents) if c >= 0)))
    return sorted(rows)


def gparray_from_replica(rc):
    """
    Builds the GpArray of the replica's nodes from the topology file rc
    names: the coordinator and one primary per content, all up, no mirrors
    and no standby coordinator.  The file's coordinator row must describe
    the coordinator rc belongs to (its data directory and the port
    whpg_replica.conf sets): the utilities start, check and stop the
    coordinator by this row.
    """
    path = rc.topology_path()
    try:
        with open(path) as fp:
            rows = parse_topology(fp.read(), path)
    except IOError as e:
        raise ReplicaTopologyError('cannot read the replica topology file %s named by %s in %s: %s'
                                   % (path, TOPOLOGY_GUC, rc.path, e))
    _, _, _, _, port, datadir = rows[0]
    own_datadir = os.path.dirname(rc.path)
    own_port = rc.settings.int('port') if 'port' in rc.settings else None
    if os.path.realpath(datadir) != os.path.realpath(own_datadir) or (own_port is not None and port != own_port):
        raise ReplicaTopologyError('the coordinator row of %s (port %d, %s) is not this coordinator (port %s, %s)'
                                   % (path, port, datadir, own_port, own_datadir))
    segments = []
    for content, dbid, hostname, address, port, datadir in rows:
        segments.append(gparray.Segment(content, gparray.ROLE_PRIMARY, dbid, gparray.ROLE_PRIMARY,
                                        gparray.MODE_NOT_SYNC, gparray.STATUS_UP,
                                        hostname, address, port, datadir))
    return gparray.GpArray(segments, [seg.copy() for seg in segments])
