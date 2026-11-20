#!/bin/bash
#-------------------------------------------------------------------------
# setup_internal_tls.sh
#
# Provision a cluster CA + node certificate mesh for QD<->QE mutual TLS and
# switch the cluster to gp_internal_tls=verify-ca, which retires the legacy
# "magic protocol bit skips authentication" bypass on internal connections.
#
# This is the single-host / demo provisioner.  A real multi-host deployment
# must distribute the same cluster CA and a per-host (or shared-host) node
# certificate to every segment data directory; the logic below (one node cert
# whose SANs cover the host identity, reused by all local segments) is correct
# for a single host and is the template for the gpinitsystem integration.
#
# Usage:
#   COORDINATOR_DATA_DIRECTORY=... PGPORT=... ./setup_internal_tls.sh
#-------------------------------------------------------------------------
set -euo pipefail

PGPORT=${PGPORT:-${COORDINATOR_DEMO_PORT:-5432}}
COORD_DIR=${COORDINATOR_DATA_DIRECTORY:-${MASTER_DATA_DIRECTORY:-}}
if [ -z "$COORD_DIR" ]; then
    echo "error: set COORDINATOR_DATA_DIRECTORY (or MASTER_DATA_DIRECTORY)" >&2
    exit 1
fi
HOST=$(hostname)
CA_DIR=${INTERNAL_TLS_CA_DIR:-${COORD_DIR}/internal_tls_ca}

echo "### generating cluster CA + node certificate in ${CA_DIR}"
rm -rf "$CA_DIR"; mkdir -p "$CA_DIR"
(
    cd "$CA_DIR"
    # Self-signed cluster CA.  Its private key signs every node certificate;
    # possession of a CA-signed cert is what proves cluster membership.
    openssl req -new -newkey rsa:4096 -days 3650 -nodes -x509 \
        -subj "/CN=warehouse-pg-cluster-CA" -keyout ca.key -out ca.crt 2>/dev/null
    # Node certificate, signed by the cluster CA, with SANs covering the host
    # identity so peers connecting by hostname or loopback validate cleanly.
    openssl req -new -newkey rsa:4096 -nodes \
        -subj "/CN=${HOST}" -keyout node.key -out node.csr 2>/dev/null
    openssl x509 -req -in node.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
        -extfile <(printf "subjectAltName=DNS:%s,DNS:localhost,IP:127.0.0.1" "$HOST") \
        -days 3650 -out node.crt -sha256 2>/dev/null
    chmod 600 node.key
)

# Enumerate every data directory in the cluster (coordinator, primaries,
# mirrors, standby) straight from the catalog so we do not hard-code topology.
mapfile -t DATADIRS < <(PGOPTIONS='-c gp_role=utility' \
    psql -tAq -p "$PGPORT" -d postgres \
    -c "select datadir from gp_segment_configuration order by dbid;")

echo "### installing certs + config into ${#DATADIRS[@]} data dirs"
for d in "${DATADIRS[@]}"; do
    [ -d "$d" ] || { echo "  skip (not local): $d"; continue; }
    cp "$CA_DIR/node.crt" "$d/server.crt"
    cp "$CA_DIR/node.key" "$d/server.key"
    cp "$CA_DIR/ca.crt"   "$d/root.crt"
    chmod 600 "$d/server.key"
    # Idempotent: replace any block we previously appended.
    sed -i '/# --- gp_internal_tls (managed) ---/,/# --- end gp_internal_tls ---/d' "$d/postgresql.conf"
    cat >> "$d/postgresql.conf" <<-EOF
	# --- gp_internal_tls (managed) ---
	ssl = on
	ssl_cert_file = 'server.crt'
	ssl_key_file = 'server.key'
	ssl_ca_file = 'root.crt'
	# Internal (intra-cluster) client identity.  In this single-CA demo it is the
	# same cluster certificate as ssl_*; "verify-ca" mandates it explicitly (no
	# fallback).  In a deployment whose coordinator serves external clients with a
	# different ssl_cert_file, these must point at the cluster certificate.
	gp_internal_tls_cert_file = 'server.crt'
	gp_internal_tls_key_file = 'server.key'
	gp_internal_tls_ca_file = 'root.crt'
	gp_internal_tls = verify-ca
	# --- end gp_internal_tls ---
	EOF
done
echo "### done; restart the cluster (gpstop -ar) to enforce mutual TLS"
