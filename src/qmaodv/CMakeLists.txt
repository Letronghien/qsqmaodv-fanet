build_lib(
  LIBNAME qmaodv
  SOURCE_FILES
    model/qmaodv-rtable.cc
    model/qmaodv-rqueue.cc
    model/qmaodv-packet.cc
    model/qmaodv-qtable.cc
    model/qmaodv-id-cache.cc
    model/qmaodv-dpd.cc
    model/qmaodv-neighbor.cc
    model/qmaodv-routing-protocol.cc
    helper/qmaodv-helper.cc
  HEADER_FILES
    model/qmaodv-rtable.h
    model/qmaodv-rqueue.h
    model/qmaodv-packet.h
    model/qmaodv-qtable.h
    model/qmaodv-id-cache.h
    model/qmaodv-dpd.h
    model/qmaodv-neighbor.h
    model/qmaodv-routing-protocol.h
    helper/qmaodv-helper.h
  LIBRARIES_TO_LINK ${libinternet}
                  ${libwifi}
)
