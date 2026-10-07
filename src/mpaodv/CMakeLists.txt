build_lib(
  LIBNAME mpaodv
  SOURCE_FILES
    model/mpaodv-rtable.cc
    model/mpaodv-rqueue.cc
    model/mpaodv-packet.cc
    model/mpaodv-qtable.cc
    model/mpaodv-id-cache.cc
    model/mpaodv-dpd.cc
    model/mpaodv-neighbor.cc
    model/mpaodv-routing-protocol.cc
    helper/mpaodv-helper.cc
  HEADER_FILES
    model/mpaodv-rtable.h
    model/mpaodv-rqueue.h
    model/mpaodv-packet.h
    model/mpaodv-qtable.h
    model/mpaodv-id-cache.h
    model/mpaodv-dpd.h
    model/mpaodv-neighbor.h
    model/mpaodv-routing-protocol.h
    helper/mpaodv-helper.h
  LIBRARIES_TO_LINK ${libinternet}
                  ${libwifi}
)
