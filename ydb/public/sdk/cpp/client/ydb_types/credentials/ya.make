LIBRARY()

OWNER(g:kikimr)

SRCS(
    credentials.cpp
    login.cpp 
)

PEERDIR(
    ydb/library/login 
    ydb/public/api/grpc
    ydb/public/sdk/cpp/client/ydb_types/status
    ydb/library/yql/public/issue
)

END()
