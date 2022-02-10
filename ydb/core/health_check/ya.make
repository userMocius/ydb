LIBRARY() 
 
OWNER( 
    xenoxeno 
    g:kikimr 
) 
 
SRCS( 
    health_check.cpp 
    health_check.h 
) 
 
PEERDIR( 
    library/cpp/actors/core
    ydb/core/base
    ydb/core/blobstorage/base
    ydb/library/aclib
    ydb/public/api/protos
    ydb/library/yql/public/issue/protos
) 
 
END() 

RECURSE_FOR_TESTS(
    ut
)
