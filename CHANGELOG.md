# Changelog
## v2.0 (ns-3.48)
- Port to ns-3.48; project layout like anbq/nbqmaodv (repo + private ns-3 tree ~/qsqmaodv-fanet/ns-3-qsq)
- MAC best-effort queue now actually read (AC_BE_NQOS on non-QoS MAC); per-next-hop occupancy
- Reward from MAC ACK / drop with measured hop delay; delay normalisation
- ACK-silence decay no longer erases negative Q-values
- Ablation switches for every mechanism; v1 switches reproduce v1 behaviour
- Simulator writes every parameter + diagnostics to CSV; energy model off by default
- Paired statistics with Holm correction; factorial ablation
## v1 (ns-3.40) — see docs/AUDIT_AND_FIXES_v2.md for known issues
