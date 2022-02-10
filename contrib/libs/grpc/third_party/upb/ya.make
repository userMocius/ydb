# Generated by devtools/yamaker.

LIBRARY()

OWNER(g:cpp-contrib) 

LICENSE(
    BSD-3-Clause AND
    Public-Domain
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

ADDINCL(
    ${ARCADIA_BUILD_ROOT}/contrib/libs/grpc
    contrib/libs/grpc
    contrib/libs/grpc/third_party/upb
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

IF (OS_LINUX OR OS_DARWIN)
    CFLAGS(
        -DGRPC_POSIX_FORK_ALLOW_PTHREAD_ATFORK=1
    )
ENDIF()

SRCS(
    upb/decode.c
    upb/encode.c
    upb/msg.c
    upb/port.c
    upb/table.c
    upb/upb.c
)

END()
