RESOURCES_LIBRARY()

OWNER(
    g:arcadia-devtools
    g:yatool
)

SET(YMAKE_PYTHON3_LINUX sbr:2693706966)
SET(YMAKE_PYTHON3_DARWIN sbr:2693705780)
SET(YMAKE_PYTHON3_DARWIN_ARM64 sbr:2693704462)
SET(YMAKE_PYTHON3_WINDOWS sbr:2693706398)

DECLARE_EXTERNAL_HOST_RESOURCES_BUNDLE(
    YMAKE_PYTHON3
    ${YMAKE_PYTHON3_DARWIN} FOR DARWIN
    ${YMAKE_PYTHON3_DARWIN_ARM64} FOR DARWIN-ARM64
    ${YMAKE_PYTHON3_LINUX} FOR LINUX
    ${YMAKE_PYTHON3_WINDOWS} FOR WIN32
)

IF (OS_LINUX)
    DECLARE_EXTERNAL_RESOURCE(EXTERNAL_YMAKE_PYTHON3 ${YMAKE_PYTHON3_LINUX})
ELSEIF (OS_DARWIN)
    IF (ARCH_ARM64)
        DECLARE_EXTERNAL_RESOURCE(EXTERNAL_YMAKE_PYTHON3 ${YMAKE_PYTHON3_DARWIN_ARM64})
    ELSEIF(ARCH_X86_64)
        DECLARE_EXTERNAL_RESOURCE(EXTERNAL_YMAKE_PYTHON3 ${YMAKE_PYTHON3_DARWIN})
    ENDIF()
ELSEIF (OS_WINDOWS)
    DECLARE_EXTERNAL_RESOURCE(EXTERNAL_YMAKE_PYTHON3 ${YMAKE_PYTHON3_WINDOWS})
ENDIF()

END()
