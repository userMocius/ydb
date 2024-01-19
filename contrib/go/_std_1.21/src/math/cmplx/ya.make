GO_LIBRARY()
IF (FALSE)
    MESSAGE(FATAL this shall never happen)

ELSEIF (OS_LINUX AND ARCH_X86_64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ELSEIF (OS_LINUX AND ARCH_ARM64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ELSEIF (OS_LINUX AND ARCH_AARCH64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ELSEIF (OS_DARWIN AND ARCH_X86_64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ELSEIF (OS_DARWIN AND ARCH_ARM64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ELSEIF (OS_DARWIN AND ARCH_AARCH64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ELSEIF (OS_WINDOWS AND ARCH_X86_64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ELSEIF (OS_WINDOWS AND ARCH_ARM64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ELSEIF (OS_WINDOWS AND ARCH_AARCH64)
    SRCS(
		abs.go
		asin.go
		conj.go
		exp.go
		isinf.go
		isnan.go
		log.go
		phase.go
		polar.go
		pow.go
		rect.go
		sin.go
		sqrt.go
		tan.go
    )
ENDIF()
END()
