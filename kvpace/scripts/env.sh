#!/bin/bash
# CPU assignment for local benchmark runs on the measured machine (Intel Core i9-12900K).
# Logical CPUs 0-15 are P-core hyper-thread pairs (0,1), (2,3), ... ; 16-23 are E-cores.
# Server and client use distinct physical P-cores (one logical CPU per core).
SERVER_CPUS=${SERVER_CPUS:-0,2,4,6}
CLIENT_CPUS=${CLIENT_CPUS:-8,10,12,14}
