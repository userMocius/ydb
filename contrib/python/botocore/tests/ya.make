OWNER(g:python-contrib roboslone)

PY23_TEST()

TEST_SRCS(
    SQS-119.py
)

PEERDIR(
    contrib/python/boto3
)

END()