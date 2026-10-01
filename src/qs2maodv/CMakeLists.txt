build_lib(
  LIBNAME qs2maodv
  SOURCE_FILES
    helper/qs2maodv-helper.cc
    model/qs2maodv-dpd.cc
    model/qs2maodv-id-cache.cc
    model/qs2maodv-neighbor.cc
    model/qs2maodv-packet.cc
    model/qs2maodv-routing-protocol.cc
    model/qs2maodv-rqueue.cc
    model/qs2maodv-rtable.cc
    model/qs2maodv-qtable.cc
  HEADER_FILES
    helper/qs2maodv-helper.h
    model/qs2maodv-dpd.h
    model/qs2maodv-id-cache.h
    model/qs2maodv-neighbor.h
    model/qs2maodv-packet.h
    model/qs2maodv-routing-protocol.h
    model/qs2maodv-rqueue.h
    model/qs2maodv-rtable.h
    model/qs2maodv-qtable.h
  LIBRARIES_TO_LINK ${libinternet}
                    ${libwifi}
                    ${libenergy}
)
