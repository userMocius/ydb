PY23_LIBRARY()

LICENSE(Service-Py23-Proxy)

OWNER(g:devtools-contrib)

IF (PYTHON2)
    PEERDIR(contrib/python/protobuf/py2)
ELSE()
    PEERDIR(contrib/python/protobuf/py3)
ENDIF()

END()

RECURSE(
    py2
    py3
)
