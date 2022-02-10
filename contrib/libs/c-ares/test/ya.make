# Generated by devtools/yamaker.

GTEST(arestest) 

OWNER(max42 g:cpp-contrib) 

LICENSE(MIT)

SIZE(MEDIUM)

TAG(ya:external)

REQUIREMENTS(network:full)

PEERDIR(
    contrib/libs/c-ares
)

ADDINCL(
    contrib/libs/c-ares
    contrib/libs/c-ares/test
)

NO_COMPILER_WARNINGS()

CFLAGS(
    -DHAVE_CONFIG_H
)

IF (NOT DLL_FOR)
    CFLAGS(
        -DCARES_STATICLIB
    )
ENDIF()

SRCS(
    ares-test-init.cc
    ares-test-internal.cc
    ares-test-live.cc
    ares-test-misc.cc
    ares-test-mock-ai.cc
    ares-test-mock.cc
    ares-test-ns.cc
    ares-test-parse-a.cc
    ares-test-parse-aaaa.cc
    ares-test-parse-mx.cc
    ares-test-parse-naptr.cc
    ares-test-parse-ns.cc
    ares-test-parse-ptr.cc
    ares-test-parse-soa-any.cc
    ares-test-parse-soa.cc
    ares-test-parse-srv.cc
    ares-test-parse-txt.cc
    ares-test-parse.cc
    ares-test.cc
    dns-proto-test.cc
    dns-proto.cc
)

END()
