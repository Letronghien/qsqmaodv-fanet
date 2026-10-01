build_lib(
  LIBNAME aomdv
  SOURCE_FILES
    model/aomdv-packet.cc
    model/aomdv-rtable.cc
    model/aomdv-routing-protocol.cc
    helper/aomdv-helper.cc
  HEADER_FILES
    model/aomdv-packet.h
    model/aomdv-rtable.h
    model/aomdv-routing-protocol.h
    helper/aomdv-helper.h
  LIBRARIES_TO_LINK
    ${libinternet}
    ${libwifi}
)
