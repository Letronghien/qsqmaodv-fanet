# QL-AODV (Ateya et al., Future Internet 17(10):473, 2025) — re-implementation used as an
# external baseline. Forked from the ns-3.48 AODV module (as the original authors did with
# ns-3.34 AODV); see docs/QLAODV_BASELINE.md for the mapping to the paper.
build_lib(
  LIBNAME qlaodv
  SOURCE_FILES
    helper/qlaodv-helper.cc
    model/qlaodv-dpd.cc
    model/qlaodv-id-cache.cc
    model/qlaodv-neighbor.cc
    model/qlaodv-packet.cc
    model/qlaodv-routing-protocol.cc
    model/qlaodv-rqueue.cc
    model/qlaodv-rtable.cc
  HEADER_FILES
    helper/qlaodv-helper.h
    model/qlaodv-dpd.h
    model/qlaodv-id-cache.h
    model/qlaodv-neighbor.h
    model/qlaodv-packet.h
    model/qlaodv-routing-protocol.h
    model/qlaodv-rqueue.h
    model/qlaodv-rtable.h
  LIBRARIES_TO_LINK
    ${libapplications}
    ${libinternet-apps}
    ${libwifi}
)
