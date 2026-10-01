build_lib(
  LIBNAME qsqmaodv
  SOURCE_FILES
    model/qsqmaodv-rtable.cc
    model/qsqmaodv-rqueue.cc
    model/qsqmaodv-packet.cc
    model/qsqmaodv-qtable.cc
    model/qsqmaodv-id-cache.cc
    model/qsqmaodv-dpd.cc
    model/qsqmaodv-neighbor.cc
    model/qsqmaodv-routing-protocol.cc
    helper/qsqmaodv-helper.cc
  HEADER_FILES
    model/qsqmaodv-rtable.h
    model/qsqmaodv-rqueue.h
    model/qsqmaodv-packet.h
    model/qsqmaodv-qtable.h
    model/qsqmaodv-id-cache.h
    model/qsqmaodv-dpd.h
    model/qsqmaodv-neighbor.h
    model/qsqmaodv-routing-protocol.h
    helper/qsqmaodv-helper.h
  LIBRARIES_TO_LINK ${libinternet}
                  ${libwifi}
)
