build_lib(
  LIBNAME pmaodv
  SOURCE_FILES
    model/pmaodv-routing-protocol.cc
    helper/pmaodv-helper.cc
  HEADER_FILES
    model/pmaodv-routing-protocol.h
    helper/pmaodv-helper.h
  LIBRARIES_TO_LINK
    ${libaomdv}
    ${libinternet}
    ${libwifi}
)
