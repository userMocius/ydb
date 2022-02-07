LIBRARY()

OWNER(g:kikimr)

PEERDIR(
    library/cpp/actors/core
    library/cpp/actors/interconnect
    library/cpp/monlib/service/pages
    ydb/core/base
    ydb/core/blobstorage/base
    ydb/core/blobstorage/groupinfo
    ydb/core/blobstorage/vdisk/anubis_osiris
    ydb/core/blobstorage/vdisk/common
    ydb/core/blobstorage/vdisk/hulldb/base
)

SRCS(
    defs.h
    blobstorage_syncer_committer.cpp
    blobstorage_syncer_committer.h
    blobstorage_syncer.cpp
    blobstorage_syncer_data.cpp
    blobstorage_syncer_data.h
    blobstorage_syncer_dataserdes.h
    blobstorage_syncer_defs.h
    blobstorage_syncer.h
    blobstorage_syncer_localwriter.cpp
    blobstorage_syncer_localwriter.h
    blobstorage_syncer_recoverlostdata.cpp
    blobstorage_syncer_recoverlostdata.h
    blobstorage_syncer_recoverlostdata_proxy.cpp
    blobstorage_syncer_recoverlostdata_proxy.h
    blobstorage_syncer_scheduler.cpp
    blobstorage_syncer_scheduler.h
    guid_firstrun.cpp
    guid_firstrun.h
    guid_propagator.cpp
    guid_propagator.h
    guid_proxybase.h
    guid_proxyobtain.cpp
    guid_proxyobtain.h
    guid_proxywrite.cpp
    guid_proxywrite.h
    guid_recovery.cpp
    guid_recovery.h
    syncer_context.h
    syncer_job_actor.cpp
    syncer_job_actor.h
    syncer_job_task.cpp
    syncer_job_task.h
)

END()

RECURSE_FOR_TESTS(
    ut
)
